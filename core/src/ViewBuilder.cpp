#include "pecore/Views.h"

#include "pecore/DotNet.h"
#include "pecore/Meta.h"
#include "pecore/Resources.h"
#include "pecore/RichHeader.h"

#include "retdec/pelib/PeFile.h"

#include <cmath>

namespace PeCore
{
    namespace
    {
        const char * EditorName(
            Editor editor
        )
        {
            switch (editor)
            {
                case Editor::EDITOR_HEX:
                    return "hex";
                case Editor::EDITOR_TEXT:
                    return "text";
                case Editor::EDITOR_FLAGS:
                    return "flags";
                case Editor::EDITOR_ENUM:
                    return "enum";
                default:
                    return "none";
            }
        }

        // 'imports/12' -> base 'imports', index 12.
        bool SplitNode(
            const std::string & node,
            std::string & base,
            size_t & index
        )
        {
            size_t slash = node.rfind( '/' );

            if (slash == std::string::npos)
            {
                return false;
            }

            base = node.substr( 0, slash );
            uint64_t value = 0;

            if (!ParseNumber( "#" + node.substr( slash + 1 ), value ))
            {
                return false;
            }

            index = static_cast<size_t>(value);
            return true;
        }

        void AddNode(
            JsonWriter & writer,
            const std::string & id,
            const std::string & name,
            const std::string & kind
        )
        {
            writer.BeginObject();
            writer.Member( "id", id );
            writer.Member( "name", name );
            writer.Member( "kind", kind );
            writer.EndObject();
        }

        void BeginNode(
            JsonWriter & writer,
            const std::string & id,
            const std::string & name,
            const std::string & kind
        )
        {
            writer.BeginObject();
            writer.Member( "id", id );
            writer.Member( "name", name );
            writer.Member( "kind", kind );
            writer.Key( "children" ).BeginArray();
        }

        void EndNode(
            JsonWriter & writer
        )
        {
            writer.EndArray();
            writer.EndObject();
        }
    }

    ViewBuilder::ViewBuilder(
        const std::string & node,
        const std::string & title,
        const std::string & layout
    )
    {
        m_Writer.BeginObject();
        m_Writer.Member( "node", node );
        m_Writer.Member( "title", title );
        m_Writer.Member( "layout", layout );
    }

    void ViewBuilder::WriteEditor(
        Editor editor,
        const std::string & choices
    )
    {
        m_Writer.Member( "editor", EditorName( editor ) );

        if (!choices.empty())
        {
            m_Writer.Member( "choices", choices );
            m_UsedChoices.insert( choices );
        }
    }

    void ViewBuilder::BeginFields(
        const std::string & key
    )
    {
        m_Writer.Key( key ).BeginArray();
        m_InArray = true;
    }

    void ViewBuilder::HighlightNext(
        const std::string & kind
    )
    {
        m_NextHighlight = kind;
    }

    void ViewBuilder::WriteHighlight()
    {
        if (!m_NextHighlight.empty())
        {
            m_Writer.Member( "highlight", m_NextHighlight );
            m_NextHighlight.clear();
        }
    }

    void ViewBuilder::Field(
        const std::string & name,
        uint64_t offset,
        size_t size,
        uint64_t value,
        const std::string & meaning,
        Editor editor,
        const std::string & choices,
        const std::string & tip
    )
    {
        m_Writer.BeginObject();
        m_Writer.Member( "name", name );

        if (offset != NO_OFFSET)
        {
            m_Writer.MemberUnsigned( "offset", offset );
        }
        else
        {
            editor = Editor::EDITOR_NONE;
        }

        m_Writer.MemberUnsigned( "size", size );
        m_Writer.Member( "type", SizeName( size ) );
        m_Writer.Member( "value", HexOfSize( value, size ) );

        if (!meaning.empty())
        {
            m_Writer.Member( "meaning", meaning );
        }

        WriteEditor( editor, choices );

        if (!tip.empty())
        {
            m_Writer.Member( "tip", tip );
        }

        WriteHighlight();
        m_Writer.EndObject();
    }

