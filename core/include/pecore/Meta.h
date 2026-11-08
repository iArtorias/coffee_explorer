#pragma once

#include "Json.h"

#include <span>
#include <string>
#include <string_view>

namespace PeCore
{
    struct Choice
    {
        uint64_t m_Value;
        const char * m_Name;

        // Bits that belong to this value when it is part of a multi bit field [.NET visibility], 0 for a plain flag.
        uint64_t m_Mask = 0;
    };

    // Named value list for enum and flag editors. Ids are part of the JSON protocol ['choices' of a field].
    struct ChoiceSet
    {
        const char * m_Id;
        bool m_Flags;
        std::span< const Choice > m_Items;
    };

    const ChoiceSet * FindChoiceSet(
        std::string_view id
    );

    // Name of an enum value, empty when unknown.
    std::string DescribeEnum(
        std::string_view id,
        uint64_t value
    );

    // Comma separated names of the set bits, unknown bits are appended as hex.
    std::string DescribeFlags(
        std::string_view id,
        uint64_t value
    );

    // Writes { "flags": bool, "items": [ { "value": "hex", "name": "..." } ] }.
    void WriteChoiceSet(
        JsonWriter & writer,
        const ChoiceSet & set
    );

    std::string DirectoryName(
        uint32_t index
    );

    // "Icons", "Version Info" ... for RT_* ids.
    std::string ResourceTypeName(
        uint32_t id
    );

    // 'English (United States)' style names for the common LANGIDs, hex otherwise.
    std::string LanguageName(
        uint32_t language
    );
}