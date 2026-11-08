#pragma once

#include "Document.h"
#include "Json.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/*
    Rich header between the DOS stub and the PE header, written by the Microsoft linker:

        'DanS' ^ key, key, key, key           Marker and three zero dwords.
        (comp.id ^ key, count ^ key) * n      comp.id = product id << 16 | build
        'Rich', key                           Plain text marker and the key.

    The key is a checksum over the DOS header and stub ['e_lfanew' excluded] and all entries, so every change of an entry
    changes the key and the whole header must be encrypted again. PeLib only reads the header, editing goes through here.
*/

namespace PeCore
{
    struct RichEntry
    {
        uint16_t m_ProductId = 0;
        uint16_t m_Build = 0;
        uint32_t m_Count = 0;
    };

    struct RichHeaderInfo
    {
        bool m_Found = false;

        // File offsets of the 'DanS' dword and of the 'Rich' dword.
        uint64_t m_DansOffset = 0;
        uint64_t m_RichOffset = 0;

        // 'e_lfanew', the header must end before it.
        uint64_t m_Limit = 0;
        uint32_t m_Key = 0;

        // The three dwords after DanS decrypt to zero.
        bool m_PaddingValid = false;
        std::vector< RichEntry > m_Entries;

        // Offset of the first byte after 'Rich' and the key.
        uint64_t GetEnd() const
        {
            return m_RichOffset + 8;
        }

        uint64_t GetEntryOffset(
            size_t index
        ) const
        {
            return m_DansOffset + 16 + index * 8ull;
        }
    };

    RichHeaderInfo ParseRichHeader(
        std::span< const uint8_t > data
    );

    // The linker checksum. DanS offset, then every byte before DanS rotated by its offset ['e_lfanew' skipped], then
    // every 'comp.id' rotated by its count.
    uint32_t ComputeRichChecksum(
        std::span< const uint8_t > data,
        uint64_t dans_offset,
        const std::vector< RichEntry > & entries
    );

    // Writes the header at info.m_DansOffset with the given entries and key, the space up to the old end [or the new
    // end, whichever is larger] is cleared. Throws when the header does not fit before 'e_lfanew'.
    void WriteRichHeader(
        std::vector< uint8_t > & data,
        const RichHeaderInfo & info,
        const std::vector< RichEntry > & entries,
        uint32_t key
    );

    // rich.set { index, field: product|build|count, value, keep_key }, rich.add, rich.remove { index },
    // rich.fix_checksum, rich.decrypt, rich.encrypt, rich.clear
    std::string ExecuteRichCommand(
        Document & document,
        const JsonValue & command
    );
}