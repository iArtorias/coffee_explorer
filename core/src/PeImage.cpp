#include "pecore/PeImage.h"

#include "retdec/pelib/PeFile.h"

#include <algorithm>

namespace PeCore
{
    PeImage::PeImage(
        std::span< const uint8_t > data
    )
        : m_Data( data )
    {
        auto characters = std::span< const char >( reinterpret_cast<const char *>(data.data()), data.size() );
        m_Stream = std::make_unique< std::ispanstream >( characters );
        m_File = std::make_unique< PeLib::PeFileT >( *m_Stream );

        // Own sanity check first, PeLib also accepts things that are not worth showing as PE.
        if (data.size() < 0x40 || data[0] != 'M' || data[1] != 'Z')
        {
            return;
        }

        m_NtOffset = Read32( 0x3C );

        if (static_cast<uint64_t>(m_NtOffset) + 24 > data.size() || Read32( m_NtOffset ) != 0x00004550)
        {
            AddWarning( "The file has an MZ header but no PE header" );
            return;
        }

        m_OptionalOffset = m_NtOffset + 24;
        m_OptionalSize = Read16( m_NtOffset + 4 + 16 );
        m_SectionTableOffset = m_OptionalOffset + m_OptionalSize;
        uint16_t magic = Read16( m_OptionalOffset );

        if (magic != 0x10B && magic != 0x20B)
        {
            AddWarning( "Unknown optional header magic " + Hex( magic, 4 ) );
            return;
        }

        m_Is64 = magic == 0x20B;
        int result = PeLib::ERROR_NONE;

        try
        {
            result = m_File->loadPeHeaders();
        }
        catch (const std::exception & error)
        {
            AddWarning( std::string( "PeLib failed to load the image: " ) + error.what() );
            return;
        }

        if (result != PeLib::ERROR_NONE)
        {
            AddWarning( "PeLib could not map the image (error " + std::to_string( result ) + ")" );
            return;
        }

        m_IsPe = true;
        PeLib::LoaderError loader_error = m_File->loaderError();

        if (loader_error != PeLib::LDR_ERROR_NONE)
        {
            AddWarning( PeLib::getLoaderErrorString( loader_error, true ) );
        }

        // PE size. End of the headers or of the last raw section, whatever is further.
        m_PeSize = std::min< uint64_t >( GetSizeOfHeaders(), data.size() );

        for (size_t index = 0; index < GetSectionCount(); ++index)
        {
            const PeLib::PELIB_SECTION_HEADER * section = GetLoader().getSectionHeader( index );

            if (section != nullptr && section->SizeOfRawData != 0)
            {
                uint64_t end = static_cast<uint64_t>( section->PointerToRawData ) + section->SizeOfRawData;
                m_PeSize = std::max( m_PeSize, std::min< uint64_t >( end, data.size() ) );
            }
        }

        ReadDirectories();
    }

    PeImage::~PeImage() = default;

    void PeImage::AddWarning(
        const std::string & warning
    )
    {
        if (std::find( m_Warnings.begin(), m_Warnings.end(), warning ) == m_Warnings.end())
        {
            m_Warnings.push_back( warning );
        }
    }

