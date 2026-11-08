#include "pecore/RichHeader.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace PeCore
{
    namespace
    {
        constexpr uint32_t RICH_MARKER = 0x68636952;
        constexpr uint32_t DANS_MARKER = 0x536E6144;
        constexpr uint64_t E_LFANEW_OFFSET = 0x3C;

        uint32_t Get32(
            std::span< const uint8_t > data,
            uint64_t offset
        )
        {
            uint32_t value = 0;
            std::memcpy( &value, data.data() + offset, 4 );
            return value;
        }

        void Put32(
            std::vector< uint8_t > & data,
            uint64_t offset,
            uint32_t value
        )
        {
            std::memcpy( data.data() + offset, &value, 4 );
        }

        std::string Answer(
            const std::string & message,
            const std::string & select = "rich"
        )
        {
            JsonWriter writer;
            writer.BeginObject();
            writer.MemberBool( "ok", true );
            writer.Member( "message", message );
            writer.Member( "select", select );
            writer.EndObject();
            return writer.TakeText();
        }
    }

    RichHeaderInfo ParseRichHeader(
        std::span< const uint8_t > data
    )
    {
        RichHeaderInfo info;

        if (data.size() < 0x40 || data[0] != 'M' || data[1] != 'Z')
        {
            return info;
        }

        uint64_t limit = std::min< uint64_t >( Get32( data, E_LFANEW_OFFSET ), data.size() );
        info.m_Limit = limit;

        // 'Rich' is dword aligned between the DOS header and 'e_lfanew', the last occurrence wins.
        for (uint64_t offset = 0x40; offset + 8 <= limit; offset += 4)
        {
            if (Get32( data, offset ) == RICH_MARKER)
            {
                info.m_RichOffset = offset;
                info.m_Found = true;
            }
        }

        if (!info.m_Found)
        {
            return info;
        }

        info.m_Key = Get32( data, info.m_RichOffset + 4 );
        info.m_Found = false;

        for (uint64_t offset = info.m_RichOffset; offset >= 0x40 + 4; )
        {
            offset -= 4;

            if ((Get32( data, offset ) ^ info.m_Key) == DANS_MARKER)
            {
                info.m_DansOffset = offset;
                info.m_Found = true;
                break;
            }
        }

        if (!info.m_Found || info.m_RichOffset < info.m_DansOffset + 16)
        {
            info.m_Found = false;
            return info;
        }

        info.m_PaddingValid = true;

        for (uint64_t index = 1; index < 4; ++index)
        {
            info.m_PaddingValid = info.m_PaddingValid && (Get32( data, info.m_DansOffset + index * 4 ) ^ info.m_Key) == 0;
        }

        for (uint64_t offset = info.m_DansOffset + 16; offset + 8 <= info.m_RichOffset; offset += 8)
        {
            uint32_t comp_id = Get32( data, offset ) ^ info.m_Key;
            RichEntry entry;
            entry.m_ProductId = static_cast<uint16_t>( comp_id >> 16 );
            entry.m_Build = static_cast<uint16_t>( comp_id & 0xFFFF );
            entry.m_Count = Get32( data, offset + 4 ) ^ info.m_Key;
            info.m_Entries.push_back( entry );
        }

        return info;
    }

    uint32_t ComputeRichChecksum(
        std::span< const uint8_t > data,
        uint64_t dans_offset,
        const std::vector< RichEntry > & entries
    )
    {
        uint32_t checksum = static_cast<uint32_t>(dans_offset);

        for (uint64_t offset = 0; offset < dans_offset && offset < data.size(); ++offset)
        {
            if (offset >= E_LFANEW_OFFSET && offset < E_LFANEW_OFFSET + 4)
            {
                continue;
            }

            checksum += std::rotl( static_cast<uint32_t>( data[offset] ), static_cast<int>( offset % 32 ) );
        }

        for (const RichEntry & entry : entries)
        {
            uint32_t comp_id = static_cast<uint32_t>( entry.m_ProductId ) << 16 | entry.m_Build;
            checksum += std::rotl( comp_id, static_cast<int>(entry.m_Count % 32) );
        }

        return checksum;
    }

    void WriteRichHeader(
        std::vector< uint8_t > & data,
        const RichHeaderInfo & info,
        const std::vector< RichEntry > & entries,
        uint32_t key
    )
    {
        Require( info.m_Found, ErrorCode::ERR_NOT_FOUND, "The file has no Rich header" );
        uint64_t new_end = info.m_DansOffset + 16 + entries.size() * 8ull + 8;
        Require( new_end <= info.m_Limit, ErrorCode::ERR_OUT_OF_RANGE,
            "The Rich header does not fit before the PE header (e_lfanew " + Hex( info.m_Limit ) + "), remove an entry first" );

        // Bytes between the old end and the new end must be free [zero] or the stub would be overwritten.
        for (uint64_t offset = info.GetEnd(); offset < new_end; ++offset)
        {
            Require( data[offset] == 0, ErrorCode::ERR_OUT_OF_RANGE, "No free space after the Rich header for another entry" );
        }

        uint64_t clear_end = std::max( info.GetEnd(), new_end );
        std::fill( data.begin() + static_cast<ptrdiff_t>( info.m_DansOffset ), data.begin() + static_cast<ptrdiff_t>( clear_end ), uint8_t( 0 ) );

        uint64_t offset = info.m_DansOffset;
        Put32( data, offset, DANS_MARKER ^ key );

        for (int index = 1; index < 4; ++index)
        {
            Put32( data, offset + index * 4ull, key );
        }

        offset += 16;

        for (const RichEntry & entry : entries)
        {
            uint32_t comp_id = static_cast<uint32_t>( entry.m_ProductId ) << 16 | entry.m_Build;
            Put32( data, offset, comp_id ^ key );
            Put32( data, offset + 4, entry.m_Count ^ key );
            offset += 8;
        }

        Put32( data, offset, RICH_MARKER );
        Put32( data, offset + 4, key );
    }

    std::string ExecuteRichCommand(
        Document & document,
        const JsonValue & command
    )
    {
        std::string id = command.GetString( "command" );
        std::vector< uint8_t > data = document.GetData();
        RichHeaderInfo info = ParseRichHeader( data );
        Require( info.m_Found, ErrorCode::ERR_NOT_FOUND, "The file has no Rich header" );

        std::vector< RichEntry > entries = info.m_Entries;
        uint32_t checksum_before = ComputeRichChecksum( data, info.m_DansOffset, info.m_Entries );

        // A decrypted header [key 0] stays decrypted, an encrypted one gets the new checksum as key.
        bool decrypted = info.m_Key == 0 && checksum_before != 0;
        std::string message;
        std::string description;
        bool keep_key = false;

        if (id == "rich.set")
        {
            // From a command cell the target comes as "argument": "<index>:<field>".
            uint64_t index = command.GetUnsigned( "index", UINT64_MAX );
            std::string field = command.GetString( "field" );
            std::string argument = command.GetString( "argument" );
            size_t colon = argument.find( ':' );

            if (colon != std::string::npos)
            {
                Require( ParseNumber( "#" + argument.substr( 0, colon ), index ), ErrorCode::ERR_INVALID_ARGUMENT, "Invalid Rich header cell " + argument );
                field = argument.substr( colon + 1 );
            }

            Require( index < entries.size(), ErrorCode::ERR_INVALID_ARGUMENT, "No such Rich header entry" );
            uint64_t value = command.GetUnsigned( "value" );
            RichEntry & entry = entries[index];

            if (field == "product" || field == "build")
            {
                Require( value <= 0xFFFF, ErrorCode::ERR_OUT_OF_RANGE, "The value does not fit into a word" );
                (field == "product" ? entry.m_ProductId : entry.m_Build) = static_cast<uint16_t>( value );
            }
            else if (field == "count")
            {
                Require( value <= 0xFFFFFFFF, ErrorCode::ERR_OUT_OF_RANGE, "The value does not fit into a dword" );
                entry.m_Count = static_cast<uint32_t>(value);
            }
            else
            {
                throw Error( ErrorCode::ERR_INVALID_ARGUMENT, "Unknown Rich header field " + field );
            }

            keep_key = command.GetBool( "keep_key", false );
            description = "Edit Rich header entry " + std::to_string( index );
            message = "Rich header entry " + std::to_string( index ) + " changed";
        }
        else if (id == "rich.add")
        {
            entries.push_back( RichEntry { 0, 0, 1 } );
            description = "Add Rich header entry";
            message = "Entry added, edit its product id, build and count";
        }
        else if (id == "rich.remove")
        {
            uint64_t index = command.GetUnsigned( "index", UINT64_MAX );
            Require( index < entries.size(), ErrorCode::ERR_INVALID_ARGUMENT, "No such Rich header entry" );
            entries.erase( entries.begin() + static_cast<ptrdiff_t>( index ) );
            description = "Remove Rich header entry";
            message = "Entry removed";
        }
        else if (id == "rich.fix_checksum" || id == "rich.encrypt")
        {
            decrypted = false;
            description = "Encrypt Rich header";
            message = "Rich header encrypted with the checksum as key";
        }
        else if (id == "rich.decrypt")
        {
            decrypted = true;
            description = "Decrypt Rich header";
            message = "Rich header stored in plain text (key 0), Encrypt restores a valid header";
        }
        else if (id == "rich.clear")
        {
            std::fill( data.begin() + static_cast<ptrdiff_t>(info.m_DansOffset), data.begin() + static_cast<ptrdiff_t>(info.GetEnd()), uint8_t( 0 ) );
            document.SetData( std::move( data ), "Remove Rich header" );
            return Answer( "Rich header removed (cleared to zero)", "dos" );
        }
        else
        {
            throw Error( ErrorCode::ERR_NOT_FOUND, "Unknown command " + id );
        }

        uint32_t key = keep_key ? info.m_Key : decrypted ? 0 : ComputeRichChecksum( data, info.m_DansOffset, entries );
        WriteRichHeader( data, info, entries, key );
        document.SetData( std::move( data ), description );
        return Answer( message );
    }
}