    void ViewBuilder::FieldAt(
        const PeImage & image,
        const std::string & name,
        uint64_t offset,
        size_t size,
        const std::string & meaning,
        Editor editor,
        const std::string & choices
    )
    {
        Field( name, offset, size, image.ReadUnsigned( offset, size ), meaning, editor, choices );
    }

    void ViewBuilder::ChoiceFieldAt(
        const PeImage & image,
        const std::string & name,
        uint64_t offset,
        size_t size,
        const std::string & choices
    )
    {
        const ChoiceSet * set = FindChoiceSet( choices );
        uint64_t value = image.ReadUnsigned( offset, size );
        bool flags = set != nullptr && set->m_Flags;
        std::string meaning = flags ? DescribeFlags( choices, value ) : DescribeEnum( choices, value );
        Field( name, offset, size, value, meaning, flags ? Editor::EDITOR_FLAGS : Editor::EDITOR_ENUM, choices );
    }

    void ViewBuilder::TextField(
        const std::string & name,
        uint64_t offset,
        size_t size,
        const std::string & text,
        Editor editor,
        const std::string & meaning
    )
    {
        m_Writer.BeginObject();
        m_Writer.Member( "name", name );

        if (offset != NO_OFFSET)
        {
            m_Writer.MemberUnsigned( "offset", offset );
        }
        else
        {
            editor = Editor::EDITOR_NONE;
        }

        m_Writer.MemberUnsigned( "size", size );
        m_Writer.Member( "type", size != 0 ? "Byte[" + std::to_string( size ) + "]" : std::string( "String" ) );
        m_Writer.Member( "value", text );

        if (!meaning.empty())
        {
            m_Writer.Member( "meaning", meaning );
        }

        WriteEditor( editor, std::string() );
        WriteHighlight();
        m_Writer.EndObject();
    }

    void ViewBuilder::InfoField(
        const std::string & name,
        const std::string & value
    )
    {
        m_Writer.BeginObject();
        m_Writer.Member( "name", name );
        m_Writer.Member( "value", value );
        m_Writer.Member( "editor", "none" );
        WriteHighlight();
        m_Writer.EndObject();
    }

    void ViewBuilder::PendingField(
        const std::string & name,
        const std::string & key
    )
    {
        m_Writer.BeginObject();
        m_Writer.Member( "name", name );
        m_Writer.Member( "value", "Computing..." );
        m_Writer.Member( "editor", "none" );
        m_Writer.Member( "pending", key );
        WriteHighlight();
        m_Writer.EndObject();
    }

    void ViewBuilder::CommandField(
        const std::string & name,
        const std::string & value,
        const std::string & command,
        const std::string & argument,
        Editor editor,
        size_t size
    )
    {
        m_Writer.BeginObject();
        m_Writer.Member( "name", name );
        m_Writer.Member( "value", value );
        m_Writer.MemberUnsigned( "size", size );
        WriteEditor( editor, std::string() );
        m_Writer.Member( "command", command );
        m_Writer.Member( "argument", argument );
        WriteHighlight();
        m_Writer.EndObject();
    }

    void ViewBuilder::EndFields()
    {
        m_Writer.EndArray();
        m_InArray = false;
    }

    void ViewBuilder::Columns(
        const std::vector< std::pair< std::string, std::string > > & columns
    )
    {
        m_Writer.Key( "columns" ).BeginArray();

        for (const auto & column : columns)
        {
            m_Writer.BeginObject();
            m_Writer.Member( "name", column.first );

            if (!column.second.empty())
            {
                m_Writer.Member( "tip", column.second );
            }

            m_Writer.EndObject();
        }

        m_Writer.EndArray();
    }

    void ViewBuilder::BeginRows()
    {
        m_Writer.Key( "rows" ).BeginArray();
        m_InArray = true;
    }