    void PeImage::ReadDirectories()
    {
        // Each directory parser runs on its own, a broken directory must not hide the others.
        auto run = [this] ( const char * name, auto && body )
        {
            try
            {
                body();
            }
            catch (const std::exception & error)
            {
                AddWarning( std::string( name ) + " directory: " + error.what() );
            }
        };

        PeLib::PeFileT & file = *m_File;
        run( "Export", [&]
        {
            file.readExportDirectory();
        } );
        run( "Import", [&]
        {
            file.readImportDirectory();

            if (file.impDir().loaderError() != PeLib::LDR_ERROR_NONE)
            {
                AddWarning( std::string( "Import directory: " ) + PeLib::getLoaderErrorString( file.impDir().loaderError(), true ) );
            }
        } );
        run( "Resource", [&]
        {
            file.readResourceDirectory();

            if (file.resDir().loaderError() != PeLib::LDR_ERROR_NONE)
            {
                AddWarning( std::string( "Resource directory: " ) + PeLib::getLoaderErrorString( file.resDir().loaderError(), true ) );
            }
        } );
        run( "Security", [&]
        {
            file.readSecurityDirectory();
        } );
        run( "Relocation", [&]
        {
            file.readRelocationsDirectory();
        } );
        run( "Debug", [&]
        {
            file.readDebugDirectory();
        } );
        run( "TLS", [&]
        {
            file.readTlsDirectory();
        } );
        run( "Load config", [&]
        {
            file.readLoadConfigDirectory();
        } );
        run( "Bound import", [&]
        {
            file.readBoundImportDirectory();
        } );
        run( "IAT", [&]
        {
            file.readIatDirectory();
        } );
        run( "Delay import", [&]
        {
            file.readDelayImportDirectory();
        } );
        run( "COM descriptor", [&]
        {
            file.readComHeaderDirectory();
        } );

        uint32_t lfanew = m_NtOffset;

        if (lfanew > 0x40)
        {
            run( "Rich header", [&]
            {
                file.readRichHeader( 0x40, (lfanew - 0x40) & ~3u, true );
            } );
        }
    }

    PeLib::PeFileT & PeImage::GetFile() const
    {
        return *m_File;
    }

    const PeLib::ImageLoader & PeImage::GetLoader() const
    {
        return m_File->imageLoader();
    }

    uint32_t PeImage::GetDataDirectoryOffset() const
    {
        return m_OptionalOffset + (m_Is64 ? 112 : 96);
    }

    uint32_t PeImage::GetNumberOfRvaAndSizes() const
    {
        return Read32( m_OptionalOffset + (m_Is64 ? 108 : 92) );
    }

    uint16_t PeImage::GetMachine() const
    {
        return Read16( m_NtOffset + 4 );
    }

    uint64_t PeImage::GetImageBase() const
    {
        return m_Is64 ? Read64( m_OptionalOffset + 24 ) : Read32( m_OptionalOffset + 28 );
    }

    uint32_t PeImage::GetEntryPoint() const
    {
        return Read32( m_OptionalOffset + 16 );
    }

    uint32_t PeImage::GetSectionAlignment() const
    {
        return Read32( m_OptionalOffset + 32 );
    }

    uint32_t PeImage::GetFileAlignment() const
    {
        return Read32( m_OptionalOffset + 36 );
    }

    uint32_t PeImage::GetSizeOfImage() const
    {
        return Read32( m_OptionalOffset + 56 );
    }

    uint32_t PeImage::GetSizeOfHeaders() const
    {
        return Read32( m_OptionalOffset + 60 );
    }

    uint64_t PeImage::GetOverlayOffset() const
    {
        return m_PeSize;
    }

    uint64_t PeImage::GetOverlaySize() const
    {
        if (!m_IsPe || m_PeSize >= m_Data.size())
        {
            return 0;
        }

        // The certificate table is appended after the image and addressed by file offset, it is not overlay.
        DataDirectory security = GetDirectory( DIR_SECURITY );
        uint64_t end = m_Data.size();

        if (security.m_Rva != 0 && security.m_Rva >= m_PeSize && static_cast<uint64_t>(security.m_Rva) + security.m_Size >= end)
        {
            end = security.m_Rva;
        }

        return end > m_PeSize ? end - m_PeSize : 0;
    }

    DataDirectory PeImage::GetDirectory(
        uint32_t index
    ) const
    {
        DataDirectory directory;

        if (!m_IsPe || index >= DIR_COUNT)
        {
            return directory;
        }

        // Entries past 'NumberOfRvaAndSizes' are ignored by the loader, so they are reported empty too.
        uint32_t count = std::min< uint32_t >( GetNumberOfRvaAndSizes(), DIR_COUNT );
        uint64_t offset = GetDataDirectoryOffset() + index * 8ull;

        if (index >= count || offset + 8 > m_OptionalOffset + m_OptionalSize)
        {
            return directory;
        }

        directory.m_Rva = Read32( offset );
        directory.m_Size = Read32( offset + 4 );
        return directory;
    }

