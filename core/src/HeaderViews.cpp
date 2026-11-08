#include "pecore/Views.h"

#include "pecore/Authenticode.h"
#include "pecore/Hash.h"
#include "pecore/Meta.h"
#include "pecore/Operations.h"
#include "pecore/Platform.h"
#include "pecore/Resources.h"
#include "pecore/RichHeader.h"
#include "pecore/Symbols.h"
#include "pecore/Tools.h"

#include "retdec/pelib/PeFile.h"

#include <cstdio>
#include <future>
#include <spanstream>

namespace PeCore
{
    namespace
    {
        struct HeaderField
        {
            const char * m_Name;
            uint32_t m_Offset;
            uint32_t m_Size;
        };

        // 'IMAGE_OPTIONAL_HEADER32' without the data directories.
        const HeaderField OPTIONAL_FIELDS_32[] =
        {
            { "Magic", 0, 2 },
            { "MajorLinkerVersion", 2, 1 },
            { "MinorLinkerVersion", 3, 1 },
            { "SizeOfCode", 4, 4 },
            { "SizeOfInitializedData", 8, 4 },
            { "SizeOfUninitializedData", 12, 4 },
            { "AddressOfEntryPoint", 16, 4 },
            { "BaseOfCode", 20, 4 },
            { "BaseOfData", 24, 4 },
            { "ImageBase", 28, 4 },
            { "SectionAlignment", 32, 4 },
            { "FileAlignment", 36, 4 },
            { "MajorOperatingSystemVersion", 40, 2 },
            { "MinorOperatingSystemVersion", 42, 2 },
            { "MajorImageVersion", 44, 2 },
            { "MinorImageVersion", 46, 2 },
            { "MajorSubsystemVersion", 48, 2 },
            { "MinorSubsystemVersion", 50, 2 },
            { "Win32VersionValue", 52, 4 },
            { "SizeOfImage", 56, 4 },
            { "SizeOfHeaders", 60, 4 },
            { "CheckSum", 64, 4 },
            { "Subsystem", 68, 2 },
            { "DllCharacteristics", 70, 2 },
            { "SizeOfStackReserve", 72, 4 },
            { "SizeOfStackCommit", 76, 4 },
            { "SizeOfHeapReserve", 80, 4 },
            { "SizeOfHeapCommit", 84, 4 },
            { "LoaderFlags", 88, 4 },
            { "NumberOfRvaAndSizes", 92, 4 }
        };

        const HeaderField OPTIONAL_FIELDS_64[] =
        {
            { "Magic", 0, 2 },
            { "MajorLinkerVersion", 2, 1 },
            { "MinorLinkerVersion", 3, 1 },
            { "SizeOfCode", 4, 4 },
            { "SizeOfInitializedData", 8, 4 },
            { "SizeOfUninitializedData", 12, 4 },
            { "AddressOfEntryPoint", 16, 4 },
            { "BaseOfCode", 20, 4 },
            { "ImageBase", 24, 8 },
            { "SectionAlignment", 32, 4 },
            { "FileAlignment", 36, 4 },
            { "MajorOperatingSystemVersion", 40, 2 },
            { "MinorOperatingSystemVersion", 42, 2 },
            { "MajorImageVersion", 44, 2 },
            { "MinorImageVersion", 46, 2 },
            { "MajorSubsystemVersion", 48, 2 },
            { "MinorSubsystemVersion", 50, 2 },
            { "Win32VersionValue", 52, 4 },
            { "SizeOfImage", 56, 4 },
            { "SizeOfHeaders", 60, 4 },
            { "CheckSum", 64, 4 },
            { "Subsystem", 68, 2 },
            { "DllCharacteristics", 70, 2 },
            { "SizeOfStackReserve", 72, 8 },
            { "SizeOfStackCommit", 80, 8 },
            { "SizeOfHeapReserve", 88, 8 },
            { "SizeOfHeapCommit", 96, 8 },
            { "LoaderFlags", 104, 4 },
            { "NumberOfRvaAndSizes", 108, 4 }
        };

