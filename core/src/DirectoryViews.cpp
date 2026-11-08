#include "pecore/Views.h"

#include "pecore/Authenticode.h"
#include "pecore/Meta.h"
#include "pecore/Operations.h"
#include "pecore/Platform.h"

#include "PeHelpers.h"

#include "retdec/pelib/PeFile.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

namespace PeCore
{
    namespace
    {
        uint32_t PointerSize(
            const PeImage & image
        )
        {
            return image.Is64() ? 8 : 4;
        }

        uint64_t OrdinalFlag(
            const PeImage & image
        )
        {
            return image.Is64() ? 0x8000000000000000ull : 0x80000000ull;
        }

        uint64_t DirectoryOffset(
            const PeImage & image,
            uint32_t index
        )
        {
            DataDirectory directory = image.GetDirectory( index );
            return directory.m_Rva != 0 ? image.RvaToOffset( directory.m_Rva ) : NO_OFFSET;
        }

        // Editable size of a zero terminated name in place: its length plus the zero padding behind it, minus the
        // terminator that always stays. A longer name needs new space, that is the job of the 'Import Adder'.
        constexpr size_t NAME_EDIT_LIMIT = 255;

        size_t NameEditSize(
            const PeImage & image,
            uint64_t offset
        )
        {
            if (offset == NO_OFFSET)
            {
                return 0;
            }

            size_t length = image.ReadString( offset, NAME_EDIT_LIMIT ).size();
            size_t padding = 0;
            uint8_t byte = 0;

            // Zero bytes behind the name, the end of the file stops the run.
            while (length + padding <= NAME_EDIT_LIMIT && image.ReadAt( offset + length + padding, &byte, 1 ) && byte == 0)
            {
                ++padding;
            }

            return padding == 0 ? 0 : std::min( length + padding - 1, NAME_EDIT_LIMIT );
        }

        // Whole declaration of an MSVC decorated name, empty for plain names [and where nothing can undecorate it].
        std::string UndecoratedText(
            const std::string & name
        )
        {
            if (name.empty() || name[0] != '?')
            {
                return {};
            }

            std::string text = UndecorateName( name, true );
            return text != name ? text : std::string();
        }

        // The 'Undecorated name' column is added when any of the names is decorated.
        template< typename RANGE, typename NAME >
        bool AnyDecorated(
            const RANGE & range,
            NAME && name_of
        )
        {
            return std::any_of( std::begin( range ), std::end( range ), [&] ( const auto & item )
            {
                const std::string & name = name_of( item );
                return !name.empty() && name[0] == '?';
            } );
        }

        constexpr const char * UNDECORATED_TIP = "C++ declaration of the decorated name (Microsoft name decoration)";

        // Module name cell, editable in place when the name is backed by file data.
        void NameCell(
            ViewBuilder & view,
            const PeImage & image,
            uint64_t name_offset,
            const std::string & name
        )
        {
            size_t size = NameEditSize( image, name_offset );

            if (size == 0)
            {
                view.Cell( Utf8FromLatin1( name ) );
                return;
            }

            view.TextCell( name_offset, size, Utf8FromLatin1( name ) );
        }

        // RVA field. Value with the owning section as meaning.
        void RvaField(
            ViewBuilder & view,
            const PeImage & image,
            const std::string & name,
            uint64_t offset
        )
        {
            uint32_t value = image.Read32( offset );
            view.Field( name, offset, 4, value, image.SectionNameOfRva( value ) );
        }

        // VA field of the pointer size of the image.
        void VaField(
            ViewBuilder & view,
            const PeImage & image,
            const std::string & name,
            uint64_t offset,
            size_t size
        )
        {
            uint64_t value = image.ReadUnsigned( offset, size );
            std::string meaning;

            if (value != 0 && value >= image.GetImageBase())
            {
                uint64_t rva = value - image.GetImageBase();

                if (rva <= 0xFFFFFFFFull)
                {
                    meaning = "RVA " + Hex( rva );
                    std::string section = image.SectionNameOfRva( static_cast<uint32_t>(rva) );

                    if (!section.empty())
                    {
                        meaning += " (" + section + ")";
                    }
                }
            }

            view.Field( name, offset, size, value, meaning );
        }

        std::string GuidText(
            const uint8_t * data
        )
        {
            char buffer[48];
            uint32_t data1 = data[0] | data[1] << 8 | data[2] << 16 | static_cast<uint32_t>(data[3]) << 24;
            uint16_t data2 = static_cast<uint16_t>(data[4] | data[5] << 8);
            uint16_t data3 = static_cast<uint16_t>(data[6] | data[7] << 8);
            std::snprintf( buffer, sizeof( buffer ), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", data1, data2, data3,
                data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15] );
            return buffer;
        }

        // Locates the import descriptor index of a PeLib record by its name RVA, PeLib skips broken descriptors.
        uint64_t ImportDescriptorOffset(
            const PeImage & image,
            size_t index
        )
        {
            uint64_t base = DirectoryOffset( image, DIR_IMPORT );
            return base == NO_OFFSET ? NO_OFFSET : base + index * 20ull;
        }

        const char * UNWIND_OPS[] =
        {
            "UWOP_PUSH_NONVOL", "UWOP_ALLOC_LARGE", "UWOP_ALLOC_SMALL", "UWOP_SET_FPREG", "UWOP_SAVE_NONVOL", "UWOP_SAVE_NONVOL_FAR",
            "UWOP_EPILOG", "UWOP_SPARE_CODE", "UWOP_SAVE_XMM128", "UWOP_SAVE_XMM128_FAR", "UWOP_PUSH_MACHFRAME", "UWOP_11", "UWOP_12",
            "UWOP_13", "UWOP_14", "UWOP_15"
        };