    void ViewBuilder::BeginRow(
        const std::string & detail,
        uint64_t offset,
        uint64_t size,
        const std::string & navigate
    )
    {
        m_Writer.BeginObject();

        if (!detail.empty())
        {
            m_Writer.Member( "detail", detail );
        }

        if (!navigate.empty())
        {
            m_Writer.Member( "navigate", navigate );
        }

        if (offset != NO_OFFSET)
        {
            m_Writer.MemberUnsigned( "offset", offset );
            m_Writer.MemberUnsigned( "size", size );
        }

        m_Writer.Key( "cells" ).BeginArray();
        m_InRow = true;
    }

    void ViewBuilder::Cell(
        const std::string & text
    )
    {
        m_Writer.BeginObject();
        m_Writer.Member( "text", text );
        m_Writer.EndObject();
    }

    void ViewBuilder::ValueCell(
        uint64_t offset,
        size_t size,
        uint64_t value,
        Editor editor,
        const std::string & choices,
        const std::string & meaning
    )
    {
        m_Writer.BeginObject();
        m_Writer.Member( "text", HexOfSize( value, size ) );

        if (offset != NO_OFFSET)
        {
            m_Writer.MemberUnsigned( "offset", offset );
            m_Writer.MemberUnsigned( "size", size );
            WriteEditor( editor, choices );
        }

        if (!meaning.empty())
        {
            m_Writer.Member( "meaning", meaning );
        }

        m_Writer.EndObject();
    }

    void ViewBuilder::ValueCellAt(
        const PeImage & image,
        uint64_t offset,
        size_t size,
        Editor editor,
        const std::string & choices
    )
    {
        uint64_t value = image.ReadUnsigned( offset, size );
        std::string meaning;

        if (!choices.empty())
        {
            const ChoiceSet * set = FindChoiceSet( choices );
            meaning = set != nullptr && set->m_Flags ? DescribeFlags( choices, value ) : DescribeEnum( choices, value );
        }

        ValueCell( offset, size, value, editor, choices, meaning );
    }

    void ViewBuilder::TextCell(
        uint64_t offset,
        size_t size,
        const std::string & text,
        Editor editor
    )
    {
        m_Writer.BeginObject();
        m_Writer.Member( "text", text );

        if (offset != NO_OFFSET)
        {
            m_Writer.MemberUnsigned( "offset", offset );
            m_Writer.MemberUnsigned( "size", size );
            WriteEditor( editor, std::string() );
        }

        m_Writer.EndObject();
    }

    void ViewBuilder::CommandCell(
        const std::string & text,
        size_t size,
        const std::string & command,
        const std::string & argument,
        Editor editor
    )
    {
        m_Writer.BeginObject();
        m_Writer.Member( "text", text );
        m_Writer.MemberUnsigned( "size", size );
        WriteEditor( editor, std::string() );
        m_Writer.Member( "command", command );
        m_Writer.Member( "argument", argument );
        m_Writer.EndObject();
    }

    void ViewBuilder::EndRow()
    {
        m_Writer.EndArray();
        m_Writer.EndObject();
        m_InRow = false;
    }

    void ViewBuilder::EndRows()
    {
        m_Writer.EndArray();
        m_InArray = false;
    }

    void ViewBuilder::DetailLayout(
        const std::string & layout
    )
    {
        m_Writer.Member( "detail_layout", layout );
    }

    void ViewBuilder::Info(
        const std::string & text
    )
    {
        m_Writer.Member( "info", text );
    }

    void ViewBuilder::Property(
        const std::string & key,
        const std::string & value
    )
    {
        m_Writer.Member( key, value );
    }

    void ViewBuilder::HexRegion(
        uint64_t offset,
        uint64_t length,
        const std::string & title
    )
    {
        m_Writer.Key( "hex" ).BeginObject();
        m_Writer.MemberUnsigned( "offset", offset );
        m_Writer.MemberUnsigned( "length", length );
        m_Writer.Member( "title", title );
        m_Writer.EndObject();
    }

    void ViewBuilder::BeginCommands()
    {
        m_Writer.Key( "commands" ).BeginArray();
    }