        const HeaderField DOS_FIELDS[] =
        {
            { "e_magic", 0x00, 2 },
            { "e_cblp", 0x02, 2 },
            { "e_cp", 0x04, 2 },
            { "e_crlc", 0x06, 2 },
            { "e_cparhdr", 0x08, 2 },
            { "e_minalloc", 0x0A, 2 },
            { "e_maxalloc", 0x0C, 2 },
            { "e_ss", 0x0E, 2 },
            { "e_sp", 0x10, 2 },
            { "e_csum", 0x12, 2 },
            { "e_ip", 0x14, 2 },
            { "e_cs", 0x16, 2 },
            { "e_lfarlc", 0x18, 2 },
            { "e_ovno", 0x1A, 2 },
            { "e_res[0]", 0x1C, 2 },
            { "e_res[1]", 0x1E, 2 },
            { "e_res[2]", 0x20, 2 },
            { "e_res[3]", 0x22, 2 },
            { "e_oemid", 0x24, 2 },
            { "e_oeminfo", 0x26, 2 },
            { "e_res2[0]", 0x28, 2 },
            { "e_res2[1]", 0x2A, 2 },
            { "e_res2[2]", 0x2C, 2 },
            { "e_res2[3]", 0x2E, 2 },
            { "e_res2[4]", 0x30, 2 },
            { "e_res2[5]", 0x32, 2 },
            { "e_res2[6]", 0x34, 2 },
            { "e_res2[7]", 0x36, 2 },
            { "e_res2[8]", 0x38, 2 },
            { "e_res2[9]", 0x3A, 2 },
            { "e_lfanew", 0x3C, 4 }
        };

        const char * DOS_TIPS[] =
        {
            "Magic number, MZ", "Bytes on last page of file", "Pages in file", "Relocations", "Size of header in paragraphs",
            "Minimum extra paragraphs needed", "Maximum extra paragraphs needed", "Initial (relative) SS value", "Initial SP value",
            "Checksum", "Initial IP value", "Initial (relative) CS value", "File address of relocation table", "Overlay number"
        };

        std::string FileTypeText(
            const PeImage & image
        )
        {
            const std::span< const uint8_t > data = image.GetData();

            if (!image.IsPe())
            {
                if (data.size() >= 2 && data[0] == 'M' && data[1] == 'Z')
                {
                    return "MS-DOS executable";
                }

                return "Unknown (not a PE file)";
            }

            uint16_t characteristics = image.Read16( image.GetFileHeaderOffset() + 18 );
            uint16_t subsystem = image.Read16( image.GetOptionalHeaderOffset() + 68 );
            std::string text = image.Is64() ? "Portable Executable 64 (PE32+)" : "Portable Executable 32 (PE32)";

            if (subsystem == 1 && (characteristics & 0x2000) == 0)
            {
                text += " driver";
            }
            else if (characteristics & 0x2000)
            {
                text += " DLL";
            }
            else if (subsystem >= 10 && subsystem <= 13)
            {
                text += " EFI image";
            }
            else
            {
                text += " executable";
            }

            std::string machine = DescribeEnum( "machine", image.GetMachine() );

            if (!machine.empty())
            {
                text += ", " + machine;
            }

            if (image.IsDotNet())
            {
                text += ", .NET assembly";
            }

            return text;
        }

        std::string VersionText(
            uint32_t major,
            uint32_t minor
        )
        {
            return std::to_string( major ) + "." + std::to_string( minor );
        }

