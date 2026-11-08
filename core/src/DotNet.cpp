#include "pecore/DotNet.h"

#include "pecore/Meta.h"

#include <algorithm>
#include <cstdio>

namespace PeCore
{
    namespace
    {
        constexpr int TABLE_COUNT = 0x2D;

        const char * TABLE_NAMES[TABLE_COUNT] =
        {
            "Module", "TypeRef", "TypeDef", "FieldPtr", "Field", "MethodPtr", "MethodDef", "ParamPtr", "Param", "InterfaceImpl",
            "MemberRef", "Constant", "CustomAttribute", "FieldMarshal", "DeclSecurity", "ClassLayout", "FieldLayout", "StandAloneSig",
            "EventMap", "EventPtr", "Event", "PropertyMap", "PropertyPtr", "Property", "MethodSemantics", "MethodImpl", "ModuleRef",
            "TypeSpec", "ImplMap", "FieldRVA", "EncLog", "EncMap", "Assembly", "AssemblyProcessor", "AssemblyOS", "AssemblyRef",
            "AssemblyRefProcessor", "AssemblyRefOS", "File", "ExportedType", "ManifestResource", "NestedClass", "GenericParam",
            "MethodSpec", "GenericParamConstraint"
        };

        enum CodedIndex
        {
            CODED_TYPE_DEF_OR_REF = 0,
            CODED_HAS_CONSTANT,
            CODED_HAS_CUSTOM_ATTRIBUTE,
            CODED_HAS_FIELD_MARSHAL,
            CODED_HAS_DECL_SECURITY,
            CODED_MEMBER_REF_PARENT,
            CODED_HAS_SEMANTICS,
            CODED_METHOD_DEF_OR_REF,
            CODED_MEMBER_FORWARDED,
            CODED_IMPLEMENTATION,
            CODED_CUSTOM_ATTRIBUTE_TYPE,
            CODED_RESOLUTION_SCOPE,
            CODED_TYPE_OR_METHOD_DEF,
            CODED_COUNT
        };

        struct CodedIndexInfo
        {
            int m_Bits;
            std::vector< int > m_Tables;
        };

        const CodedIndexInfo & GetCodedIndex(
            int id
        )
        {
            static const CodedIndexInfo INFOS[CODED_COUNT] =
            {
                { 2, { 0x02, 0x01, 0x1B } },
                { 2, { 0x04, 0x08, 0x17 } },
                { 5, { 0x06, 0x04, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x00, 0x0E, 0x17, 0x14, 0x11, 0x1A, 0x1B, 0x20, 0x23, 0x26, 0x27, 0x28, 0x2A, 0x2C, 0x2B } },
                { 1, { 0x04, 0x08 } },
                { 2, { 0x02, 0x06, 0x20 } },
                { 3, { 0x02, 0x01, 0x1A, 0x06, 0x1B } },
                { 1, { 0x14, 0x17 } },
                { 1, { 0x06, 0x0A } },
                { 1, { 0x04, 0x06 } },
                { 2, { 0x26, 0x23, 0x27 } },
                { 3, { -1, -1, 0x06, 0x0A, -1 } },
                { 2, { 0x00, 0x1A, 0x23, 0x01 } },
                { 1, { 0x02, 0x06 } }
            };

            return INFOS[id];
        }

        // Column description. Kind 'c' constant of 'm_Size' bytes, 's' string, 'g' guid, 'b' blob, 't' table, 'x' coded.
        struct ColumnSpec
        {
            const char * m_Name;
            char m_Kind;
            int m_Value;
            const char * m_Choices;
        };