    void ViewBuilder::Command(
        const std::string & id,
        const std::string & label,
        const std::string & scope,
        bool enabled,
        const std::string & reason
    )
    {
        m_Writer.BeginObject();
        m_Writer.Member( "id", id );
        m_Writer.Member( "label", label );
        m_Writer.Member( "scope", scope );
        m_Writer.MemberBool( "enabled", enabled );

        if (!reason.empty())
        {
            m_Writer.Member( "reason", reason );
        }

        m_Writer.EndObject();
    }

    void ViewBuilder::EndCommands()
    {
        m_Writer.EndArray();
    }

    void ViewBuilder::Warnings(
        const std::vector< std::string > & warnings
    )
    {
        if (warnings.empty())
        {
            return;
        }

        m_Writer.Key( "warnings" ).BeginArray();

        for (const std::string & warning : warnings)
        {
            m_Writer.String( warning );
        }

        m_Writer.EndArray();
    }

    std::string ViewBuilder::Finish()
    {
        if (!m_UsedChoices.empty())
        {
            m_Writer.Key( "choice_sets" ).BeginObject();

            for (const std::string & id : m_UsedChoices)
            {
                const ChoiceSet * set = FindChoiceSet( id );

                if (set != nullptr)
                {
                    m_Writer.Key( id );
                    WriteChoiceSet( m_Writer, *set );
                }
            }

            m_Writer.EndObject();
        }

        m_Writer.EndObject();
        return m_Writer.TakeText();
    }

    std::string SectionFlagsSummary(
        uint32_t characteristics
    )
    {
        std::string text;
        text += (characteristics & 0x40000000) ? 'R' : '-';
        text += (characteristics & 0x80000000) ? 'W' : '-';
        text += (characteristics & 0x20000000) ? 'X' : '-';
        return text;
    }

    double ComputeEntropy(
        std::span< const uint8_t > data
    )
    {
        if (data.empty())
        {
            return 0.0;
        }

        uint64_t counts[256] = {};

        for (uint8_t byte : data)
        {
            ++counts[byte];
        }

        double entropy = 0.0;
        double total = static_cast<double>(data.size());

        for (uint64_t count : counts)
        {
            if (count != 0)
            {
                double probability = count / total;
                entropy -= probability * std::log2( probability );
            }
        }

        return entropy;
    }

    std::string BuildTree(
        const ViewContext & context
    )
    {
        const PeImage & image = context.m_Image;
        JsonWriter writer;
        writer.BeginObject();
        writer.Key( "nodes" ).BeginArray();
        BeginNode( writer, "overview", "File: " + context.m_Document.GetName(), "file" );

        if (image.IsPe())
        {
            AddNode( writer, "dos", "DOS Header", "struct" );

            if (ParseRichHeader( image.GetData() ).m_Found)
            {
                AddNode( writer, "rich", "Rich Header", "struct" );
            }

            BeginNode( writer, "nt", "NT Headers", "struct" );
            AddNode( writer, "file", "File Header", "struct" );
            BeginNode( writer, "optional", "Optional Header", "struct" );
            AddNode( writer, "datadirs", "Data Directories", "list" );
            EndNode( writer );
            EndNode( writer );
            AddNode( writer, "sections", "Section Headers", "list" );

            struct DirectoryNode
            {
                uint32_t m_Index;
                const char * m_Id;
            };

            static const DirectoryNode DIRECTORIES[] =
            {
                { DIR_EXPORT, "exports" },
                { DIR_IMPORT, "imports" },
                { DIR_RESOURCE, "resources" },
                { DIR_EXCEPTION, "exceptions" },
                { DIR_SECURITY, "security" },
                { DIR_BASERELOC, "relocs" },
                { DIR_DEBUG, "debug" },
                { DIR_TLS, "tls" },
                { DIR_LOAD_CONFIG, "loadconfig" },
                { DIR_BOUND_IMPORT, "bound" },
                { DIR_IAT, "iat" },
                { DIR_DELAY_IMPORT, "delay" }
            };

            for (const DirectoryNode & directory : DIRECTORIES)
            {
                if (image.HasDirectory( directory.m_Index ))
                {
                    AddNode( writer, directory.m_Id, DirectoryName( directory.m_Index ), "folder" );
                }
            }

            if (image.IsDotNet())
            {
                WriteDotNetTree( writer, image );
            }

            if (image.GetOverlaySize() != 0)
            {
                AddNode( writer, "overlay", "Overlay", "folder" );
            }
        }

        BeginNode( writer, "tools", "Tools", "group" );

        if (image.IsPe())
        {
            AddNode( writer, "tool.address", "Address Converter", "tool" );
            AddNode( writer, "tool.dependencies", "Dependency Walker", "tool" );
        }

        AddNode( writer, "tool.hex", "Hex Editor", "tool" );
        AddNode( writer, "tool.die", "Detect It Easy", "tool" );

        if (image.IsPe())
        {
            AddNode( writer, "tool.importadder", "Import Adder", "tool" );
            AddNode( writer, "tool.disassembler", "Disassembler", "tool" );
            AddNode( writer, "tool.rebuilder", "Rebuilder", "tool" );
            AddNode( writer, "tool.resources", "Resource Editor", "tool" );
        }

        EndNode( writer );
        EndNode( writer );
        writer.EndArray();
        writer.EndObject();
        return writer.TakeText();
    }

