#pragma once

#include "Json.h"
#include "PeImage.h"
#include "Views.h"

#include <string>
#include <vector>

namespace PeCore
{
    struct MetadataStream
    {
        std::string m_Name;
        uint32_t m_Offset = 0;
        uint32_t m_Size = 0;

        // File offsets of the stream header entry and of the stream data.
        uint64_t m_HeaderOffset = 0;
        uint64_t m_FileOffset = NO_OFFSET;
    };

    struct MetadataColumn
    {
        std::string m_Name;

        // Byte size in this file [2 or 4 for indexes, fixed for constants].
        uint32_t m_Size = 0;

        // Kind. 'c' constant, 's' #Strings, 'g' #GUID, 'b' #Blob, 't' simple table index, 'x' coded index.
        char m_Kind = 'c';

        // Target table for 't', coded index id for 'x'.
        int m_Target = 0;
        std::string m_Choices;
    };

    struct MetadataTable
    {
        int m_Id = 0;
        std::string m_Name;
        uint32_t m_Rows = 0;
        uint32_t m_RowSize = 0;
        uint64_t m_FileOffset = NO_OFFSET;
        std::vector< MetadataColumn > m_Columns;
    };

    // Parsed CLI metadata of a .NET image [ECMA-335 II.24].
    class Metadata
    {
    private:

        const PeImage & m_Image;
        bool m_Valid = false;
        uint64_t m_Cor20Offset = NO_OFFSET;
        uint64_t m_RootOffset = NO_OFFSET;
        std::string m_Version;
        std::vector< MetadataStream > m_Streams;
        uint64_t m_TablesOffset = NO_OFFSET;
        uint8_t m_HeapSizes = 0;
        uint64_t m_Valid64 = 0;
        std::vector< MetadataTable > m_Tables;
        std::string m_Error;

        void ParseTables();

    public:

        explicit Metadata(
            const PeImage & image
        );

        bool IsValid() const
        {
            return m_Valid;
        }

        const std::string & GetError() const
        {
            return m_Error;
        }

        uint64_t GetCor20Offset() const
        {
            return m_Cor20Offset;
        }

        uint64_t GetRootOffset() const
        {
            return m_RootOffset;
        }

        const std::string & GetVersion() const
        {
            return m_Version;
        }

        const std::vector< MetadataStream > & GetStreams() const
        {
            return m_Streams;
        }

        const MetadataStream * FindStream(
            const std::string & name
        ) const;

        uint64_t GetTablesOffset() const
        {
            return m_TablesOffset;
        }

        const std::vector< MetadataTable > & GetTables() const
        {
            return m_Tables;
        }

        const MetadataTable * FindTable(
            int id
        ) const;

        std::string GetString(
            uint32_t index
        ) const;

        std::string GetUserString(
            uint32_t index,
            uint32_t & length
        ) const;

        std::string GetGuid(
            uint32_t index
        ) const;

        // Decodes a coded index into 'TableName[row]'.
        std::string DescribeIndex(
            const MetadataColumn & column,
            uint32_t value
        ) const;

        // Short display name of a row [type name, method name etc] used in the row list.
        std::string DescribeRow(
            const MetadataTable & table,
            uint32_t row
        ) const;
    };

    void WriteDotNetTree(
        JsonWriter & writer,
        const PeImage & image
    );

    std::string BuildDotNetView(
        const ViewContext & context,
        const std::string & node
    );
}