        const char * REGISTERS_64[] =
        {
            "RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"
        };
    }

    std::string BuildExportView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        const PeLib::ExportDirectory & exports = image.GetFile().expDir();
        ViewBuilder view( "exports", "Export Directory", "list" );
        uint64_t offset = DirectoryOffset( image, DIR_EXPORT );
        DataDirectory directory = image.GetDirectory( DIR_EXPORT );

        if (offset == NO_OFFSET)
        {
            view.Warnings( { "The export directory is not backed by file data" } );
            return view.Finish();
        }

        view.BeginFields();
        view.FieldAt( image, "Characteristics", offset, 4 );
        view.FieldAt( image, "TimeDateStamp", offset + 4, 4, FormatTimestamp( image.Read32( offset + 4 ) ) );
        view.FieldAt( image, "MajorVersion", offset + 8, 2 );
        view.FieldAt( image, "MinorVersion", offset + 10, 2 );
        uint32_t dll_name_rva = image.Read32( offset + 12 );
        std::string dll_name = image.ReadStringAtRva( dll_name_rva, 512 );
        view.FieldAt( image, "Name", offset + 12, 4, Utf8FromLatin1( dll_name ) );

        // The string behind Name, editable in place like the import names.
        uint64_t dll_name_offset = dll_name_rva != 0 ? image.RvaToOffset( dll_name_rva ) : NO_OFFSET;
        size_t dll_name_size = NameEditSize( image, dll_name_offset );
        view.TextField( "Name (string)", dll_name_size != 0 ? dll_name_offset : NO_OFFSET, dll_name_size, Utf8FromLatin1( dll_name ), Editor::EDITOR_TEXT );
        view.FieldAt( image, "Base", offset + 16, 4 );
        view.FieldAt( image, "NumberOfFunctions", offset + 20, 4, std::to_string( image.Read32( offset + 20 ) ) );
        view.FieldAt( image, "NumberOfNames", offset + 24, 4, std::to_string( image.Read32( offset + 24 ) ) );
        RvaField( view, image, "AddressOfFunctions", offset + 28 );
        RvaField( view, image, "AddressOfNames", offset + 32 );
        RvaField( view, image, "AddressOfNameOrdinals", offset + 36 );
        view.EndFields();

        uint32_t functions_rva = image.Read32( offset + 28 );
        uint32_t names_rva = image.Read32( offset + 32 );
        uint32_t ordinals_rva = image.Read32( offset + 36 );
        uint64_t functions_offset = image.RvaToOffset( functions_rva );
        uint64_t names_offset = image.RvaToOffset( names_rva );
        uint64_t ordinals_offset = image.RvaToOffset( ordinals_rva );
        uint32_t base = image.Read32( offset + 16 );

        // Position of each function in the name arrays, for the cell offsets
        std::map< uint32_t, uint32_t > name_index;
        uint32_t number_of_names = std::min< uint32_t >( image.Read32( offset + 24 ), 0x100000 );

        for (uint32_t index = 0; ordinals_offset != NO_OFFSET && index < number_of_names; ++index)
        {
            name_index.emplace( image.Read16( ordinals_offset + index * 2ull ), index );
        }

        bool decorated = false;

        for (uint32_t index = 0; index < exports.calcNumberOfFunctions() && !decorated; ++index)
        {
            const std::string & name = exports.getFunctionName( index );
            decorated = !name.empty() && name[0] == '?';
        }

        std::vector< std::pair< std::string, std::string > > columns = { { "Ordinal", "Base + index" }, { "Function RVA", "AddressOfFunctions[ index ], Dword" },
            { "Name Ordinal", "AddressOfNameOrdinals entry, Word" }, { "Name RVA", "AddressOfNames entry, Dword" },
            { "Name", "" }, { "Forwarder or section", "" } };

        if (decorated)
        {
            columns.push_back( { "Undecorated name", UNDECORATED_TIP } );
        }

        view.Columns( columns );
        view.BeginRows();

        for (uint32_t index = 0; index < exports.calcNumberOfFunctions(); ++index)
        {
            uint32_t function_rva = exports.getAddressOfFunction( index );
            uint64_t function_cell = functions_offset != NO_OFFSET ? functions_offset + index * 4ull : NO_OFFSET;
            view.BeginRow( std::string(), function_cell, 4 );
            view.Cell( Hex( base + index ) );
            view.ValueCell( function_cell, 4, function_rva );
            auto name = name_index.find( index );

            if (name != name_index.end())
            {
                view.ValueCell( ordinals_offset + name->second * 2ull, 2, index );
                view.ValueCell( names_offset != NO_OFFSET ? names_offset + name->second * 4ull : NO_OFFSET, 4, exports.getAddressOfName( index ) );
                uint32_t name_rva = exports.getAddressOfName( index );
                NameCell( view, image, name_rva != 0 ? image.RvaToOffset( name_rva ) : NO_OFFSET, exports.getFunctionName( index ) );
            }
            else
            {
                view.Cell( "" );
                view.Cell( "" );
                view.Cell( "(by ordinal)" );
            }

            // An RVA inside the export directory is a forwarder string "dll.function"
            if (function_rva >= directory.m_Rva && function_rva < static_cast<uint64_t>( directory.m_Rva ) + directory.m_Size)
            {
                NameCell( view, image, image.RvaToOffset( function_rva ), image.ReadStringAtRva( function_rva, 512 ) );
            }
            else
            {
                view.Cell( function_rva != 0 ? image.SectionNameOfRva( function_rva ) : std::string() );
            }

            if (decorated)
            {
                view.Cell( name != name_index.end() ? UndecoratedText( exports.getFunctionName( index ) ) : std::string() );
            }

            view.EndRow();
        }

        view.EndRows();
        view.DetailLayout( "none" );

        // The loader binary searches AddressOfNames, a rename that breaks the order hides names from GetProcAddress
        std::string previous;

        for (uint32_t index = 0; names_offset != NO_OFFSET && index < number_of_names; ++index)
        {
            std::string current = image.ReadStringAtRva( image.Read32( names_offset + index * 4ull ), 512 );

            if (index > 0 && current < previous)
            {
                view.Warnings( { "AddressOfNames is not sorted (\"" + Utf8FromLatin1( current ) + "\" follows \"" + Utf8FromLatin1( previous ) +
                    "\"). GetProcAddress uses a binary search, names out of order may not be found." } );
                break;
            }

            previous = std::move( current );
        }

        return view.Finish();
    }

    std::string BuildImportView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "imports", "Import Directory", "list" );
        view.Columns( { { "Module Name", "" }, { "Imports", "Number of functions" }, { "OriginalFirstThunk", "Import name table RVA, Dword" },
            { "TimeDateStamp", "Dword, -1 when bound" }, { "ForwarderChain", "Dword" }, { "Name", "Name RVA, Dword" }, { "FirstThunk", "Import address table RVA, Dword" } } );
        view.BeginRows();
        const std::vector< ImportModule > list = ReadImports( image );

        for (size_t index = 0; index < list.size(); ++index)
        {
            const ImportModule & module = list[index];
            uint64_t offset = ImportDescriptorOffset( image, index );
            view.BeginRow( "imports/" + std::to_string( index ), offset, 20 );
            NameCell( view, image, module.m_NameRva != 0 ? image.RvaToOffset( module.m_NameRva ) : NO_OFFSET, module.m_Name );
            view.Cell( std::to_string( module.m_Functions.size() ) );
            view.ValueCellAt( image, offset, 4 );
            view.ValueCellAt( image, offset + 4, 4 );
            view.ValueCellAt( image, offset + 8, 4 );
            view.ValueCellAt( image, offset + 12, 4 );
            view.ValueCellAt( image, offset + 16, 4 );
            view.EndRow();
        }

        view.EndRows();
        view.DetailLayout( "list" );
        view.BeginCommands();
        view.Command( "imports.move_up", "Move up (load before)", "row", list.size() > 1 );
        view.Command( "imports.move_down", "Move down (load after)", "row", list.size() > 1 );
        view.Command( "imports.delete", "Delete import descriptor", "row", !list.empty() );
        view.EndCommands();
        return view.Finish();
    }

    std::string BuildImportDetailView(
        const ViewContext & context,
        size_t index
    )
    {
        const PeImage & image = context.m_Image;
        const std::vector< ImportModule > list = ReadImports( image );
        Require( index < list.size(), ErrorCode::ERR_NOT_FOUND, "No such import descriptor" );
        const ImportModule & module = list[index];
        ViewBuilder view( "imports/" + std::to_string( index ), Utf8FromLatin1( module.m_Name ), "list" );
        uint32_t pointer = PointerSize( image );
        uint64_t oft_offset = module.m_OriginalFirstThunk != 0 ? image.RvaToOffset( module.m_OriginalFirstThunk ) : NO_OFFSET;
        uint64_t ft_offset = image.RvaToOffset( module.m_FirstThunk );
        bool decorated = AnyDecorated( module.m_Functions, [] ( const ImportFunction & thunk ) -> const std::string &
        {
            return thunk.m_Name;
        } );
        std::vector< std::pair< std::string, std::string > > columns = { { "OriginalFirstThunk", pointer == 8 ? "Import name table entry, Qword" : "Import name table entry, Dword" },
            { "FirstThunk", pointer == 8 ? "Import address table entry, Qword" : "Import address table entry, Dword" },
            { "Hint", "Word" }, { "Name", "" }, { "IAT RVA", "RVA the loader patches" } };

        if (decorated)
        {
            columns.push_back( { "Undecorated name", UNDECORATED_TIP } );
        }

        view.Columns( columns );
        view.BeginRows();

        for (size_t function = 0; function < module.m_Functions.size(); ++function)
        {
            const ImportFunction & thunk = module.m_Functions[function];
            uint64_t oft_cell = oft_offset != NO_OFFSET ? oft_offset + function * pointer : NO_OFFSET;
            uint64_t ft_cell = ft_offset != NO_OFFSET ? ft_offset + function * pointer : NO_OFFSET;
            uint64_t lookup = oft_cell != NO_OFFSET ? image.ReadUnsigned( oft_cell, pointer ) : image.ReadUnsigned( ft_cell, pointer );
            bool by_ordinal = (lookup & OrdinalFlag( image )) != 0;
            uint64_t hint_offset = !by_ordinal && lookup != 0 ? image.RvaToOffset( static_cast<uint32_t>( lookup ) ) : NO_OFFSET;

            view.BeginRow( std::string(), ft_cell, pointer );

            if (oft_cell != NO_OFFSET)
            {
                view.ValueCellAt( image, oft_cell, pointer );
            }
            else
            {
                view.Cell( "" );
            }

            if (ft_cell != NO_OFFSET)
            {
                view.ValueCellAt( image, ft_cell, pointer );
            }
            else
            {
                view.Cell( "" );
            }

            if (hint_offset != NO_OFFSET)
            {
                view.ValueCellAt( image, hint_offset, 2 );
                NameCell( view, image, hint_offset + 2, thunk.m_Name );
            }
            else
            {
                view.Cell( "" );
                view.Cell( "Ordinal " + Hex( lookup & 0xFFFF, 4 ) + " (" + std::to_string( lookup & 0xFFFF ) + ")" );
            }

            view.Cell( Hex( thunk.m_PatchRva ) );

            if (decorated)
            {
                view.Cell( hint_offset != NO_OFFSET ? UndecoratedText( thunk.m_Name ) : std::string() );
            }

            view.EndRow();
        }

        view.EndRows();
        return view.Finish();
    }

    std::string BuildExceptionView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "exceptions", "Exception Directory", "list" );
        DataDirectory directory = image.GetDirectory( DIR_EXCEPTION );
        uint64_t offset = DirectoryOffset( image, DIR_EXCEPTION );
        uint16_t machine = image.GetMachine();
        bool arm64 = machine == 0xAA64 || machine == 0xA641 || machine == 0xA64E;
        bool x64 = machine == 0x8664;

        if (offset == NO_OFFSET || (!arm64 && !x64))
        {
            view.Warnings( { offset == NO_OFFSET ? "The exception directory is not backed by file data"
                : "Exception data of " + DescribeEnum( "machine", machine ) + " is shown as raw data" } );

            if (offset != NO_OFFSET)
            {
                view.HexRegion( offset, directory.m_Size, "Exception directory" );
            }

            return view.Finish();
        }

        uint32_t entry_size = arm64 ? 8 : 12;

        // Function names from the PDB ['Debug Directory', 'Load symbols'] or the exports.
        bool named = context.m_Symbols != nullptr || image.HasDirectory( DIR_EXPORT );
        AddressNames names( image, context.m_Symbols );
        std::pair< std::string, std::string > function_column { "Function", context.m_Symbols != nullptr ? "From the loaded PDB" :
            "From the exports, load the PDB in the Debug Directory for all names" };
        size_t count = std::min< size_t >( directory.m_Size / entry_size, 0x200000 );

        if (arm64)
        {
            std::vector< std::pair< std::string, std::string > > columns { { "BeginAddress", "Function start RVA, Dword" },
                { "UnwindData", "Packed unwind data or .xdata RVA, Dword" },
                { "Kind", "" }, { "Section", "" } };

            if (named)
            {
                columns.push_back( function_column );
            }

            view.Columns( columns );
        }
        else
        {
            std::vector< std::pair< std::string, std::string > > columns { { "BeginAddress", "Function start RVA, Dword" }, { "EndAddress", "Function end RVA, Dword" },
                { "UnwindInfoAddress", "UNWIND_INFO RVA, Dword" }, { "Function size", "" }, { "Section", "" } };

            if (named)
            {
                columns.push_back( function_column );
            }

            view.Columns( columns );
        }

        view.BeginRows();

        for (size_t index = 0; index < count; ++index)
        {
            uint64_t row = offset + index * entry_size;

            if (row + entry_size > image.GetFileSize())
            {
                break;
            }

            uint32_t begin = image.Read32( row );

            if (arm64)
            {
                uint32_t unwind = image.Read32( row + 4 );
                view.BeginRow( (unwind & 3) == 0 ? "exceptions/" + std::to_string( index ) : std::string(), row, entry_size );
                view.ValueCellAt( image, row, 4 );
                view.ValueCellAt( image, row + 4, 4 );
                view.Cell( (unwind & 3) == 0 ? ".xdata record" : (unwind & 3) == 1 ? "Packed unwind data" : "Packed (fragment)" );
            }
            else
            {
                uint32_t end = image.Read32( row + 4 );
                view.BeginRow( "exceptions/" + std::to_string( index ), row, entry_size );
                view.ValueCellAt( image, row, 4 );
                view.ValueCellAt( image, row + 4, 4 );
                view.ValueCellAt( image, row + 8, 4 );
                view.Cell( end >= begin ? Hex( end - begin, 1 ) + "h" : std::string( "invalid" ) );
            }

            view.Cell( image.SectionNameOfRva( begin ) );

            if (named)
            {
                view.Cell( names.Describe( begin ) );
            }

            view.EndRow();
        }

        view.EndRows();
        view.DetailLayout( "list" );
        return view.Finish();
    }

    std::string BuildExceptionDetailView(
        const ViewContext & context,
        size_t index
    )
    {
        const PeImage & image = context.m_Image;
        uint64_t directory_offset = DirectoryOffset( image, DIR_EXCEPTION );
        Require( directory_offset != NO_OFFSET, ErrorCode::ERR_NOT_FOUND, "No exception directory" );
        uint16_t machine = image.GetMachine();
        bool arm64 = machine == 0xAA64 || machine == 0xA641 || machine == 0xA64E;
        uint64_t row = directory_offset + index * (arm64 ? 8ull : 12ull);
        uint32_t unwind_rva = arm64 ? image.Read32( row + 4 ) : image.Read32( row + 8 ) & ~1u;
        uint64_t unwind = image.RvaToOffset( unwind_rva );
        ViewBuilder view( "exceptions/" + std::to_string( index ), "Unwind info of " + Hex( image.Read32( row ) ), "list" );

        if (unwind == NO_OFFSET)
        {
            view.Warnings( { "The unwind information is not backed by file data" } );
            return view.Finish();
        }

        if (arm64)
        {
            // '.xdata' header word. Function length, version, X, E, epilog count, code words.
            uint32_t header = image.Read32( unwind );
            view.BeginFields();
            view.Field( "Header", unwind, 4, header, "Function length " + Hex( (header & 0x3FFFF) * 4, 1 ) + "h, version " + std::to_string( (header >> 18) & 3 ) +
                ((header >> 20) & 1 ? ", has exception data" : "") + ((header >> 21) & 1 ? ", single epilog" : "") +
                ", epilogs " + std::to_string( (header >> 22) & 0x1F ) + ", code words " + std::to_string( (header >> 27) & 0x1F ) );
            view.EndFields();
            view.HexRegion( unwind, 4 + (((header >> 27) & 0x1F) + ((header >> 22) & 0x1F)) * 4ull, "Unwind record" );
            return view.Finish();
        }

        uint8_t version_flags = image.Read8( unwind );
        uint8_t codes = image.Read8( unwind + 2 );
        uint8_t frame = image.Read8( unwind + 3 );
        view.BeginFields();
        view.Field( "Version", unwind, 1, version_flags & 7, "", Editor::EDITOR_NONE );
        view.Field( "Flags", unwind, 1, version_flags >> 3, DescribeFlags( "unwind_flags", version_flags >> 3 ), Editor::EDITOR_NONE );
        view.FieldAt( image, "SizeOfProlog", unwind + 1, 1 );
        view.FieldAt( image, "CountOfCodes", unwind + 2, 1 );
        view.Field( "FrameRegister", unwind + 3, 1, frame & 0x0F, (frame & 0x0F) != 0 ? REGISTERS_64[frame & 0x0F] : "None", Editor::EDITOR_NONE );
        view.Field( "FrameOffset", unwind + 3, 1, frame >> 4, "Scaled by 16: " + Hex( (frame >> 4) * 16, 1 ) + "h", Editor::EDITOR_NONE );

        uint64_t after = unwind + 4 + ((codes + 1ull) & ~1ull) * 2;
        uint8_t flags = version_flags >> 3;

        if (flags & 0x04)
        {
            view.FieldAt( image, "ChainedFunction.BeginAddress", after, 4 );
            view.FieldAt( image, "ChainedFunction.EndAddress", after + 4, 4 );
            view.FieldAt( image, "ChainedFunction.UnwindInfo", after + 8, 4 );
        }
        else if (flags & 0x03)
        {
            RvaField( view, image, "ExceptionHandler", after );
        }

        view.EndFields();
        view.Columns( { { "Offset in prolog", "Byte" }, { "Operation", "" }, { "Info", "" } } );
        view.BeginRows();

        for (uint32_t code = 0; code < codes; ++code)
        {
            uint64_t cell = unwind + 4 + code * 2ull;
            uint8_t prolog = image.Read8( cell );
            uint8_t op_info = image.Read8( cell + 1 );
            uint8_t op = op_info & 0x0F;
            uint8_t info = op_info >> 4;
            std::string detail;

            switch (op)
            {
                case 0:
                    detail = REGISTERS_64[info];
                    break;
                case 1:
                    detail = info == 0 ? "Size " + Hex( image.Read16( cell + 2 ) * 8ull, 1 ) + "h" : "Size " + Hex( image.Read32( cell + 2 ), 1 ) + "h";
                    code += info == 0 ? 1 : 2;
                    break;
                case 2:
                    detail = "Size " + Hex( info * 8 + 8, 1 ) + "h";
                    break;
                case 3:
                    detail = "Frame pointer";
                    break;
                case 4:
                case 8:
                    detail = std::string( op == 4 ? REGISTERS_64[info] : "XMM" + std::to_string( info ) ) + " at " + Hex( image.Read16( cell + 2 ) * (op == 4 ? 8ull : 16ull), 1 ) + "h";
                    code += 1;
                    break;
                case 5:
                case 9:
                    detail = std::string( op == 5 ? REGISTERS_64[info] : "XMM" + std::to_string( info ) ) + " at " + Hex( image.Read32( cell + 2 ), 1 ) + "h";
                    code += 2;
                    break;
                case 10:
                    detail = info ? "With error code" : "Without error code";
                    break;
                default:
                    detail = "Info " + std::to_string( info );
                    break;
            }

            view.BeginRow( std::string(), cell, 2 );
            view.Cell( Hex( prolog, 2 ) );
            view.Cell( UNWIND_OPS[op] );
            view.Cell( detail );
            view.EndRow();
        }

        view.EndRows();
        return view.Finish();
    }

    std::string BuildSecurityView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        const Document & document = context.m_Document;
        DataDirectory directory = image.GetDirectory( DIR_SECURITY );
        ViewBuilder view( "security", "Security Directory", "list" );
        std::shared_ptr< const AuthenticodeInfo > info_pointer = GetAuthenticodeInfo( document, image );
        const AuthenticodeInfo & info = *info_pointer;
        std::vector< std::string > warnings = info.m_Problems;

        // 'WinVerifyTrust' hashes the whole file again, once per revision is enough. It reads a file, the saved one, or a
        // temporary copy of the edited bytes.
        std::shared_ptr< const TrustResult > trust_pointer = document.GetCachedObject< TrustResult >( "trust", [&]
        {
            TrustResult trust;
            std::error_code error;

            if (!IsTrustVerificationAvailable())
            {
                return VerifyFileTrust( document.GetPath() );
            }

            if (!document.IsModified() && !document.GetPath().empty() && std::filesystem::exists( document.GetPath(), error ))
            {
                return VerifyFileTrust( document.GetPath() );
            }

            std::filesystem::path copy = std::filesystem::temp_directory_path( error ) / ("pecore_trust_" + std::to_string( document.GetRevision() ) + ".bin");

            try
            {
                WriteWholeFile( copy, document.GetData() );
                trust = VerifyFileTrust( copy );
            }
            catch (const Error & failure)
            {
                trust.m_Message = std::string( "Cannot write a temporary copy: " ) + failure.what();
            }

            std::filesystem::remove( copy, error );
            return trust;
        } );
        const TrustResult & trust = *trust_pointer;

        view.BeginFields();
        view.InfoField( "Status", DescribeAuthenticode( info ) );
        view.InfoField( "Windows trust", trust.m_Available ? trust.m_Message + (document.IsModified() ? " (edited bytes, not saved)" : "") : trust.m_Message );

        for (size_t index = 0; index < info.m_Signatures.size(); ++index)
        {
            const SignatureInfo & signature = info.m_Signatures[index];
            std::string prefix = signature.m_Depth == 0 && index == 0 ? std::string() : "Nested " + std::to_string( index ) + ": ";
            std::string signer = signature.m_SignerIndex >= 0 ? signature.m_Certificates[signature.m_SignerIndex].m_SubjectName : std::string( "unknown" );
            std::string issuer = signature.m_SignerIndex >= 0 ? signature.m_Certificates[signature.m_SignerIndex].m_IssuerName : std::string();

            view.InfoField( prefix + "Signer", signer + (issuer.empty() ? "" : ", issued by " + issuer) );
            view.InfoField( prefix + "Algorithm", signature.m_DigestAlgorithm + " digest, " + signature.m_SignatureAlgorithm +
                (signature.m_PageHashes ? ", page hashes" : "") );
            view.InfoField( prefix + "Signed digest", HashToHex( signature.m_SignedDigest, true ) );
            view.InfoField( prefix + "File digest", signature.m_AlgorithmSupported ? HashToHex( signature.m_FileDigest, true ) +
                (signature.m_DigestMatches ? "  (matches)" : "  (MISMATCH)") : std::string( "Algorithm not supported" ) );

            if (signature.m_ContentDigestChecked)
            {
                view.InfoField( prefix + "Signed attributes", signature.m_ContentDigestMatches ? "messageDigest matches the signed content" :
                    "messageDigest does not match, the signature blob is damaged" );
            }

            if (!signature.m_SigningTime.empty())
            {
                view.InfoField( prefix + "Signing time", signature.m_SigningTime + " (claimed by the signer)" );
            }

            view.InfoField( prefix + "Timestamp", signature.m_Timestamp.m_Present ?
                signature.m_Timestamp.m_Time + " by " + signature.m_Timestamp.m_Authority + " (" + signature.m_Timestamp.m_Kind + ", " +
                signature.m_Timestamp.m_DigestAlgorithm + ")" :
                std::string( "None, the signature is only valid while the signer certificate is" ) );

            if (!signature.m_ProgramName.empty() || !signature.m_MoreInfo.empty())
            {
                view.InfoField( prefix + "Description", signature.m_ProgramName + (signature.m_MoreInfo.empty() ? "" : "  " + signature.m_MoreInfo) );
            }

            if (signature.m_AlgorithmSupported && !signature.m_DigestMatches)
            {
                warnings.push_back( "The " + signature.m_DigestAlgorithm + " file digest does not match the signature: the file was changed after signing" +
                    " and Windows rejects it. Re-sign the file after editing." );
            }

            for (const std::string & problem : signature.m_Problems)
            {
                warnings.push_back( prefix + problem );
            }
        }

        view.EndFields();
        view.Warnings( warnings );

        view.Columns( { { "Offset", "File offset of WIN_CERTIFICATE" }, { "dwLength", "Dword" }, { "wRevision", "Word" }, { "wCertificateType", "Word" }, { "Type", "" } } );
        view.BeginRows();

        // The security directory "RVA" is a file offset, entries are 8 byte aligned
        uint64_t offset = directory.m_Rva;
        uint64_t end = std::min< uint64_t >( static_cast<uint64_t>(directory.m_Rva) + directory.m_Size, image.GetFileSize() );
        size_t index = 0;

        while (offset + 8 <= end && index < 64)
        {
            uint32_t length = image.Read32( offset );
            view.BeginRow( "security/" + std::to_string( index ), offset, 8 );
            view.Cell( Hex( offset ) );
            view.ValueCellAt( image, offset, 4 );
            view.ValueCellAt( image, offset + 4, 2, Editor::EDITOR_ENUM, "certificate_revision" );
            view.ValueCellAt( image, offset + 6, 2, Editor::EDITOR_ENUM, "certificate_type" );
            view.Cell( DescribeEnum( "certificate_type", image.Read16( offset + 6 ) ) );
            view.EndRow();

            if (length < 8)
            {
                break;
            }

            offset += (length + 7ull) & ~7ull;
            ++index;
        }

        view.EndRows();
        view.DetailLayout( "list" );
        view.BeginCommands();
        view.Command( "security.save", "Save certificate (PKCS #7)...", "row" );
        view.Command( "security.remove", "Remove signature", "view" );
        view.EndCommands();
        return view.Finish();
    }

    std::string BuildSecurityDetailView(
        const ViewContext & context,
        size_t index
    )
    {
        const PeImage & image = context.m_Image;
        DataDirectory directory = image.GetDirectory( DIR_SECURITY );
        uint64_t offset = directory.m_Rva;
        uint64_t end = std::min< uint64_t >( static_cast<uint64_t>(directory.m_Rva) + directory.m_Size, image.GetFileSize() );

        for (size_t skip = 0; skip < index && offset + 8 <= end; ++skip)
        {
            offset += (image.Read32( offset ) + 7ull) & ~7ull;
        }

        Require( offset + 8 <= end, ErrorCode::ERR_NOT_FOUND, "No such certificate" );
        uint32_t length = image.Read32( offset );
        uint64_t blob_length = std::min< uint64_t >( length >= 8 ? length - 8 : 0, end - offset - 8 );
        ViewBuilder view( "security/" + std::to_string( index ), "Certificates of entry " + std::to_string( index ), "list" );
        view.Info( DescribeEnum( "certificate_type", image.Read16( offset + 6 ) ) + ", " + FormatBytes( blob_length ) +
            " at " + Hex( offset + 8 ) + ". Save certificate exports the PKCS #7 blob." );
        view.Columns( { { "Signature", "Primary or nested signature" }, { "Role", "" }, { "Subject", "Common name" }, { "Issued by", "" },
            { "Valid from", "UTC" }, { "Valid to", "UTC" }, { "Serial number", "" }, { "Thumbprint", "SHA-1 of the certificate" },
            { "Signature algorithm", "" }, { "Subject (full)", "" } } );
        view.BeginRows();

        if (image.Read16( offset + 6 ) == 0x0002)
        {
            std::vector< SignatureInfo > signatures = ParseAuthenticodeSignature( image.Slice( offset + 8, blob_length ), image );

            for (size_t number = 0; number < signatures.size(); ++number)
            {
                for (const CertificateInfo & certificate : signatures[number].m_Certificates)
                {
                    view.BeginRow();
                    view.Cell( number == 0 ? "Primary (" + signatures[number].m_DigestAlgorithm + ")" :
                        "Nested " + std::to_string( number ) + " (" + signatures[number].m_DigestAlgorithm + ")" );
                    view.Cell( certificate.m_Role.empty() ? "Chain" : certificate.m_Role );
                    view.Cell( certificate.m_SubjectName );
                    view.Cell( certificate.m_IssuerName );
                    view.Cell( certificate.m_ValidFrom );
                    view.Cell( certificate.m_ValidTo );
                    view.Cell( certificate.m_Serial );
                    view.Cell( certificate.m_Thumbprint );
                    view.Cell( certificate.m_SignatureAlgorithm );
                    view.Cell( certificate.m_Subject );
                    view.EndRow();
                }
            }
        }

        view.EndRows();
        view.DetailLayout( "none" );
        return view.Finish();
    }

    std::string BuildRelocationView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        const PeLib::RelocationsDirectory & relocations = image.GetFile().relocDir();
        ViewBuilder view( "relocs", "Relocation Directory", "list" );
        view.Columns( { { "VirtualAddress", "Page RVA, Dword" }, { "SizeOfBlock", "Dword" }, { "Items", "Number of entries" }, { "Section", "" } } );
        view.BeginRows();
        uint64_t offset = DirectoryOffset( image, DIR_BASERELOC );

        for (unsigned int block = 0; block < relocations.calcNumberOfRelocations() && offset != NO_OFFSET; ++block)
        {
            uint32_t page = relocations.getVirtualAddress( block );
            uint32_t size = relocations.getSizeOfBlock( block );
            view.BeginRow( "relocs/" + std::to_string( block ), offset, 8 );
            view.ValueCellAt( image, offset, 4 );
            view.ValueCellAt( image, offset + 4, 4 );
            view.Cell( std::to_string( relocations.calcNumberOfRelocationData( block ) ) );
            view.Cell( image.SectionNameOfRva( page ) );
            view.EndRow();
            offset += std::max< uint32_t >( size, 8 );
        }

        view.EndRows();
        view.DetailLayout( "list" );
        return view.Finish();
    }

    std::string BuildRelocationDetailView(
        const ViewContext & context,
        size_t index
    )
    {
        const PeImage & image = context.m_Image;
        const PeLib::RelocationsDirectory & relocations = image.GetFile().relocDir();
        Require( index < relocations.calcNumberOfRelocations(), ErrorCode::ERR_NOT_FOUND, "No such relocation block" );
        uint64_t offset = DirectoryOffset( image, DIR_BASERELOC );

        for (size_t block = 0; block < index; ++block)
        {
            offset += std::max< uint32_t >( relocations.getSizeOfBlock( static_cast<unsigned int>( block ) ), 8 );
        }

        uint32_t page = relocations.getVirtualAddress( static_cast<unsigned int>( index ) );
        ViewBuilder view( "relocs/" + std::to_string( index ), "Relocations of page " + Hex( page ), "list" );
        view.Columns( { { "Item", "Type in the high 4 bits, page offset in the low 12, Word" }, { "RVA", "" }, { "Type", "" }, { "Target", "Current pointer value" } } );
        view.BeginRows();
        unsigned int count = relocations.calcNumberOfRelocationData( static_cast<unsigned int>( index ) );

        for (unsigned int item = 0; item < count; ++item)
        {
            uint64_t cell = offset + 8 + item * 2ull;
            uint16_t data = relocations.getRelocationData( static_cast<unsigned int>( index ), item );
            uint32_t type = data >> 12;
            uint32_t rva = page + (data & 0x0FFF);
            std::string target;

            if (type == 3 || type == 10)
            {
                uint64_t target_offset = image.RvaToOffset( rva );

                if (target_offset != NO_OFFSET)
                {
                    target = Hex( image.ReadUnsigned( target_offset, type == 10 ? 8 : 4 ), type == 10 ? 16 : 8 );
                }
            }

            view.BeginRow( std::string(), cell, 2 );
            view.ValueCellAt( image, cell, 2 );
            view.Cell( type == 0 ? std::string() : Hex( rva ) );
            std::string name = DescribeEnum( "relocation_type", type );
            view.Cell( name.empty() ? std::to_string( type ) : name.substr( 0, name.find( ' ' ) ) );
            view.Cell( target );
            view.EndRow();
        }

        view.EndRows();
        return view.Finish();
    }

    std::string BuildDebugView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "debug", "Debug Directory", "list" );
        view.Columns( { { "Characteristics", "Dword" }, { "TimeDateStamp", "Dword" }, { "MajorVersion", "Word" }, { "MinorVersion", "Word" }, { "Type", "Dword" },
            { "SizeOfData", "Dword" }, { "AddressOfRawData", "RVA, Dword" }, { "PointerToRawData", "File offset, Dword" }, { "Description", "" } } );
        view.BeginRows();
        uint64_t offset = DirectoryOffset( image, DIR_DEBUG );
        DataDirectory directory = image.GetDirectory( DIR_DEBUG );
        size_t count = offset != NO_OFFSET ? std::min< size_t >( directory.m_Size / 28, 64 ) : 0;

        for (size_t index = 0; index < count; ++index)
        {
            uint64_t row = offset + index * 28;
            uint32_t type = image.Read32( row + 12 );
            uint32_t pointer = image.Read32( row + 24 );
            std::string description = DescribeEnum( "debug_type", type );

            if (type == 2 && image.Read32( pointer ) == 0x53445352)
            {
                description = "PDB " + Utf8FromLatin1( image.ReadString( pointer + 24, 1024 ) );
            }

            view.BeginRow( "debug/" + std::to_string( index ), row, 28 );
            view.ValueCellAt( image, row, 4 );
            view.ValueCellAt( image, row + 4, 4 );
            view.ValueCellAt( image, row + 8, 2 );
            view.ValueCellAt( image, row + 10, 2 );
            view.ValueCellAt( image, row + 12, 4, Editor::EDITOR_ENUM, "debug_type" );
            view.ValueCellAt( image, row + 16, 4 );
            view.ValueCellAt( image, row + 20, 4 );
            view.ValueCellAt( image, row + 24, 4 );
            view.Cell( description );
            view.EndRow();
        }

        view.EndRows();
        view.DetailLayout( "fields" );

        // PDB symbols of the RSDS record, the UI runs these through the 'symbols_load' query [nothing in the file changes].
        CodeViewInfo code_view = ReadCodeView( image );

        if (code_view.m_Present)
        {
            view.Info( context.m_Symbols != nullptr ? "Symbols loaded from " + Utf8FromUtf16( context.m_Symbols->GetSource().u16string() ) + ", " +
                std::to_string( context.m_Symbols->GetCount() ) + " symbols" :
                "PDB " + code_view.m_PdbPath + ", " + code_view.GuidText() + " age " + std::to_string( code_view.m_Age ) + ", not loaded" );
        }

        view.BeginCommands();

        if (code_view.m_Present)
        {
            view.Command( "symbols.load", "Load symbols", "view", true );
            view.Command( "symbols.browse", "Load PDB from file...", "view", true );
            view.Command( "symbols.download", "Download from the Microsoft symbol server", "view", true );
        }

        view.Command( "debug.remove", "Remove debug directory", "view", directory.m_Rva != 0 && directory.m_Size != 0 );
        view.EndCommands();
        return view.Finish();
    }

    std::string BuildDebugDetailView(
        const ViewContext & context,
        size_t index
    )
    {
        const PeImage & image = context.m_Image;
        uint64_t offset = DirectoryOffset( image, DIR_DEBUG );
        Require( offset != NO_OFFSET, ErrorCode::ERR_NOT_FOUND, "No debug directory" );
        uint64_t row = offset + index * 28;
        uint32_t type = image.Read32( row + 12 );
        uint32_t size = image.Read32( row + 16 );
        uint64_t data = image.Read32( row + 24 );
        ViewBuilder view( "debug/" + std::to_string( index ), DescribeEnum( "debug_type", type ), "fields" );
        view.BeginFields();

        if (type == 2 && image.Read32( data ) == 0x53445352)
        {
            uint8_t guid[16];
            image.ReadAt( data + 4, guid, 16 );
            view.TextField( "Signature", data, 4, "RSDS" );
            view.TextField( "Guid", data + 4, 16, GuidText( guid ) );
            view.FieldAt( image, "Age", data + 20, 4, std::to_string( image.Read32( data + 20 ) ) );
            view.TextField( "PdbFileName", data + 24, 0, Utf8FromLatin1( image.ReadString( data + 24, 1024 ) ) );
            char symbol[64];
            std::snprintf( symbol, sizeof( symbol ), "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
                guid[3] << 24 | guid[2] << 16 | guid[1] << 8 | guid[0], guid[5] << 8 | guid[4], guid[7] << 8 | guid[6],
                guid[8], guid[9], guid[10], guid[11], guid[12], guid[13], guid[14], guid[15], image.Read32( data + 20 ) );
            view.InfoField( "Symbol server key", symbol );
        }
        else if (type == 2 && image.Read32( data ) == 0x3031424E)
        {
            view.TextField( "Signature", data, 4, "NB10" );
            view.FieldAt( image, "Offset", data + 4, 4 );
            view.FieldAt( image, "Signature (time)", data + 8, 4, FormatTimestamp( image.Read32( data + 8 ) ) );
            view.FieldAt( image, "Age", data + 12, 4 );
            view.TextField( "PdbFileName", data + 16, 0, Utf8FromLatin1( image.ReadString( data + 16, 1024 ) ) );
        }
        else if (type == 12 && size >= 20)
        {
            static const char * NAMES[] = { "Pre-VC++ 11.00 objects", "C/C++ objects", "/GS objects", "/sdl objects", "guardN objects" };

            for (int item = 0; item < 5; ++item)
            {
                view.FieldAt( image, NAMES[item], data + item * 4ull, 4, std::to_string( image.Read32( data + item * 4ull ) ) );
            }
        }
        else if (type == 20 && size >= 4)
        {
            view.ChoiceFieldAt( image, "ExtendedDllCharacteristics", data, 4, "debug_ex_characteristics" );
        }
        else if (type == 13 && size >= 4)
        {
            uint32_t signature = image.Read32( data );
            view.TextField( "Signature", data, 4, signature == 0x50475500 ? "PGU" : signature == 0x50474900 ? "PGI" : Hex( signature ) );
            uint64_t cursor = data + 4;
            uint64_t end = data + size;
            int entries = 0;

            while (cursor + 8 < end && entries < 4096)
            {
                uint32_t rva = image.Read32( cursor );
                uint32_t length = image.Read32( cursor + 4 );
                std::string name = image.ReadString( cursor + 8, 256 );
                view.Field( name, cursor, 4, rva, "Size " + Hex( length, 1 ) + "h, " + image.SectionNameOfRva( rva ) );
                cursor += 8 + ((name.size() + 4) & ~3ull);
                ++entries;
            }
        }
        else if (type == 16 && size >= 4)
        {
            uint32_t length = image.Read32( data );
            std::string hash;

            for (uint32_t byte = 0; byte < length && byte < 64; ++byte)
            {
                hash += Hex( image.Read8( data + 4 + byte ), 2 );
            }

            view.FieldAt( image, "HashLength", data, 4 );
            view.TextField( "Hash", data + 4, length, hash );
        }
        else
        {
            view.InfoField( "Data", FormatBytes( size ) + " at offset " + Hex( data ) );
        }

        view.EndFields();

        if (size != 0 && data != 0)
        {
            view.HexRegion( data, size, "Debug data" );
        }

        return view.Finish();
    }

    std::string BuildTlsView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "tls", "TLS Directory", "list" );
        uint64_t offset = DirectoryOffset( image, DIR_TLS );

        if (offset == NO_OFFSET)
        {
            view.Warnings( { "The TLS directory is not backed by file data" } );
            return view.Finish();
        }

        uint32_t pointer = PointerSize( image );
        view.BeginFields();
        VaField( view, image, "StartAddressOfRawData", offset, pointer );
        VaField( view, image, "EndAddressOfRawData", offset + pointer, pointer );
        VaField( view, image, "AddressOfIndex", offset + pointer * 2ull, pointer );
        VaField( view, image, "AddressOfCallBacks", offset + pointer * 3ull, pointer );
        view.FieldAt( image, "SizeOfZeroFill", offset + pointer * 4ull, 4 );
        view.FieldAt( image, "Characteristics", offset + pointer * 4ull + 4, 4,
            (image.Read32( offset + pointer * 4ull + 4 ) >> 20) & 0xF ? "Alignment " + std::to_string( 1u << (((image.Read32( offset + pointer * 4ull + 4 ) >> 20) & 0xF) - 1) ) + " bytes" : std::string() );
        view.EndFields();

        view.Columns( { { "Callback", pointer == 8 ? "VA, Qword" : "VA, Dword" }, { "RVA", "" }, { "Section", "" } } );
        view.BeginRows();
        uint64_t callbacks = image.VaToOffset( image.ReadUnsigned( offset + pointer * 3ull, pointer ) );

        for (size_t index = 0; callbacks != NO_OFFSET && index < 256; ++index)
        {
            uint64_t cell = callbacks + index * pointer;
            uint64_t value = image.ReadUnsigned( cell, pointer );

            if (value == 0)
            {
                break;
            }

            uint64_t rva = value - image.GetImageBase();
            view.BeginRow( std::string(), cell, pointer );
            view.ValueCellAt( image, cell, pointer );
            view.Cell( Hex( rva ) );
            view.Cell( image.SectionNameOfRva( static_cast<uint32_t>(rva) ) );
            view.EndRow();
        }

        view.EndRows();
        view.DetailLayout( "none" );
        return view.Finish();
    }

    std::string BuildLoadConfigView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "loadconfig", "Load Config Directory", "fields" );
        uint64_t offset = DirectoryOffset( image, DIR_LOAD_CONFIG );

        if (offset == NO_OFFSET)
        {
            view.Warnings( { "The load config directory is not backed by file data" } );
            return view.Finish();
        }

        // Name and size per field, 0 means pointer size. The structure grows with every Windows release, the Size
        // field says how much of it this image has.
        struct LoadConfigField
        {
            const char * m_Name;
            uint32_t m_Size;
            char m_Kind;
        };

        static const LoadConfigField FIELDS[] =
        {
            { "Size", 4, 'n' }, { "TimeDateStamp", 4, 't' }, { "MajorVersion", 2, 'n' }, { "MinorVersion", 2, 'n' }, { "GlobalFlagsClear", 4, 'n' },
            { "GlobalFlagsSet", 4, 'n' }, { "CriticalSectionDefaultTimeout", 4, 'n' }, { "DeCommitFreeBlockThreshold", 0, 'n' },
            { "DeCommitTotalFreeThreshold", 0, 'n' }, { "LockPrefixTable", 0, 'v' }, { "MaximumAllocationSize", 0, 'n' },
            { "VirtualMemoryThreshold", 0, 'n' }, { "ProcessAffinityMask", 0, 'n' }, { "ProcessHeapFlags", 4, 'n' }, { "CSDVersion", 2, 'n' },
            { "DependentLoadFlags", 2, 'n' }, { "EditList", 0, 'v' }, { "SecurityCookie", 0, 'v' }, { "SEHandlerTable", 0, 'v' },
            { "SEHandlerCount", 0, 'n' }, { "GuardCFCheckFunctionPointer", 0, 'v' }, { "GuardCFDispatchFunctionPointer", 0, 'v' },
            { "GuardCFFunctionTable", 0, 'v' }, { "GuardCFFunctionCount", 0, 'n' }, { "GuardFlags", 4, 'g' }, { "CodeIntegrity.Flags", 2, 'n' },
            { "CodeIntegrity.Catalog", 2, 'n' }, { "CodeIntegrity.CatalogOffset", 4, 'n' }, { "CodeIntegrity.Reserved", 4, 'n' },
            { "GuardAddressTakenIatEntryTable", 0, 'v' }, { "GuardAddressTakenIatEntryCount", 0, 'n' }, { "GuardLongJumpTargetTable", 0, 'v' },
            { "GuardLongJumpTargetCount", 0, 'n' }, { "DynamicValueRelocTable", 0, 'v' }, { "CHPEMetadataPointer", 0, 'v' },
            { "GuardRFFailureRoutine", 0, 'v' }, { "GuardRFFailureRoutineFunctionPointer", 0, 'v' }, { "DynamicValueRelocTableOffset", 4, 'n' },
            { "DynamicValueRelocTableSection", 2, 'n' }, { "Reserved2", 2, 'n' }, { "GuardRFVerifyStackPointerFunctionPointer", 0, 'v' },
            { "HotPatchTableOffset", 4, 'n' }, { "Reserved3", 4, 'n' }, { "EnclaveConfigurationPointer", 0, 'v' }, { "VolatileMetadataPointer", 0, 'v' },
            { "GuardEHContinuationTable", 0, 'v' }, { "GuardEHContinuationCount", 0, 'n' }, { "GuardXFGCheckFunctionPointer", 0, 'v' },
            { "GuardXFGDispatchFunctionPointer", 0, 'v' }, { "GuardXFGTableDispatchFunctionPointer", 0, 'v' }, { "CastGuardOsDeterminedFailureMode", 0, 'v' },
            { "GuardMemcpyFunctionPointer", 0, 'v' }
        };

        uint32_t pointer = PointerSize( image );
        uint32_t structure_size = image.Read32( offset );
        uint32_t position = 0;
        view.BeginFields();

        for (const LoadConfigField & field : FIELDS)
        {
            uint32_t size = field.m_Size != 0 ? field.m_Size : pointer;

            if (position + size > structure_size)
            {
                break;
            }

            uint64_t cell = offset + position;

            switch (field.m_Kind)
            {
                case 't':
                    view.FieldAt( image, field.m_Name, cell, size, FormatTimestamp( image.Read32( cell ) ) );
                    break;
                case 'v':
                    VaField( view, image, field.m_Name, cell, size );
                    break;
                case 'g':
                {
                    uint32_t flags = image.Read32( cell );
                    std::string meaning = DescribeFlags( "guard_flags", flags & 0x0FFFFFFF );
                    view.Field( field.m_Name, cell, size, flags, meaning + (flags >> 28 ? ", function table stride " + std::to_string( flags >> 28 ) : ""),
                        Editor::EDITOR_FLAGS, "guard_flags" );
                    break;
                }
                default:
                    view.FieldAt( image, field.m_Name, cell, size );
                    break;
            }

            position += size;
        }

        view.EndFields();
        return view.Finish();
    }

    std::string BuildBoundImportView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "bound", "Bound Import Directory", "list" );
        view.Columns( { { "Module", "" }, { "TimeDateStamp", "Dword" }, { "OffsetModuleName", "Word, from the directory start" },
            { "NumberOfModuleForwarderRefs", "Word" }, { "Bound to", "" } } );
        view.BeginRows();

        // Bound imports usually live in the headers and are addressed by 'RVA == offset'.
        DataDirectory directory = image.GetDirectory( DIR_BOUND_IMPORT );
        uint64_t base = image.RvaToOffset( directory.m_Rva );
        uint64_t cursor = base;
        size_t rows = 0;

        while (base != NO_OFFSET && cursor + 8 <= base + directory.m_Size && rows < 1024)
        {
            uint32_t stamp = image.Read32( cursor );
            uint16_t name = image.Read16( cursor + 4 );
            uint16_t forwarders = image.Read16( cursor + 6 );

            if (stamp == 0 && name == 0)
            {
                break;
            }

            for (uint32_t entry = 0; entry <= forwarders && cursor + 8 <= base + directory.m_Size; ++entry)
            {
                uint64_t row = cursor;
                view.BeginRow( std::string(), row, 8 );
                view.Cell( (entry == 0 ? "" : "    -> ") + Utf8FromLatin1( image.ReadString( base + image.Read16( row + 4 ), 256 ) ) );
                view.ValueCellAt( image, row, 4 );
                view.ValueCellAt( image, row + 4, 2 );
                view.ValueCellAt( image, row + 6, 2 );
                view.Cell( FormatTimestamp( image.Read32( row ) ) );
                view.EndRow();
                cursor += 8;
                ++rows;
            }
        }

        view.EndRows();
        view.DetailLayout( "none" );
        view.Warnings( { "Bound imports are ignored by ASLR enabled loaders. The Rebuilder can remove them." } );
        return view.Finish();
    }

    std::string BuildIatView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "iat", "Import Address Table", "list" );
        uint32_t pointer = PointerSize( image );
        view.Columns( { { "RVA", "" }, { "Value", pointer == 8 ? "Qword" : "Dword" }, { "Module", "" }, { "Function", "" } } );
        view.BeginRows();

        // Names from the import directory by IAT slot
        std::map< uint32_t, std::pair< std::string, std::string > > names;

        for (const ImportModule & module : ReadImports( image ))
        {
            for (const ImportFunction & thunk : module.m_Functions)
            {
                names[thunk.m_PatchRva] = { module.m_Name, thunk.m_ByOrdinal ? "Ordinal " + std::to_string( thunk.m_Ordinal ) : thunk.m_Name };
            }
        }

        DataDirectory directory = image.GetDirectory( DIR_IAT );
        uint64_t offset = image.RvaToOffset( directory.m_Rva );
        size_t count = std::min< size_t >( directory.m_Size / pointer, 0x40000 );

        for (size_t index = 0; offset != NO_OFFSET && index < count; ++index)
        {
            uint64_t cell = offset + index * pointer;
            uint32_t rva = directory.m_Rva + static_cast<uint32_t>( index * pointer );
            auto name = names.find( rva );
            view.BeginRow( std::string(), cell, pointer );
            view.Cell( Hex( rva ) );
            view.ValueCellAt( image, cell, pointer );
            view.Cell( name != names.end() ? Utf8FromLatin1( name->second.first ) : std::string() );
            view.Cell( name != names.end() ? Utf8FromLatin1( name->second.second ) : std::string() );
            view.EndRow();
        }

        view.EndRows();
        view.DetailLayout( "none" );
        return view.Finish();
    }

    std::string BuildDelayImportView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        const PeLib::DelayImportDirectory & delay = image.GetFile().delayImports();
        ViewBuilder view( "delay", "Delay Import Directory", "list" );
        view.Columns( { { "Module", "" }, { "Imports", "" }, { "Attributes", "Dword, bit 0 means RVAs (version 2)" }, { "DllNameRVA", "Dword" },
            { "ModuleHandleRVA", "Dword" }, { "ImportAddressTableRVA", "Dword" }, { "ImportNameTableRVA", "Dword" },
            { "BoundImportAddressTableRVA", "Dword" }, { "UnloadInformationTableRVA", "Dword" }, { "TimeDateStamp", "Dword" } } );
        view.BeginRows();
        uint64_t offset = DirectoryOffset( image, DIR_DELAY_IMPORT );

        for (size_t index = 0; offset != NO_OFFSET && index < delay.getNumberOfFiles(); ++index)
        {
            const PeLib::PELIB_IMAGE_DELAY_IMPORT_DIRECTORY_RECORD * record = delay.getFile( index );
            uint64_t row = offset + index * 32;
            view.BeginRow( "delay/" + std::to_string( index ), row, 32 );

            // Version 1 descriptors [Attributes bit 0 clear] hold VAs instead of RVAs.
            uint32_t name_value = image.Read32( row + 4 );
            uint64_t name_offset = name_value == 0 ? NO_OFFSET :
                (image.Read32( row ) & 1) != 0 ? image.RvaToOffset( name_value ) : image.VaToOffset( name_value );
            NameCell( view, image, name_offset, record->Name );
            view.Cell( std::to_string( record->getNumberOfFunctions() ) );

            for (int column = 0; column < 8; ++column)
            {
                view.ValueCellAt( image, row + column * 4ull, 4 );
            }

            view.EndRow();
        }

        view.EndRows();
        view.DetailLayout( "list" );
        return view.Finish();
    }

    std::string BuildDelayImportDetailView(
        const ViewContext & context,
        size_t index
    )
    {
        const PeImage & image = context.m_Image;
        const PeLib::DelayImportDirectory & delay = image.GetFile().delayImports();
        const PeLib::PELIB_IMAGE_DELAY_IMPORT_DIRECTORY_RECORD * record = delay.getFile( index );
        Require( record != nullptr, ErrorCode::ERR_NOT_FOUND, "No such delay import descriptor" );
        ViewBuilder view( "delay/" + std::to_string( index ), Utf8FromLatin1( record->Name ), "list" );
        uint32_t pointer = PointerSize( image );
        uint64_t iat = image.RvaToOffset( record->delayedImport.DelayImportAddressTableRva );
        uint64_t names = image.RvaToOffset( record->delayedImport.DelayImportNameTableRva );
        bool decorated = false;

        for (size_t function = 0; function < record->getNumberOfFunctions() && !decorated; ++function)
        {
            const std::string & name = record->getFunction( function )->fname;
            decorated = !name.empty() && name[0] == '?';
        }

        std::vector< std::pair< std::string, std::string > > columns = { { "Name table entry", pointer == 8 ? "Qword" : "Dword" }, { "Address table entry", pointer == 8 ? "Qword" : "Dword" },
            { "Hint", "" }, { "Name", "" } };

        if (decorated)
        {
            columns.push_back( { "Undecorated name", UNDECORATED_TIP } );
        }

        view.Columns( columns );
        view.BeginRows();

        for (size_t function = 0; function < record->getNumberOfFunctions(); ++function)
        {
            const PeLib::PELIB_DELAY_IMPORT * entry = record->getFunction( function );
            uint64_t name_cell = names != NO_OFFSET ? names + function * pointer : NO_OFFSET;
            uint64_t iat_cell = iat != NO_OFFSET ? iat + function * pointer : NO_OFFSET;
            view.BeginRow( std::string(), name_cell, pointer );

            if (name_cell != NO_OFFSET)
            {
                view.ValueCellAt( image, name_cell, pointer );
            }
            else
            {
                view.Cell( "" );
            }

            if (iat_cell != NO_OFFSET)
            {
                view.ValueCellAt( image, iat_cell, pointer );
            }
            else
            {
                view.Cell( "" );
            }

            view.Cell( entry->fname.empty() ? std::string() : Hex( entry->hint, 4 ) );
            view.Cell( entry->fname.empty() ? "Ordinal " + std::to_string( entry->hint ) : Utf8FromLatin1( entry->fname ) );

            if (decorated)
            {
                view.Cell( UndecoratedText( entry->fname ) );
            }

            view.EndRow();
        }

        view.EndRows();
        return view.Finish();
    }

    std::string BuildOverlayView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "overlay", "Overlay", "hex" );
        uint64_t offset = image.GetOverlayOffset();
        uint64_t size = image.GetOverlaySize();
        char entropy[16];
        std::snprintf( entropy, sizeof( entropy ), "%.3f", ComputeEntropy( image.Slice( offset, size ) ) );
        view.Info( FormatBytes( size ) + " of data after the end of the PE image at offset " + Hex( offset ) + ", entropy " + entropy +
            ". Installers and self extracting archives keep their payload here." );
        view.HexRegion( offset, size, "Overlay" );
        view.BeginCommands();
        view.Command( "overlay.save", "Save overlay...", "view" );
        view.Command( "overlay.remove", "Remove overlay", "view" );
        view.EndCommands();
        return view.Finish();
    }
}