    std::string BuildView(
        const ViewContext & context,
        const std::string & node
    )
    {
        if (node == "overview")
        {
            return BuildOverviewView( context );
        }

        Require( context.m_Image.IsPe() || node.rfind( "tool.", 0 ) == 0, ErrorCode::ERR_NOT_PE, "The file is not a PE image" );

        if (node == "dos")
            return BuildDosHeaderView( context );

        if (node == "rich")
            return BuildRichHeaderView( context );

        if (node == "nt")
            return BuildNtHeadersView( context );

        if (node == "file")
            return BuildFileHeaderView( context );

        if (node == "optional")
            return BuildOptionalHeaderView( context );

        if (node == "datadirs")
            return BuildDataDirectoriesView( context );

        if (node == "sections")
            return BuildSectionsView( context );

        if (node == "exports")
            return BuildExportView( context );

        if (node == "imports")
            return BuildImportView( context );

        if (node == "resources")
            return BuildResourceView( context );

        if (node == "tool.resources")
            return BuildResourceView( context, "Resource Editor" );

        if (node == "exceptions")
            return BuildExceptionView( context );

        if (node == "security")
            return BuildSecurityView( context );

        if (node == "relocs")
            return BuildRelocationView( context );

        if (node == "debug")
            return BuildDebugView( context );

        if (node == "tls")
            return BuildTlsView( context );

        if (node == "loadconfig")
            return BuildLoadConfigView( context );

        if (node == "bound")
            return BuildBoundImportView( context );

        if (node == "iat")
            return BuildIatView( context );

        if (node == "delay")
            return BuildDelayImportView( context );

        if (node == "overlay")
            return BuildOverlayView( context );

        if (node.rfind( "dotnet", 0 ) == 0)
        {
            return BuildDotNetView( context, node );
        }

        if (node.rfind( "resources/", 0 ) == 0)
        {
            return BuildResourceItemView( context, node.substr( 10 ) );
        }

        std::string base;
        size_t index = 0;

        if (SplitNode( node, base, index ))
        {
            if (base == "sections")
                return BuildSectionDetailView( context, index );

            if (base == "imports")
                return BuildImportDetailView( context, index );

            if (base == "exceptions")
                return BuildExceptionDetailView( context, index );

            if (base == "security")
                return BuildSecurityDetailView( context, index );

            if (base == "relocs")
                return BuildRelocationDetailView( context, index );

            if (base == "debug")
                return BuildDebugDetailView( context, index );

            if (base == "delay")
                return BuildDelayImportDetailView( context, index );
        }

        throw Error( ErrorCode::ERR_NOT_FOUND, "Unknown view " + node );
    }
}