    bool PeImage::HasDirectory(
        uint32_t index
    ) const
    {
        DataDirectory directory = GetDirectory( index );
        return directory.m_Rva != 0 && directory.m_Size != 0;
    }

    bool PeImage::IsDotNet() const
    {
        return HasDirectory( DIR_COM_DESCRIPTOR );
    }

    size_t PeImage::GetSectionCount() const
    {
        return m_IsPe ? GetLoader().getNumberOfSections() : 0;
    }

    uint64_t PeImage::GetSectionHeaderOffset(
        size_t index
    ) const
    {
        return m_SectionTableOffset + index * 40ull;
    }

    std::string PeImage::GetSectionName(
        size_t index
    ) const
    {
        const PeLib::PELIB_SECTION_HEADER * section = m_IsPe ? GetLoader().getSectionHeader( index ) : nullptr;

        if (section == nullptr)
        {
            return std::string();
        }

        // PeLib resolves long names from the COFF string table ['/4' style], fall back to the raw 8 bytes.
        std::string name = section->getName();

        if (name.empty())
        {
            name.assign( reinterpret_cast<const char *>(section->Name), strnlen( reinterpret_cast<const char *>(section->Name), 8 ) );
        }

        return Utf8FromLatin1( name );
    }

    size_t PeImage::FindSectionByRva(
        uint32_t rva
    ) const
    {
        if (!m_IsPe)
        {
            return NO_SECTION;
        }

        // Exact virtual range first. PeLib aligns VirtualSize up to SectionAlignment, which is what the loader maps.
        for (size_t index = 0; index < GetSectionCount(); ++index)
        {
            const PeLib::PELIB_SECTION_HEADER * section = GetLoader().getSectionHeader( index );
            uint32_t size = std::max( section->VirtualSize, section->SizeOfRawData );

            if (rva >= section->VirtualAddress && rva < static_cast<uint64_t>( section->VirtualAddress ) + size)
            {
                return index;
            }
        }

        size_t index = GetLoader().getSectionIndexByRva( rva );
        return index == SIZE_MAX ? NO_SECTION : index;
    }

    size_t PeImage::FindSectionByOffset(
        uint64_t offset
    ) const
    {
        for (size_t index = 0; index < GetSectionCount(); ++index)
        {
            const PeLib::PELIB_SECTION_HEADER * section = GetLoader().getSectionHeader( index );

            if (section->SizeOfRawData != 0 && offset >= section->PointerToRawData &&
                offset < static_cast<uint64_t>( section->PointerToRawData ) + section->SizeOfRawData)
            {
                return index;
            }
        }

        return NO_SECTION;
    }

    std::string PeImage::SectionNameOfRva(
        uint32_t rva
    ) const
    {
        if (rva == 0)
        {
            return std::string();
        }

        size_t index = FindSectionByRva( rva );

        if (index != NO_SECTION)
        {
            return GetSectionName( index );
        }

        return rva < GetSizeOfHeaders() ? "Headers" : std::string();
    }

