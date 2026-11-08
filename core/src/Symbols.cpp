#include "pecore/Symbols.h"

#include "pecore/Document.h"
#include "pecore/Platform.h"

#include "PeHelpers.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace PeCore
{
    namespace
    {
        constexpr char MSF_MAGIC[] = "Microsoft C/C++ MSF 7.00\r\n\x1a" "DS\0\0";
        constexpr size_t MSF_MAGIC_SIZE = 32;
        constexpr uint32_t STREAM_PDB_INFO = 1;
        constexpr uint32_t STREAM_DBI = 3;
        constexpr uint64_t MAX_PDB_SIZE = 4ull * 1024 * 1024 * 1024;

        // CodeView symbol record kinds.
        constexpr uint16_t S_PUB32 = 0x110E;
        constexpr uint16_t S_LDATA32 = 0x110C;
        constexpr uint16_t S_GDATA32 = 0x110D;
        constexpr uint16_t S_LPROC32 = 0x110F;
        constexpr uint16_t S_GPROC32 = 0x1110;
        constexpr uint16_t S_LPROC32_ID = 0x1146;
        constexpr uint16_t S_GPROC32_ID = 0x1147;

        uint32_t Get32(
            std::span< const uint8_t > data,
            size_t offset
        )
        {
            uint32_t value = 0;

            if (offset + 4 <= data.size())
            {
                std::memcpy( &value, data.data() + offset, 4 );
            }

            return value;
        }

        uint16_t Get16(
            std::span< const uint8_t > data,
            size_t offset
        )
        {
            uint16_t value = 0;

            if (offset + 2 <= data.size())
            {
                std::memcpy( &value, data.data() + offset, 2 );
            }

            return value;
        }

        std::string GetString(
            std::span< const uint8_t > data,
            size_t offset
        )
        {
            std::string text;

            for (; offset < data.size() && data[offset] != 0; ++offset)
            {
                text.push_back( static_cast<char>( data[offset] ) );
            }

            return text;
        }

        // Multi stream file. Fixed size blocks, a directory of streams, each stream a list of blocks.
        class MsfFile
        {
        private:
            std::vector< uint8_t > m_Data;
            uint32_t m_BlockSize = 0;
            std::vector< uint32_t > m_StreamSizes;
            std::vector< std::vector< uint32_t > > m_StreamBlocks;

            bool ReadBlocks(
                const std::vector< uint32_t > & blocks,
                uint64_t size,
                std::vector< uint8_t > & out
            ) const
            {
                out.clear();

                // A damaged size must not reserve gigabytes, the blocks bound what can be read.
                out.reserve( static_cast<size_t>(std::min< uint64_t >( size, static_cast<uint64_t>(blocks.size()) * m_BlockSize )) );

                for (uint32_t block : blocks)
                {
                    uint64_t start = static_cast<uint64_t>(block) * m_BlockSize;

                    if (start >= m_Data.size())
                    {
                        return false;
                    }

                    uint64_t length = std::min< uint64_t >( { m_BlockSize, size - out.size(), m_Data.size() - start } );
                    out.insert( out.end(), m_Data.begin() + static_cast<ptrdiff_t>(start), m_Data.begin() + static_cast<ptrdiff_t>(start + length) );

                    if (out.size() == size)
                    {
                        break;
                    }
                }

                return out.size() == size;
            }

        public:

            bool Open(
                std::vector< uint8_t > data,
                std::string & error
            )
            {
                m_Data = std::move( data );

                if (m_Data.size() < 56 || std::memcmp( m_Data.data(), MSF_MAGIC, MSF_MAGIC_SIZE ) != 0)
                {
                    error = "Not a PDB 7.0 file (MSF signature missing)";
                    return false;
                }

                m_BlockSize = Get32( m_Data, 32 );
                uint32_t directory_bytes = Get32( m_Data, 44 );
                uint32_t block_map = Get32( m_Data, 52 );

                if (m_BlockSize != 512 && m_BlockSize != 1024 && m_BlockSize != 2048 && m_BlockSize != 4096)
                {
                    error = "Unsupported PDB block size";
                    return false;
                }

                uint64_t directory_blocks = (static_cast<uint64_t>(directory_bytes) + m_BlockSize - 1) / m_BlockSize;
                uint64_t map_offset = static_cast<uint64_t>(block_map) * m_BlockSize;

                if (map_offset + directory_blocks * 4 > m_Data.size())
                {
                    error = "The PDB stream directory is outside of the file";
                    return false;
                }

                std::vector< uint32_t > blocks;

                for (uint64_t index = 0; index < directory_blocks; ++index)
                {
                    blocks.push_back( Get32( m_Data, static_cast<size_t>( map_offset + index * 4 ) ) );
                }

                std::vector< uint8_t > directory;

                if (!ReadBlocks( blocks, directory_bytes, directory ) || directory.size() < 4)
                {
                    error = "The PDB stream directory is damaged";
                    return false;
                }

                uint32_t streams = Get32( directory, 0 );

                if (4ull + streams * 4ull > directory.size())
                {
                    error = "The PDB stream directory is damaged";
                    return false;
                }

                size_t cursor = 4 + static_cast<size_t>(streams) * 4;

                for (uint32_t stream = 0; stream < streams; ++stream)
                {
                    uint32_t size = Get32( directory, 4 + stream * 4ull );
                    size = size == 0xFFFFFFFF ? 0 : size;
                    uint32_t count = static_cast<uint32_t>( (static_cast<uint64_t>( size ) + m_BlockSize - 1) / m_BlockSize );

                    if (cursor + count * 4ull > directory.size())
                    {
                        error = "The PDB stream directory is truncated";
                        return false;
                    }

                    std::vector< uint32_t > list( count );

                    for (uint32_t index = 0; index < count; ++index)
                    {
                        list[index] = Get32( directory, cursor + index * 4ull );
                    }

                    cursor += count * 4ull;
                    m_StreamSizes.push_back( size );
                    m_StreamBlocks.push_back( std::move( list ) );
                }

                return true;
            }

            bool ReadStream(
                uint32_t stream,
                std::vector< uint8_t > & out
            ) const
            {
                if (stream >= m_StreamSizes.size())
                {
                    return false;
                }

                return ReadBlocks( m_StreamBlocks[stream], m_StreamSizes[stream], out );
            }
        };

        // Every record is 'uint16 length, uint16 kind, data', length counts kind and data.
        template< typename Visitor >
        void WalkRecords(
            std::span< const uint8_t > records,
            Visitor visit
        )
        {
            size_t cursor = 0;

            while (cursor + 4 <= records.size())
            {
                uint16_t length = Get16( records, cursor );
                uint16_t kind = Get16( records, cursor + 2 );

                if (length < 2 || cursor + 2 + length > records.size())
                {
                    break;
                }

                visit( kind, records.subspan( cursor + 4, length - 2 ) );
                cursor += 2ull + length;
            }
        }

        uint64_t SectionRva(
            const PeImage & image,
            uint16_t segment,
            uint32_t offset,
            bool & valid
        )
        {
            valid = segment >= 1 && segment <= image.GetSectionCount();

            if (!valid)
            {
                return 0;
            }

            return static_cast<uint64_t>(image.Read32( image.GetSectionHeaderOffset( segment - 1u ) + 12 )) + offset;
        }

        std::string HexKey(
            uint64_t value,
            int digits
        )
        {
            char text[24];
            std::snprintf( text, sizeof( text ), "%0*llX", digits, static_cast<unsigned long long>(value) );
            return text;
        }

        // Folders of a symbol path. 'srv*C:\cache*https://...' gives the cache, 'cache*C:\dir' the dir, plain folders as they are.
        std::vector< std::filesystem::path > SymbolPathFolders(
            const char * variable
        )
        {
            std::vector< std::filesystem::path > folders;
            std::string text = GetEnvironmentText( variable );

            if (text.empty())
            {
                return folders;
            }

            size_t start = 0;

            while (start <= text.size())
            {
                size_t end = text.find( ';', start );
                std::string part = text.substr( start, end == std::string::npos ? std::string::npos : end - start );
                start = end == std::string::npos ? text.size() + 1 : end + 1;

                if (part.empty())
                {
                    continue;
                }

                std::string lower = part;
                std::transform( lower.begin(), lower.end(), lower.begin(), [] ( unsigned char character )
                {
                    return static_cast<char>(std::tolower( character ));
                } );

                if (lower.rfind( "srv*", 0 ) == 0 || lower.rfind( "symsrv*", 0 ) == 0 || lower.rfind( "cache*", 0 ) == 0)
                {
                    std::vector< std::string > pieces;
                    size_t from = 0;

                    while (from <= part.size())
                    {
                        size_t star = part.find( '*', from );
                        pieces.push_back( part.substr( from, star == std::string::npos ? std::string::npos : star - from ) );
                        from = star == std::string::npos ? part.size() + 1 : star + 1;
                    }

                    for (size_t index = 1; index < pieces.size(); ++index)
                    {
                        if (!pieces[index].empty() && pieces[index].find( "://" ) == std::string::npos && pieces[index] != "symsrv.dll")
                        {
                            folders.emplace_back( Utf16FromUtf8( pieces[index] ) );
                        }
                    }
                }
                else
                {
                    folders.emplace_back( Utf16FromUtf8( part ) );
                }
            }

            return folders;
        }
    }

    std::string CodeViewInfo::GuidText() const
    {
        const auto & g = m_Guid;
        char text[48];
        std::snprintf( text, sizeof( text ), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
            static_cast<unsigned int>(g[3] << 24 | g[2] << 16 | g[1] << 8 | g[0]), g[5] << 8 | g[4], g[7] << 8 | g[6],
            g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15] );
        return text;
    }

    std::string CodeViewInfo::SearchKey() const
    {
        std::string text = GuidText();
        std::string key;

        for (char character : text)
        {
            if (character != '{' && character != '}' && character != '-')
            {
                key.push_back( character );
            }
        }

        char age[16];
        std::snprintf( age, sizeof( age ), "%X", m_Age );
        return key + age;
    }

    std::string CodeViewInfo::PdbFileName() const
    {
        size_t slash = m_PdbPath.find_last_of( "\\/" );
        return slash == std::string::npos ? m_PdbPath : m_PdbPath.substr( slash + 1 );
    }

    CodeViewInfo ReadCodeView(
        const PeImage & image
    )
    {
        CodeViewInfo info;

        if (!image.IsPe() || !image.HasDirectory( DIR_DEBUG ))
        {
            return info;
        }

        DataDirectory directory = image.GetDirectory( DIR_DEBUG );
        uint64_t offset = image.RvaToOffset( directory.m_Rva );

        if (offset == NO_OFFSET)
        {
            return info;
        }

        for (uint32_t index = 0; index < directory.m_Size / 28 && index < 64; ++index)
        {
            uint64_t row = offset + index * 28ull;
            uint32_t data = image.Read32( row + 24 );

            if (image.Read32( row + 12 ) == 2 && image.Read32( row + 16 ) >= 24 && image.Read32( data ) == 0x53445352)
            {
                info.m_Present = image.ReadAt( data + 4, info.m_Guid.data(), 16 );
                info.m_Age = image.Read32( data + 20 );
                info.m_PdbPath = image.ReadString( data + 24, 1024 );
                return info;
            }
        }

        return info;
    }

    std::shared_ptr< SymbolTable > SymbolTable::Load(
        const std::filesystem::path & path,
        const PeImage & image,
        const CodeViewInfo & code_view,
        bool require_match,
        std::string & error
    )
    {
        MsfFile msf;

        try
        {
            if (!msf.Open( ReadWholeFile( path, MAX_PDB_SIZE ), error ))
            {
                return nullptr;
            }
        }
        catch (const Error & failure)
        {
            error = failure.what();
            return nullptr;
        }

        std::vector< uint8_t > info;
        std::vector< uint8_t > dbi;

        if (!msf.ReadStream( STREAM_PDB_INFO, info ) || info.size() < 28 || !msf.ReadStream( STREAM_DBI, dbi ) || dbi.size() < 64)
        {
            error = "The PDB has no info or DBI stream";
            return nullptr;
        }

        // The DBI age is the one the linker writes into the image, the info stream age can be higher.
        uint32_t age = Get32( dbi, 8 );
        bool same_guid = std::memcmp( info.data() + 12, code_view.m_Guid.data(), 16 ) == 0;

        if (require_match && code_view.m_Present && (!same_guid || age != code_view.m_Age))
        {
            error = same_guid ? "The PDB belongs to another link of this build (age " + std::to_string( age ) + ", the file wants " +
                std::to_string( code_view.m_Age ) + ")" :
                "The PDB belongs to another build (GUID differs)";
            return nullptr;
        }

        auto table = std::make_shared< SymbolTable >();
        table->m_Source = path;
        std::map< uint32_t, Symbol > by_rva;

        auto add = [&] ( uint16_t segment, uint32_t offset, uint32_t size, std::string name, bool function, bool prefer )
        {
            bool valid = false;
            uint64_t rva = SectionRva( image, segment, offset, valid );

            if (!valid || name.empty() || rva > 0xFFFFFFFF)
            {
                return;
            }

            auto found = by_rva.find( static_cast<uint32_t>(rva) );

            // Procedures carry a size and an undecorated name, they win over publics at the same address.
            if (found == by_rva.end() || (prefer && !found->second.m_Function))
            {
                by_rva[static_cast<uint32_t>(rva)] = Symbol { static_cast<uint32_t>(rva), size, std::move( name ), function };
            }
        };

        // Module streams. ModInfo records follow the 64 byte DBI header.
        uint32_t module_info_size = Get32( dbi, 24 );
        std::span< const uint8_t > modules = std::span< const uint8_t >( dbi ).subspan( 64, std::min< size_t >( module_info_size, dbi.size() - 64 ) );
        size_t cursor = 0;

        while (cursor + 64 <= modules.size())
        {
            uint16_t stream = Get16( modules, cursor + 34 );
            uint32_t symbol_bytes = Get32( modules, cursor + 36 );
            size_t name_offset = cursor + 64;
            std::string module_name = GetString( modules, name_offset );
            std::string object_name = GetString( modules, name_offset + module_name.size() + 1 );
            cursor = (name_offset + module_name.size() + 1 + object_name.size() + 1 + 3) & ~static_cast<size_t>(3);

            std::vector< uint8_t > symbols;

            if (stream == 0xFFFF || !msf.ReadStream( stream, symbols ) || symbols.size() < 4)
            {
                continue;
            }

            std::span< const uint8_t > records = std::span< const uint8_t >( symbols ).subspan( 4, std::min< size_t >( symbol_bytes, symbols.size() ) - 4 );

            WalkRecords( records, [&] ( uint16_t kind, std::span< const uint8_t > data )
            {
                if ((kind == S_GPROC32 || kind == S_LPROC32 || kind == S_GPROC32_ID || kind == S_LPROC32_ID) && data.size() > 35)
                {
                    // Compiler generated functions [dynamic initializers, unwind funclets] have decorated names in the PDB.
                    add( Get16( data, 32 ), Get32( data, 28 ), Get32( data, 12 ), UndecorateName( GetString( data, 35 ) ), true, true );
                    ++table->m_Functions;
                }
            } );
        }

        // Public and global data symbols.
        uint16_t record_stream = Get16( dbi, 20 );
        std::vector< uint8_t > records;

        if (record_stream != 0xFFFF && msf.ReadStream( record_stream, records ))
        {
            WalkRecords( records, [&] ( uint16_t kind, std::span< const uint8_t > data )
            {
                if (kind == S_PUB32 && data.size() > 10)
                {
                    uint32_t flags = Get32( data, 0 );
                    add( Get16( data, 8 ), Get32( data, 4 ), 0, UndecorateName( GetString( data, 10 ) ), (flags & 0x3) != 0, false );
                    ++table->m_Publics;
                }
                else if ((kind == S_GDATA32 || kind == S_LDATA32) && data.size() > 10)
                {
                    add( Get16( data, 8 ), Get32( data, 4 ), 0, GetString( data, 10 ), false, false );
                }
            } );
        }

        table->m_Symbols.reserve( by_rva.size() );

        for (auto & entry : by_rva)
        {
            table->m_Symbols.push_back( std::move( entry.second ) );
        }

        if (table->m_Symbols.empty())
        {
            error = "The PDB has no symbols for this image";
            return nullptr;
        }

        return table;
    }

    const Symbol * SymbolTable::Find(
        uint32_t rva
    ) const
    {
        auto next = std::upper_bound( m_Symbols.begin(), m_Symbols.end(), rva, [] ( uint32_t value, const Symbol & symbol )
        {
            return value < symbol.m_Rva;
        } );

        if (next == m_Symbols.begin())
        {
            return nullptr;
        }

        const Symbol & symbol = *(next - 1);

        // A sized function ends somewhere, an address after it belongs to code without a symbol.
        if (symbol.m_Size != 0 && rva >= static_cast<uint64_t>(symbol.m_Rva) + symbol.m_Size)
        {
            return nullptr;
        }

        return &symbol;
    }

    AddressNames::AddressNames(
        const PeImage & image,
        std::shared_ptr< const SymbolTable > symbols
    )
        : m_Symbols( std::move( symbols ) )
    {
        if (!image.IsPe())
        {
            return;
        }

        uint32_t pointer = image.Is64() ? 8 : 4;

        for (const ImportModule & module : ReadImports( image ))
        {
            for (size_t index = 0; index < module.m_Functions.size(); ++index)
            {
                const ImportFunction & function = module.m_Functions[index];
                uint32_t slot = static_cast<uint32_t>( module.m_FirstThunk + index * pointer );
                m_ImportSlots[slot] = module.m_Name + "!" + (function.m_ByOrdinal ? "#" + std::to_string( function.m_Ordinal ) : function.m_Name);
            }
        }

        const PeLib::ExportDirectory & exports = image.GetFile().expDir();

        for (uint32_t index = 0; index < exports.calcNumberOfFunctions(); ++index)
        {
            std::string name = exports.getFunctionName( index );

            if (!name.empty())
            {
                m_Exports.emplace( exports.getAddressOfFunction( index ), name );
            }
        }
    }

    std::string AddressNames::ImportSlot(
        uint32_t rva
    ) const
    {
        auto found = m_ImportSlots.find( rva );
        return found != m_ImportSlots.end() ? found->second : std::string();
    }

    std::string AddressNames::Describe(
        uint32_t rva,
        bool exact_only
    ) const
    {
        if (std::string slot = ImportSlot( rva ); !slot.empty())
        {
            return slot;
        }

        if (m_Symbols != nullptr)
        {
            if (const Symbol * symbol = m_Symbols->Find( rva ))
            {
                if (symbol->m_Rva == rva)
                {
                    return symbol->m_Name;
                }

                if (!exact_only)
                {
                    return symbol->m_Name + "+0x" + HexKey( rva - symbol->m_Rva, 1 );
                }
            }
        }

        auto exported = m_Exports.find( rva );
        return exported != m_Exports.end() ? exported->second : std::string();
    }

    std::vector< std::filesystem::path > PdbCandidates(
        const CodeViewInfo & code_view,
        const std::filesystem::path & image_path,
        const std::filesystem::path & cache
    )
    {
        std::vector< std::filesystem::path > candidates;

        if (!code_view.m_Present || code_view.m_PdbPath.empty())
        {
            return candidates;
        }

        std::filesystem::path name = std::filesystem::path( Utf16FromUtf8( code_view.PdbFileName() ) );
        std::string key = code_view.SearchKey();
        auto store = [&] ( const std::filesystem::path & folder )
        {
            candidates.push_back( folder / name / Utf16FromUtf8( key ) / name );
            candidates.push_back( folder / name );
        };

        candidates.emplace_back( Utf16FromUtf8( code_view.m_PdbPath ) );

        if (!image_path.empty())
        {
            candidates.push_back( image_path.parent_path() / name );
        }

        for (const char * variable : { "_NT_SYMBOL_PATH", "_NT_ALT_SYMBOL_PATH" })
        {
            for (const std::filesystem::path & folder : SymbolPathFolders( variable ))
            {
                store( folder );
            }
        }

        if (!cache.empty())
        {
            store( cache );
        }

        // Caches of Visual Studio, WinDbg and the usual hand made folders.
        if (std::string temp = GetEnvironmentText( "TEMP" ); !temp.empty())
        {
            store( std::filesystem::path( Utf16FromUtf8( temp ) ) / "SymbolCache" );
        }

        if (std::string program_data = GetEnvironmentText( "ProgramData" ); !program_data.empty())
        {
            store( std::filesystem::path( Utf16FromUtf8( program_data ) ) / "dbg" / "sym" );
        }

        store( "C:\\Symbols" );

        // Without duplicates, in order
        std::vector< std::filesystem::path > unique;
        std::set< std::filesystem::path > seen;

        for (const std::filesystem::path & candidate : candidates)
        {
            if (seen.insert( candidate.lexically_normal() ).second)
            {
                unique.push_back( candidate );
            }
        }

        return unique;
    }

    std::string SymbolServerUrl(
        const CodeViewInfo & code_view,
        const std::string & server
    )
    {
        std::string base = server.empty() ? std::string( "https://msdl.microsoft.com/download/symbols" ) : server;

        while (!base.empty() && base.back() == '/')
        {
            base.pop_back();
        }

        std::string name = code_view.PdbFileName();
        return base + "/" + name + "/" + code_view.SearchKey() + "/" + name;
    }

    std::filesystem::path SymbolStorePath(
        const CodeViewInfo & code_view,
        const std::filesystem::path & cache
    )
    {
        std::filesystem::path name( Utf16FromUtf8( code_view.PdbFileName() ) );
        return cache / name / Utf16FromUtf8( code_view.SearchKey() ) / name;
    }
}