        // What a section holds, the 'This section contains' info of CFF.
        std::string DescribeSectionContents(
            const PeImage & image,
            size_t index
        )
        {
            const PeLib::PELIB_SECTION_HEADER * section = image.GetLoader().getSectionHeader( index );

            if (section == nullptr)
            {
                return std::string();
            }

            std::vector< std::string > items;
            uint32_t entry = image.GetEntryPoint();

            if (entry != 0 && image.FindSectionByRva( entry ) == index)
            {
                items.push_back( "Entry point (RVA " + Hex( entry ) + ")" );
            }

            for (uint32_t directory = 0; directory < DIR_COUNT; ++directory)
            {
                DataDirectory entry_dir = image.GetDirectory( directory );

                if (entry_dir.m_Rva == 0 || entry_dir.m_Size == 0 || directory == DIR_SECURITY)
                {
                    continue;
                }

                if (image.FindSectionByRva( entry_dir.m_Rva ) == index)
                {
                    items.push_back( DirectoryName( directory ) + " (RVA " + Hex( entry_dir.m_Rva ) + ", " + Hex( entry_dir.m_Size, 1 ) + "h bytes)" );
                }
            }

            std::string text;
            auto slice = image.Slice( section->PointerToRawData, section->SizeOfRawData );
            char entropy[64];
            std::snprintf( entropy, sizeof( entropy ), "%.3f", ComputeEntropy( slice ) );

            text += "Section " + image.GetSectionName( index ) + "  " + SectionFlagsSummary( section->Characteristics ) +
                ", raw " + Hex( section->PointerToRawData ) + " - " + Hex( static_cast<uint64_t>(section->PointerToRawData) + section->SizeOfRawData ) +
                ", entropy " + entropy + (ComputeEntropy( slice ) > 7.2 ? " (packed or encrypted data likely)" : "") + "\n";

            if (items.empty())
            {
                text += "This section contains no directory and no entry point.";
            }
            else
            {
                text += "This section contains:";

                for (const std::string & item : items)
                {
                    text += "\n  " + item;
                }
            }

            return text;
        }
    }

    void ComputeOverviewHashes(
        const Document & document,
        const PeImage & image
    )
    {
        // The Authenticode digest and the file hashes read the whole file, they run at the same time [both are cached].
        if (image.IsPe() && image.HasDirectory( DIR_SECURITY ))
        {
            auto signature = std::async( std::launch::async, [&]
            {
                return GetAuthenticodeInfo( document, image );
            } );
            document.GetHashes();
            signature.get();
        }
        else
        {
            document.GetHashes();
        }
    }

    std::string BuildOverviewHashes(
        const Document & document,
        const PeImage & image
    )
    {
        ComputeOverviewHashes( document, image );
        const FileHashes & hashes = document.GetHashes();
        JsonWriter writer;
        writer.BeginObject();
        writer.Member( "md5", hashes.m_Md5 );
        writer.Member( "sha1", hashes.m_Sha1 );
        writer.Member( "sha256", hashes.m_Sha256 );
        writer.Member( "blake3", hashes.m_Blake3 );

        if (image.IsPe() && image.HasDirectory( DIR_SECURITY ))
        {
            AuthenticodeSummary summary = GetAuthenticodeSummary( document, image );
            writer.Member( "signature", summary.m_Text );
            writer.MemberBool( "matches", summary.m_Matches );
        }

        writer.EndObject();
        return writer.TakeText();
    }

    std::string BuildOverviewView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        const Document & document = context.m_Document;

        bool signed_file = image.IsPe() && image.HasDirectory( DIR_SECURITY );

        // Deferred. Without cached results the whole file is not read here, the UI asks 'overview_hashes' afterwards.
        bool deferred = context.m_DeferSlow && (!document.HasCached( "hashes" ) || (signed_file && !document.HasCached( "authenticode" )));

        if (!deferred)
        {
            ComputeOverviewHashes( document, image );
        }

        ViewBuilder view( "overview", "File: " + document.GetName(), "overview" );
        view.BeginFields();

        std::string path = document.GetPath().empty() ? document.GetName() : Utf8FromUtf16( document.GetPath().u16string() );
        view.InfoField( "File name", path );
        view.InfoField( "File type", FileTypeText( image ) );
        view.InfoField( "File size", FormatBytes( document.GetSize() ) );

        if (image.IsPe())
        {
            view.InfoField( "PE size", FormatBytes( image.GetPeSize() ) );
            uint64_t overlay = image.GetOverlaySize();

            if (overlay != 0)
            {
                view.InfoField( "Overlay", FormatBytes( overlay ) + " at offset " + Hex( image.GetOverlayOffset() ) );
            }

            if (signed_file)
            {
                if (deferred)
                {
                    view.PendingField( "Signature", "signature" );
                }
                else
                {
                    view.InfoField( "Signature", GetAuthenticodeSummary( document, image ).m_Text );
                }
            }
        }

        FileTimes times;

        if (!document.GetPath().empty() && GetFileTimes( document.GetPath(), times ))
        {
            view.InfoField( "Created", times.m_Created );
            view.InfoField( "Modified", times.m_Modified );
            view.InfoField( "Accessed", times.m_Accessed );
        }