    uint64_t PeImage::RvaToOffset(
        uint32_t rva
    ) const
    {
        if (!m_IsPe)
        {
            return NO_OFFSET;
        }

        // The loader's own rules, PeLib's mapping refuses sections with VirtualSize 0 [Watcom and other old linkers write
        // 0, the loader then uses 'SizeOfRawData']. The header area maps 1:1, a section maps 'PointerToRawData' rounded down
        // to 512 bytes, the part past its raw data is zero filled memory without file bytes.
        uint64_t section_alignment = std::max< uint32_t >( GetSectionAlignment(), 1 );
        uint64_t best_start = 0;
        uint64_t offset = NO_OFFSET;
        bool found = false;

        for (size_t index = 0; index < GetSectionCount(); ++index)
        {
            const PeLib::PELIB_SECTION_HEADER * section = GetLoader().getSectionHeader( index );

            if (section == nullptr)
            {
                continue;
            }

            uint64_t virtual_size = section->VirtualSize != 0 ? section->VirtualSize : section->SizeOfRawData;
            uint64_t span = (std::max< uint64_t >( virtual_size, 1 ) + section_alignment - 1) / section_alignment * section_alignment;
            uint64_t start = section->VirtualAddress;

            // The section with the highest start that contains rva, like the loader that maps them in order.
            if (rva < start || rva >= start + span || (found && start < best_start))
            {
                continue;
            }

            found = true;
            best_start = start;
            uint64_t delta = rva - start;
            uint64_t raw_start = GetFileAlignment() >= 0x200 ? section->PointerToRawData & ~0x1FFull : section->PointerToRawData;
            uint64_t raw_size = std::min< uint64_t >( section->SizeOfRawData, virtual_size );
            offset = section->PointerToRawData != 0 && delta < raw_size ? raw_start + delta : NO_OFFSET;
        }

        if (!found && rva < GetSizeOfHeaders())
        {
            offset = rva;
        }

        return offset != NO_OFFSET && offset < m_Data.size() ? offset : NO_OFFSET;
    }

    bool PeImage::OffsetToRva(
        uint64_t offset,
        uint32_t & rva
    ) const
    {
        if (!m_IsPe || offset >= m_Data.size())
        {
            return false;
        }

        if (offset < GetSizeOfHeaders())
        {
            rva = static_cast<uint32_t>( offset );
            return true;
        }

        size_t index = FindSectionByOffset( offset );

        if (index == NO_SECTION)
        {
            return false;
        }

        const PeLib::PELIB_SECTION_HEADER * section = GetLoader().getSectionHeader( index );
        uint64_t raw_start = GetLoader().getRealPointerToRawData( index );
        rva = static_cast<uint32_t>(section->VirtualAddress + (offset - raw_start));
        return true;
    }

    uint64_t PeImage::VaToOffset(
        uint64_t va
    ) const
    {
        uint64_t base = GetImageBase();

        if (va < base || va - base > 0xFFFFFFFFull)
        {
            return NO_OFFSET;
        }

        return RvaToOffset( static_cast<uint32_t>(va - base) );
    }

    bool PeImage::ReadAt(
        uint64_t offset,
        void * buffer,
        size_t size
    ) const
    {
        if (offset > m_Data.size() || size > m_Data.size() - offset)
        {
            std::memset( buffer, 0, size );
            return false;
        }

        std::memcpy( buffer, m_Data.data() + offset, size );
        return true;
    }

    uint64_t PeImage::ReadUnsigned(
        uint64_t offset,
        size_t size
    ) const
    {
        uint8_t bytes[8] = {};

        if (size > 8 || !ReadAt( offset, bytes, size ))
        {
            return 0;
        }

        uint64_t value = 0;

        for (size_t index = 0; index < size; ++index)
        {
            value |= static_cast<uint64_t>( bytes[index] ) << (index * 8);
        }

        return value;
    }

    std::string PeImage::ReadString(
        uint64_t offset,
        size_t max_length
    ) const
    {
        std::string text;

        while (offset < m_Data.size() && text.size() < max_length)
        {
            char character = static_cast<char>(m_Data[offset++]);

            if (character == 0)
            {
                break;
            }

            text.push_back( character );
        }

        return text;
    }

    std::string PeImage::ReadStringAtRva(
        uint32_t rva,
        size_t max_length
    ) const
    {
        uint64_t offset = RvaToOffset( rva );
        return offset == NO_OFFSET ? std::string() : ReadString( offset, max_length );
    }

    std::span< const uint8_t > PeImage::Slice(
        uint64_t offset,
        uint64_t length
    ) const
    {
        if (offset >= m_Data.size())
        {
            return {};
        }

        length = std::min< uint64_t >( length, m_Data.size() - offset );
        return m_Data.subspan( static_cast<size_t>(offset), static_cast<size_t>(length) );
    }
}