        const std::vector< ColumnSpec > & GetSchema(
            int table
        )
        {
            static const std::vector< ColumnSpec > SCHEMAS[TABLE_COUNT] =
            {
                { { "Generation", 'c', 2, "" }, { "Name", 's', 0, "" }, { "Mvid", 'g', 0, "" }, { "EncId", 'g', 0, "" }, { "EncBaseId", 'g', 0, "" } },
                { { "ResolutionScope", 'x', CODED_RESOLUTION_SCOPE, "" }, { "TypeName", 's', 0, "" }, { "TypeNamespace", 's', 0, "" } },
                { { "Flags", 'c', 4, "type_attributes" }, { "TypeName", 's', 0, "" }, { "TypeNamespace", 's', 0, "" }, { "Extends", 'x', CODED_TYPE_DEF_OR_REF, "" },
                { "FieldList", 't', 0x04, "" }, { "MethodList", 't', 0x06, "" } },
                { { "Field", 't', 0x04, "" } },
                { { "Flags", 'c', 2, "field_attributes" }, { "Name", 's', 0, "" }, { "Signature", 'b', 0, "" } },
                { { "Method", 't', 0x06, "" } },
                { { "RVA", 'c', 4, "" }, { "ImplFlags", 'c', 2, "" }, { "Flags", 'c', 2, "method_attributes" }, { "Name", 's', 0, "" }, { "Signature", 'b', 0, "" },
                { "ParamList", 't', 0x08, "" } },
                { { "Param", 't', 0x08, "" } },
                { { "Flags", 'c', 2, "" }, { "Sequence", 'c', 2, "" }, { "Name", 's', 0, "" } },
                { { "Class", 't', 0x02, "" }, { "Interface", 'x', CODED_TYPE_DEF_OR_REF, "" } },
                { { "Class", 'x', CODED_MEMBER_REF_PARENT, "" }, { "Name", 's', 0, "" }, { "Signature", 'b', 0, "" } },
                { { "Type", 'c', 1, "" }, { "Padding", 'c', 1, "" }, { "Parent", 'x', CODED_HAS_CONSTANT, "" }, { "Value", 'b', 0, "" } },
                { { "Parent", 'x', CODED_HAS_CUSTOM_ATTRIBUTE, "" }, { "Type", 'x', CODED_CUSTOM_ATTRIBUTE_TYPE, "" }, { "Value", 'b', 0, "" } },
                { { "Parent", 'x', CODED_HAS_FIELD_MARSHAL, "" }, { "NativeType", 'b', 0, "" } },
                { { "Action", 'c', 2, "" }, { "Parent", 'x', CODED_HAS_DECL_SECURITY, "" }, { "PermissionSet", 'b', 0, "" } },
                { { "PackingSize", 'c', 2, "" }, { "ClassSize", 'c', 4, "" }, { "Parent", 't', 0x02, "" } },
                { { "Offset", 'c', 4, "" }, { "Field", 't', 0x04, "" } },
                { { "Signature", 'b', 0, "" } },
                { { "Parent", 't', 0x02, "" }, { "EventList", 't', 0x14, "" } },
                { { "Event", 't', 0x14, "" } },
                { { "EventFlags", 'c', 2, "" }, { "Name", 's', 0, "" }, { "EventType", 'x', CODED_TYPE_DEF_OR_REF, "" } },
                { { "Parent", 't', 0x02, "" }, { "PropertyList", 't', 0x17, "" } },
                { { "Property", 't', 0x17, "" } },
                { { "Flags", 'c', 2, "" }, { "Name", 's', 0, "" }, { "Type", 'b', 0, "" } },
                { { "Semantics", 'c', 2, "" }, { "Method", 't', 0x06, "" }, { "Association", 'x', CODED_HAS_SEMANTICS, "" } },
                { { "Class", 't', 0x02, "" }, { "MethodBody", 'x', CODED_METHOD_DEF_OR_REF, "" }, { "MethodDeclaration", 'x', CODED_METHOD_DEF_OR_REF, "" } },
                { { "Name", 's', 0, "" } },
                { { "Signature", 'b', 0, "" } },
                { { "MappingFlags", 'c', 2, "" }, { "MemberForwarded", 'x', CODED_MEMBER_FORWARDED, "" }, { "ImportName", 's', 0, "" }, { "ImportScope", 't', 0x1A, "" } },
                { { "RVA", 'c', 4, "" }, { "Field", 't', 0x04, "" } },
                { { "Token", 'c', 4, "" }, { "FuncCode", 'c', 4, "" } },
                { { "Token", 'c', 4, "" } },
                { { "HashAlgId", 'c', 4, "" }, { "MajorVersion", 'c', 2, "" }, { "MinorVersion", 'c', 2, "" }, { "BuildNumber", 'c', 2, "" }, { "RevisionNumber", 'c', 2, "" },
                { "Flags", 'c', 4, "" }, { "PublicKey", 'b', 0, "" }, { "Name", 's', 0, "" }, { "Culture", 's', 0, "" } },
                { { "Processor", 'c', 4, "" } },
                { { "OSPlatformID", 'c', 4, "" }, { "OSMajorVersion", 'c', 4, "" }, { "OSMinorVersion", 'c', 4, "" } },
                { { "MajorVersion", 'c', 2, "" }, { "MinorVersion", 'c', 2, "" }, { "BuildNumber", 'c', 2, "" }, { "RevisionNumber", 'c', 2, "" }, { "Flags", 'c', 4, "" },
                { "PublicKeyOrToken", 'b', 0, "" }, { "Name", 's', 0, "" }, { "Culture", 's', 0, "" }, { "HashValue", 'b', 0, "" } },
                { { "Processor", 'c', 4, "" }, { "AssemblyRef", 't', 0x23, "" } },
                { { "OSPlatformId", 'c', 4, "" }, { "OSMajorVersion", 'c', 4, "" }, { "OSMinorVersion", 'c', 4, "" }, { "AssemblyRef", 't', 0x23, "" } },
                { { "Flags", 'c', 4, "" }, { "Name", 's', 0, "" }, { "HashValue", 'b', 0, "" } },
                { { "Flags", 'c', 4, "type_attributes" }, { "TypeDefId", 'c', 4, "" }, { "TypeName", 's', 0, "" }, { "TypeNamespace", 's', 0, "" },
                { "Implementation", 'x', CODED_IMPLEMENTATION, "" } },
                { { "Offset", 'c', 4, "" }, { "Flags", 'c', 4, "" }, { "Name", 's', 0, "" }, { "Implementation", 'x', CODED_IMPLEMENTATION, "" } },
                { { "NestedClass", 't', 0x02, "" }, { "EnclosingClass", 't', 0x02, "" } },
                { { "Number", 'c', 2, "" }, { "Flags", 'c', 2, "" }, { "Owner", 'x', CODED_TYPE_OR_METHOD_DEF, "" }, { "Name", 's', 0, "" } },
                { { "Method", 'x', CODED_METHOD_DEF_OR_REF, "" }, { "Instantiation", 'b', 0, "" } },
                { { "Owner", 't', 0x2A, "" }, { "Constraint", 'x', CODED_TYPE_DEF_OR_REF, "" } }
            };

            return SCHEMAS[table];
        }

