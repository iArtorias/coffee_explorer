#pragma once

#include "Document.h"
#include "Json.h"
#include "PeImage.h"
#include "Symbols.h"

#include <set>
#include <string>
#include <vector>

/*
    View protocol [UTF-8 JSON, part of the C ABI, see 'Api.h', 'PcQuery']

    Tree:  { "nodes": [ Node ] }
           Node = { "id", "name", "kind": "file|struct|folder|tool|group", "children": [ Node ] }

    View:  {
             "node", "title",
             "layout": "fields|list|hex|overview|resources|empty",
             "fields":  [ Field ],                         Fields layout, or the header part of a list.
             "columns": [ { "name", "tip" } ],             List layout.
             "rows":    [ { "cells": [ Cell ], "detail": node id, "navigate": node id, "offset", "size" } ],
             "detail_layout": "list|fields|hex|none",      What the detail node of a row shows.
             "info":  "text",                              Info text above a hex region.
             "hex":   { "offset", "length", "title" },     Hex region of the view.
             "commands": [ { "id", "label", "scope": "view|row", "enabled", "reason" } ],
             "choice_sets": { "id": { "flags", "items": [ { "value", "name", "mask" } ] } },
             "warnings": [ "text" ]
           }

    Field = { "name", "offset", "size", "type", "value", "meaning", "editor": "hex|text|flags|enum|none",
              "choices", "tip", "command", "argument", "pending" }
            "command" Fields have no offset, an edit runs the command like a command cell [version information].
            "pending" fields [views requested with "defer": true] show a placeholder, the value is the member of that
            name in the "overview_hashes" answer.

    Cell  = { "text", "offset", "size", "editor", "choices", "meaning", "command", "argument" }
            "command" cells have no offset, an edit runs the command with "argument" and "value" [see CommandCell].

    Offsets are file offsets. Values of hex editors are little endian integers of 'size' bytes, text editors are
    fixed size zero padded 8 bit strings. Missing offset means read only.
*/

namespace PeCore
{
    struct ViewContext
    {
        const Document & m_Document;
        const PeImage & m_Image;

        // PDB symbols loaded for the document, null without.
        std::shared_ptr< const SymbolTable > m_Symbols = nullptr;

        // Views leave out what reads the whole file [hashes, Authenticode digest] unless it is cached already, they mark
        // those fields 'pending' and the UI fills them from the 'overview_hashes' query on a worker thread.
        bool m_DeferSlow = false;
    };

    enum class Editor
    {
        EDITOR_NONE = 0,
        EDITOR_HEX,
        EDITOR_TEXT,
        EDITOR_FLAGS,
        EDITOR_ENUM
    };

    class ViewBuilder
    {
    private:

        JsonWriter m_Writer;
        std::set< std::string > m_UsedChoices;
        bool m_InArray = false;
        bool m_InRow = false;

        // Highlight of the next field ["entry" for the entry point], written once and cleared
        std::string m_NextHighlight;

        void WriteHighlight();

        void WriteEditor(
            Editor editor,
            const std::string & choices
        );

    public:

        ViewBuilder(
            const std::string & node,
            const std::string & title,
            const std::string & layout
        );

        // Marks the next field so the UI can make it stand out ["entry": the entry point].
        void HighlightNext(
            const std::string & kind
        );

        // Fields, key is 'fields' or a second property list such as 'version' in the overview.
        void BeginFields(
            const std::string & key = "fields"
        );

        void Field(
            const std::string & name,
            uint64_t offset,
            size_t size,
            uint64_t value,
            const std::string & meaning = std::string(),
            Editor editor = Editor::EDITOR_HEX,
            const std::string & choices = std::string(),
            const std::string & tip = std::string()
        );

        // Reads the little endian value from the image.
        void FieldAt(
            const PeImage & image,
            const std::string & name,
            uint64_t offset,
            size_t size,
            const std::string & meaning = std::string(),
            Editor editor = Editor::EDITOR_HEX,
            const std::string & choices = std::string()
        );

        // Enum or flags field, the meaning is derived from the choice set.
        void ChoiceFieldAt(
            const PeImage & image,
            const std::string & name,
            uint64_t offset,
            size_t size,
            const std::string & choices
        );

        void TextField(
            const std::string & name,
            uint64_t offset,
            size_t size,
            const std::string & text,
            Editor editor = Editor::EDITOR_NONE,
            const std::string & meaning = std::string()
        );

        // Property and value row without an offset.
        void InfoField(
            const std::string & name,
            const std::string & value
        );

        // Property and value row whose value is computed later ['Computing...'], the UI fills it from the answer member key
        // of the 'overview_hashes' query.
        void PendingField(
            const std::string & name,
            const std::string & key
        );

