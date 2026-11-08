#pragma once

#include "Document.h"
#include "Json.h"
#include "PeImage.h"
#include "Views.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

/*
    Resource protocol.

    Commands:

      resources.replace   Path, data [hex] or file.
      resources.add       Type, name, language, data or file.
      resources.remove    Path.
      resources.import    File [another PE], replace [bool]    Copies its resources into this file.
      resources.set_version  argument "<index>:<target>", value   edits a version information resource in place
                             target "fixed.FileVersion", "fixed.ProductVersion" (text "1.2.3.4"),
                             "fixed.FileFlagsMask", "fixed.FileFlags", "fixed.FileOS", "fixed.FileType",
                             "fixed.FileSubtype" (number), "string.<Key>" [text, every string table with the key].
*/

namespace PeCore
{
    struct ResourceItem
    {
        // Type, name and language. A string name is kept in 'm_*Name' with 'm_*IsString' set.
        uint32_t m_TypeId = 0;
        std::string m_TypeName;
        bool m_TypeIsString = false;
        uint32_t m_NameId = 0;
        std::string m_Name;
        bool m_NameIsString = false;
        uint32_t m_Language = 0;
        uint32_t m_DataRva = 0;
        uint32_t m_Size = 0;
        uint32_t m_CodePage = 0;
        uint64_t m_EntryOffset = NO_OFFSET;
        uint64_t m_DataOffset = NO_OFFSET;
        std::vector< uint8_t > m_Data;
    };

    std::vector< ResourceItem > EnumerateResources(
        const PeImage & image,
        bool with_data
    );

    std::string ResourcePath(
        const ResourceItem & item
    );

    // "image", "icon", "cursor", "text", "version", "strings" or "hex".
    std::string ResourcePreviewKind(
        const ResourceItem & item
    );

    struct VersionInfo
    {
        // Index of the 'RT_VERSION' item in 'EnumerateResources' [ReadVersionInfo], the target of 'resources.set_version'.
        size_t m_Resource = SIZE_MAX;
        bool m_HasFixed = false;
        std::string m_FileVersion;
        std::string m_ProductVersion;
        std::vector< std::pair< std::string, std::string > > m_Fixed;
        std::vector< std::pair< std::string, std::string > > m_Strings;
    };

    VersionInfo DecodeVersionInfo(
        std::span< const uint8_t > data
    );

    VersionInfo ReadVersionInfo(
        const PeImage & image
    );

    // Changes one value of a 'VS_VERSIONINFO' block [target as in 'resources.set_version'] and returns the new block.
    // The block is parsed into its tree and written back, so strings can grow or shrink. 'text' is the value for
    // versions and strings, number the value of the fixed flags fields.
    std::vector< uint8_t > EditVersionInfo(
        std::span< const uint8_t > data,
        const std::string & target,
        const std::string & text,
        uint64_t number
    );

    // A complete file for preview or 'Save'. .ico for icon groups, .cur for cursor groups, .bmp for bitmaps,
    // the raw bytes for everything else. extension receives the suggested file extension. With preview set cursors
    // come back as .ico because the Windows imaging codecs do not read .cur files.
    std::vector< uint8_t > ExportResource(
        const PeImage & image,
        const std::string & path,
        bool preview,
        std::string & extension
    );

    // The 'Resource Directory' node and the 'Resource Editor' tool show the same view under their own title.
    std::string BuildResourceView(
        const ViewContext & context,
        const std::string & title = "Resource Directory"
    );

    std::string BuildResourceItemView(
        const ViewContext & context,
        const std::string & path
    );

    // Serializes a resource directory for the given items at 'rsrc_rva'.
    std::vector< uint8_t > BuildResourceSection(
        const std::vector< ResourceItem > & items,
        uint32_t rsrc_rva
    );

    // Writes the items as the new resource directory of data [in place when it fits, else a new section].
    std::string WriteResources(
        std::vector< uint8_t > & data,
        const std::vector< ResourceItem > & items
    );

    std::string ExecuteResourceCommand(
        Document & document,
        const JsonValue & command
    );
}