        // ECMA-335 compressed unsigned integer [blob and #US lengths].
        uint32_t ReadCompressed(
            const PeImage & image,
            uint64_t offset,
            uint32_t & size
        )
        {
            uint8_t first = image.Read8( offset );

            if ((first & 0x80) == 0)
            {
                size = 1;
                return first;
            }

            if ((first & 0xC0) == 0x80)
            {
                size = 2;
                return ((first & 0x3F) << 8) | image.Read8( offset + 1 );
            }

            size = 4;
            return ((first & 0x1F) << 24) | (image.Read8( offset + 1 ) << 16) | (image.Read8( offset + 2 ) << 8) | image.Read8( offset + 3 );
        }

        std::string FormatGuid(
            const uint8_t * data
        )
        {
            char buffer[48];
            uint32_t data1 = data[0] | data[1] << 8 | data[2] << 16 | static_cast<uint32_t>(data[3]) << 24;
            std::snprintf( buffer, sizeof( buffer ), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", data1, data[4] | data[5] << 8,
                data[6] | data[7] << 8, data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15] );
            return buffer;
        }
    }

    Metadata::Metadata(
        const PeImage & image
    )
        : m_Image( image )
    {
        if (!image.IsDotNet())
        {
            m_Error = "Not a .NET image";
            return;
        }

        m_Cor20Offset = image.RvaToOffset( image.GetDirectory( DIR_COM_DESCRIPTOR ).m_Rva );

        if (m_Cor20Offset == NO_OFFSET)
        {
            m_Error = "The COR20 header is not backed by file data";
            return;
        }

        uint32_t metadata_rva = image.Read32( m_Cor20Offset + 8 );
        m_RootOffset = image.RvaToOffset( metadata_rva );

        if (m_RootOffset == NO_OFFSET || image.Read32( m_RootOffset ) != 0x424A5342)
        {
            m_Error = "The metadata root has no BSJB signature";
            m_RootOffset = NO_OFFSET;
            return;
        }

        uint32_t length = image.Read32( m_RootOffset + 12 );
        m_Version = image.ReadString( m_RootOffset + 16, std::min< uint32_t >( length, 255 ) );
        uint64_t cursor = m_RootOffset + 16 + ((length + 3) & ~3u);
        uint16_t streams = image.Read16( cursor + 2 );
        cursor += 4;

        for (uint16_t index = 0; index < streams && index < 16; ++index)
        {
            MetadataStream stream;
            stream.m_HeaderOffset = cursor;
            stream.m_Offset = image.Read32( cursor );
            stream.m_Size = image.Read32( cursor + 4 );
            stream.m_Name = image.ReadString( cursor + 8, 32 );
            stream.m_FileOffset = m_RootOffset + stream.m_Offset < image.GetFileSize() ? m_RootOffset + stream.m_Offset : NO_OFFSET;
            cursor += 8 + ((stream.m_Name.size() + 4) & ~3ull);
            m_Streams.push_back( stream );
        }

        m_Valid = true;
        ParseTables();
    }

    const MetadataStream * Metadata::FindStream(
        const std::string & name
    ) const
    {
        for (const MetadataStream & stream : m_Streams)
        {
            if (stream.m_Name == name)
            {
                return &stream;
            }
        }

        return nullptr;
    }

    const MetadataTable * Metadata::FindTable(
        int id
    ) const
    {
        for (const MetadataTable & table : m_Tables)
        {
            if (table.m_Id == id)
            {
                return &table;
            }
        }

        return nullptr;
    }

    void Metadata::ParseTables()
    {
        const MetadataStream * stream = FindStream( "#~" );

        if (stream == nullptr)
        {
            stream = FindStream( "#-" );
        }

        if (stream == nullptr || stream->m_FileOffset == NO_OFFSET)
        {
            return;
        }

        m_TablesOffset = stream->m_FileOffset;
        m_HeapSizes = m_Image.Read8( m_TablesOffset + 6 );
        m_Valid64 = m_Image.Read64( m_TablesOffset + 8 );
        uint64_t cursor = m_TablesOffset + 24;
        uint32_t rows[64] = {};

        for (int bit = 0; bit < 64; ++bit)
        {
            if (m_Valid64 & (1ull << bit))
            {
                rows[bit] = m_Image.Read32( cursor );
                cursor += 4;
            }
        }

        // Uncompressed '#-' streams may carry 4 extra bytes.
        if (m_HeapSizes & 0x40)
        {
            cursor += 4;
        }

        uint32_t string_size = (m_HeapSizes & 1) ? 4 : 2;
        uint32_t guid_size = (m_HeapSizes & 2) ? 4 : 2;
        uint32_t blob_size = (m_HeapSizes & 4) ? 4 : 2;

        auto table_size = [&] ( int table ) -> uint32_t
        {
            return table >= 0 && rows[table] >= 0x10000 ? 4 : 2;
        };
        auto coded_size = [&] ( int id ) -> uint32_t
        {
            const CodedIndexInfo & info = GetCodedIndex( id );
            uint32_t largest = 0;

            for (int table : info.m_Tables)
            {
                largest = table >= 0 ? std::max( largest, rows[table] ) : largest;
            }

            return largest < (1u << (16 - info.m_Bits)) ? 2 : 4;
        };

        for (int id = 0; id < 64; ++id)
        {
            if ((m_Valid64 & (1ull << id)) == 0)
            {
                continue;
            }

            if (id >= TABLE_COUNT)
            {
                // Without the schema the following tables cannot be located.
                m_Error = "Table " + Hex( id, 2 ) + " is not known, the tables after it are not shown";
                break;
            }

            MetadataTable table;
            table.m_Id = id;
            table.m_Name = TABLE_NAMES[id];
            table.m_Rows = rows[id];
            table.m_FileOffset = cursor;

            for (const ColumnSpec & spec : GetSchema( id ))
            {
                MetadataColumn column;
                column.m_Name = spec.m_Name;
                column.m_Kind = spec.m_Kind;
                column.m_Target = spec.m_Value;
                column.m_Choices = spec.m_Choices;

                switch (spec.m_Kind)
                {
                    case 'c':
                        column.m_Size = static_cast<uint32_t>(spec.m_Value);
                        break;
                    case 's':
                        column.m_Size = string_size;
                        break;
                    case 'g':
                        column.m_Size = guid_size;
                        break;
                    case 'b':
                        column.m_Size = blob_size;
                        break;
                    case 't':
                        column.m_Size = table_size( spec.m_Value );
                        break;
                    default:
                        column.m_Size = coded_size( spec.m_Value );
                        break;
                }

                table.m_RowSize += column.m_Size;
                table.m_Columns.push_back( column );
            }

            cursor += static_cast<uint64_t>(table.m_RowSize) * table.m_Rows;
            m_Tables.push_back( std::move( table ) );
        }
    }

    std::string Metadata::GetString(
        uint32_t index
    ) const
    {
        const MetadataStream * stream = FindStream( "#Strings" );

        if (stream == nullptr || stream->m_FileOffset == NO_OFFSET || index >= stream->m_Size)
        {
            return std::string();
        }

        return m_Image.ReadString( stream->m_FileOffset + index, std::min< uint32_t >( stream->m_Size - index, 1024 ) );
    }

    std::string Metadata::GetUserString(
        uint32_t index,
        uint32_t & length
    ) const
    {
        const MetadataStream * stream = FindStream( "#US" );
        length = 0;

        if (stream == nullptr || stream->m_FileOffset == NO_OFFSET || index >= stream->m_Size)
        {
            return std::string();
        }

        uint32_t prefix = 0;
        length = ReadCompressed( m_Image, stream->m_FileOffset + index, prefix );
        auto bytes = m_Image.Slice( stream->m_FileOffset + index + prefix, length > 0 ? length - 1 : 0 );
        length += prefix;
        return Utf8FromUtf16Bytes( bytes.data(), bytes.size(), false );
    }

    std::string Metadata::GetGuid(
        uint32_t index
    ) const
    {
        const MetadataStream * stream = FindStream( "#GUID" );

        if (index == 0 || stream == nullptr || stream->m_FileOffset == NO_OFFSET || index * 16ull > stream->m_Size)
        {
            return std::string();
        }

        uint8_t guid[16];
        m_Image.ReadAt( stream->m_FileOffset + (index - 1) * 16ull, guid, 16 );
        return FormatGuid( guid );
    }

    std::string Metadata::DescribeIndex(
        const MetadataColumn & column,
        uint32_t value
    ) const
    {
        switch (column.m_Kind)
        {
            case 's':
                return GetString( value );
            case 'g':
                return GetGuid( value );
            case 'b':
                return value != 0 ? "#Blob+" + Hex( value, 1 ) : std::string();
            case 't':
            {
                const MetadataTable * table = FindTable( column.m_Target );
                std::string name = TABLE_NAMES[column.m_Target];

                if (value == 0)
                {
                    return std::string();
                }

                std::string row = table != nullptr && value <= table->m_Rows ? DescribeRow( *table, value ) : std::string();
                return name + "[" + std::to_string( value ) + "]" + (row.empty() ? "" : " " + row);
            }
            case 'x':
            {
                const CodedIndexInfo & info = GetCodedIndex( column.m_Target );
                uint32_t tag = value & ((1u << info.m_Bits) - 1);
                uint32_t row = value >> info.m_Bits;

                if (tag >= info.m_Tables.size() || info.m_Tables[tag] < 0)
                {
                    return value != 0 ? "Invalid coded index" : std::string();
                }

                if (row == 0)
                {
                    return std::string();
                }

                int target = info.m_Tables[tag];
                const MetadataTable * table = FindTable( target );
                std::string detail = table != nullptr && row <= table->m_Rows ? DescribeRow( *table, row ) : std::string();
                return std::string( TABLE_NAMES[target] ) + "[" + std::to_string( row ) + "]" + (detail.empty() ? "" : " " + detail);
            }
            default:
                return std::string();
        }
    }

    std::string Metadata::DescribeRow(
        const MetadataTable & table,
        uint32_t row
    ) const
    {
        if (row == 0 || row > table.m_Rows)
        {
            return std::string();
        }

        uint64_t offset = table.m_FileOffset + static_cast<uint64_t>(row - 1) * table.m_RowSize;
        std::string name;
        std::string space;

        for (const MetadataColumn & column : table.m_Columns)
        {
            if (column.m_Kind == 's' && (column.m_Name == "Name" || column.m_Name == "TypeName"))
            {
                name = GetString( static_cast<uint32_t>(m_Image.ReadUnsigned( offset, column.m_Size )) );
            }
            else if (column.m_Kind == 's' && column.m_Name == "TypeNamespace")
            {
                space = GetString( static_cast<uint32_t>(m_Image.ReadUnsigned( offset, column.m_Size )) );
            }

            offset += column.m_Size;
        }

        return space.empty() ? name : space + "." + name;
    }

    void WriteDotNetTree(
        JsonWriter & writer,
        const PeImage & image
    )
    {
        Metadata metadata( image );
        writer.BeginObject();
        writer.Member( "id", "dotnet" );
        writer.Member( "name", ".NET Directory" );
        writer.Member( "kind", "folder" );
        writer.Key( "children" ).BeginArray();

        auto add = [&] ( const char * id, const std::string & name, const char * kind )
        {
            writer.BeginObject();
            writer.Member( "id", id );
            writer.Member( "name", name );
            writer.Member( "kind", kind );
            writer.EndObject();
        };

        if (metadata.IsValid())
        {
            add( "dotnet.metadata", "Metadata Header", "struct" );
            add( "dotnet.streams", "Metadata Streams", "list" );

            if (metadata.GetTablesOffset() != NO_OFFSET)
            {
                add( "dotnet.tables", "Tables", "list" );
            }

            for (const MetadataStream & stream : metadata.GetStreams())
            {
                if (stream.m_Name == "#Strings") add( "dotnet.strings", "#Strings", "list" );
                if (stream.m_Name == "#US") add( "dotnet.us", "#US", "list" );
                if (stream.m_Name == "#GUID") add( "dotnet.guid", "#GUID", "list" );
                if (stream.m_Name == "#Blob") add( "dotnet.blob", "#Blob", "list" );
            }
        }

        writer.EndArray();
        writer.EndObject();
    }

    std::string BuildDotNetView(
        const ViewContext & context,
        const std::string & node
    )
    {
        const PeImage & image = context.m_Image;
        Metadata metadata( image );

        if (node == "dotnet")
        {
            ViewBuilder view( "dotnet", ".NET Directory", "fields" );
            uint64_t offset = metadata.GetCor20Offset();
            Require( offset != NO_OFFSET, ErrorCode::ERR_NOT_FOUND, metadata.GetError() );
            view.BeginFields();
            view.FieldAt( image, "cb", offset, 4 );
            view.FieldAt( image, "MajorRuntimeVersion", offset + 4, 2 );
            view.FieldAt( image, "MinorRuntimeVersion", offset + 6, 2 );
            view.FieldAt( image, "MetaData RVA", offset + 8, 4, image.SectionNameOfRva( image.Read32( offset + 8 ) ) );
            view.FieldAt( image, "MetaData Size", offset + 12, 4 );
            view.ChoiceFieldAt( image, "Flags", offset + 16, 4, "cor20_flags" );
            uint32_t token = image.Read32( offset + 20 );
            std::string entry;

            if (image.Read32( offset + 16 ) & 0x10)
            {
                entry = "Native entry point RVA";
            }
            else if ((token >> 24) < TABLE_COUNT && token != 0)
            {
                const MetadataTable * table = metadata.FindTable( static_cast<int>(token >> 24) );
                entry = std::string( TABLE_NAMES[token >> 24] ) + "[" + std::to_string( token & 0xFFFFFF ) + "]";

                if (table != nullptr)
                {
                    entry += " " + metadata.DescribeRow( *table, token & 0xFFFFFF );
                }
            }

            view.FieldAt( image, "EntryPointToken", offset + 20, 4, entry );
            static const char * DIRECTORIES[] = { "Resources", "StrongNameSignature", "CodeManagerTable", "VTableFixups", "ExportAddressTableJumps", "ManagedNativeHeader" };

            for (int index = 0; index < 6; ++index)
            {
                uint64_t cell = offset + 24 + index * 8ull;
                view.FieldAt( image, std::string( DIRECTORIES[index] ) + " RVA", cell, 4, image.SectionNameOfRva( image.Read32( cell ) ) );
                view.FieldAt( image, std::string( DIRECTORIES[index] ) + " Size", cell + 4, 4 );
            }

            view.EndFields();

            if (!metadata.GetError().empty())
            {
                view.Warnings( { metadata.GetError() } );
            }

            return view.Finish();
        }

        Require( metadata.IsValid(), ErrorCode::ERR_NOT_FOUND, metadata.GetError() );
        uint64_t root = metadata.GetRootOffset();

        if (node == "dotnet.metadata")
        {
            ViewBuilder view( node, "Metadata Header", "fields" );
            view.BeginFields();
            view.FieldAt( image, "Signature", root, 4, "BSJB" );
            view.FieldAt( image, "MajorVersion", root + 4, 2 );
            view.FieldAt( image, "MinorVersion", root + 6, 2 );
            view.FieldAt( image, "Reserved", root + 8, 4 );
            view.FieldAt( image, "VersionLength", root + 12, 4 );
            view.TextField( "Version", root + 16, image.Read32( root + 12 ), metadata.GetVersion() );
            uint64_t after = root + 16 + ((image.Read32( root + 12 ) + 3) & ~3u);
            view.FieldAt( image, "Flags", after, 2 );
            view.FieldAt( image, "NumberOfStreams", after + 2, 2, std::to_string( image.Read16( after + 2 ) ) );
            view.EndFields();
            return view.Finish();
        }

        if (node == "dotnet.streams")
        {
            static const std::pair< const char *, const char * > NODES[] =
            {
                { "#~", "dotnet.tables" }, { "#-", "dotnet.tables" }, { "#Strings", "dotnet.strings" }, { "#US", "dotnet.us" }, { "#GUID", "dotnet.guid" }, { "#Blob", "dotnet.blob" }
            };

            ViewBuilder view( node, "Metadata Streams", "list" );
            view.Columns( { { "Offset", "From the metadata root, Dword" }, { "Size", "Dword" }, { "Name", "" }, { "File offset", "" } } );
            view.BeginRows();

            for (const MetadataStream & stream : metadata.GetStreams())
            {
                std::string navigate;

                for (const auto & pair : NODES)
                {
                    navigate = stream.m_Name == pair.first ? pair.second : navigate;
                }

                view.BeginRow( std::string(), stream.m_HeaderOffset, 8, navigate );
                view.ValueCellAt( image, stream.m_HeaderOffset, 4 );
                view.ValueCellAt( image, stream.m_HeaderOffset + 4, 4 );
                view.Cell( stream.m_Name );
                view.Cell( stream.m_FileOffset != NO_OFFSET ? Hex( stream.m_FileOffset ) : std::string() );
                view.EndRow();
            }

            view.EndRows();
            view.DetailLayout( "none" );
            return view.Finish();
        }

        if (node == "dotnet.tables")
        {
            uint64_t offset = metadata.GetTablesOffset();
            Require( offset != NO_OFFSET, ErrorCode::ERR_NOT_FOUND, "No #~ stream" );
            ViewBuilder view( node, "Tables", "list" );
            view.BeginFields();
            view.FieldAt( image, "Reserved", offset, 4 );
            view.FieldAt( image, "MajorVersion", offset + 4, 1 );
            view.FieldAt( image, "MinorVersion", offset + 5, 1 );
            uint8_t heaps = image.Read8( offset + 6 );
            view.FieldAt( image, "HeapOffsetSizes", offset + 6, 1, std::string( heaps & 1 ? "#Strings 4 " : "#Strings 2 " ) + (heaps & 2 ? "#GUID 4 " : "#GUID 2 ") + (heaps & 4 ? "#Blob 4" : "#Blob 2") );
            view.FieldAt( image, "Reserved", offset + 7, 1 );
            view.FieldAt( image, "MaskValid", offset + 8, 8, std::to_string( metadata.GetTables().size() ) + " tables" );
            view.FieldAt( image, "MaskSorted", offset + 16, 8 );
            view.EndFields();
            view.Columns( { { "Table", "" }, { "Rows", "" }, { "Row size", "Bytes" }, { "Offset", "File offset" } } );
            view.BeginRows();

            for (const MetadataTable & table : metadata.GetTables())
            {
                view.BeginRow( "dotnet.table/" + std::to_string( table.m_Id ), table.m_FileOffset, static_cast<uint64_t>(table.m_RowSize) * table.m_Rows );
                view.Cell( Hex( table.m_Id, 2 ) + " " + table.m_Name );
                view.Cell( std::to_string( table.m_Rows ) );
                view.Cell( std::to_string( table.m_RowSize ) );
                view.Cell( Hex( table.m_FileOffset ) );
                view.EndRow();
            }

            view.EndRows();
            view.DetailLayout( "list" );

            if (!metadata.GetError().empty())
            {
                view.Warnings( { metadata.GetError() } );
            }

            return view.Finish();
        }

        if (node.rfind( "dotnet.table/", 0 ) == 0)
        {
            uint64_t id = 0;
            Require( ParseNumber( "#" + node.substr( 13 ), id ), ErrorCode::ERR_NOT_FOUND, "Unknown table" );
            const MetadataTable * table = metadata.FindTable( static_cast<int>(id) );
            Require( table != nullptr, ErrorCode::ERR_NOT_FOUND, "The table is not present" );
            ViewBuilder view( node, table->m_Name + " (" + std::to_string( table->m_Rows ) + (table->m_Rows == 1 ? " row)" : " rows)"), "list" );
            std::vector< std::pair< std::string, std::string > > columns = { { "Row", "Token = table << 24 | row" } };

            for (const MetadataColumn & column : table->m_Columns)
            {
                std::string tip = SizeName( column.m_Size );

                switch (column.m_Kind)
                {
                    case 's':
                        tip += ", #Strings index";
                        break;
                    case 'g':
                        tip += ", #GUID index";
                        break;
                    case 'b':
                        tip += ", #Blob index";
                        break;
                    case 't':
                        tip += ", " + std::string( TABLE_NAMES[column.m_Target] ) + " index";
                        break;
                    case 'x':
                        tip += ", coded index";
                        break;
                    default:
                        break;
                }

                columns.emplace_back( column.m_Name, tip );
            }

            view.Columns( columns );
            view.BeginRows();
            uint32_t rows = std::min< uint32_t >( table->m_Rows, 200000 );

            for (uint32_t row = 1; row <= rows; ++row)
            {
                uint64_t offset = table->m_FileOffset + static_cast<uint64_t>(row - 1) * table->m_RowSize;

                if (offset + table->m_RowSize > image.GetFileSize())
                {
                    break;
                }

                view.BeginRow( std::string(), offset, table->m_RowSize );
                view.Cell( std::to_string( row ) + " (" + Hex( (static_cast<uint32_t>(table->m_Id) << 24) | row ) + ")" );
                uint64_t cell = offset;

                for (const MetadataColumn & column : table->m_Columns)
                {
                    uint64_t value = image.ReadUnsigned( cell, column.m_Size );
                    std::string meaning = column.m_Kind == 'c' ? (column.m_Choices.empty() ? std::string() : DescribeFlags( column.m_Choices, value ))
                        : metadata.DescribeIndex( column, static_cast<uint32_t>(value) );

                    if (column.m_Name == "RVA" && value != 0)
                    {
                        meaning = image.SectionNameOfRva( static_cast<uint32_t>(value) );
                    }

                    view.ValueCell( cell, column.m_Size, value, column.m_Choices.empty() ? Editor::EDITOR_HEX : Editor::EDITOR_FLAGS, column.m_Choices, meaning );
                    cell += column.m_Size;
                }

                view.EndRow();
            }

            view.EndRows();
            return view.Finish();
        }

        if (node == "dotnet.strings" || node == "dotnet.blob" || node == "dotnet.us" || node == "dotnet.guid")
        {
            const char * names[] = { "#Strings", "#Blob", "#US", "#GUID" };
            const char * name = node == "dotnet.strings" ? names[0] : node == "dotnet.blob" ? names[1] : node == "dotnet.us" ? names[2] : names[3];
            const MetadataStream * stream = metadata.FindStream( name );
            Require( stream != nullptr && stream->m_FileOffset != NO_OFFSET, ErrorCode::ERR_NOT_FOUND, std::string( "No " ) + name + " stream" );
            ViewBuilder view( node, name, "list" );
            uint64_t base = stream->m_FileOffset;
            uint64_t end = std::min< uint64_t >( base + stream->m_Size, image.GetFileSize() );
            size_t limit = 200000;

            if (node == "dotnet.strings")
            {
                view.Columns( { { "Index", "Offset in the heap" }, { "String", "" } } );
                view.BeginRows();
                uint64_t cursor = base + 1;

                for (size_t count = 0; cursor < end && count < limit; ++count)
                {
                    std::string text = image.ReadString( cursor, static_cast<size_t>( end - cursor ) );
                    view.BeginRow( std::string(), cursor, text.size() + 1 );
                    view.Cell( Hex( cursor - base, 1 ) );
                    view.Cell( text );
                    view.EndRow();
                    cursor += text.size() + 1;
                }

                view.EndRows();
            }
            else if (node == "dotnet.us")
            {
                view.Columns( { { "Index", "Offset in the heap" }, { "Length", "Bytes" }, { "String", "" } } );
                view.BeginRows();
                uint64_t cursor = base + 1;

                for (size_t count = 0; cursor < end && count < limit; ++count)
                {
                    uint32_t length = 0;
                    std::string text = metadata.GetUserString( static_cast<uint32_t>( cursor - base ), length );

                    if (length == 0)
                    {
                        break;
                    }

                    view.BeginRow( std::string(), cursor, length );
                    view.Cell( Hex( cursor - base, 1 ) );
                    view.Cell( std::to_string( length ) );
                    view.Cell( text );
                    view.EndRow();
                    cursor += length;
                }

                view.EndRows();
            }
            else if (node == "dotnet.guid")
            {
                view.Columns( { { "Index", "1 based" }, { "GUID", "" } } );
                view.BeginRows();

                for (uint32_t index = 1; index * 16ull <= stream->m_Size && index < limit; ++index)
                {
                    view.BeginRow( std::string(), base + (index - 1) * 16ull, 16 );
                    view.Cell( std::to_string( index ) );
                    view.Cell( metadata.GetGuid( index ) );
                    view.EndRow();
                }

                view.EndRows();
            }
            else
            {
                view.Columns( { { "Index", "Offset in the heap" }, { "Size", "Bytes" }, { "Data", "First bytes" } } );
                view.BeginRows();
                uint64_t cursor = base + 1;

                for (size_t count = 0; cursor < end && count < limit; ++count)
                {
                    uint32_t prefix = 0;
                    uint32_t length = ReadCompressed( image, cursor, prefix );
                    std::string preview;

                    for (uint32_t byte = 0; byte < length && byte < 24; ++byte)
                    {
                        preview += Hex( image.Read8( cursor + prefix + byte ), 2 ) + " ";
                    }

                    view.BeginRow( std::string(), cursor, prefix + length );
                    view.Cell( Hex( cursor - base, 1 ) );
                    view.Cell( std::to_string( length ) );
                    view.Cell( preview + (length > 24 ? "..." : "") );
                    view.EndRow();
                    cursor += prefix + length;
                }

                view.EndRows();
            }

            view.DetailLayout( "none" );
            view.HexRegion( base, end - base, name );
            return view.Finish();
        }

        throw Error( ErrorCode::ERR_NOT_FOUND, "Unknown view " + node );
    }
}