#include "pecore/Operations.h"

#include "pecore/Meta.h"
#include "pecore/Resources.h"
#include "pecore/RichHeader.h"

#include "PeHelpers.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace PeCore
{
    uint32_t ComputePeChecksum(
        std::span< const uint8_t > data,
        uint64_t checksum_offset
    )
    {
        uint64_t sum = 0;
        size_t size = data.size();

        for (size_t index = 0; index < size; index += 2)
        {
            if (index == checksum_offset || index == checksum_offset + 2)
            {
                continue;
            }

            uint32_t word = data[index];

            if (index + 1 < size)
            {
                word |= static_cast<uint32_t>(data[index + 1]) << 8;
            }

            sum += word;
            sum = (sum & 0xFFFF) + (sum >> 16);
        }

        sum = (sum & 0xFFFF) + (sum >> 16);
        return static_cast<uint32_t>(sum + size);
    }

    uint32_t AlignUp(
        uint32_t value,
        uint32_t alignment
    )
    {
        if (alignment == 0)
        {
            return value;
        }

        return static_cast<uint32_t>((static_cast<uint64_t>(value) + alignment - 1) / alignment * alignment);
    }

    void Poke(
        std::vector< uint8_t > & data,
        uint64_t offset,
        uint64_t value,
        size_t size
    )
    {
        Require( offset + size <= data.size(), ErrorCode::ERR_OUT_OF_RANGE, "Write outside of the file" );

        for (size_t index = 0; index < size; ++index)
        {
            data[offset + index] = static_cast<uint8_t>( value >> (index * 8) );
        }
    }

    uint64_t Peek(
        std::span< const uint8_t > data,
        uint64_t offset,
        size_t size
    )
    {
        if (offset + size > data.size())
        {
            return 0;
        }

        uint64_t value = 0;

        for (size_t index = 0; index < size; ++index)
        {
            value |= static_cast<uint64_t>( data[offset + index] ) << (index * 8);
        }

        return value;
    }

    namespace
    {
        // Raw header layout of a PE file in a byte vector, re-read after every structural change.
        struct Layout
        {
            uint32_t m_Nt = 0;
            uint32_t m_FileHeader = 0;
            uint32_t m_Optional = 0;
            uint32_t m_OptionalSize = 0;
            uint32_t m_SectionTable = 0;
            uint32_t m_Sections = 0;
            bool m_Is64 = false;

            explicit Layout(
                std::span< const uint8_t > data
            )
            {
                Require( data.size() >= 0x40 && data[0] == 'M' && data[1] == 'Z', ErrorCode::ERR_NOT_PE, "The file is not a PE image" );
                m_Nt = static_cast<uint32_t>(Peek( data, 0x3C, 4 ));
                Require( Peek( data, m_Nt, 4 ) == 0x4550, ErrorCode::ERR_NOT_PE, "The file is not a PE image" );
                m_FileHeader = m_Nt + 4;
                m_Optional = m_Nt + 24;
                m_OptionalSize = static_cast<uint32_t>(Peek( data, m_FileHeader + 16, 2 ));
                m_SectionTable = m_Optional + m_OptionalSize;
                m_Sections = static_cast<uint32_t>(Peek( data, m_FileHeader + 2, 2 ));
                m_Is64 = Peek( data, m_Optional, 2 ) == 0x20B;
            }

            uint32_t Section(
                uint32_t index
            ) const
            {
                return m_SectionTable + index * 40;
            }

            uint32_t DataDirectory(
                uint32_t index
            ) const
            {
                return m_Optional + (m_Is64 ? 112 : 96) + index * 8;
            }

            uint32_t NumberOfRvaAndSizes(
                std::span< const uint8_t > data
            ) const
            {
                return static_cast<uint32_t>(Peek( data, m_Optional + (m_Is64 ? 108 : 92), 4 ));
            }
        };

        constexpr uint32_t OPT_SIZE_OF_CODE = 4;
        constexpr uint32_t OPT_SIZE_OF_INIT = 8;
        constexpr uint32_t OPT_SIZE_OF_UNINIT = 12;
        constexpr uint32_t OPT_BASE_OF_CODE = 20;
        constexpr uint32_t OPT_BASE_OF_DATA = 24;
        constexpr uint32_t OPT_SECTION_ALIGNMENT = 32;
        constexpr uint32_t OPT_FILE_ALIGNMENT = 36;
        constexpr uint32_t OPT_SIZE_OF_IMAGE = 56;
        constexpr uint32_t OPT_SIZE_OF_HEADERS = 60;
        constexpr uint32_t OPT_CHECKSUM = 64;
        constexpr uint32_t OPT_DLL_CHARACTERISTICS = 70;

        struct SectionHeader
        {
            uint32_t m_VirtualSize = 0;
            uint32_t m_VirtualAddress = 0;
            uint32_t m_SizeOfRawData = 0;
            uint32_t m_PointerToRawData = 0;
            uint32_t m_Characteristics = 0;
        };

        SectionHeader ReadSection(
            std::span< const uint8_t > data,
            const Layout & layout,
            uint32_t index
        )
        {
            uint32_t offset = layout.Section( index );
            SectionHeader section;
            section.m_VirtualSize = static_cast<uint32_t>(Peek( data, offset + 8, 4 ));
            section.m_VirtualAddress = static_cast<uint32_t>(Peek( data, offset + 12, 4 ));
            section.m_SizeOfRawData = static_cast<uint32_t>(Peek( data, offset + 16, 4 ));
            section.m_PointerToRawData = static_cast<uint32_t>(Peek( data, offset + 20, 4 ));
            section.m_Characteristics = static_cast<uint32_t>(Peek( data, offset + 36, 4 ));
            return section;
        }

        uint32_t ReadOptional32(
            std::span< const uint8_t > data,
            const Layout & layout,
            uint32_t field
        )
        {
            return static_cast<uint32_t>(Peek( data, layout.m_Optional + field, 4 ));
        }

        // End of the raw data of the image [headers and sections], where new section data goes.
        uint64_t RawImageEnd(
            std::span< const uint8_t > data,
            const Layout & layout
        )
        {
            uint64_t end = ReadOptional32( data, layout, OPT_SIZE_OF_HEADERS );

            for (uint32_t index = 0; index < layout.m_Sections; ++index)
            {
                SectionHeader section = ReadSection( data, layout, index );

                if (section.m_SizeOfRawData != 0)
                {
                    end = std::max< uint64_t >( end, static_cast<uint64_t>( section.m_PointerToRawData ) + section.m_SizeOfRawData );
                }
            }

            return std::min< uint64_t >( end, data.size() );
        }

        uint32_t VirtualImageEnd(
            std::span< const uint8_t > data,
            const Layout & layout
        )
        {
            uint32_t alignment = ReadOptional32( data, layout, OPT_SECTION_ALIGNMENT );
            uint32_t end = AlignUp( ReadOptional32( data, layout, OPT_SIZE_OF_HEADERS ), alignment );

            for (uint32_t index = 0; index < layout.m_Sections; ++index)
            {
                SectionHeader section = ReadSection( data, layout, index );
                uint32_t size = std::max( section.m_VirtualSize, section.m_SizeOfRawData );
                end = std::max( end, AlignUp( section.m_VirtualAddress + size, alignment ) );
            }

            return end;
        }

        // Moves file data at or after from by delta bytes and fixes the raw pointers that refer to it.
        void InsertBytes(
            std::vector< uint8_t > & data,
            const Layout & layout,
            uint64_t at,
            uint32_t count
        )
        {
            data.insert( data.begin() + static_cast<ptrdiff_t>(at), count, 0 );

            for (uint32_t index = 0; index < layout.m_Sections; ++index)
            {
                uint32_t offset = layout.Section( index );
                uint32_t pointer = static_cast<uint32_t>( Peek( data, offset + 20, 4 ) );

                if (pointer != 0 && pointer >= at)
                {
                    Poke( data, offset + 20, pointer + count, 4 );
                }
            }

            // The certificate table is addressed by file offset.
            if (layout.NumberOfRvaAndSizes( data ) > DIR_SECURITY)
            {
                uint32_t security = static_cast<uint32_t>(Peek( data, layout.DataDirectory( DIR_SECURITY ), 4 ));

                if (security != 0 && security >= at)
                {
                    Poke( data, layout.DataDirectory( DIR_SECURITY ), security + count, 4 );
                }
            }
        }

        void RemoveBytes(
            std::vector< uint8_t > & data,
            const Layout & layout,
            uint64_t at,
            uint32_t count
        )
        {
            data.erase( data.begin() + static_cast<ptrdiff_t>(at), data.begin() + static_cast<ptrdiff_t>(at + count) );

            for (uint32_t index = 0; index < layout.m_Sections; ++index)
            {
                uint32_t offset = layout.Section( index );
                uint32_t pointer = static_cast<uint32_t>( Peek( data, offset + 20, 4 ) );

                if (pointer != 0 && pointer > at)
                {
                    Poke( data, offset + 20, pointer >= at + count ? pointer - count : static_cast<uint32_t>(at), 4 );
                }
            }

            if (layout.NumberOfRvaAndSizes( data ) > DIR_SECURITY)
            {
                uint32_t security = static_cast<uint32_t>(Peek( data, layout.DataDirectory( DIR_SECURITY ), 4 ));

                if (security != 0 && security > at)
                {
                    Poke( data, layout.DataDirectory( DIR_SECURITY ), security - count, 4 );
                }
            }
        }

        void SetDirectory(
            std::vector< uint8_t > & data,
            const Layout & layout,
            uint32_t index,
            uint32_t rva,
            uint32_t size
        )
        {
            if (index >= layout.NumberOfRvaAndSizes( data ))
            {
                Require( rva == 0 && size == 0, ErrorCode::ERR_UNSUPPORTED, "The optional header has no room for data directory " + std::to_string( index ) );
                return;
            }

            Poke( data, layout.DataDirectory( index ), rva, 4 );
            Poke( data, layout.DataDirectory( index ) + 4, size, 4 );
        }

        uint32_t GetDirectoryRva(
            std::span< const uint8_t > data,
            const Layout & layout,
            uint32_t index
        )
        {
            return index < layout.NumberOfRvaAndSizes( data ) ? static_cast<uint32_t>( Peek( data, layout.DataDirectory( index ), 4 ) ) : 0;
        }

        uint32_t GetDirectorySize(
            std::span< const uint8_t > data,
            const Layout & layout,
            uint32_t index
        )
        {
            return index < layout.NumberOfRvaAndSizes( data ) ? static_cast<uint32_t>( Peek( data, layout.DataDirectory( index ) + 4, 4 ) ) : 0;
        }

        // Makes sure there is room for one more section header, growing the headers by one file alignment if needed.
        std::string MakeRoomForSectionHeader(
            std::vector< uint8_t > & data,
            const Layout & layout
        )
        {
            uint32_t table_end = layout.Section( layout.m_Sections );
            uint32_t size_of_headers = ReadOptional32( data, layout, OPT_SIZE_OF_HEADERS );
            uint32_t file_alignment = ReadOptional32( data, layout, OPT_FILE_ALIGNMENT );
            uint32_t first_raw = size_of_headers;
            uint32_t first_virtual = UINT32_MAX;

            for (uint32_t index = 0; index < layout.m_Sections; ++index)
            {
                SectionHeader section = ReadSection( data, layout, index );

                if (section.m_SizeOfRawData != 0 && section.m_PointerToRawData != 0)
                {
                    first_raw = std::min( first_raw, section.m_PointerToRawData );
                }

                first_virtual = std::min( first_virtual, section.m_VirtualAddress );
            }

            std::string note;

            // Old linkers put the bound import table right after the section table.
            uint32_t bound = GetDirectoryRva( data, layout, DIR_BOUND_IMPORT );

            if (bound != 0 && bound < table_end + 40 && bound + GetDirectorySize( data, layout, DIR_BOUND_IMPORT ) > table_end)
            {
                SetDirectory( data, layout, DIR_BOUND_IMPORT, 0, 0 );
                note = " The bound import table that used the header space was removed.";
            }

            if (table_end + 40 <= first_raw && table_end + 40 <= size_of_headers)
            {
                // The new header area must not contain other data
                std::fill( data.begin() + table_end, data.begin() + table_end + 40, 0 );
                return note;
            }

            // The headers grow by inserting bytes where the first section data starts, that must be behind the section
            // table. Packed files that let raw data overlap the table, or a 'SizeOfHeaders' that ends inside it, would get
            // the insertion in the middle of the table.
            Require( std::min( first_raw, size_of_headers ) >= table_end, ErrorCode::ERR_UNSUPPORTED,
                "Section data or SizeOfHeaders overlaps the section table, there is no safe place for another section header" );
            uint32_t grow = AlignUp( table_end + 40, file_alignment ) - std::min( first_raw, size_of_headers );
            grow = AlignUp( grow, file_alignment );
            uint32_t new_headers = AlignUp( size_of_headers + grow, file_alignment );
            Require( new_headers <= first_virtual, ErrorCode::ERR_UNSUPPORTED,
                "There is no room for another section header, the headers would overlap the first section in memory" );

            InsertBytes( data, layout, first_raw, grow );
            Poke( data, layout.m_Optional + OPT_SIZE_OF_HEADERS, new_headers, 4 );
            std::fill( data.begin() + table_end, data.begin() + table_end + 40, 0 );
            return " The headers grew by " + Hex( grow, 1 ) + "h bytes and the section data moved." + note;
        }

        void WriteSectionName(
            std::vector< uint8_t > & data,
            uint32_t header,
            const std::string & name
        )
        {
            std::memset( data.data() + header, 0, 8 );
            std::memcpy( data.data() + header, name.data(), std::min< size_t >( name.size(), 8 ) );
        }
    }

    uint32_t AppendSection(
        std::vector< uint8_t > & data,
        const NewSection & section
    )
    {
        Layout layout( data );
        Require( layout.m_Sections < 96, ErrorCode::ERR_UNSUPPORTED, "Windows does not load images with more than 96 sections" );
        MakeRoomForSectionHeader( data, layout );
        layout = Layout( data );

        uint32_t file_alignment = std::max< uint32_t >( ReadOptional32( data, layout, OPT_FILE_ALIGNMENT ), 1 );
        uint32_t section_alignment = std::max< uint32_t >( ReadOptional32( data, layout, OPT_SECTION_ALIGNMENT ), 1 );
        uint32_t virtual_address = VirtualImageEnd( data, layout );
        uint32_t virtual_size = std::max< uint32_t >( section.m_VirtualSize, static_cast<uint32_t>( section.m_Data.size() ) );
        virtual_size = std::max< uint32_t >( virtual_size, 1 );
        uint32_t raw_size = section.m_HeaderOnly ? 0 : AlignUp( static_cast<uint32_t>( section.m_Data.size() ), file_alignment );
        uint32_t raw_pointer = 0;

        if (raw_size != 0)
        {
            // Section data goes after the last section and before an overlay or certificate table
            uint64_t raw_end = RawImageEnd( data, layout );
            raw_pointer = AlignUp( static_cast<uint32_t>(raw_end), file_alignment );
            InsertBytes( data, layout, raw_end, raw_pointer - static_cast<uint32_t>(raw_end) + raw_size );
            std::copy( section.m_Data.begin(), section.m_Data.end(), data.begin() + raw_pointer );
        }

        uint32_t header = layout.Section( layout.m_Sections );
        WriteSectionName( data, header, section.m_Name );
        Poke( data, header + 8, virtual_size, 4 );
        Poke( data, header + 12, virtual_address, 4 );
        Poke( data, header + 16, raw_size, 4 );
        Poke( data, header + 20, raw_pointer, 4 );
        Poke( data, header + 24, 0, 8 );
        Poke( data, header + 32, 0, 4 );
        Poke( data, header + 36, section.m_Characteristics, 4 );
        Poke( data, layout.m_FileHeader + 2, layout.m_Sections + 1, 2 );
        Poke( data, layout.m_Optional + OPT_SIZE_OF_IMAGE, AlignUp( virtual_address + virtual_size, section_alignment ), 4 );
        return virtual_address;
    }

    namespace
    {
        void RebuildImageSize(
            std::vector< uint8_t > & data
        )
        {
            Layout layout( data );
            Poke( data, layout.m_Optional + OPT_SIZE_OF_IMAGE, VirtualImageEnd( data, layout ), 4 );
        }

        std::string RebuildPeHeader(
            std::vector< uint8_t > & data
        )
        {
            Layout layout( data );
            uint32_t file_alignment = ReadOptional32( data, layout, OPT_FILE_ALIGNMENT );
            uint32_t code = 0;
            uint32_t initialized = 0;
            uint32_t uninitialized = 0;
            uint32_t base_of_code = 0;
            uint32_t base_of_data = 0;
            uint32_t first_raw = UINT32_MAX;

            for (uint32_t index = 0; index < layout.m_Sections; ++index)
            {
                SectionHeader section = ReadSection( data, layout, index );

                if (section.m_Characteristics & 0x20)
                {
                    code += AlignUp( section.m_SizeOfRawData, file_alignment );
                    base_of_code = base_of_code == 0 ? section.m_VirtualAddress : base_of_code;
                }

                if (section.m_Characteristics & 0x40)
                {
                    initialized += AlignUp( section.m_SizeOfRawData, file_alignment );
                    base_of_data = base_of_data == 0 ? section.m_VirtualAddress : base_of_data;
                }

                if (section.m_Characteristics & 0x80)
                {
                    // VirtualSize 0 [old linkers]. The loader uses 'SizeOfRawData'.
                    uninitialized += AlignUp( section.m_VirtualSize != 0 ? section.m_VirtualSize : section.m_SizeOfRawData, file_alignment );
                }

                if (section.m_SizeOfRawData != 0 && section.m_PointerToRawData != 0)
                {
                    first_raw = std::min( first_raw, section.m_PointerToRawData );
                }
            }

            Poke( data, layout.m_Optional + OPT_SIZE_OF_CODE, code, 4 );
            Poke( data, layout.m_Optional + OPT_SIZE_OF_INIT, initialized, 4 );
            Poke( data, layout.m_Optional + OPT_SIZE_OF_UNINIT, uninitialized, 4 );

            if (base_of_code != 0)
            {
                Poke( data, layout.m_Optional + OPT_BASE_OF_CODE, base_of_code, 4 );
            }

            if (!layout.m_Is64 && base_of_data != 0)
            {
                Poke( data, layout.m_Optional + OPT_BASE_OF_DATA, base_of_data, 4 );
            }

            // 'SizeOfHeaders' covers the section table rounded to the file alignment, but never past the first section data.
            uint32_t headers = AlignUp( layout.Section( layout.m_Sections ), file_alignment );

            if (first_raw == UINT32_MAX || headers <= first_raw)
            {
                Poke( data, layout.m_Optional + OPT_SIZE_OF_HEADERS, headers, 4 );
            }

            Poke( data, layout.m_Optional + OPT_SIZE_OF_IMAGE, VirtualImageEnd( data, layout ), 4 );
            return "PE header rebuilt: SizeOfCode " + Hex( code, 1 ) + "h, SizeOfInitializedData " + Hex( initialized, 1 ) + "h, SizeOfImage " +
                Hex( VirtualImageEnd( data, layout ), 1 ) + "h";
        }

        std::string UpdateChecksum(
            std::vector< uint8_t > & data
        )
        {
            Layout layout( data );
            uint32_t checksum = ComputePeChecksum( data, layout.m_Optional + OPT_CHECKSUM );
            Poke( data, layout.m_Optional + OPT_CHECKSUM, checksum, 4 );
            return "Checksum updated to " + Hex( checksum );
        }

        void DeleteSection(
            std::vector< uint8_t > & data,
            uint32_t index,
            bool with_data
        )
        {
            Layout layout( data );
            Require( index < layout.m_Sections, ErrorCode::ERR_NOT_FOUND, "No such section" );
            SectionHeader section = ReadSection( data, layout, index );

            if (with_data && section.m_SizeOfRawData != 0 && section.m_PointerToRawData != 0 &&
                static_cast<uint64_t>( section.m_PointerToRawData ) + section.m_SizeOfRawData <= data.size())
            {
                // Clear the header first so 'RemoveBytes' does not move its own pointer.
                Poke( data, layout.Section( index ) + 20, 0, 4 );
                RemoveBytes( data, layout, section.m_PointerToRawData, section.m_SizeOfRawData );
            }

            uint32_t header = layout.Section( index );
            uint32_t table_end = layout.Section( layout.m_Sections );
            std::memmove( data.data() + header, data.data() + header + 40, table_end - header - 40 );
            std::memset( data.data() + table_end - 40, 0, 40 );
            Poke( data, layout.m_FileHeader + 2, layout.m_Sections - 1, 2 );
            RebuildImageSize( data );
        }

        std::string RemoveDirectoryData(
            std::vector< uint8_t > & data,
            const PeImage & image,
            uint32_t directory,
            const char * what
        )
        {
            Layout layout( data );
            DataDirectory entry = image.GetDirectory( directory );

            if (entry.m_Rva == 0 || entry.m_Size == 0)
            {
                return std::string( "No " ) + what + " to remove";
            }

            uint64_t offset = image.RvaToOffset( entry.m_Rva );

            if (offset != NO_OFFSET && offset + entry.m_Size <= data.size())
            {
                std::fill( data.begin() + static_cast<ptrdiff_t>(offset), data.begin() + static_cast<ptrdiff_t>(offset + entry.m_Size), 0 );
            }

            SetDirectory( data, layout, directory, 0, 0 );
            return std::string( "Removed the " ) + what;
        }

        std::string RemoveRelocations(
            std::vector< uint8_t > & data,
            const PeImage & image
        )
        {
            if (!image.HasDirectory( DIR_BASERELOC ))
            {
                return "No relocations to remove";
            }

            std::string message = RemoveDirectoryData( data, image, DIR_BASERELOC, "relocations" );
            Layout layout( data );
            uint16_t characteristics = static_cast<uint16_t>(Peek( data, layout.m_FileHeader + 18, 2 ));
            Poke( data, layout.m_FileHeader + 18, characteristics | 0x0001, 2 );
            uint16_t dll = static_cast<uint16_t>(Peek( data, layout.m_Optional + OPT_DLL_CHARACTERISTICS, 2 ));
            Poke( data, layout.m_Optional + OPT_DLL_CHARACTERISTICS, dll & ~0x0060, 2 );
            return message + ", the image is no longer relocatable (ASLR off)";
        }

        std::string RemoveDebug(
            std::vector< uint8_t > & data,
            const PeImage & image
        )
        {
            if (!image.HasDirectory( DIR_DEBUG ))
            {
                return "No debug directory to remove";
            }

            // Clear the debug data each entry points to, then the entries.
            uint64_t offset = image.RvaToOffset( image.GetDirectory( DIR_DEBUG ).m_Rva );
            uint32_t count = image.GetDirectory( DIR_DEBUG ).m_Size / 28;

            for (uint32_t index = 0; offset != NO_OFFSET && index < count && index < 64; ++index)
            {
                uint32_t size = static_cast<uint32_t>( Peek( data, offset + index * 28ull + 16, 4 ) );
                uint32_t pointer = static_cast<uint32_t>( Peek( data, offset + index * 28ull + 24, 4 ) );

                if (pointer != 0 && static_cast<uint64_t>( pointer ) + size <= data.size())
                {
                    std::fill( data.begin() + pointer, data.begin() + pointer + size, 0 );
                }
            }

            return RemoveDirectoryData( data, image, DIR_DEBUG, "debug directory" );
        }

        std::string Rebase(
            std::vector< uint8_t > & data,
            const PeImage & image,
            uint64_t new_base
        )
        {
            Require( image.HasDirectory( DIR_BASERELOC ), ErrorCode::ERR_UNSUPPORTED, "The image has no relocations and cannot be rebased" );
            Require( (new_base & 0xFFFF) == 0, ErrorCode::ERR_INVALID_ARGUMENT, "The image base must be a multiple of 64 KB" );
            Require( image.Is64() || new_base <= 0xFFFFFFFFull, ErrorCode::ERR_INVALID_ARGUMENT, "A 32 bit image base must be below 4 GB" );
            Layout layout( data );
            uint64_t old_base = image.GetImageBase();
            uint64_t delta = new_base - old_base;
            const PeLib::RelocationsDirectory & relocations = image.GetFile().relocDir();
            uint32_t applied = 0;

            for (unsigned int block = 0; block < relocations.calcNumberOfRelocations(); ++block)
            {
                uint32_t page = relocations.getVirtualAddress( block );

                for (unsigned int item = 0; item < relocations.calcNumberOfRelocationData( block ); ++item)
                {
                    uint16_t entry = relocations.getRelocationData( block, item );
                    uint32_t type = entry >> 12;
                    uint64_t offset = image.RvaToOffset( page + (entry & 0x0FFF) );

                    if (offset == NO_OFFSET)
                    {
                        continue;
                    }

                    switch (type)
                    {
                        case 3:
                            Poke( data, offset, static_cast<uint32_t>(Peek( data, offset, 4 ) + delta), 4 );
                            ++applied;
                            break;
                        case 10:
                            Poke( data, offset, Peek( data, offset, 8 ) + delta, 8 );
                            ++applied;
                            break;
                        case 1:
                            Poke( data, offset, static_cast<uint16_t>(Peek( data, offset, 2 ) + ((delta >> 16) & 0xFFFF)), 2 );
                            ++applied;
                            break;
                        case 2:
                            Poke( data, offset, static_cast<uint16_t>(Peek( data, offset, 2 ) + (delta & 0xFFFF)), 2 );
                            ++applied;
                            break;
                        default:
                            break;
                    }
                }
            }

            Poke( data, layout.m_Optional + (layout.m_Is64 ? 24 : 28), new_base, layout.m_Is64 ? 8 : 4 );
            return "Rebased from " + Hex( old_base, 1 ) + " to " + Hex( new_base, 1 ) + ", " + std::to_string( applied ) + " relocations applied";
        }

        std::string Realign(
            std::vector< uint8_t > & data,
            uint32_t alignment
        )
        {
            Require( alignment >= 512 && (alignment & (alignment - 1)) == 0 && alignment <= 0x10000, ErrorCode::ERR_INVALID_ARGUMENT,
                "The file alignment must be a power of two between 512 and 64 KB" );
            Layout layout( data );
            Require( alignment <= ReadOptional32( data, layout, OPT_SECTION_ALIGNMENT ), ErrorCode::ERR_INVALID_ARGUMENT,
                "The file alignment cannot be larger than the section alignment" );

            // Sections in raw order, each with its data trimmed of trailing zeros.
            struct Item
            {
                uint32_t m_Index;
                uint32_t m_Pointer;
                std::vector< uint8_t > m_Data;
            };

            std::vector< Item > items;

            for (uint32_t index = 0; index < layout.m_Sections; ++index)
            {
                SectionHeader section = ReadSection( data, layout, index );

                if (section.m_SizeOfRawData == 0 || section.m_PointerToRawData == 0)
                {
                    continue;
                }

                uint64_t end = std::min< uint64_t >( static_cast<uint64_t>( section.m_PointerToRawData ) + section.m_SizeOfRawData, data.size() );
                std::vector< uint8_t > bytes( data.begin() + section.m_PointerToRawData, data.begin() + static_cast<ptrdiff_t>( end ) );

                while (!bytes.empty() && bytes.back() == 0)
                {
                    bytes.pop_back();
                }

                items.push_back( { index, section.m_PointerToRawData, std::move( bytes ) } );
            }

            std::sort( items.begin(), items.end(), [] ( const Item & left, const Item & right )
            {
                return left.m_Pointer < right.m_Pointer;
            } );
            uint64_t raw_end = RawImageEnd( data, layout );
            std::vector< uint8_t > tail( data.begin() + static_cast<ptrdiff_t>( raw_end ), data.end() );
            uint32_t security = GetDirectoryRva( data, layout, DIR_SECURITY );
            uint32_t headers = AlignUp( layout.Section( layout.m_Sections ), alignment );
            std::vector< uint8_t > result( data.begin(), data.begin() + std::min< size_t >( headers, data.size() ) );
            result.resize( headers, 0 );

            for (const Item & item : items)
            {
                uint32_t pointer = static_cast<uint32_t>(result.size());
                uint32_t size = AlignUp( static_cast<uint32_t>(item.m_Data.size()), alignment );
                result.insert( result.end(), item.m_Data.begin(), item.m_Data.end() );
                result.resize( pointer + size, 0 );
                Poke( result, layout.Section( item.m_Index ) + 16, size, 4 );
                Poke( result, layout.Section( item.m_Index ) + 20, size != 0 ? pointer : 0, 4 );
            }

            uint64_t new_end = result.size();
            result.insert( result.end(), tail.begin(), tail.end() );
            Poke( result, layout.m_Optional + OPT_FILE_ALIGNMENT, alignment, 4 );
            Poke( result, layout.m_Optional + OPT_SIZE_OF_HEADERS, headers, 4 );

            if (security != 0 && security >= raw_end)
            {
                Poke( result, layout.DataDirectory( DIR_SECURITY ), static_cast<uint32_t>(security - raw_end + new_end), 4 );
            }

            size_t before = data.size();
            data = std::move( result );
            return "Realigned to " + Hex( alignment, 1 ) + "h, size " + FormatBytes( before ) + " -> " + FormatBytes( data.size() );
        }

        // Import Adder. Rebuilds the import table into a new section with the existing descriptors plus new ones
        // Room at the end of the last section for appended data [the 'Import Adder' without a new section, like CFF
        // Explorer does it]. The data starts after the virtual size, so zero initialized memory stays zero.
        struct LastSectionSpace
        {
            bool m_Usable = false;
            uint32_t m_Index = 0;
            uint32_t m_Offset = 0;
            uint32_t m_Rva = 0;
            std::string m_Reason;
        };

        LastSectionSpace FindLastSectionSpace(
            std::span< const uint8_t > data,
            const Layout & layout
        )
        {
            LastSectionSpace space;

            if (layout.m_Sections == 0)
            {
                space.m_Reason = "the file has no sections";
                return space;
            }

            for (uint32_t index = 1; index < layout.m_Sections; ++index)
            {
                if (ReadSection( data, layout, index ).m_VirtualAddress > ReadSection( data, layout, space.m_Index ).m_VirtualAddress)
                {
                    space.m_Index = index;
                }
            }

            SectionHeader section = ReadSection( data, layout, space.m_Index );

            if (section.m_SizeOfRawData == 0 || section.m_PointerToRawData == 0)
            {
                space.m_Reason = "the last section has no file data";
                return space;
            }

            if (static_cast<uint64_t>(section.m_PointerToRawData) + section.m_SizeOfRawData != RawImageEnd( data, layout ))
            {
                space.m_Reason = "the last section in memory is not the last one in the file";
                return space;
            }

            space.m_Usable = true;
            space.m_Offset = AlignUp( std::max( section.m_VirtualSize, section.m_SizeOfRawData ), 16 );
            space.m_Rva = section.m_VirtualAddress + space.m_Offset;
            return space;
        }

        void ExtendLastSection(
            std::vector< uint8_t > & data,
            const LastSectionSpace & space,
            std::span< const uint8_t > bytes
        )
        {
            Layout layout( data );
            SectionHeader section = ReadSection( data, layout, space.m_Index );
            uint32_t file_alignment = std::max< uint32_t >( ReadOptional32( data, layout, OPT_FILE_ALIGNMENT ), 1 );
            uint32_t raw_size = AlignUp( space.m_Offset + static_cast<uint32_t>(bytes.size()), file_alignment );
            InsertBytes( data, layout, static_cast<uint64_t>(section.m_PointerToRawData) + section.m_SizeOfRawData, raw_size - section.m_SizeOfRawData );
            std::copy( bytes.begin(), bytes.end(), data.begin() + section.m_PointerToRawData + space.m_Offset );

            uint32_t header = layout.Section( space.m_Index );
            Poke( data, header + 8, space.m_Offset + static_cast<uint32_t>(bytes.size()), 4 );
            Poke( data, header + 16, raw_size, 4 );

            // Readable, writable initialized data [the loader patches the address tables], never discardable.
            constexpr uint32_t READ_WRITE_DATA = 0xC0000040;
            constexpr uint32_t DISCARDABLE = 0x02000000;
            Poke( data, header + 36, (section.m_Characteristics | READ_WRITE_DATA) & ~DISCARDABLE, 4 );
            RebuildImageSize( data );
        }

        std::string AddImports(
            std::vector< uint8_t > & data,
            const PeImage & image,
            const JsonValue & command
        )
        {
            const JsonValue * modules = command.Find( "modules" );
            Require( modules != nullptr && modules->IsArray() && !modules->GetItems().empty(), ErrorCode::ERR_INVALID_ARGUMENT, "No modules to import" );
            bool use_oft = command.GetBool( "use_oft", true );
            uint32_t pointer = image.Is64() ? 8 : 4;
            uint64_t ordinal_flag = image.Is64() ? 0x8000000000000000ull : 0x80000000ull;

            // Existing descriptors are copied as they are, their thunks keep working.
            std::vector< std::array< uint8_t, 20 > > descriptors;
            uint64_t import_offset = image.HasDirectory( DIR_IMPORT ) ? image.RvaToOffset( image.GetDirectory( DIR_IMPORT ).m_Rva ) : NO_OFFSET;

            for (uint32_t index = 0; import_offset != NO_OFFSET && index < 4096; ++index)
            {
                std::array< uint8_t, 20 > descriptor {};
                image.ReadAt( import_offset + index * 20ull, descriptor.data(), 20 );

                if (std::all_of( descriptor.begin(), descriptor.end(), [] ( uint8_t byte )
                {
                    return byte == 0;
                } ))
                {
                    break;
                }

                descriptors.push_back( descriptor );
            }

            struct NewModule
            {
                std::string m_Name;
                std::vector< std::pair< std::string, uint32_t > > m_Functions;
            };

            std::vector< NewModule > added;

            for (const JsonValue & module : modules->GetItems())
            {
                NewModule entry;
                entry.m_Name = module.GetString( "name" );
                Require( !entry.m_Name.empty(), ErrorCode::ERR_INVALID_ARGUMENT, "A module has no name" );
                const JsonValue * functions = module.Find( "functions" );
                Require( functions != nullptr && functions->IsArray() && !functions->GetItems().empty(), ErrorCode::ERR_INVALID_ARGUMENT,
                    "Module " + entry.m_Name + " has no functions" );

                for (const JsonValue & function : functions->GetItems())
                {
                    entry.m_Functions.emplace_back( function.GetString( "name" ), static_cast<uint32_t>(function.GetUnsigned( "ordinal", 0 )) );
                }

                added.push_back( std::move( entry ) );
            }

            // Where the new table goes. Either the end of the last section [default, no new section header] or a new section.
            Layout layout( data );
            bool create_section = command.GetBool( "create_section", false );
            LastSectionSpace space;
            std::string note;

            if (!create_section)
            {
                space = FindLastSectionSpace( data, layout );

                if (!space.m_Usable)
                {
                    create_section = true;
                    note = " (a new section was needed: " + space.m_Reason + ")";
                }
            }

            // Layout of the new table. Descriptors, then per module the OFT and FT arrays, then hint and name entries and names.
            uint32_t descriptor_bytes = static_cast<uint32_t>((descriptors.size() + added.size() + 1) * 20);
            std::vector< uint8_t > section( descriptor_bytes, 0 );
            uint32_t section_rva = create_section ? VirtualImageEnd( data, layout ) : space.m_Rva;

            auto append = [&] ( const void * bytes, size_t length ) -> uint32_t
            {
                uint32_t position = static_cast<uint32_t>(section.size());
                section.insert( section.end(), static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + length );
                return position;
            };

            auto align = [&] ( uint32_t to )
            {
                section.resize( AlignUp( static_cast<uint32_t>(section.size()), to ), 0 );
            };

            for (size_t index = 0; index < descriptors.size(); ++index)
            {
                std::memcpy( section.data() + index * 20, descriptors[index].data(), 20 );
            }

            for (size_t index = 0; index < added.size(); ++index)
            {
                const NewModule & module = added[index];
                align( 8 );
                uint32_t thunk_count = static_cast<uint32_t>( module.m_Functions.size() + 1 );
                uint32_t oft = append( std::vector< uint8_t >( thunk_count * pointer, 0 ).data(), thunk_count * pointer );
                align( 8 );
                uint32_t ft = append( std::vector< uint8_t >( thunk_count * pointer, 0 ).data(), thunk_count * pointer );

                for (size_t function = 0; function < module.m_Functions.size(); ++function)
                {
                    uint64_t thunk = 0;

                    if (module.m_Functions[function].first.empty())
                    {
                        thunk = ordinal_flag | (module.m_Functions[function].second & 0xFFFF);
                    }
                    else
                    {
                        align( 2 );
                        uint16_t hint = static_cast<uint16_t>(module.m_Functions[function].second);
                        uint32_t hint_name = append( &hint, 2 );
                        append( module.m_Functions[function].first.c_str(), module.m_Functions[function].first.size() + 1 );
                        thunk = section_rva + hint_name;
                    }

                    Poke( section, oft + function * pointer, thunk, pointer );
                    Poke( section, ft + function * pointer, thunk, pointer );
                }

                uint32_t name = append( module.m_Name.c_str(), module.m_Name.size() + 1 );
                uint32_t descriptor = static_cast<uint32_t>((descriptors.size() + index) * 20);
                Poke( section, descriptor, use_oft ? section_rva + oft : 0, 4 );
                Poke( section, descriptor + 12, section_rva + name, 4 );
                Poke( section, descriptor + 16, section_rva + ft, 4 );
            }

            std::string place;
            uint32_t rva = section_rva;

            if (create_section)
            {
                NewSection new_section;
                new_section.m_Name = command.GetString( "section_name", ".imports" );
                new_section.m_Data = std::move( section );

                // Writable. The loader patches the new FirstThunk arrays in place.
                new_section.m_Characteristics = 0xC0000040;
                rva = AppendSection( data, new_section );
                Require( rva == section_rva, ErrorCode::ERR_INTERNAL, "Unexpected section address" );
                place = "new section " + new_section.m_Name + note;
            }
            else
            {
                std::string name = image.GetSectionName( space.m_Index );
                ExtendLastSection( data, space, section );
                place = "section " + name + " (appended)";
            }

            layout = Layout( data );
            SetDirectory( data, layout, DIR_IMPORT, rva, descriptor_bytes );
            SetDirectory( data, layout, DIR_BOUND_IMPORT, 0, 0 );

            size_t functions = 0;

            for (const NewModule & module : added)
            {
                functions += module.m_Functions.size();
            }

            return "Import table rebuilt in " + place + " at RVA " + Hex( rva ) + ", " + std::to_string( functions ) + " functions from " +
                std::to_string( added.size() ) + " modules added";
        }

        std::string AnswerOk(
            const std::string & message,
            const std::string & select
        )
        {
            JsonWriter writer;
            writer.BeginObject();
            writer.MemberBool( "ok", true );
            writer.Member( "message", message );

            if (!select.empty())
            {
                writer.Member( "select", select );
            }

            writer.EndObject();
            return writer.TakeText();
        }

        uint32_t SectionIndexArgument(
            const JsonValue & command,
            const Layout & layout
        )
        {
            uint64_t index = command.GetUnsigned( "index", UINT64_MAX );
            Require( index < layout.m_Sections, ErrorCode::ERR_INVALID_ARGUMENT, "No such section" );
            return static_cast<uint32_t>( index );
        }

        std::vector< uint8_t > DataArgument(
            const JsonValue & command
        )
        {
            std::string path = command.GetString( "path" );

            if (!path.empty())
            {
                return ReadWholeFile( std::filesystem::path( Utf16FromUtf8( path ) ), 512ull * 1024 * 1024 );
            }

            std::string hex = command.GetString( "data" );
            std::vector< uint8_t > bytes;

            for (size_t index = 0; index + 1 < hex.size(); index += 2)
            {
                uint64_t value = 0;
                Require( ParseNumber( "0x" + hex.substr( index, 2 ), value ), ErrorCode::ERR_PARSE, "Invalid hex data" );
                bytes.push_back( static_cast<uint8_t>(value) );
            }

            return bytes;
        }
    }

    std::string ExecuteCommand(
        Document & document,
        const JsonValue & command
    )
    {
        std::string id = command.GetString( "command" );

        if (id.rfind( "resources.", 0 ) == 0)
        {
            return ExecuteResourceCommand( document, command );
        }

        if (id.rfind( "rich.", 0 ) == 0)
        {
            return ExecuteRichCommand( document, command );
        }

        std::vector< uint8_t > data = document.GetData();
        PeImage image( document.GetData() );
        Require( image.IsPe(), ErrorCode::ERR_NOT_PE, "The file is not a PE image" );
        Layout layout( data );
        std::string message;
        std::string select;
        std::string description;

        if (id == "section.add_empty" || id == "section.add_file" || id == "section.add_header")
        {
            NewSection section;
            section.m_Name = command.GetString( "name", ".new" );
            section.m_Characteristics = static_cast<uint32_t>(command.GetUnsigned( "characteristics", id == "section.add_file" ? 0x40000040 : 0xC0000040 ));
            section.m_HeaderOnly = id == "section.add_header";
            uint32_t size = static_cast<uint32_t>(command.GetUnsigned( "size", 0x1000 ));

            if (id == "section.add_file")
            {
                section.m_Data = DataArgument( command );
                Require( !section.m_Data.empty(), ErrorCode::ERR_INVALID_ARGUMENT, "The data for the new section is empty" );
            }
            else if (id == "section.add_empty")
            {
                Require( size != 0, ErrorCode::ERR_INVALID_ARGUMENT, "The section size cannot be zero" );
                section.m_Data.assign( size, 0 );
            }

            section.m_VirtualSize = section.m_HeaderOnly ? size : 0;
            uint32_t rva = AppendSection( data, section );
            message = "Section " + section.m_Name + " added at RVA " + Hex( rva );
            select = "sections";
            description = "Add section " + section.m_Name;
        }
        else if (id == "section.delete" || id == "section.delete_header")
        {
            uint32_t index = SectionIndexArgument( command, layout );
            std::string name = image.GetSectionName( index );
            DeleteSection( data, index, id == "section.delete" );
            message = "Section " + name + " deleted";
            select = "sections";
            description = "Delete section " + name;
        }
        else if (id == "section.replace")
        {
            uint32_t index = SectionIndexArgument( command, layout );
            std::vector< uint8_t > bytes = DataArgument( command );
            SectionHeader section = ReadSection( data, layout, index );
            uint32_t file_alignment = ReadOptional32( data, layout, OPT_FILE_ALIGNMENT );
            bool last = RawImageEnd( data, layout ) == static_cast<uint64_t>(section.m_PointerToRawData) + section.m_SizeOfRawData;

            if (bytes.size() > section.m_SizeOfRawData)
            {
                Require( last && section.m_PointerToRawData != 0, ErrorCode::ERR_UNSUPPORTED,
                    "The new data is larger than the section, only the last section can grow" );
                uint32_t grow = AlignUp( static_cast<uint32_t>(bytes.size()), file_alignment ) - section.m_SizeOfRawData;
                InsertBytes( data, layout, static_cast<uint64_t>(section.m_PointerToRawData) + section.m_SizeOfRawData, grow );
                section.m_SizeOfRawData += grow;
                Poke( data, layout.Section( index ) + 16, section.m_SizeOfRawData, 4 );

                if (section.m_VirtualSize < bytes.size())
                {
                    Poke( data, layout.Section( index ) + 8, static_cast<uint32_t>( bytes.size() ), 4 );
                }

                RebuildImageSize( data );
            }

            std::fill( data.begin() + section.m_PointerToRawData, data.begin() + section.m_PointerToRawData + section.m_SizeOfRawData, 0 );
            std::copy( bytes.begin(), bytes.end(), data.begin() + section.m_PointerToRawData );
            message = "Section data replaced (" + FormatBytes( bytes.size() ) + ")";
            select = "sections";
            description = "Replace section data";
        }
        else if (id == "rebuild.image_size")
        {
            RebuildImageSize( data );
            message = "SizeOfImage rebuilt";
            description = "Rebuild image size";
        }
        else if (id == "rebuild.pe_header")
        {
            message = RebuildPeHeader( data );
            description = "Rebuild PE header";
        }
        else if (id == "rebuild")
        {
            // Rebuilder tool, the options run in a fixed order and each adds a line to the log.
            std::vector< std::string > log;

            if (command.GetBool( "remove_relocations" ))
            {
                log.push_back( RemoveRelocations( data, image ) );
            }

            if (command.GetBool( "remove_debug" ))
            {
                PeImage current( data );
                log.push_back( RemoveDebug( data, current ) );
            }

            if (command.GetBool( "remove_bound_imports" ))
            {
                SetDirectory( data, Layout( data ), DIR_BOUND_IMPORT, 0, 0 );
                log.push_back( "Bound import table removed" );
            }

            if (command.GetBool( "remove_strong_name" ))
            {
                PeImage current( data );
                uint64_t cor20 = current.HasDirectory( DIR_COM_DESCRIPTOR ) ? current.RvaToOffset( current.GetDirectory( DIR_COM_DESCRIPTOR ).m_Rva ) : NO_OFFSET;

                if (cor20 != NO_OFFSET)
                {
                    Poke( data, cor20 + 16, Peek( data, cor20 + 16, 4 ) & ~0x8ull, 4 );
                    Poke( data, cor20 + 32, 0, 8 );
                    log.push_back( "Strong name signature removed" );
                }
                else
                {
                    log.push_back( "Not a .NET assembly, no strong name to remove" );
                }
            }

            std::string new_base = command.GetString( "new_image_base" );

            if (!new_base.empty())
            {
                uint64_t value = 0;
                Require( ParseNumber( new_base, value ), ErrorCode::ERR_PARSE, "Invalid image base " + new_base );
                std::vector< uint8_t > snapshot = data;
                PeImage current( snapshot );
                log.push_back( Rebase( data, current, value ) );
            }

            uint32_t realign = static_cast<uint32_t>(command.GetUnsigned( "realign", 0 ));

            if (realign != 0)
            {
                log.push_back( Realign( data, realign ) );
            }

            if (command.GetBool( "rebuild_header" ))
            {
                log.push_back( RebuildPeHeader( data ) );
            }

            if (command.GetBool( "update_checksum" ))
            {
                log.push_back( UpdateChecksum( data ) );
            }

            for (const std::string & line : log)
            {
                message += (message.empty() ? "" : "\n") + line;
            }

            if (message.empty())
            {
                message = "Nothing to do, select at least one option";
            }

            description = "Rebuild";
        }
        else if (id == "imports.move_up" || id == "imports.move_down")
        {
            uint64_t index = command.GetUnsigned( "index", UINT64_MAX );
            size_t count = ReadImports( image ).size();
            Require( index < count, ErrorCode::ERR_INVALID_ARGUMENT, "No such import descriptor" );
            uint64_t other = id == "imports.move_up" ? index - 1 : index + 1;
            Require( other < count, ErrorCode::ERR_INVALID_ARGUMENT, "The descriptor cannot move further" );
            uint64_t base = image.RvaToOffset( image.GetDirectory( DIR_IMPORT ).m_Rva );
            std::swap_ranges( data.begin() + static_cast<ptrdiff_t>( base + index * 20 ), data.begin() + static_cast<ptrdiff_t>( base + index * 20 + 20 ),
                data.begin() + static_cast<ptrdiff_t>( base + other * 20 ) );
            message = "Import descriptor moved";
            select = "imports";
            description = "Move import descriptor";
        }
        else if (id == "imports.delete")
        {
            uint64_t index = command.GetUnsigned( "index", UINT64_MAX );
            std::vector< ImportModule > modules = ReadImports( image );
            Require( index < modules.size(), ErrorCode::ERR_INVALID_ARGUMENT, "No such import descriptor" );
            uint64_t base = image.RvaToOffset( image.GetDirectory( DIR_IMPORT ).m_Rva );
            uint64_t start = base + index * 20;
            uint64_t end = base + (modules.size() + 1) * 20;
            std::memmove( data.data() + start, data.data() + start + 20, end - start - 20 );
            std::memset( data.data() + end - 20, 0, 20 );
            message = "Import descriptor " + modules[index].m_Name + " deleted";
            select = "imports";
            description = "Delete import descriptor";
        }
        else if (id == "imports.add")
        {
            message = AddImports( data, image, command );
            select = "imports";
            description = "Add imports";
        }
        else if (id == "security.remove")
        {
            DataDirectory security = image.GetDirectory( DIR_SECURITY );
            Require( security.m_Rva != 0, ErrorCode::ERR_NOT_FOUND, "The file is not signed" );

            if (static_cast<uint64_t>(security.m_Rva) + security.m_Size >= data.size() && security.m_Rva <= data.size())
            {
                data.resize( security.m_Rva );
            }

            SetDirectory( data, Layout( data ), DIR_SECURITY, 0, 0 );

            message = "Signature removed";
            select = "overview";
            description = "Remove signature";
        }
        else if (id == "debug.remove")
        {
            // The table, the data of every entry [CodeView PDB path, POGO, VC feature etc] and the directory entry.
            DataDirectory debug = image.GetDirectory( DIR_DEBUG );
            Require( debug.m_Rva != 0 && debug.m_Size != 0, ErrorCode::ERR_NOT_FOUND, "The file has no debug directory" );
            uint64_t table = image.RvaToOffset( debug.m_Rva );
            uint64_t pe_size = image.GetPeSize();
            uint64_t trailing = data.size();
            size_t entries = 0;
            uint64_t cleared = 0;
            std::vector< std::pair< uint64_t, uint64_t > > blocks;

            for (uint64_t index = 0; table != NO_OFFSET && index < debug.m_Size / 28; ++index)
            {
                uint64_t entry = table + index * 28;

                if (entry + 28 > data.size())
                {
                    break;
                }

                uint64_t size = Peek( data, entry + 16, 4 );
                uint64_t pointer = Peek( data, entry + 24, 4 );

                if (pointer != 0 && size != 0 && pointer + size <= data.size())
                {
                    blocks.emplace_back( pointer, size );
                }

                std::memset( data.data() + entry, 0, 28 );
                ++entries;
            }

            // Data after the last section that ends the file [some linkers put the CodeView record there] is cut off,
            // everything else is zeroed in place.
            std::sort( blocks.begin(), blocks.end(), [] ( const auto & left, const auto & right )
            {
                return left.first > right.first;
            } );

            for (const auto & [pointer, size] : blocks)
            {
                std::memset( data.data() + pointer, 0, size );
                cleared += size;

                if (pointer >= pe_size && pointer + size >= trailing)
                {
                    trailing = pointer;
                }
            }

            if (trailing < data.size() && image.GetDirectory( DIR_SECURITY ).m_Rva == 0)
            {
                data.resize( trailing );
            }

            SetDirectory( data, Layout( data ), DIR_DEBUG, 0, 0 );

            message = "Debug directory removed (" + std::to_string( entries ) + (entries == 1 ? " entry, " : " entries, ") + FormatBytes( cleared ) + " of debug data cleared)";
            select = "overview";
            description = "Remove debug directory";
        }
        else if (id == "overlay.remove")
        {
            uint64_t offset = image.GetOverlayOffset();
            uint64_t size = image.GetOverlaySize();
            Require( size != 0, ErrorCode::ERR_NOT_FOUND, "The file has no overlay" );
            RemoveBytes( data, layout, offset, static_cast<uint32_t>(size) );
            message = "Overlay removed (" + FormatBytes( size ) + ")";
            select = "overview";
            description = "Remove overlay";
        }
        else
        {
            throw Error( ErrorCode::ERR_NOT_FOUND, "Unknown command " + id );
        }

        document.SetData( std::move( data ), description );
        return AnswerOk( message, select );
    }
}