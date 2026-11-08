#include "pecore/Types.h"

#include <charconv>
#include <cstdio>
#include <ctime>

namespace PeCore
{
    std::string Hex(
        uint64_t value,
        int digits
    )
    {
        char buffer[24];
        std::snprintf( buffer, sizeof( buffer ), "%0*llX", digits, static_cast<unsigned long long>(value) );
        return buffer;
    }

    std::string HexOfSize(
        uint64_t value,
        size_t size
    )
    {
        int digits = size == 1 ? 2 : size == 2 ? 4 : size <= 4 ? 8 : 16;
        return Hex( value, digits );
    }

    std::string SizeName(
        size_t size
    )
    {
        switch (size)
        {
            case 1:
                return "Byte";
            case 2:
                return "Word";
            case 4:
                return "Dword";
            case 8:
                return "Qword";
            default:
                return std::to_string( size ) + " bytes";
        }
    }

    std::string FormatBytes(
        uint64_t size
    )
    {
        char buffer[64];

        if (size >= 1024ull * 1024 * 1024)
        {
            std::snprintf( buffer, sizeof( buffer ), "%.2f GB (%llu bytes)", size / (1024.0 * 1024.0 * 1024.0), static_cast<unsigned long long>(size) );
        }
        else if (size >= 1024ull * 1024)
        {
            std::snprintf( buffer, sizeof( buffer ), "%.2f MB (%llu bytes)", size / (1024.0 * 1024.0), static_cast<unsigned long long>(size) );
        }
        else if (size >= 1024)
        {
            std::snprintf( buffer, sizeof( buffer ), "%.2f KB (%llu bytes)", size / 1024.0, static_cast<unsigned long long>(size) );
        }
        else
        {
            std::snprintf( buffer, sizeof( buffer ), "%llu bytes", static_cast<unsigned long long>(size) );
        }

        return buffer;
    }

    std::string FormatTimestamp(
        uint32_t time_date_stamp
    )
    {
        // Linkers with '/Brepro' write a hash instead of a time, anything before 1990 or far in the future is shown raw.
        if (time_date_stamp == 0 || time_date_stamp == 0xFFFFFFFF)
        {
            return std::string();
        }

        std::time_t time = static_cast<std::time_t>(time_date_stamp);
        std::tm parts {};

        #ifdef _WIN32
        if (gmtime_s( &parts, &time ) != 0)
        {
            return std::string();
        }
        #else
        if (gmtime_r( &time, &parts ) == nullptr)
        {
            return std::string();
        }
        #endif

        if (parts.tm_year + 1900 < 1990 || parts.tm_year + 1900 > 2040)
        {
            return "Reproducible build hash";
        }

        char buffer[40];
        std::strftime( buffer, sizeof( buffer ), "%Y-%m-%d %H:%M:%S UTC", &parts );
        return buffer;
    }

    std::string FormatTimestampLocal(
        uint32_t time_date_stamp
    )
    {
        if (time_date_stamp == 0 || time_date_stamp == 0xFFFFFFFF)
        {
            return std::string();
        }

        std::time_t time = static_cast<std::time_t>(time_date_stamp);
        std::tm universal {};
        std::tm parts {};

        // The plausibility check runs on UTC so both formats agree on what a '/Brepro' hash is.
        #ifdef _WIN32
        if (gmtime_s( &universal, &time ) != 0 || localtime_s( &parts, &time ) != 0)
        {
            return std::string();
        }
        #else
        if (gmtime_r( &time, &universal ) == nullptr || localtime_r( &time, &parts ) == nullptr)
        {
            return std::string();
        }
        #endif

        if (universal.tm_year + 1900 < 1990 || universal.tm_year + 1900 > 2040)
        {
            return "Reproducible build hash";
        }

        char buffer[32];
        std::strftime( buffer, sizeof( buffer ), "%Y-%m-%d %H:%M:%S", &parts );
        return buffer;
    }

