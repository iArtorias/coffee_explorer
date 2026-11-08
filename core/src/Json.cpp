#include "pecore/Json.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace PeCore
{
    void JsonWriter::Separator()
    {
        if (m_AfterKey)
        {
            m_AfterKey = false;
            return;
        }

        if (!m_HasElement.empty())
        {
            if (m_HasElement.back())
            {
                m_Text.push_back( ',' );
            }

            m_HasElement.back() = true;
        }
    }

    void JsonWriter::AppendEscaped(
        std::string_view text
    )
    {
        m_Text.push_back( '"' );

        for (char character : text)
        {
            unsigned char value = static_cast<unsigned char>(character);

            switch (character)
            {
                case '"':
                    m_Text += "\\\"";
                    break;
                case '\\':
                    m_Text += "\\\\";
                    break;
                case '\n':
                    m_Text += "\\n";
                    break;
                case '\r':
                    m_Text += "\\r";
                    break;
                case '\t':
                    m_Text += "\\t";
                    break;
                default:
                    if (value < 0x20)
                    {
                        char buffer[8];
                        std::snprintf( buffer, sizeof( buffer ), "\\u%04X", value );
                        m_Text += buffer;
                    }
                    else
                    {
                        m_Text.push_back( character );
                    }
                    break;
            }
        }

        m_Text.push_back( '"' );
    }

    JsonWriter & JsonWriter::BeginObject()
    {
        Separator();
        m_Text.push_back( '{' );
        m_HasElement.push_back( false );
        return *this;
    }

    JsonWriter & JsonWriter::EndObject()
    {
        m_Text.push_back( '}' );
        m_HasElement.pop_back();
        return *this;
    }

    JsonWriter & JsonWriter::BeginArray()
    {
        Separator();
        m_Text.push_back( '[' );
        m_HasElement.push_back( false );
        return *this;
    }

    JsonWriter & JsonWriter::EndArray()
    {
        m_Text.push_back( ']' );
        m_HasElement.pop_back();
        return *this;
    }

    JsonWriter & JsonWriter::Key(
        std::string_view name
    )
    {
        Separator();
        AppendEscaped( name );
        m_Text.push_back( ':' );
        m_AfterKey = true;
        return *this;
    }

    JsonWriter & JsonWriter::String(
        std::string_view value
    )
    {
        Separator();
        AppendEscaped( value );
        return *this;
    }

    JsonWriter & JsonWriter::Number(
        int64_t value
    )
    {
        Separator();
        m_Text += std::to_string( value );
        return *this;
    }

    JsonWriter & JsonWriter::Unsigned(
        uint64_t value
    )
    {
        Separator();
        m_Text += std::to_string( value );
        return *this;
    }

    JsonWriter & JsonWriter::Double(
        double value
    )
    {
        Separator();

        if (!std::isfinite( value ))
        {
            m_Text += "0";
            return *this;
        }

        char buffer[32];
        std::snprintf( buffer, sizeof( buffer ), "%.6g", value );
        m_Text += buffer;
        return *this;
    }

    JsonWriter & JsonWriter::Bool(
        bool value
    )
    {
        Separator();
        m_Text += value ? "true" : "false";
        return *this;
    }

    JsonWriter & JsonWriter::Null()
    {
        Separator();
        m_Text += "null";
        return *this;
    }

    JsonWriter & JsonWriter::Raw(
        std::string_view json
    )
    {
        Separator();
        m_Text += json;
        return *this;
    }

    JsonWriter & JsonWriter::Member(
        std::string_view name,
        std::string_view value
    )
    {
        return Key( name ).String( value );
    }

    JsonWriter & JsonWriter::Member(
        std::string_view name,
        const char * value
    )
    {
        return Key( name ).String( value != nullptr ? std::string_view( value ) : std::string_view() );
    }

    JsonWriter & JsonWriter::Member(
        std::string_view name,
        const std::string & value
    )
    {
        return Key( name ).String( value );
    }

    JsonWriter & JsonWriter::MemberNumber(
        std::string_view name,
        int64_t value
    )
    {
        return Key( name ).Number( value );
    }

    JsonWriter & JsonWriter::MemberUnsigned(
        std::string_view name,
        uint64_t value
    )
    {
        return Key( name ).Unsigned( value );
    }

    JsonWriter & JsonWriter::MemberBool(
        std::string_view name,
        bool value
    )
    {
        return Key( name ).Bool( value );
    }

    // Recursive descent parser, strict JSON plus a depth limit.
    class JsonParser
    {
    private:

        std::string_view m_Text;
        size_t m_Position = 0;
        int m_Depth = 0;

        static constexpr int MAX_DEPTH = 64;

        [[noreturn]] void Fail(
            const char * message
        ) const
        {
            throw Error( ErrorCode::ERR_PARSE, std::string( "Invalid JSON request: " ) + message + " at " + std::to_string( m_Position ) );
        }

        void SkipWhitespace()
        {
            while (m_Position < m_Text.size())
            {
                char character = m_Text[m_Position];

                if (character != ' ' && character != '\t' && character != '\r' && character != '\n')
                {
                    break;
                }

                ++m_Position;
            }
        }

        bool Consume(
            char expected
        )
        {
            SkipWhitespace();

            if (m_Position < m_Text.size() && m_Text[m_Position] == expected)
            {
                ++m_Position;
                return true;
            }

            return false;
        }

        void Expect(
            char expected
        )
        {
            if (!Consume( expected ))
            {
                Fail( "unexpected character" );
            }
        }

        static void AppendUtf8(
            std::string & out,
            uint32_t code
        )
        {
            if (code < 0x80)
            {
                out.push_back( static_cast<char>( code ) );
            }
            else if (code < 0x800)
            {
                out.push_back( static_cast<char>( 0xC0 | (code >> 6) ) );
                out.push_back( static_cast<char>( 0x80 | (code & 0x3F) ) );
            }
            else if (code < 0x10000)
            {
                out.push_back( static_cast<char>( 0xE0 | (code >> 12) ) );
                out.push_back( static_cast<char>( 0x80 | ((code >> 6) & 0x3F) ) );
                out.push_back( static_cast<char>( 0x80 | (code & 0x3F) ) );
            }
            else
            {
                out.push_back( static_cast<char>( 0xF0 | (code >> 18) ) );
                out.push_back( static_cast<char>( 0x80 | ((code >> 12) & 0x3F) ) );
                out.push_back( static_cast<char>( 0x80 | ((code >> 6) & 0x3F) ) );
                out.push_back( static_cast<char>(0x80 | (code & 0x3F)) );
            }
        }

        uint32_t ReadHex4()
        {
            if (m_Position + 4 > m_Text.size())
            {
                Fail( "truncated escape" );
            }

            uint32_t value = 0;
            auto result = std::from_chars( m_Text.data() + m_Position, m_Text.data() + m_Position + 4, value, 16 );

            if (result.ptr != m_Text.data() + m_Position + 4)
            {
                Fail( "bad escape" );
            }

            m_Position += 4;
            return value;
        }

        std::string ParseString()
        {
            Expect( '"' );
            std::string out;

            while (true)
            {
                if (m_Position >= m_Text.size())
                {
                    Fail( "unterminated string" );
                }

                char character = m_Text[m_Position++];

                if (character == '"')
                {
                    return out;
                }

                if (character != '\\')
                {
                    out.push_back( character );
                    continue;
                }

                if (m_Position >= m_Text.size())
                {
                    Fail( "unterminated escape" );
                }

                char escape = m_Text[m_Position++];

                switch (escape)
                {
                    case '"':
                    case '\\':
                    case '/':
                        out.push_back( escape );
                        break;
                    case 'b':
                        out.push_back( '\b' );
                        break;
                    case 'f':
                        out.push_back( '\f' );
                        break;
                    case 'n':
                        out.push_back( '\n' );
                        break;
                    case 'r':
                        out.push_back( '\r' );
                        break;
                    case 't':
                        out.push_back( '\t' );
                        break;
                    case 'u':
                    {
                        uint32_t code = ReadHex4();

                        // Surrogate pair
                        if (code >= 0xD800 && code < 0xDC00 && m_Position + 6 <= m_Text.size() && m_Text[m_Position] == '\\' && m_Text[m_Position + 1] == 'u')
                        {
                            m_Position += 2;
                            uint32_t low = ReadHex4();
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        }

                        AppendUtf8( out, code );
                        break;
                    }
                    default:
                        Fail( "unknown escape" );
                }
            }
        }

        JsonValue ParseValue()
        {
            if (++m_Depth > MAX_DEPTH)
            {
                Fail( "nesting too deep" );
            }

            SkipWhitespace();

            if (m_Position >= m_Text.size())
            {
                Fail( "unexpected end" );
            }

            JsonValue value;
            char character = m_Text[m_Position];

            if (character == '{')
            {
                ++m_Position;
                value.m_Type = JsonType::JSON_OBJECT;

                if (!Consume( '}' ))
                {
                    do
                    {
                        SkipWhitespace();
                        std::string key = ParseString();
                        Expect( ':' );
                        value.m_Members.emplace_back( std::move( key ), ParseValue() );
                    } while (Consume( ',' ));

                    Expect( '}' );
                }
            }
            else if (character == '[')
            {
                ++m_Position;
                value.m_Type = JsonType::JSON_ARRAY;

                if (!Consume( ']' ))
                {
                    do
                    {
                        value.m_Items.push_back( ParseValue() );
                    } while (Consume( ',' ));

                    Expect( ']' );
                }
            }
            else if (character == '"')
            {
                value.m_Type = JsonType::JSON_STRING;
                value.m_Text = ParseString();
            }
            else if (m_Text.substr( m_Position, 4 ) == "true")
            {
                m_Position += 4;
                value.m_Type = JsonType::JSON_BOOL;
                value.m_Bool = true;
            }
            else if (m_Text.substr( m_Position, 5 ) == "false")
            {
                m_Position += 5;
                value.m_Type = JsonType::JSON_BOOL;
            }
            else if (m_Text.substr( m_Position, 4 ) == "null")
            {
                m_Position += 4;
            }
            else if (character == '-' || (character >= '0' && character <= '9'))
            {
                size_t start = m_Position;

                while (m_Position < m_Text.size())
                {
                    char digit = m_Text[m_Position];

                    if (!((digit >= '0' && digit <= '9') || digit == '-' || digit == '+' || digit == '.' || digit == 'e' || digit == 'E'))
                    {
                        break;
                    }

                    ++m_Position;
                }

                value.m_Type = JsonType::JSON_NUMBER;
                value.m_Text = std::string( m_Text.substr( start, m_Position - start ) );
                value.m_Number = std::strtod( value.m_Text.c_str(), nullptr );
            }
            else
            {
                Fail( "unexpected token" );
            }

            --m_Depth;
            return value;
        }

    public:

        explicit JsonParser(
            std::string_view text
        )
            : m_Text( text )
        {}

        JsonValue Run()
        {
            JsonValue value = ParseValue();
            SkipWhitespace();

            if (m_Position != m_Text.size())
            {
                Fail( "trailing characters" );
            }

            return value;
        }
    };

    JsonValue JsonValue::Parse(
        std::string_view text
    )
    {
        return JsonParser( text ).Run();
    }

    const JsonValue * JsonValue::Find(
        std::string_view key
    ) const
    {
        if (m_Type != JsonType::JSON_OBJECT)
        {
            return nullptr;
        }

        for (const auto & member : m_Members)
        {
            if (member.first == key)
            {
                return &member.second;
            }
        }

        return nullptr;
    }

    std::string JsonValue::AsString(
        const std::string & fallback
    ) const
    {
        if (m_Type == JsonType::JSON_STRING || m_Type == JsonType::JSON_NUMBER)
        {
            return m_Text;
        }

        if (m_Type == JsonType::JSON_BOOL)
        {
            return m_Bool ? "true" : "false";
        }

        return fallback;
    }

    bool JsonValue::AsBool(
        bool fallback
    ) const
    {
        if (m_Type == JsonType::JSON_BOOL)
        {
            return m_Bool;
        }

        if (m_Type == JsonType::JSON_NUMBER)
        {
            return m_Number != 0.0;
        }

        if (m_Type == JsonType::JSON_STRING)
        {
            return m_Text == "true" || m_Text == "1";
        }

        return fallback;
    }

    uint64_t JsonValue::AsUnsigned(
        uint64_t fallback
    ) const
    {
        if (m_Type == JsonType::JSON_NUMBER)
        {
            uint64_t value = 0;
            auto result = std::from_chars( m_Text.data(), m_Text.data() + m_Text.size(), value, 10 );

            if (result.ec == std::errc() && result.ptr == m_Text.data() + m_Text.size())
            {
                return value;
            }

            return m_Number >= 0.0 ? static_cast<uint64_t>(m_Number) : fallback;
        }

        if (m_Type == JsonType::JSON_STRING)
        {
            uint64_t value = 0;
            return ParseNumber( m_Text, value ) ? value : fallback;
        }

        if (m_Type == JsonType::JSON_BOOL)
        {
            return m_Bool ? 1 : 0;
        }

        return fallback;
    }

    int64_t JsonValue::AsInteger(
        int64_t fallback
    ) const
    {
        if (m_Type == JsonType::JSON_NUMBER)
        {
            int64_t value = 0;
            auto result = std::from_chars( m_Text.data(), m_Text.data() + m_Text.size(), value, 10 );

            if (result.ec == std::errc() && result.ptr == m_Text.data() + m_Text.size())
            {
                return value;
            }

            return static_cast<int64_t>(m_Number);
        }

        if (m_Type == JsonType::JSON_STRING || m_Type == JsonType::JSON_BOOL)
        {
            return static_cast<int64_t>(AsUnsigned( static_cast<uint64_t>(fallback) ));
        }

        return fallback;
    }

    std::string JsonValue::GetString(
        std::string_view key,
        const std::string & fallback
    ) const
    {
        const JsonValue * value = Find( key );
        return value != nullptr ? value->AsString( fallback ) : fallback;
    }

    uint64_t JsonValue::GetUnsigned(
        std::string_view key,
        uint64_t fallback
    ) const
    {
        const JsonValue * value = Find( key );
        return value != nullptr ? value->AsUnsigned( fallback ) : fallback;
    }

    int64_t JsonValue::GetInteger(
        std::string_view key,
        int64_t fallback
    ) const
    {
        const JsonValue * value = Find( key );
        return value != nullptr ? value->AsInteger( fallback ) : fallback;
    }

    bool JsonValue::GetBool(
        std::string_view key,
        bool fallback
    ) const
    {
        const JsonValue * value = Find( key );
        return value != nullptr ? value->AsBool( fallback ) : fallback;
    }
}