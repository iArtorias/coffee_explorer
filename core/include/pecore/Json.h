#pragma once

#include "Types.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace PeCore
{
    // Streaming JSON writer. Every structured answer of the C ABI is built with it, keys and strings are UTF-8.
    class JsonWriter
    {
    private:

        std::string m_Text;

        // One entry per open container, true once the container has its first element.
        std::vector< bool > m_HasElement;
        bool m_AfterKey = false;

        void Separator();

        void AppendEscaped(
            std::string_view text
        );

    public:

        JsonWriter & BeginObject();
        JsonWriter & EndObject();
        JsonWriter & BeginArray();
        JsonWriter & EndArray();

        JsonWriter & Key(
            std::string_view name
        );

        JsonWriter & String(
            std::string_view value
        );

        JsonWriter & Number(
            int64_t value
        );

        JsonWriter & Unsigned(
            uint64_t value
        );

        JsonWriter & Double(
            double value
        );

        JsonWriter & Bool(
            bool value
        );

        JsonWriter & Null();

        // Inserts already serialized JSON as the next value.
        JsonWriter & Raw(
            std::string_view json
        );

        // Key and value shortcuts.
        JsonWriter & Member(
            std::string_view name,
            std::string_view value
        );

        JsonWriter & Member(
            std::string_view name,
            const char * value
        );

        JsonWriter & Member(
            std::string_view name,
            const std::string & value
        );

        JsonWriter & MemberNumber(
            std::string_view name,
            int64_t value
        );

        JsonWriter & MemberUnsigned(
            std::string_view name,
            uint64_t value
        );

        JsonWriter & MemberBool(
            std::string_view name,
            bool value
        );

        const std::string & GetText() const
        {
            return m_Text;
        }

        std::string TakeText()
        {
            return std::move( m_Text );
        }
    };

    enum class JsonType
    {
        JSON_NULL = 0,
        JSON_BOOL,
        JSON_NUMBER,
        JSON_STRING,
        JSON_ARRAY,
        JSON_OBJECT
    };

    // Parsed JSON value, used for the requests the UI sends to the core.
    class JsonValue
    {
    private:

        JsonType m_Type = JsonType::JSON_NULL;
        bool m_Bool = false;
        double m_Number = 0.0;

        // Numbers keep their source text so 64 bit integers survive the trip.
        std::string m_Text;
        std::vector< JsonValue > m_Items;
        std::vector< std::pair< std::string, JsonValue > > m_Members;

        friend class JsonParser;

    public:

        static JsonValue Parse(
            std::string_view text
        );

        JsonType GetType() const
        {
            return m_Type;
        }

        bool IsNull() const
        {
            return m_Type == JsonType::JSON_NULL;
        }

        bool IsObject() const
        {
            return m_Type == JsonType::JSON_OBJECT;
        }

        bool IsArray() const
        {
            return m_Type == JsonType::JSON_ARRAY;
        }

        const JsonValue * Find(
            std::string_view key
        ) const;

        const std::vector< JsonValue > & GetItems() const
        {
            return m_Items;
        }

        std::string AsString(
            const std::string & fallback = std::string()
        ) const;

        bool AsBool(
            bool fallback = false
        ) const;

        // Accepts JSON numbers and strings with hex or decimal numbers.
        uint64_t AsUnsigned(
            uint64_t fallback = 0
        ) const;

        int64_t AsInteger(
            int64_t fallback = 0
        ) const;

        std::string GetString(
            std::string_view key,
            const std::string & fallback = std::string()
        ) const;

        uint64_t GetUnsigned(
            std::string_view key,
            uint64_t fallback = 0
        ) const;

        int64_t GetInteger(
            std::string_view key,
            int64_t fallback = 0
        ) const;

        bool GetBool(
            std::string_view key,
            bool fallback = false
        ) const;

        bool Has(
            std::string_view key
        ) const
        {
            return Find( key ) != nullptr;
        }
    };
}