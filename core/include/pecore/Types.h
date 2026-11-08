#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace PeCore
{
    enum class ErrorCode
    {
        ERR_NONE = 0,
        ERR_INVALID_ARGUMENT,
        ERR_IO,
        ERR_ACCESS_DENIED,
        ERR_NOT_FOUND,
        ERR_READ_ONLY,
        ERR_NOT_PE,
        ERR_OUT_OF_RANGE,
        ERR_PARSE,
        ERR_UNSUPPORTED,
        ERR_INTERNAL
    };

    // Exception used inside the core, the C ABI converts it to a result code and a message.
    class Error : public std::runtime_error
    {
    private:

        ErrorCode m_Code;

    public:

        Error(
            ErrorCode code,
            const std::string & message
        )
            : std::runtime_error( message )
            , m_Code( code )
        {}

        ErrorCode GetCode() const
        {
            return m_Code;
        }
    };

    inline void Require(
        bool condition,
        ErrorCode code,
        const std::string & message
    )
    {
        if (!condition)
        {
            throw Error( code, message );
        }
    }

    // Formats value as upper case hex with at least digits characters.
    std::string Hex(
        uint64_t value,
        int digits = 8
    );

    // Hex digits for a field of size bytes [Byte 2, Word 4, Dword 8, Qword 16].
    std::string HexOfSize(
        uint64_t value,
        size_t size
    );

    // CFF style size names. Byte, Word, Dword, Qword, otherwise 'N bytes'.
    std::string SizeName(
        size_t size
    );

    std::string FormatBytes(
        uint64_t size
    );

    std::string FormatTimestamp(
        uint32_t time_date_stamp
    );

    // The stamp in local time without a zone, like the file times of the overview and Detect It Easy. Empty for '0' and
    // '0xFFFFFFFF', 'Reproducible build hash' when the value is not a plausible time.
    std::string FormatTimestampLocal(
        uint32_t time_date_stamp
    );

    // Parses a number, hex by default, '0x' prefix or 'h' suffix accepted, 'd' suffix or '#' prefix for decimal.
    bool ParseNumber(
        const std::string & text,
        uint64_t & value
    );

    std::string Utf8FromUtf16(
        const std::u16string & text
    );

    std::u16string Utf16FromUtf8(
        const std::string & text
    );

    // Utf-16 LE bytes [resource strings, version info] to UTF-8, stops at the first zero when 'stop_at_zero' is set.
    std::string Utf8FromUtf16Bytes(
        const uint8_t * data,
        size_t length,
        bool stop_at_zero
    );

    // Latin-1 or ASCII bytes to UTF-8, used for names stored as 8 bit strings.
    std::string Utf8FromLatin1(
        const std::string & text
    );
}