        // Property and value row whose edit runs the core command with { "argument", "value" }, like CommandCell.
        void CommandField(
            const std::string & name,
            const std::string & value,
            const std::string & command,
            const std::string & argument,
            Editor editor = Editor::EDITOR_TEXT,
            size_t size = 0
        );

        void EndFields();

        // List.
        void Columns(
            const std::vector< std::pair< std::string, std::string > > & columns
        );

        void BeginRows();

        void BeginRow(
            const std::string & detail = std::string(),
            uint64_t offset = NO_OFFSET,
            uint64_t size = 0,
            const std::string & navigate = std::string()
        );

        void Cell(
            const std::string & text
        );

        void ValueCell(
            uint64_t offset,
            size_t size,
            uint64_t value,
            Editor editor = Editor::EDITOR_HEX,
            const std::string & choices = std::string(),
            const std::string & meaning = std::string()
        );

        void ValueCellAt(
            const PeImage & image,
            uint64_t offset,
            size_t size,
            Editor editor = Editor::EDITOR_HEX,
            const std::string & choices = std::string()
        );

        void TextCell(
            uint64_t offset,
            size_t size,
            const std::string & text,
            Editor editor = Editor::EDITOR_TEXT
        );

        // Editable cell without a file offset: an edit runs the core command with { "argument", "value" }, for values that
        // are not stored as plain bytes [the encrypted Rich header entries].
        void CommandCell(
            const std::string & text,
            size_t size,
            const std::string & command,
            const std::string & argument,
            Editor editor = Editor::EDITOR_HEX
        );

        void EndRow();

        void EndRows();

        void DetailLayout(
            const std::string & layout
        );

        void Info(
            const std::string & text
        );

        // Any extra string property of the view ['preview', 'text' etc].
        void Property(
            const std::string & key,
            const std::string & value
        );

        void HexRegion(
            uint64_t offset,
            uint64_t length,
            const std::string & title
        );

        void BeginCommands();

        void Command(
            const std::string & id,
            const std::string & label,
            const std::string & scope,
            bool enabled = true,
            const std::string & reason = std::string()
        );

        void EndCommands();

        void Warnings(
            const std::vector< std::string > & warnings
        );

        // Direct access for view specific members [resource lists].
        JsonWriter & GetWriter()
        {
            return m_Writer;
        }

        std::string Finish();
    };

    std::string BuildTree(
        const ViewContext & context
    );

    std::string BuildView(
        const ViewContext & context,
        const std::string & node
    );

    // Header views.
    std::string BuildOverviewView( const ViewContext & context );

    // Hashes and the Authenticode digest of the overview, computed in parallel and cached per revision.
    void ComputeOverviewHashes( const Document & document, const PeImage & image );

    // { "md5", "sha1", "sha256", "blake3", "signature", "matches" }, the pending fields of a deferred overview.
    std::string BuildOverviewHashes( const Document & document, const PeImage & image );
    std::string BuildDosHeaderView( const ViewContext & context );
    std::string BuildRichHeaderView( const ViewContext & context );
    std::string BuildNtHeadersView( const ViewContext & context );
    std::string BuildFileHeaderView( const ViewContext & context );
    std::string BuildOptionalHeaderView( const ViewContext & context );
    std::string BuildDataDirectoriesView( const ViewContext & context );
    std::string BuildSectionsView( const ViewContext & context );
    std::string BuildSectionDetailView( const ViewContext & context, size_t index );

    // Directory views.
    std::string BuildExportView( const ViewContext & context );
    std::string BuildImportView( const ViewContext & context );
    std::string BuildImportDetailView( const ViewContext & context, size_t index );
    std::string BuildExceptionView( const ViewContext & context );
    std::string BuildExceptionDetailView( const ViewContext & context, size_t index );
    std::string BuildSecurityView( const ViewContext & context );
    std::string BuildSecurityDetailView( const ViewContext & context, size_t index );
    std::string BuildRelocationView( const ViewContext & context );
    std::string BuildRelocationDetailView( const ViewContext & context, size_t index );
    std::string BuildDebugView( const ViewContext & context );
    std::string BuildDebugDetailView( const ViewContext & context, size_t index );
    std::string BuildTlsView( const ViewContext & context );
    std::string BuildLoadConfigView( const ViewContext & context );
    std::string BuildBoundImportView( const ViewContext & context );
    std::string BuildIatView( const ViewContext & context );
    std::string BuildDelayImportView( const ViewContext & context );
    std::string BuildDelayImportDetailView( const ViewContext & context, size_t index );
    std::string BuildOverlayView( const ViewContext & context );

    // Small helpers shared by the view files.
    std::string SectionFlagsSummary(
        uint32_t characteristics
    );

    double ComputeEntropy(
        std::span< const uint8_t > data
    );
}