        // 'FileHeader.TimeDateStamp' in local time next to the file times. 'Linked' below keeps
        // the raw value with UTC.
        if (image.IsPe())
        {
            uint32_t stamp = image.Read32( image.GetFileHeaderOffset() + 4 );
            std::string text = FormatTimestampLocal( stamp );

            if (text.empty() || text == "Reproducible build hash")
            {
                text = Hex( stamp ) + (text.empty() ? std::string() : "  " + text);
            }

            view.InfoField( "Time date stamp", text );
        }

        if (deferred)
        {
            view.PendingField( "MD5", "md5" );
            view.PendingField( "SHA-1", "sha1" );
            view.PendingField( "SHA-256", "sha256" );
            view.PendingField( "BLAKE3", "blake3" );
        }
        else
        {
            const FileHashes & hashes = document.GetHashes();
            view.InfoField( "MD5", hashes.m_Md5 );
            view.InfoField( "SHA-1", hashes.m_Sha1 );
            view.InfoField( "SHA-256", hashes.m_Sha256 );
            view.InfoField( "BLAKE3", hashes.m_Blake3 );
        }

        if (image.IsPe())
        {
            uint32_t entry = image.GetEntryPoint();
            std::string section = image.SectionNameOfRva( entry );
            view.HighlightNext( "entry" );
            view.InfoField( "Entry point", entry != 0 ? Hex( entry ) + " RVA" + (section.empty() ? std::string() : " (" + section + ")") : std::string( "None" ) );
            view.InfoField( "Image base", Hex( image.GetImageBase(), image.Is64() ? 16 : 8 ) );

            CodeViewInfo code_view = ReadCodeView( image );

            if (context.m_Symbols != nullptr)
            {
                view.InfoField( "Symbols", std::to_string( context.m_Symbols->GetCount() ) + " symbols from " + Utf8FromUtf16( context.m_Symbols->GetSource().u16string() ) );
            }
            else if (code_view.m_Present)
            {
                view.InfoField( "Symbols", code_view.PdbFileName() + " not loaded (Debug Directory, Load symbols)" );
            }
            view.InfoField( "Subsystem", DescribeEnum( "subsystem", image.Read16( image.GetOptionalHeaderOffset() + 68 ) ) );
            uint32_t stamp = image.Read32( image.GetFileHeaderOffset() + 4 );
            view.InfoField( "Linked", Hex( stamp ) + "  " + FormatTimestamp( stamp ) );
            view.InfoField( "Linker version", VersionText( image.Read8( image.GetOptionalHeaderOffset() + 2 ), image.Read8( image.GetOptionalHeaderOffset() + 3 ) ) );
            uint16_t dll = image.Read16( image.GetOptionalHeaderOffset() + 70 );
            std::string mitigations;
            mitigations += (dll & 0x0040) ? "ASLR" : "no ASLR";
            mitigations += (dll & 0x0020) ? ", high entropy VA" : "";
            mitigations += (dll & 0x0100) ? ", DEP" : ", no DEP";
            mitigations += (dll & 0x4000) ? ", CFG" : "";
            mitigations += (dll & 0x0400) ? ", no SEH" : "";
            view.InfoField( "Mitigations", mitigations );
        }

        view.EndFields();

        if (image.IsPe())
        {
            VersionInfo version = ReadVersionInfo( image );

            if (!version.m_Strings.empty() || version.m_HasFixed)
            {
                // Every value is editable, the edit rewrites the version resource [resources.set_version].
                std::string prefix = std::to_string( version.m_Resource ) + ":";
                view.BeginFields( "version" );

                if (version.m_HasFixed)
                {
                    view.CommandField( "File version (fixed)", version.m_FileVersion, "resources.set_version", prefix + "fixed.FileVersion" );
                    view.CommandField( "Product version (fixed)", version.m_ProductVersion, "resources.set_version", prefix + "fixed.ProductVersion" );
                }

                for (const auto & item : version.m_Strings)
                {
                    view.CommandField( item.first, item.second, "resources.set_version", prefix + "string." + item.first );
                }

                view.EndFields();
            }
        }

