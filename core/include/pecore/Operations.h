#pragma once

#include "Document.h"
#include "Json.h"
#include "PeImage.h"

#include <span>
#include <string>
#include <vector>

/*
    Commands, request { "command": id, ... }, answer { "ok": true, "message": text, "select": node }

    section.add_empty     name, size, characteristics           New section with zeroed raw data.
    section.add_file      name, data (hex string) or path, characteristics
    section.add_header    name, size, characteristics           Header only, raw size 0.
    section.delete        index                                 Header and raw data, following data moves up.
    section.delete_header index                                 Header only, data stays in the file.
    section.replace       index, path                           Replaces the raw data, the section grows if needed.
    rebuild.image_size                                          'SizeOfImage' from the sections.
    rebuild.pe_header                                           'NumberOfSections', 'SizeOfHeaders', 'SizeOfCode', ...
    rebuild               options (see Rebuild)                 Rebuilder tool.
    imports.move          index, delta (-1 / +1)                Swaps two import descriptors.
    imports.delete        index                                 Removes an import descriptor.
    imports.add           modules: [ { name, functions: [ { name | ordinal } ] } ], new_section, use_oft
    resources.*           See 'Resources.h'.
*/

namespace PeCore
{
    // Standard PE checksum [the 'CheckSumMappedFile' algorithm], the checksum field itself counts as zero.
    uint32_t ComputePeChecksum(
        std::span< const uint8_t > data,
        uint64_t checksum_offset
    );

    uint32_t AlignUp(
        uint32_t value,
        uint32_t alignment
    );

    struct NewSection
    {
        std::string m_Name;
        std::vector< uint8_t > m_Data;
        uint32_t m_VirtualSize = 0;
        uint32_t m_Characteristics = 0x40000040;
        bool m_HeaderOnly = false;
    };

    // Appends a section after the last one, makes room in the headers when needed. Returns the new section RVA.
    uint32_t AppendSection(
        std::vector< uint8_t > & data,
        const NewSection & section
    );

    // Writes a little endian value into a byte vector.
    void Poke(
        std::vector< uint8_t > & data,
        uint64_t offset,
        uint64_t value,
        size_t size
    );

    uint64_t Peek(
        std::span< const uint8_t > data,
        uint64_t offset,
        size_t size
    );

    std::string ExecuteCommand(
        Document & document,
        const JsonValue & command
    );
}