    bool ParseNumber(
        const std::string & text,
        uint64_t & value
    )
    {
        size_t start = 0;
        size_t end = text.size();

        while (start < end && (text[start] == ' ' || text[start] == '\t'))
        {
            ++start;
        }

        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        {
            --end;
        }

        if (start == end)
        {
            return false;
        }

        int base = 16;

        if (text[start] == '#')
        {
            base = 10;
            ++start;
        }
        else if (end - start > 2 && text[start] == '0' && (text[start + 1] == 'x' || text[start + 1] == 'X'))
        {
            start += 2;
        }
        else if (end - start > 2 && (text[end - 1] == 'd' || text[end - 1] == 'D') && text[end - 2] == ' ')
        {
            // '123 d' is decimal like in the CFF Explorer go to dialog, the space keeps it apart from the hex digit D.
            base = 10;
            --end;

            while (end > start && text[end - 1] == ' ')
            {
                --end;
            }
        }
        else if (text[end - 1] == 'h' || text[end - 1] == 'H')
        {
            --end;

            while (end > start && text[end - 1] == ' ')
            {
                --end;
            }
        }

        if (start == end)
        {
            return false;
        }

        uint64_t parsed = 0;
        auto result = std::from_chars( text.data() + start, text.data() + end, parsed, base );

        if (result.ec != std::errc() || result.ptr != text.data() + end)
        {
            return false;
        }

        value = parsed;
        return true;
    }

    namespace
    {
        void AppendUtf8(
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

        std::string FromUtf16Units(
            const char16_t * units,
            size_t count
        )
        {
            std::string out;
            out.reserve( count );

            for (size_t index = 0; index < count; ++index)
            {
                uint32_t code = units[index];

                if (code >= 0xD800 && code < 0xDC00 && index + 1 < count && units[index + 1] >= 0xDC00 && units[index + 1] < 0xE000)
                {
                    code = 0x10000 + ((code - 0xD800) << 10) + (units[index + 1] - 0xDC00);
                    ++index;
                }
                else if (code >= 0xD800 && code < 0xE000)
                {
                    // Lone surrogate, keep the text readable.
                    code = 0xFFFD;
                }

                AppendUtf8( out, code );
            }

            return out;
        }
    }

    std::string Utf8FromUtf16(
        const std::u16string & text
    )
    {
        return FromUtf16Units( text.data(), text.size() );
    }

    std::u16string Utf16FromUtf8(
        const std::string & text
    )
    {
        std::u16string out;
        out.reserve( text.size() );
        size_t index = 0;

        while (index < text.size())
        {
            unsigned char lead = static_cast<unsigned char>( text[index] );
            uint32_t code = 0;
            size_t extra = 0;

            if (lead < 0x80)
            {
                code = lead;
            }
            else if ((lead & 0xE0) == 0xC0)
            {
                code = lead & 0x1F;
                extra = 1;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                code = lead & 0x0F;
                extra = 2;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
                code = lead & 0x07;
                extra = 3;
            }
            else
            {
                code = 0xFFFD;
            }

            ++index;

            for (size_t count = 0; count < extra; ++count)
            {
                if (index >= text.size() || (static_cast<unsigned char>( text[index] ) & 0xC0) != 0x80)
                {
                    code = 0xFFFD;
                    break;
                }

                code = (code << 6) | (static_cast<unsigned char>( text[index] ) & 0x3F);
                ++index;
            }

            if (code >= 0x10000)
            {
                code -= 0x10000;
                out.push_back( static_cast<char16_t>(0xD800 + (code >> 10)) );
                out.push_back( static_cast<char16_t>(0xDC00 + (code & 0x3FF)) );
            }
            else
            {
                out.push_back( static_cast<char16_t>(code) );
            }
        }

        return out;
    }

    std::string Utf8FromUtf16Bytes(
        const uint8_t * data,
        size_t length,
        bool stop_at_zero
    )
    {
        std::u16string units;
        units.reserve( length / 2 );

        for (size_t index = 0; index + 1 < length; index += 2)
        {
            char16_t unit = static_cast<char16_t>(data[index] | (data[index + 1] << 8));

            if (unit == 0 && stop_at_zero)
            {
                break;
            }

            units.push_back( unit );
        }

        return FromUtf16Units( units.data(), units.size() );
    }

    std::string Utf8FromLatin1(
        const std::string & text
    )
    {
        std::string out;
        out.reserve( text.size() );

        for (char character : text)
        {
            AppendUtf8( out, static_cast<unsigned char>(character) );
        }

        return out;
    }
}