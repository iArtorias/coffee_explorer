#pragma once

#include "Types.h"

#include <cstring>
#include <memory>
#include <span>
#include <spanstream>
#include <string>
#include <vector>

namespace PeLib
{
    class PeFileT;
    class ImageLoader;
}

namespace PeCore
{
    constexpr uint64_t NO_OFFSET = ~0ull;
    constexpr size_t NO_SECTION = ~static_cast<size_t>(0);

    // Data directory indexes [IMAGE_DIRECTORY_ENTRY_*].
    enum DirectoryIndex : uint32_t
    {
        DIR_EXPORT = 0,
        DIR_IMPORT = 1,
        DIR_RESOURCE = 2,
        DIR_EXCEPTION = 3,
        DIR_SECURITY = 4,
        DIR_BASERELOC = 5,
        DIR_DEBUG = 6,
        DIR_ARCHITECTURE = 7,
        DIR_GLOBALPTR = 8,
        DIR_TLS = 9,
        DIR_LOAD_CONFIG = 10,
        DIR_BOUND_IMPORT = 11,
        DIR_IAT = 12,
        DIR_DELAY_IMPORT = 13,
        DIR_COM_DESCRIPTOR = 14,
        DIR_RESERVED = 15,
        DIR_COUNT = 16
    };

    struct DataDirectory
    {
        uint32_t m_Rva = 0;
        uint32_t m_Size = 0;
    };

    // PeLib wrapper over the bytes of a document. Parsed once per document revision, every own read is bounds checked
    // because PeLib structures may describe data outside of the file.
    class PeImage
    {
    private:

        std::span< const uint8_t > m_Data;
        std::unique_ptr< std::ispanstream > m_Stream;
        std::unique_ptr< PeLib::PeFileT > m_File;
        bool m_IsPe = false;
        bool m_Is64 = false;
        uint32_t m_NtOffset = 0;
        uint32_t m_OptionalOffset = 0;
        uint32_t m_OptionalSize = 0;
        uint32_t m_SectionTableOffset = 0;
        uint64_t m_PeSize = 0;
        std::vector< std::string > m_Warnings;

        void ReadDirectories();

        void AddWarning(
            const std::string & warning
        );

    public:

        explicit PeImage(
            std::span< const uint8_t > data
        );

        ~PeImage();

        PeImage( const PeImage & ) = delete;
        PeImage & operator=( const PeImage & ) = delete;

        bool IsPe() const
        {
            return m_IsPe;
        }

        bool Is64() const
        {
            return m_Is64;
        }

        std::span< const uint8_t > GetData() const
        {
            return m_Data;
        }

        uint64_t GetFileSize() const
        {
            return m_Data.size();
        }

        PeLib::PeFileT & GetFile() const;

        const PeLib::ImageLoader & GetLoader() const;

        const std::vector< std::string > & GetWarnings() const
        {
            return m_Warnings;
        }

        // Header layout.
        uint32_t GetNtHeadersOffset() const
        {
            return m_NtOffset;
        }

        uint32_t GetFileHeaderOffset() const
        {
            return m_NtOffset + 4;
        }

        uint32_t GetOptionalHeaderOffset() const
        {
            return m_OptionalOffset;
        }

        uint32_t GetOptionalHeaderSize() const
        {
            return m_OptionalSize;
        }

        // File offset of 'IMAGE_OPTIONAL_HEADER::DataDirectory[0]'.
        uint32_t GetDataDirectoryOffset() const;

        uint32_t GetNumberOfRvaAndSizes() const;

        uint32_t GetSectionTableOffset() const
        {
            return m_SectionTableOffset;
        }

        uint16_t GetMachine() const;

        uint64_t GetImageBase() const;

        uint32_t GetEntryPoint() const;

        uint32_t GetSectionAlignment() const;

        uint32_t GetFileAlignment() const;

        uint32_t GetSizeOfImage() const;

        uint32_t GetSizeOfHeaders() const;

        // End of the last section raw data [or 'SizeOfHeaders'], what CFF calls 'PE Size'.
        uint64_t GetPeSize() const
        {
            return m_PeSize;
        }

        // Data after the PE image that is not the certificate table.
        uint64_t GetOverlayOffset() const;

        uint64_t GetOverlaySize() const;

        DataDirectory GetDirectory(
            uint32_t index
        ) const;

        bool HasDirectory(
            uint32_t index
        ) const;

        bool IsDotNet() const;

        size_t GetSectionCount() const;

        // Section header file offset.
        uint64_t GetSectionHeaderOffset(
            size_t index
        ) const;

        std::string GetSectionName(
            size_t index
        ) const;

        size_t FindSectionByRva(
            uint32_t rva
        ) const;

        size_t FindSectionByOffset(
            uint64_t offset
        ) const;

        // Section name for the 'Meaning' or 'Section' column, empty when the RVA is in the headers or nowhere.
        std::string SectionNameOfRva(
            uint32_t rva
        ) const;

        // 'NO_OFFSET' when the RVA is not backed by file data.
        uint64_t RvaToOffset(
            uint32_t rva
        ) const;

        // RVA of a file offset, false for data outside of the headers and the section raw data.
        bool OffsetToRva(
            uint64_t offset,
            uint32_t & rva
        ) const;

        uint64_t VaToOffset(
            uint64_t va
        ) const;

        // Bounds checked reads in file offsets.
        bool ReadAt(
            uint64_t offset,
            void * buffer,
            size_t size
        ) const;

        uint64_t ReadUnsigned(
            uint64_t offset,
            size_t size
        ) const;

        uint8_t Read8(
            uint64_t offset
        ) const
        {
            return static_cast<uint8_t>(ReadUnsigned( offset, 1 ));
        }

        uint16_t Read16(
            uint64_t offset
        ) const
        {
            return static_cast<uint16_t>(ReadUnsigned( offset, 2 ));
        }

        uint32_t Read32(
            uint64_t offset
        ) const
        {
            return static_cast<uint32_t>(ReadUnsigned( offset, 4 ));
        }

        uint64_t Read64(
            uint64_t offset
        ) const
        {
            return ReadUnsigned( offset, 8 );
        }

        // Zero terminated 8 bit string, at most 'max_length' characters.
        std::string ReadString(
            uint64_t offset,
            size_t max_length
        ) const;

        std::string ReadStringAtRva(
            uint32_t rva,
            size_t max_length
        ) const;

        std::span< const uint8_t > Slice(
            uint64_t offset,
            uint64_t length
        ) const;
    };
}