        view.Warnings( image.GetWarnings() );
        return view.Finish();
    }

    std::string BuildDosHeaderView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "dos", "DOS Header", "fields" );
        view.BeginFields();
        size_t index = 0;

        for (const HeaderField & field : DOS_FIELDS)
        {
            std::string meaning;

            if (field.m_Offset == 0)
            {
                meaning = image.Read16( 0 ) == 0x5A4D ? "MZ" : "Invalid, MZ expected";
            }
            else if (field.m_Offset == 0x3C)
            {
                meaning = "Offset of the NT headers";
            }

            uint64_t value = image.ReadUnsigned( field.m_Offset, field.m_Size );
            view.Field( field.m_Name, field.m_Offset, field.m_Size, value, meaning, Editor::EDITOR_HEX, std::string(),
                index < std::size( DOS_TIPS ) ? DOS_TIPS[index] : std::string() );
            ++index;
        }

        view.EndFields();
        uint32_t lfanew = image.GetNtHeadersOffset();

        if (lfanew > 0x40)
        {
            view.HexRegion( 0x40, lfanew - 0x40, "DOS stub" );
        }

        return view.Finish();
    }

    namespace
    {
        // PeLib pads some product names ['Import (old)').
        std::string CollapseSpaces(
            const std::string & text
        )
        {
            std::string result;

            for (char character : text)
            {
                if (character != ' ' || (!result.empty() && result.back() != ' '))
                {
                    result.push_back( character );
                }
            }

            return result;
        }
    }

    std::string BuildRichHeaderView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        RichHeaderInfo info = ParseRichHeader( image.GetData() );
        ViewBuilder view( "rich", "Rich Header", "list" );

        if (!info.m_Found)
        {
            view.Warnings( { "No Rich header between the DOS stub and the PE header" } );
            return view.Finish();
        }

        uint32_t checksum = ComputeRichChecksum( image.GetData(), info.m_DansOffset, info.m_Entries );
        bool decrypted = info.m_Key == 0;
        bool valid = info.m_Key == checksum;
        std::string state = valid ? "Valid, the key is the checksum" : decrypted ? "Decrypted (plain text), Encrypt restores the checksum key" :
            "Checksum mismatch, the header was edited after linking";

        // Raw dwords of the decrypted header, the PeLib "Rich hash" convention
        std::vector< uint8_t > decrypted_bytes;

        for (uint64_t offset = info.m_DansOffset; offset < info.m_RichOffset; offset += 4)
        {
            uint32_t value = image.Read32( offset ) ^ info.m_Key;

            for (int shift = 0; shift < 32; shift += 8)
            {
                decrypted_bytes.push_back( static_cast<uint8_t>( value >> shift ) );
            }
        }

        view.BeginFields();
        view.Field( "DanS marker", info.m_DansOffset, 4, image.Read32( info.m_DansOffset ), Hex( info.m_DansOffset ) + ", \"DanS\" XOR key", Editor::EDITOR_NONE );
        view.Field( "Rich marker", info.m_RichOffset, 4, image.Read32( info.m_RichOffset ), "\"Rich\"", Editor::EDITOR_NONE );
        view.Field( "Key", info.m_RichOffset + 4, 4, info.m_Key, state, Editor::EDITOR_NONE );
        view.Field( "Checksum", NO_OFFSET, 4, checksum, valid ? "Matches the key" : "Computed over the DOS header, the stub and the entries" );
        view.InfoField( "Padding", info.m_PaddingValid ? "Three zero dwords after DanS" : "Not zero, the header is damaged or hand made" );
        view.InfoField( "Space", std::to_string( info.m_Entries.size() ) + " entries, " +
            std::to_string( (info.m_Limit - info.GetEnd()) / 8 ) + " more fit before the PE header if that space is free" );
        view.InfoField( "Rich hash", ComputeHashHex( HashAlgorithm::HASH_MD5, decrypted_bytes ) );
        view.EndFields();

        view.Columns( { { "Product ID", "Compiler or tool id, high word of @comp.id. Edits are encrypted with the new checksum" },
            { "Build", "Tool build number, low word of @comp.id" }, { "Count", "Object files built with it" },
            { "Product", "" }, { "Visual Studio", "" }, { "Stored (encrypted)", "comp.id and count as they are in the file" } } );
        view.BeginRows();

        // Product names from PeLib. It reads the current bytes [the loaded header of the file is stale after an edit and
        // misses headers PeLib does not recognize on load], from the DOS header up to the key after 'Rich'.
        size_t rich_end = static_cast<size_t>(info.m_RichOffset) + 8;
        std::ispanstream rich_stream( std::span< const char >( reinterpret_cast<const char *>(image.GetData().data()), rich_end ) );
        PeLib::RichHeader rich;
        rich.read( rich_stream, 0, rich_end, true );
        std::vector< PeLib::PELIB_IMAGE_RICH_HEADER_RECORD > records( rich.begin(), rich.end() );

        // With 'ignoreInvalidKey' older PeLib versions analyze a valid header twice and keep both copies [fixed in 'pelib-updated' fork],
        // either way the first copy is the header.
        bool names = records.size() >= info.m_Entries.size();

        for (size_t index = 0; index < info.m_Entries.size(); ++index)
        {
            const RichEntry & entry = info.m_Entries[index];
            uint64_t offset = info.GetEntryOffset( index );
            std::string prefix = std::to_string( index ) + ":";
            view.BeginRow( std::string(), offset, 8 );
            view.CommandCell( Hex( entry.m_ProductId, 4 ), 2, "rich.set", prefix + "product" );
            view.CommandCell( Hex( entry.m_Build, 4 ), 2, "rich.set", prefix + "build" );
            view.CommandCell( Hex( entry.m_Count, 8 ), 4, "rich.set", prefix + "count" );
            view.Cell( names && records[index].ProductId == entry.m_ProductId ? CollapseSpaces( records[index].ProductName ) : std::string() );
            view.Cell( names && records[index].ProductId == entry.m_ProductId ? records[index].VisualStudioName : std::string() );
            view.Cell( Hex( image.Read32( offset ) ) + " " + Hex( image.Read32( offset + 4 ) ) );
            view.EndRow();
        }

        view.EndRows();
        view.DetailLayout( "none" );
        view.BeginCommands();
        view.Command( "rich.add", "Add entry", "view", true );
        view.Command( "rich.remove", "Remove entry", "row", !info.m_Entries.empty() );
        view.Command( "rich.encrypt", valid ? "Encrypt (header is valid)" : "Encrypt with the checksum", "view", !valid );
        view.Command( "rich.decrypt", "Decrypt in place", "view", !decrypted );
        view.Command( "rich.clear", "Remove Rich header", "view", true );
        view.EndCommands();
        return view.Finish();
    }

    std::string BuildNtHeadersView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "nt", "NT Headers", "fields" );
        view.BeginFields();
        uint32_t offset = image.GetNtHeadersOffset();
        view.FieldAt( image, "Signature", offset, 4, image.Read32( offset ) == 0x4550 ? "PE" : "Invalid, PE expected" );
        view.EndFields();
        return view.Finish();
    }

    std::string BuildFileHeaderView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "file", "File Header", "fields" );
        uint64_t offset = image.GetFileHeaderOffset();
        view.BeginFields();
        view.ChoiceFieldAt( image, "Machine", offset, 2, "machine" );
        view.FieldAt( image, "NumberOfSections", offset + 2, 2, std::to_string( image.Read16( offset + 2 ) ) );
        uint32_t stamp = image.Read32( offset + 4 );
        view.FieldAt( image, "TimeDateStamp", offset + 4, 4, FormatTimestamp( stamp ) );
        view.FieldAt( image, "PointerToSymbolTable", offset + 8, 4 );
        view.FieldAt( image, "NumberOfSymbols", offset + 12, 4 );
        view.FieldAt( image, "SizeOfOptionalHeader", offset + 16, 2 );
        view.ChoiceFieldAt( image, "Characteristics", offset + 18, 2, "file_characteristics" );
        view.EndFields();
        return view.Finish();
    }

    std::string BuildOptionalHeaderView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "optional", "Optional Header", "fields" );
        uint64_t base = image.GetOptionalHeaderOffset();
        uint32_t limit = image.GetOptionalHeaderSize();
        std::span< const HeaderField > fields = image.Is64() ? std::span< const HeaderField >( OPTIONAL_FIELDS_64 ) : std::span< const HeaderField >( OPTIONAL_FIELDS_32 );
        view.BeginFields();

        for (const HeaderField & field : fields)
        {
            // 'SizeOfOptionalHeader' can be smaller than the structure, those fields are not part of the header.
            if (field.m_Offset + field.m_Size > limit)
            {
                break;
            }

            std::string name = field.m_Name;
            uint64_t offset = base + field.m_Offset;
            uint64_t value = image.ReadUnsigned( offset, field.m_Size );

            if (name == "Magic")
            {
                view.ChoiceFieldAt( image, name, offset, field.m_Size, "optional_magic" );
            }
            else if (name == "Subsystem")
            {
                view.ChoiceFieldAt( image, name, offset, field.m_Size, "subsystem" );
            }
            else if (name == "DllCharacteristics")
            {
                view.ChoiceFieldAt( image, name, offset, field.m_Size, "dll_characteristics" );
            }
            else if (name == "AddressOfEntryPoint" || name == "BaseOfCode" || name == "BaseOfData")
            {
                if (name == "AddressOfEntryPoint")
                {
                    view.HighlightNext( "entry" );
                }

                view.Field( name, offset, field.m_Size, value, image.SectionNameOfRva( static_cast<uint32_t>(value) ) );
            }
            else if (name == "CheckSum")
            {
                uint32_t computed = ComputePeChecksum( image.GetData(), offset );
                std::string meaning = value == 0 ? "Not set" : value == computed ? "Valid" : "Invalid";
                view.Field( name, offset, field.m_Size, value, meaning + ", computed " + Hex( computed ) );
            }
            else if (name == "ImageBase" || name.find( "Version" ) != std::string::npos || name.rfind( "SizeOf", 0 ) == 0)
            {
                std::string meaning;

                if (name.rfind( "SizeOf", 0 ) == 0 && value != 0)
                {
                    meaning = FormatBytes( value );
                }

                view.Field( name, offset, field.m_Size, value, meaning );
            }
            else
            {
                view.Field( name, offset, field.m_Size, value );
            }
        }

        view.EndFields();
        return view.Finish();
    }

    std::string BuildDataDirectoriesView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "datadirs", "Data Directories", "list" );
        view.Columns( { { "Directory", "" }, { "RVA", "VirtualAddress, Dword (file offset for the security directory)" }, { "Size", "Size, Dword" },
            { "Section", "Section that holds the directory" }, { "File offset", "" } } );

        view.BeginRows();

        uint32_t count = std::min< uint32_t >( image.GetNumberOfRvaAndSizes(), DIR_COUNT );
        uint64_t base = image.GetDataDirectoryOffset();
        static const char * NODES[] = { "exports", "imports", "resources", "exceptions", "security", "relocs", "debug", "", "", "tls",
            "loadconfig", "bound", "iat", "delay", "dotnet", "" };

        for (uint32_t index = 0; index < DIR_COUNT; ++index)
        {
            uint64_t offset = base + index * 8ull;
            bool present = index < count && offset + 8 <= image.GetOptionalHeaderOffset() + static_cast<uint64_t>( image.GetOptionalHeaderSize() );
            DataDirectory directory = image.GetDirectory( index );
            std::string navigate = present && directory.m_Rva != 0 && directory.m_Size != 0 ? NODES[index] : "";

            view.BeginRow( std::string(), present ? offset : NO_OFFSET, 8, navigate );
            view.Cell( DirectoryName( index ) + (present ? "" : " (beyond NumberOfRvaAndSizes)") );

            if (present)
            {
                view.ValueCellAt( image, offset, 4 );
                view.ValueCellAt( image, offset + 4, 4 );
            }
            else
            {
                view.Cell( "" );
                view.Cell( "" );
            }

            if (index == DIR_SECURITY)
            {
                view.Cell( directory.m_Rva != 0 ? "(file offset)" : "" );
                view.Cell( directory.m_Rva != 0 ? Hex( directory.m_Rva ) : "" );
            }
            else
            {
                view.Cell( image.SectionNameOfRva( directory.m_Rva ) );
                uint64_t file_offset = directory.m_Rva != 0 ? image.RvaToOffset( directory.m_Rva ) : NO_OFFSET;
                view.Cell( file_offset != NO_OFFSET ? Hex( file_offset ) : "" );
            }

            view.EndRow();
        }

        view.EndRows();
        view.DetailLayout( "none" );
        return view.Finish();
    }

    std::string BuildSectionsView(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "sections", "Section Headers", "list" );
        view.Columns( { { "Name", "Name, Byte[8]" }, { "VirtualSize", "Dword" }, { "VirtualAddress", "Dword (RVA)" }, { "SizeOfRawData", "Dword" },
            { "PointerToRawData", "Dword (file offset)" }, { "PointerToRelocations", "Dword" }, { "PointerToLinenumbers", "Dword" },
            { "NumberOfRelocations", "Word" }, { "NumberOfLinenumbers", "Word" }, { "Characteristics", "Dword" }, { "Access", "Read, write, execute" },
            { "Entropy", "Shannon entropy of the raw data, 0 to 8 bits per byte" } } );

        view.BeginRows();
        size_t count = image.Read16( image.GetFileHeaderOffset() + 2 );

        for (size_t index = 0; index < count; ++index)
        {
            uint64_t offset = image.GetSectionHeaderOffset( index );

            if (offset + 40 > image.GetFileSize())
            {
                break;
            }

            char name[9] = {};
            image.ReadAt( offset, name, 8 );
            uint32_t raw_offset = image.Read32( offset + 20 );
            uint32_t raw_size = image.Read32( offset + 16 );
            uint32_t characteristics = image.Read32( offset + 36 );
            char entropy[16];
            std::snprintf( entropy, sizeof( entropy ), "%.3f", ComputeEntropy( image.Slice( raw_offset, raw_size ) ) );

            view.BeginRow( "sections/" + std::to_string( index ), offset, 40 );
            view.TextCell( offset, 8, Utf8FromLatin1( name ) );
            view.ValueCellAt( image, offset + 8, 4 );
            view.ValueCellAt( image, offset + 12, 4 );
            view.ValueCellAt( image, offset + 16, 4 );
            view.ValueCellAt( image, offset + 20, 4 );
            view.ValueCellAt( image, offset + 24, 4 );
            view.ValueCellAt( image, offset + 28, 4 );
            view.ValueCellAt( image, offset + 32, 2 );
            view.ValueCellAt( image, offset + 34, 2 );
            view.ValueCellAt( image, offset + 36, 4, Editor::EDITOR_FLAGS, "section_characteristics" );
            view.Cell( SectionFlagsSummary( characteristics ) );
            view.Cell( entropy );
            view.EndRow();
        }

        view.EndRows();
        view.DetailLayout( "hex" );

        bool has_sections = count != 0;
        view.BeginCommands();
        view.Command( "section.add_empty", "Add section (empty space)", "view" );
        view.Command( "section.add_file", "Add section (file data)...", "view" );
        view.Command( "section.add_header", "Add section (header only)", "view" );
        view.Command( "section.delete", "Delete section (header and data)", "row", has_sections );
        view.Command( "section.delete_header", "Delete section (header only)", "row", has_sections );
        view.Command( "section.dump", "Save section data...", "row", has_sections );
        view.Command( "section.replace", "Replace section data...", "row", has_sections );
        view.Command( "rebuild.image_size", "Rebuild image size", "view" );
        view.Command( "rebuild.pe_header", "Rebuild PE header", "view" );
        view.EndCommands();
        return view.Finish();
    }

    std::string BuildSectionDetailView(
        const ViewContext & context,
        size_t index
    )
    {
        const PeImage & image = context.m_Image;
        Require( index < image.GetSectionCount(), ErrorCode::ERR_NOT_FOUND, "No such section" );
        const PeLib::PELIB_SECTION_HEADER * section = image.GetLoader().getSectionHeader( index );
        ViewBuilder view( "sections/" + std::to_string( index ), "Section " + image.GetSectionName( index ), "hex" );
        view.Info( DescribeSectionContents( image, index ) );
        uint64_t length = section->PointerToRawData < image.GetFileSize() ? std::min< uint64_t >( section->SizeOfRawData, image.GetFileSize() - section->PointerToRawData ) : 0;
        view.HexRegion( section->PointerToRawData, length, "Raw data of " + image.GetSectionName( index ) );
        return view.Finish();
    }
}