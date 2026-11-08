#include "pecore/Resources.h"

#include "pecore/Meta.h"
#include "pecore/Operations.h"

#include "retdec/pelib/PeFile.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace PeCore
{
    namespace
    {
        constexpr uint32_t RT_CURSOR = 1;
        constexpr uint32_t RT_BITMAP = 2;
        constexpr uint32_t RT_ICON = 3;
        constexpr uint32_t RT_STRING = 6;
        constexpr uint32_t RT_GROUP_CURSOR = 12;
        constexpr uint32_t RT_GROUP_ICON = 14;
        constexpr uint32_t RT_VERSION = 16;
        constexpr uint32_t RT_HTML = 23;
        constexpr uint32_t RT_MANIFEST = 24;

        // Counted UTF-16 name of a named directory entry, offset relative to the resource directory start.
        std::string ReadEntryName(
            const PeImage & image,
            uint64_t rsrc_offset,
            uint32_t name_field
        )
        {
            uint64_t offset = rsrc_offset + (name_field & 0x7FFFFFFF);
            uint16_t length = image.Read16( offset );
            auto bytes = image.Slice( offset + 2, length * 2ull );
            return Utf8FromUtf16Bytes( bytes.data(), bytes.size(), false );
        }

        struct Identifier
        {
            uint32_t m_Id = 0;
            std::string m_Name;
            bool m_IsString = false;
        };

        Identifier ReadIdentifier(
            const PeImage & image,
            uint64_t rsrc_offset,
            const PeLib::ResourceChild * child
        )
        {
            Identifier identifier;
            uint32_t field = child->getOffsetToName();

            if (field & 0x80000000)
            {
                identifier.m_IsString = true;
                identifier.m_Name = ReadEntryName( image, rsrc_offset, field );
            }
            else
            {
                identifier.m_Id = field & 0xFFFF;
            }

            return identifier;
        }

        void AddLeaf(
            const PeImage & image,
            uint32_t rsrc_rva,
            const Identifier & type,
            const Identifier & name,
            uint32_t language,
            const PeLib::ResourceChild * child,
            bool with_data,
            std::vector< ResourceItem > & items
        )
        {
            const PeLib::ResourceLeaf * leaf = static_cast<const PeLib::ResourceLeaf *>(child->getNode());
            ResourceItem item;
            item.m_TypeId = type.m_Id;
            item.m_TypeName = type.m_Name;
            item.m_TypeIsString = type.m_IsString;
            item.m_NameId = name.m_Id;
            item.m_Name = name.m_Name;
            item.m_NameIsString = name.m_IsString;
            item.m_Language = language;
            item.m_DataRva = leaf->getOffsetToData();
            item.m_Size = leaf->getSize();
            item.m_CodePage = leaf->getCodePage();
            item.m_EntryOffset = image.RvaToOffset( rsrc_rva + (child->getOffsetToData() & 0x7FFFFFFF) );
            item.m_DataOffset = image.RvaToOffset( item.m_DataRva );

            if (with_data && item.m_DataOffset != NO_OFFSET)
            {
                auto bytes = image.Slice( item.m_DataOffset, item.m_Size );
                item.m_Data.assign( bytes.begin(), bytes.end() );
            }

            items.push_back( std::move( item ) );
        }

        const ResourceItem * FindItem(
            const std::vector< ResourceItem > & items,
            uint32_t type,
            uint32_t id
        )
        {
            for (const ResourceItem & item : items)
            {
                if (!item.m_TypeIsString && item.m_TypeId == type && !item.m_NameIsString && item.m_NameId == id)
                {
                    return &item;
                }
            }

            return nullptr;
        }

        uint16_t Get16(
            std::span< const uint8_t > data,
            size_t offset
        )
        {
            return offset + 2 <= data.size() ? static_cast<uint16_t>(data[offset] | data[offset + 1] << 8) : 0;
        }

        uint32_t Get32(
            std::span< const uint8_t > data,
            size_t offset
        )
        {
            return offset + 4 <= data.size() ? static_cast<uint32_t>(data[offset] | data[offset + 1] << 8 | data[offset + 2] << 16 | static_cast<uint32_t>(data[offset + 3]) << 24) : 0;
        }

        void Put16(
            std::vector< uint8_t > & data,
            uint16_t value
        )
        {
            data.push_back( static_cast<uint8_t>(value) );
            data.push_back( static_cast<uint8_t>(value >> 8) );
        }

        void Put32(
            std::vector< uint8_t > & data,
            uint32_t value
        )
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                data.push_back( static_cast<uint8_t>( value >> shift ) );
            }
        }

        bool StartsWith(
            const std::vector< uint8_t > & data,
            std::initializer_list< uint8_t > magic
        )
        {
            return data.size() >= magic.size() && std::equal( magic.begin(), magic.end(), data.begin() );
        }

        bool IsImageData(
            const std::vector< uint8_t > & data
        )
        {
            return StartsWith( data, { 0x89, 'P', 'N', 'G' } ) || StartsWith( data, { 'G', 'I', 'F', '8' } ) || StartsWith( data, { 0xFF, 0xD8, 0xFF } ) ||
                StartsWith( data, { 'B', 'M' } ) || StartsWith( data, { 0, 0, 1, 0 } );
        }

        bool LooksLikeText(
            const std::vector< uint8_t > & data
        )
        {
            if (data.empty())
            {
                return false;
            }

            if (StartsWith( data, { 0xEF, 0xBB, 0xBF } ) || StartsWith( data, { 0xFF, 0xFE } ) || StartsWith( data, { '<', '?', 'x', 'm', 'l' } ))
            {
                return true;
            }

            size_t printable = 0;
            size_t sample = std::min< size_t >( data.size(), 4096 );

            for (size_t index = 0; index < sample; ++index)
            {
                uint8_t byte = data[index];

                if ((byte >= 0x20 && byte < 0x7F) || byte == '\r' || byte == '\n' || byte == '\t' || byte >= 0x80)
                {
                    ++printable;
                }
            }

            return printable * 100 >= sample * 97;
        }

        std::string DecodeText(
            const std::vector< uint8_t > & data
        )
        {
            if (StartsWith( data, { 0xFF, 0xFE } ))
            {
                return Utf8FromUtf16Bytes( data.data() + 2, data.size() - 2, false );
            }

            if (StartsWith( data, { 0xEF, 0xBB, 0xBF } ))
            {
                return std::string( data.begin() + 3, data.end() );
            }

            return std::string( data.begin(), data.end() );
        }

        // Pixel size of an icon image [PNG or BITMAPINFOHEADER with the doubled height].
        void IconImageSize(
            const std::vector< uint8_t > & image,
            uint32_t & width,
            uint32_t & height,
            uint16_t & bit_count
        )
        {
            width = height = 0;
            bit_count = 32;

            if (StartsWith( image, { 0x89, 'P', 'N', 'G' } ) && image.size() >= 24)
            {
                width = static_cast<uint32_t>(image[16] << 24 | image[17] << 16 | image[18] << 8 | image[19]);
                height = static_cast<uint32_t>(image[20] << 24 | image[21] << 16 | image[22] << 8 | image[23]);
                return;
            }

            width = Get32( image, 4 );
            height = Get32( image, 8 ) / 2;
            bit_count = Get16( image, 14 );
        }

        // .ico or .cur file from a group resource and its 'RT_ICON' or 'RT_CURSOR' members.
        std::vector< uint8_t > BuildIconFile(
            const std::vector< ResourceItem > & items,
            const ResourceItem & group,
            bool cursor,
            bool as_icon
        )
        {
            std::span< const uint8_t > data( group.m_Data );
            uint16_t count = Get16( data, 4 );
            std::vector< std::vector< uint8_t > > images;
            std::vector< std::array< uint8_t, 16 > > entries;

            for (uint16_t index = 0; index < count && 6 + index * 14u + 14 <= data.size(); ++index)
            {
                size_t entry = 6 + index * 14u;
                uint16_t id = Get16( data, entry + 12 );
                const ResourceItem * member = FindItem( items, cursor ? RT_CURSOR : RT_ICON, id );

                if (member == nullptr)
                {
                    continue;
                }

                std::vector< uint8_t > bytes = member->m_Data;
                std::array< uint8_t, 16 > header {};

                if (cursor)
                {
                    // 'RT_CURSOR' data starts with the hotspot, a .cur entry stores it in the directory instead.
                    uint16_t hotspot_x = Get16( bytes, 0 );
                    uint16_t hotspot_y = Get16( bytes, 2 );
                    bytes.erase( bytes.begin(), bytes.begin() + std::min< size_t >( 4, bytes.size() ) );
                    uint32_t width = 0;
                    uint32_t height = 0;
                    uint16_t bits = 0;
                    IconImageSize( bytes, width, height, bits );
                    header[0] = static_cast<uint8_t>(width >= 256 ? 0 : width);
                    header[1] = static_cast<uint8_t>(height >= 256 ? 0 : height);
                    header[4] = static_cast<uint8_t>(as_icon ? 1 : hotspot_x);
                    header[5] = static_cast<uint8_t>(as_icon ? 0 : hotspot_x >> 8);
                    header[6] = static_cast<uint8_t>(as_icon ? bits : hotspot_y);
                    header[7] = static_cast<uint8_t>(as_icon ? bits >> 8 : hotspot_y >> 8);
                }
                else
                {
                    std::copy( data.begin() + entry, data.begin() + entry + 8, header.begin() );
                }

                uint32_t size = static_cast<uint32_t>(bytes.size());
                std::memcpy( header.data() + 8, &size, 4 );
                entries.push_back( header );
                images.push_back( std::move( bytes ) );
            }

            std::vector< uint8_t > file;
            Put16( file, 0 );
            Put16( file, cursor && !as_icon ? 2 : 1 );
            Put16( file, static_cast<uint16_t>(entries.size()) );
            uint32_t offset = static_cast<uint32_t>(6 + entries.size() * 16);

            for (size_t index = 0; index < entries.size(); ++index)
            {
                std::memcpy( entries[index].data() + 12, &offset, 4 );
                file.insert( file.end(), entries[index].begin(), entries[index].end() );
                offset += static_cast<uint32_t>( images[index].size() );
            }

            for (const auto & image : images)
            {
                file.insert( file.end(), image.begin(), image.end() );
            }

            return file;
        }

        // A lone 'RT_ICON' as a one image .ico.
        std::vector< uint8_t > BuildSingleIconFile(
            const std::vector< uint8_t > & image
        )
        {
            uint32_t width = 0;
            uint32_t height = 0;
            uint16_t bits = 0;
            IconImageSize( image, width, height, bits );
            std::vector< uint8_t > file;
            Put16( file, 0 );
            Put16( file, 1 );
            Put16( file, 1 );
            file.push_back( static_cast<uint8_t>(width >= 256 ? 0 : width) );
            file.push_back( static_cast<uint8_t>(height >= 256 ? 0 : height) );
            file.push_back( 0 );
            file.push_back( 0 );
            Put16( file, 1 );
            Put16( file, bits );
            Put32( file, static_cast<uint32_t>(image.size()) );
            Put32( file, 22 );
            file.insert( file.end(), image.begin(), image.end() );
            return file;
        }

        std::vector< uint8_t > BuildBitmapFile(
            const std::vector< uint8_t > & dib
        )
        {
            std::span< const uint8_t > data( dib );
            uint32_t header_size = Get32( data, 0 );
            uint16_t bit_count = Get16( data, 14 );
            uint32_t compression = Get32( data, 16 );
            uint32_t colors = Get32( data, 32 );

            if (colors == 0 && bit_count <= 8)
            {
                colors = 1u << bit_count;
            }

            uint32_t masks = (compression == 3 && header_size == 40) ? 12 : 0;
            uint32_t bits_offset = 14 + header_size + masks + colors * (header_size == 12 ? 3 : 4);
            std::vector< uint8_t > file;
            file.push_back( 'B' );
            file.push_back( 'M' );
            Put32( file, static_cast<uint32_t>(14 + dib.size()) );
            Put32( file, 0 );
            Put32( file, bits_offset );
            file.insert( file.end(), dib.begin(), dib.end() );
            return file;
        }

        void ParseVersionBlock(
            std::span< const uint8_t > data,
            size_t offset,
            size_t end,
            int depth,
            VersionInfo & info
        )
        {
            while (offset + 6 <= end && depth < 6)
            {
                uint16_t length = Get16( data, offset );
                uint16_t value_length = Get16( data, offset + 2 );
                uint16_t type = Get16( data, offset + 4 );

                if (length < 6 || offset + length > end)
                {
                    return;
                }

                size_t key_start = offset + 6;
                size_t key_end = key_start;

                while (key_end + 1 < offset + length && Get16( data, key_end ) != 0)
                {
                    key_end += 2;
                }

                std::string key = Utf8FromUtf16Bytes( data.data() + key_start, key_end - key_start, false );
                size_t value = (key_end + 2 + 3) & ~static_cast<size_t>(3);
                size_t block_end = offset + length;

                if (depth == 0 && value_length >= 52 && Get32( data, value ) == 0xFEEF04BD)
                {
                    auto version = [&] ( size_t at )
                    {
                        uint32_t high = Get32( data, at );
                        uint32_t low = Get32( data, at + 4 );
                        return std::to_string( high >> 16 ) + "." + std::to_string( high & 0xFFFF ) + "." + std::to_string( low >> 16 ) + "." + std::to_string( low & 0xFFFF );
                    };

                    info.m_HasFixed = true;
                    info.m_FileVersion = version( value + 8 );
                    info.m_ProductVersion = version( value + 16 );
                    info.m_Fixed.emplace_back( "FileVersion", info.m_FileVersion );
                    info.m_Fixed.emplace_back( "ProductVersion", info.m_ProductVersion );
                    info.m_Fixed.emplace_back( "FileFlagsMask", Hex( Get32( data, value + 24 ) ) );
                    info.m_Fixed.emplace_back( "FileFlags", Hex( Get32( data, value + 28 ) ) );
                    info.m_Fixed.emplace_back( "FileOS", Hex( Get32( data, value + 32 ) ) );
                    info.m_Fixed.emplace_back( "FileType", Hex( Get32( data, value + 36 ) ) );
                    info.m_Fixed.emplace_back( "FileSubtype", Hex( Get32( data, value + 40 ) ) );
                }

                size_t children = (value + (type == 1 ? value_length * 2u : value_length) + 3) & ~static_cast<size_t>(3);

                if (depth == 3 || (depth == 2 && type == 1 && value_length != 0 && key != "Translation"))
                {
                    // String value inside a 'StringTable'.
                    size_t text_end = std::min( block_end, value + value_length * 2u );
                    info.m_Strings.emplace_back( key, Utf8FromUtf16Bytes( data.data() + value, text_end > value ? text_end - value : 0, true ) );
                }
                else if (key == "Translation" && value_length >= 4)
                {
                    std::string text;

                    for (size_t at = value; at + 4 <= value + value_length && at + 4 <= block_end; at += 4)
                    {
                        text += (text.empty() ? "" : ", ") + Hex( Get16( data, at ), 4 ) + " " + Hex( Get16( data, at + 2 ), 4 ) + " (" + LanguageName( Get16( data, at ) ) + ")";
                    }

                    info.m_Fixed.emplace_back( "Translation", text );
                }
                else if (children < block_end)
                {
                    ParseVersionBlock( data, children, block_end, depth + 1, info );
                }

                offset = (block_end + 3) & ~static_cast<size_t>( 3 );
            }
        }

        // 'VS_VERSIONINFO' as a tree of blocks [wLength, wValueLength, wType, szKey, Value, Children] for editing.
        // Writing the tree back recomputes every length, so values can change size.
        struct VersionNode
        {
            std::u16string m_Key;
            uint16_t m_Type = 0;
            std::vector< uint8_t > m_Value;
            std::vector< VersionNode > m_Children;
        };

        size_t Align4(
            size_t value
        )
        {
            return (value + 3) & ~static_cast<size_t>(3);
        }

        // Parses the block at offset, returns its end or 0 when the block is invalid.
        size_t ParseVersionNode(
            std::span< const uint8_t > data,
            size_t offset,
            size_t end,
            int depth,
            VersionNode & node
        )
        {
            if (offset + 6 > end || depth > 8)
            {
                return 0;
            }

            uint16_t length = Get16( data, offset );
            uint16_t value_length = Get16( data, offset + 2 );
            uint16_t type = Get16( data, offset + 4 );

            if (length < 6 || offset + length > end)
            {
                return 0;
            }

            size_t block_end = offset + length;
            size_t key_end = offset + 6;

            while (key_end + 1 < block_end && Get16( data, key_end ) != 0)
            {
                key_end += 2;
            }

            node.m_Key.clear();

            for (size_t at = offset + 6; at < key_end; at += 2)
            {
                node.m_Key.push_back( static_cast<char16_t>( Get16( data, at ) ) );
            }

            node.m_Type = type;
            size_t value = std::min( Align4( key_end + 2 ), block_end );
            size_t value_size = std::min< size_t >( type == 1 ? value_length * 2u : value_length, block_end - value );
            size_t children = Align4( value + value_size );

            if (type == 1 && value_size != 0)
            {
                // Text values end at their terminator [some linkers count 'wValueLength' in bytes instead of characters].
                size_t text_end = value;

                while (text_end + 2 <= value + value_size && Get16( data, text_end ) != 0)
                {
                    text_end += 2;
                }

                value_size = std::min( text_end + 2, value + value_size ) - value;
            }

            node.m_Value.assign( data.begin() + value, data.begin() + value + value_size );
            node.m_Children.clear();

            while (children + 6 <= block_end)
            {
                VersionNode child;
                size_t child_end = ParseVersionNode( data, children, block_end, depth + 1, child );

                if (child_end == 0)
                {
                    break;
                }

                node.m_Children.push_back( std::move( child ) );
                children = Align4( child_end );
            }

            return block_end;
        }

        void WriteVersionNode(
            const VersionNode & node,
            std::vector< uint8_t > & out
        )
        {
            size_t start = out.size();
            Put16( out, 0 );
            Put16( out, static_cast<uint16_t>(node.m_Type == 1 ? node.m_Value.size() / 2 : node.m_Value.size()) );
            Put16( out, node.m_Type );

            for (char16_t character : node.m_Key)
            {
                Put16( out, static_cast<uint16_t>(character) );
            }

            Put16( out, 0 );
            out.resize( Align4( out.size() ), 0 );
            out.insert( out.end(), node.m_Value.begin(), node.m_Value.end() );

            for (const VersionNode & child : node.m_Children)
            {
                out.resize( Align4( out.size() ), 0 );
                WriteVersionNode( child, out );
            }

            size_t length = out.size() - start;
            Require( length <= 0xFFFF, ErrorCode::ERR_OUT_OF_RANGE, "The version information is larger than 64 KB" );
            out[start] = static_cast<uint8_t>(length);
            out[start + 1] = static_cast<uint8_t>(length >> 8);
        }

        // String values below 'StringFileInfo' with the given key, at any depth.
        void FindVersionStrings(
            VersionNode & node,
            const std::u16string & key,
            bool inside,
            std::vector< VersionNode * > & found
        )
        {
            for (VersionNode & child : node.m_Children)
            {
                if (inside && child.m_Key == key && child.m_Children.empty())
                {
                    found.push_back( &child );
                }
                else
                {
                    FindVersionStrings( child, key, inside || child.m_Key == u"StringFileInfo", found );
                }
            }
        }

        // '1.2.3.4', '1,2,3,4' or shorter forms ['2.0'], missing parts are zero.
        bool ParseVersionNumber(
            const std::string & text,
            uint32_t & high,
            uint32_t & low
        )
        {
            uint32_t parts[4] = {};
            size_t count = 0;
            size_t at = 0;

            while (at < text.size() && text[at] == ' ')
            {
                ++at;
            }

            while (at < text.size())
            {
                if (count == 4 || text[at] < '0' || text[at] > '9')
                {
                    return false;
                }

                uint32_t part = 0;

                while (at < text.size() && text[at] >= '0' && text[at] <= '9')
                {
                    part = part * 10 + static_cast<uint32_t>( text[at] - '0' );

                    if (part > 0xFFFF)
                    {
                        return false;
                    }

                    ++at;
                }

                parts[count++] = part;

                while (at < text.size() && text[at] == ' ')
                {
                    ++at;
                }

                if (at < text.size())
                {
                    if (text[at] != '.' && text[at] != ',')
                    {
                        return false;
                    }

                    ++at;

                    while (at < text.size() && text[at] == ' ')
                    {
                        ++at;
                    }

                    if (at == text.size())
                    {
                        return false;
                    }
                }
            }

            high = parts[0] << 16 | parts[1];
            low = parts[2] << 16 | parts[3];
            return count != 0;
        }

        void Set32(
            std::vector< uint8_t > & data,
            size_t offset,
            uint32_t value
        )
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                data[offset + shift / 8] = static_cast<uint8_t>( value >> shift );
            }
        }

        std::string TypeText(
            const ResourceItem & item
        )
        {
            return item.m_TypeIsString ? item.m_TypeName : ResourceTypeName( item.m_TypeId );
        }

        std::string NameText(
            const ResourceItem & item
        )
        {
            return item.m_NameIsString ? item.m_Name : std::to_string( item.m_NameId );
        }

        // Resource directory serializer helpers.
        struct NameKey
        {
            bool m_IsString = false;
            uint32_t m_Id = 0;
            std::string m_Name;

            bool operator<(
                const NameKey & other
                ) const
            {
                // Named entries first, sorted by name, then ids ascending, which is the order the loader expects.
                if (m_IsString != other.m_IsString)
                {
                    return m_IsString;
                }

                if (m_IsString)
                {
                    std::u16string left = Utf16FromUtf8( m_Name );
                    std::u16string right = Utf16FromUtf8( other.m_Name );

                    auto upper = [] ( char16_t character )
                    {
                        return character >= u'a' && character <= u'z' ? static_cast<char16_t>(character - 32) : character;
                    };

                    return std::lexicographical_compare( left.begin(), left.end(), right.begin(), right.end(),
                        [&] ( char16_t a, char16_t b )
                    {
                        return upper( a ) < upper( b );
                    } );
                }

                return m_Id < other.m_Id;
            }
        };

        NameKey TypeKey(
            const ResourceItem & item
        )
        {
            return { item.m_TypeIsString, item.m_TypeId, item.m_TypeName };
        }

        NameKey ItemKey(
            const ResourceItem & item
        )
        {
            return { item.m_NameIsString, item.m_NameId, item.m_Name };
        }

        Identifier ParseIdentifier(
            const JsonValue & command,
            const std::string & key
        )
        {
            Identifier identifier;
            const JsonValue * value = command.Find( key );
            Require( value != nullptr, ErrorCode::ERR_INVALID_ARGUMENT, "Missing " + key );
            std::string text = value->AsString();
            uint64_t number = 0;

            if (value->GetType() == JsonType::JSON_NUMBER || ParseNumber( "#" + text, number ))
            {
                identifier.m_Id = static_cast<uint32_t>(value->GetType() == JsonType::JSON_NUMBER ? value->AsUnsigned() : number);
                Require( identifier.m_Id <= 0xFFFF, ErrorCode::ERR_INVALID_ARGUMENT, "Resource ids are 16 bit" );
            }
            else
            {
                Require( !text.empty(), ErrorCode::ERR_INVALID_ARGUMENT, "Empty resource name" );
                identifier.m_IsString = true;
                identifier.m_Name = text;

                for (char & character : identifier.m_Name)
                {
                    if (character >= 'a' && character <= 'z')
                    {
                        character = static_cast<char>(character - 32);
                    }
                }
            }

            return identifier;
        }

        std::vector< uint8_t > ReadDataArgument(
            const JsonValue & command
        )
        {
            std::string path = command.GetString( "path" );
            Require( !path.empty(), ErrorCode::ERR_INVALID_ARGUMENT, "No file given" );
            return ReadWholeFile( std::filesystem::path( Utf16FromUtf8( path ) ), 256ull * 1024 * 1024 );
        }

        uint32_t NextFreeId(
            const std::vector< ResourceItem > & items,
            uint32_t type
        )
        {
            uint32_t next = 1;

            for (const ResourceItem & item : items)
            {
                if (!item.m_TypeIsString && item.m_TypeId == type && !item.m_NameIsString)
                {
                    next = std::max( next, item.m_NameId + 1 );
                }
            }

            return next;
        }

        // Splits an .ico or .cur file into 'RT_ICON' or 'RT_CURSOR' items and builds the group data.
        std::vector< uint8_t > ImportIconFile(
            std::vector< ResourceItem > & items,
            const std::vector< uint8_t > & file,
            bool cursor,
            uint32_t language
        )
        {
            std::span< const uint8_t > data( file );
            Require( file.size() >= 6 && Get16( data, 0 ) == 0 && (Get16( data, 2 ) == 1 || Get16( data, 2 ) == 2), ErrorCode::ERR_PARSE,
                "The file is not an .ico or .cur file" );
            uint16_t count = Get16( data, 4 );
            uint32_t type = cursor ? RT_CURSOR : RT_ICON;
            uint32_t next = NextFreeId( items, type );
            std::vector< uint8_t > group;
            Put16( group, 0 );
            Put16( group, cursor ? 2 : 1 );
            Put16( group, count );

            for (uint16_t index = 0; index < count; ++index)
            {
                size_t entry = 6 + index * 16u;
                Require( entry + 16 <= file.size(), ErrorCode::ERR_PARSE, "The icon file is truncated" );
                uint32_t size = Get32( data, entry + 8 );
                uint32_t offset = Get32( data, entry + 12 );
                Require( static_cast<uint64_t>( offset ) + size <= file.size(), ErrorCode::ERR_PARSE, "The icon file is truncated" );

                ResourceItem image;
                image.m_TypeId = type;
                image.m_NameId = next + index;
                image.m_Language = language;
                std::vector< uint8_t > bytes( file.begin() + offset, file.begin() + offset + size );

                if (cursor)
                {
                    // The hotspot moves from the directory into the 'RT_CURSOR' data.
                    std::vector< uint8_t > with_hotspot;
                    Put16( with_hotspot, Get16( data, entry + 4 ) );
                    Put16( with_hotspot, Get16( data, entry + 6 ) );
                    with_hotspot.insert( with_hotspot.end(), bytes.begin(), bytes.end() );
                    bytes = std::move( with_hotspot );
                }

                uint32_t width = 0;
                uint32_t height = 0;
                uint16_t bits = 0;
                IconImageSize( cursor ? std::vector< uint8_t >( bytes.begin() + 4, bytes.end() ) : bytes, width, height, bits );

                if (cursor)
                {
                    Put16( group, static_cast<uint16_t>(width) );
                    Put16( group, static_cast<uint16_t>(height * 2) );
                    Put16( group, 1 );
                    Put16( group, bits );
                }
                else
                {
                    group.insert( group.end(), file.begin() + entry, file.begin() + entry + 8 );
                }

                Put32( group, static_cast<uint32_t>(bytes.size()) );
                Put16( group, static_cast<uint16_t>(next + index) );
                image.m_Data = std::move( bytes );
                image.m_Size = static_cast<uint32_t>(image.m_Data.size());
                items.push_back( std::move( image ) );
            }

            return group;
        }

        void RemoveGroupMembers(
            std::vector< ResourceItem > & items,
            const ResourceItem & group
        )
        {
            bool cursor = !group.m_TypeIsString && group.m_TypeId == RT_GROUP_CURSOR;
            std::span< const uint8_t > data( group.m_Data );
            uint16_t count = Get16( data, 4 );

            for (uint16_t index = 0; index < count; ++index)
            {
                uint16_t id = Get16( data, 6 + index * 14u + 12 );
                std::erase_if( items, [&] ( const ResourceItem & item )
                {
                    return !item.m_TypeIsString && item.m_TypeId == (cursor ? RT_CURSOR : RT_ICON) && !item.m_NameIsString && item.m_NameId == id;
                } );
            }
        }

        std::string Answer(
            const std::string & message
        )
        {
            JsonWriter writer;
            writer.BeginObject();
            writer.MemberBool( "ok", true );
            writer.Member( "message", message );
            writer.Member( "select", "resources" );
            writer.EndObject();
            return writer.TakeText();
        }
    }

    std::vector< ResourceItem > EnumerateResources(
        const PeImage & image,
        bool with_data
    )
    {
        std::vector< ResourceItem > items;

        if (!image.IsPe() || !image.HasDirectory( DIR_RESOURCE ))
        {
            return items;
        }

        uint32_t rsrc_rva = image.GetDirectory( DIR_RESOURCE ).m_Rva;
        uint64_t rsrc_offset = image.RvaToOffset( rsrc_rva );

        if (rsrc_offset == NO_OFFSET)
        {
            return items;
        }

        const PeLib::ResourceNode * root = image.GetFile().resDir().getRoot();

        for (unsigned int type_index = 0; root != nullptr && type_index < root->getNumberOfChildren(); ++type_index)
        {
            const PeLib::ResourceChild * type_child = root->getChild( type_index );
            Identifier type = ReadIdentifier( image, rsrc_offset, type_child );

            if (type_child->getNode() == nullptr || type_child->getNode()->isLeaf())
            {
                continue;
            }

            const PeLib::ResourceNode * names = static_cast<const PeLib::ResourceNode *>( type_child->getNode() );

            for (unsigned int name_index = 0; name_index < names->getNumberOfChildren(); ++name_index)
            {
                const PeLib::ResourceChild * name_child = names->getChild( name_index );
                Identifier name = ReadIdentifier( image, rsrc_offset, name_child );

                if (name_child->getNode() == nullptr)
                {
                    continue;
                }

                // Malformed two level trees put the data right below the name.
                if (name_child->getNode()->isLeaf())
                {
                    AddLeaf( image, rsrc_rva, type, name, 0, name_child, with_data, items );
                    continue;
                }

                const PeLib::ResourceNode * languages = static_cast<const PeLib::ResourceNode *>(name_child->getNode());

                for (unsigned int language_index = 0; language_index < languages->getNumberOfChildren(); ++language_index)
                {
                    const PeLib::ResourceChild * language_child = languages->getChild( language_index );

                    if (language_child->getNode() == nullptr || !language_child->getNode()->isLeaf())
                    {
                        continue;
                    }

                    AddLeaf( image, rsrc_rva, type, name, language_child->getOffsetToName() & 0xFFFF, language_child, with_data, items );
                }
            }
        }

        return items;
    }

    std::string ResourcePath(
        const ResourceItem & item
    )
    {
        return TypeText( item ) + " / " + NameText( item ) + " / " + std::to_string( item.m_Language );
    }

    std::string ResourcePreviewKind(
        const ResourceItem & item
    )
    {
        if (!item.m_TypeIsString)
        {
            switch (item.m_TypeId)
            {
                case RT_GROUP_ICON:
                case RT_ICON:
                    return "icon";
                case RT_GROUP_CURSOR:
                case RT_CURSOR:
                    return "cursor";
                case RT_BITMAP:
                    return "image";
                case RT_VERSION:
                    return "version";
                case RT_STRING:
                    return "strings";
                case RT_MANIFEST:
                case RT_HTML:
                    return "text";
                default:
                    break;
            }
        }

        if (IsImageData( item.m_Data ))
        {
            return "image";
        }

        if (LooksLikeText( item.m_Data ))
        {
            return "text";
        }

        return "hex";
    }

    VersionInfo DecodeVersionInfo(
        std::span< const uint8_t > data
    )
    {
        VersionInfo info;
        ParseVersionBlock( data, 0, data.size(), 0, info );
        return info;
    }

    VersionInfo ReadVersionInfo(
        const PeImage & image
    )
    {
        std::vector< ResourceItem > items = EnumerateResources( image, true );

        for (size_t index = 0; index < items.size(); ++index)
        {
            if (!items[index].m_TypeIsString && items[index].m_TypeId == RT_VERSION)
            {
                VersionInfo info = DecodeVersionInfo( items[index].m_Data );
                info.m_Resource = index;
                return info;
            }
        }

        return VersionInfo();
    }

    std::vector< uint8_t > EditVersionInfo(
        std::span< const uint8_t > data,
        const std::string & target,
        const std::string & text,
        uint64_t number
    )
    {
        VersionNode root;
        Require( ParseVersionNode( data, 0, data.size(), 0, root ) != 0 && root.m_Key == u"VS_VERSION_INFO", ErrorCode::ERR_PARSE,
            "The version information cannot be parsed" );

        if (target.rfind( "fixed.", 0 ) == 0)
        {
            std::string field = target.substr( 6 );
            Require( root.m_Value.size() >= 52 && Get32( root.m_Value, 0 ) == 0xFEEF04BD, ErrorCode::ERR_NOT_FOUND,
                "The version information has no fixed part" );

            if (field == "FileVersion" || field == "ProductVersion")
            {
                uint32_t high = 0;
                uint32_t low = 0;
                Require( ParseVersionNumber( text, high, low ), ErrorCode::ERR_INVALID_ARGUMENT,
                    "\"" + text + "\" is not a version. Use up to four numbers from 0 to 65535, such as 1.2.3.4." );
                size_t offset = field == "FileVersion" ? 8 : 16;
                Set32( root.m_Value, offset, high );
                Set32( root.m_Value, offset + 4, low );
            }
            else
            {
                static const std::pair< const char *, size_t > FIELDS[] = {
                    { "FileFlagsMask", 24 }, { "FileFlags", 28 }, { "FileOS", 32 }, { "FileType", 36 }, { "FileSubtype", 40 }
                };
                const auto * entry = std::find_if( std::begin( FIELDS ), std::end( FIELDS ), [&] ( const auto & item )
                {
                    return field == item.first;
                } );
                Require( entry != std::end( FIELDS ), ErrorCode::ERR_INVALID_ARGUMENT, "Unknown version field " + field );
                Require( number <= 0xFFFFFFFF, ErrorCode::ERR_OUT_OF_RANGE, "The value does not fit into a dword" );
                Set32( root.m_Value, entry->second, static_cast<uint32_t>(number) );
            }
        }
        else if (target.rfind( "string.", 0 ) == 0)
        {
            std::string key = target.substr( 7 );
            std::vector< VersionNode * > found;
            FindVersionStrings( root, Utf16FromUtf8( key ), false, found );
            Require( !found.empty(), ErrorCode::ERR_NOT_FOUND, "The version information has no string " + key );
            std::u16string value = Utf16FromUtf8( text );

            for (VersionNode * node : found)
            {
                node->m_Type = 1;
                node->m_Value.clear();

                for (char16_t character : value)
                {
                    Put16( node->m_Value, static_cast<uint16_t>(character) );
                }

                Put16( node->m_Value, 0 );
            }
        }
        else
        {
            throw Error( ErrorCode::ERR_INVALID_ARGUMENT, "Unknown version field " + target );
        }

        std::vector< uint8_t > out;
        WriteVersionNode( root, out );
        return out;
    }

    std::vector< uint8_t > ExportResource(
        const PeImage & image,
        const std::string & path,
        bool preview,
        std::string & extension
    )
    {
        std::vector< ResourceItem > items = EnumerateResources( image, true );
        uint64_t index = 0;
        Require( ParseNumber( "#" + path, index ) && index < items.size(), ErrorCode::ERR_NOT_FOUND, "No such resource" );
        const ResourceItem & item = items[index];
        extension = ".bin";

        if (!item.m_TypeIsString)
        {
            switch (item.m_TypeId)
            {
                case RT_GROUP_ICON:
                    extension = ".ico";
                    return BuildIconFile( items, item, false, true );
                case RT_GROUP_CURSOR:
                    extension = preview ? ".ico" : ".cur";
                    return BuildIconFile( items, item, true, preview );
                case RT_CURSOR:
                    extension = ".ico";
                    return BuildSingleIconFile( std::vector< uint8_t >( item.m_Data.begin() + std::min< size_t >( 4, item.m_Data.size() ), item.m_Data.end() ) );
                case RT_ICON:
                    extension = ".ico";
                    return BuildSingleIconFile( item.m_Data );
                case RT_BITMAP:
                    extension = ".bmp";
                    return BuildBitmapFile( item.m_Data );
                case RT_MANIFEST:
                    extension = ".manifest";
                    return item.m_Data;
                case RT_HTML:
                    extension = ".html";
                    return item.m_Data;
                default:
                    break;
            }
        }

        if (StartsWith( item.m_Data, { 0x89, 'P', 'N', 'G' } ))
        {
            extension = ".png";
        }
        else if (StartsWith( item.m_Data, { 0xFF, 0xD8, 0xFF } ))
        {
            extension = ".jpg";
        }
        else if (StartsWith( item.m_Data, { 'G', 'I', 'F', '8' } ))
        {
            extension = ".gif";
        }
        else if (LooksLikeText( item.m_Data ))
        {
            extension = ".txt";
        }

        return item.m_Data;
    }

    std::string BuildResourceView(
        const ViewContext & context,
        const std::string & title
    )
    {
        const PeImage & image = context.m_Image;
        ViewBuilder view( "resources", title, "resources" );
        std::vector< ResourceItem > items = EnumerateResources( image, true );
        JsonWriter & writer = view.GetWriter();
        writer.Key( "resources" ).BeginArray();

        for (size_t index = 0; index < items.size(); ++index)
        {
            const ResourceItem & item = items[index];
            writer.BeginObject();
            writer.Member( "path", std::to_string( index ) );
            writer.Member( "type", item.m_TypeIsString ? "\"" + item.m_TypeName + "\"" : std::to_string( item.m_TypeId ) );
            writer.Member( "type_name", TypeText( item ) );
            writer.Member( "name", NameText( item ) );
            writer.MemberUnsigned( "language", item.m_Language );
            writer.Member( "language_name", LanguageName( item.m_Language ) );
            writer.Member( "rva", Hex( item.m_DataRva ) );
            writer.MemberUnsigned( "size", item.m_Size );

            if (item.m_DataOffset != NO_OFFSET)
            {
                writer.MemberUnsigned( "offset", item.m_DataOffset );
            }

            if (item.m_EntryOffset != NO_OFFSET)
            {
                writer.MemberUnsigned( "entry_offset", item.m_EntryOffset );
            }

            writer.MemberUnsigned( "codepage", item.m_CodePage );
            writer.Member( "preview", ResourcePreviewKind( item ) );
            writer.EndObject();
        }

        writer.EndArray();
        view.BeginCommands();
        view.Command( "resources.add", "Add resource (raw)...", "view" );
        view.Command( "resources.add_icon", "Add icon...", "view" );
        view.Command( "resources.add_bitmap", "Add bitmap...", "view" );
        view.Command( "resources.replace", "Replace resource (raw)...", "row" );
        view.Command( "resources.replace_icon", "Replace icon...", "row" );
        view.Command( "resources.remove", "Remove resource", "row" );
        view.Command( "resources.save", "Save resource...", "row" );
        view.Command( "resources.import", "Import resources from a PE file...", "view" );
        view.EndCommands();
        view.Warnings( image.GetWarnings() );
        return view.Finish();
    }

    std::string BuildResourceItemView(
        const ViewContext & context,
        const std::string & path
    )
    {
        const PeImage & image = context.m_Image;
        std::vector< ResourceItem > items = EnumerateResources( image, true );
        uint64_t index = 0;
        Require( ParseNumber( "#" + path, index ) && index < items.size(), ErrorCode::ERR_NOT_FOUND, "No such resource" );
        const ResourceItem & item = items[index];
        std::string kind = ResourcePreviewKind( item );
        ViewBuilder view( "resources/" + path, ResourcePath( item ), "fields" );
        view.Property( "preview", kind );
        view.BeginFields();

        if (item.m_EntryOffset != NO_OFFSET)
        {
            view.FieldAt( image, "OffsetToData", item.m_EntryOffset, 4, image.SectionNameOfRva( item.m_DataRva ) );
            view.FieldAt( image, "Size", item.m_EntryOffset + 4, 4, FormatBytes( item.m_Size ) );
            view.FieldAt( image, "CodePage", item.m_EntryOffset + 8, 4 );
            view.FieldAt( image, "Reserved", item.m_EntryOffset + 12, 4 );
        }

        view.InfoField( "Language", std::to_string( item.m_Language ) + " (" + LanguageName( item.m_Language ) + ")" );
        view.EndFields();

        if (kind == "version")
        {
            VersionInfo info = DecodeVersionInfo( item.m_Data );
            std::string prefix = path + ":";
            view.Columns( { { "Name", "" }, { "Value", "" } } );
            view.BeginRows();

            for (const auto & pair : info.m_Fixed)
            {
                view.BeginRow();
                view.Cell( pair.first );

                if (pair.first == "FileVersion" || pair.first == "ProductVersion")
                {
                    view.CommandCell( pair.second, 0, "resources.set_version", prefix + "fixed." + pair.first, Editor::EDITOR_TEXT );
                }
                else if (pair.first == "Translation")
                {
                    view.Cell( pair.second );
                }
                else
                {
                    view.CommandCell( pair.second, 4, "resources.set_version", prefix + "fixed." + pair.first, Editor::EDITOR_HEX );
                }

                view.EndRow();
            }

            for (const auto & pair : info.m_Strings)
            {
                view.BeginRow();
                view.Cell( pair.first );
                view.CommandCell( pair.second, 0, "resources.set_version", prefix + "string." + pair.first, Editor::EDITOR_TEXT );
                view.EndRow();
            }

            view.EndRows();
        }
        else if (kind == "strings" && !item.m_NameIsString)
        {
            view.Columns( { { "ID", "String id" }, { "String", "" } } );
            view.BeginRows();
            std::span< const uint8_t > data( item.m_Data );
            size_t offset = 0;

            for (uint32_t slot = 0; slot < 16 && offset + 2 <= data.size(); ++slot)
            {
                uint16_t length = Get16( data, offset );
                std::string text = Utf8FromUtf16Bytes( data.data() + offset + 2, std::min< size_t >( length * 2u, data.size() - offset - 2 ), false );
                uint64_t cell = item.m_DataOffset != NO_OFFSET ? item.m_DataOffset + offset : NO_OFFSET;
                offset += 2 + length * 2u;

                if (length == 0)
                {
                    continue;
                }

                view.BeginRow( std::string(), cell, 2 + length * 2u );
                view.Cell( std::to_string( (item.m_NameId - 1) * 16 + slot ) );
                view.Cell( text );
                view.EndRow();
            }

            view.EndRows();
        }
        else if (kind == "text")
        {
            view.Property( "text", DecodeText( item.m_Data ) );
        }

        if (item.m_DataOffset != NO_OFFSET)
        {
            view.HexRegion( item.m_DataOffset, item.m_Data.size(), "Resource data" );
        }

        return view.Finish();
    }

    std::vector< uint8_t > BuildResourceSection(
        const std::vector< ResourceItem > & items,
        uint32_t rsrc_rva
    )
    {
        // type, name, language, item index
        std::map< NameKey, std::map< NameKey, std::map< uint32_t, size_t > > > tree;

        for (size_t index = 0; index < items.size(); ++index)
        {
            tree[TypeKey( items[index] )][ItemKey( items[index] )][items[index].m_Language] = index;
        }

        // Sizes of the three parts. Directory tables, data entries, strings, then the data.
        uint32_t tables = 16 + static_cast<uint32_t>( tree.size() ) * 8;
        uint32_t strings = 0;
        size_t leaf_count = 0;

        for (const auto & type : tree)
        {
            tables += 16 + static_cast<uint32_t>(type.second.size()) * 8;
            strings += type.first.m_IsString ? 2 + static_cast<uint32_t>(Utf16FromUtf8( type.first.m_Name ).size()) * 2 : 0;

            for (const auto & name : type.second)
            {
                tables += 16 + static_cast<uint32_t>(name.second.size()) * 8;
                strings += name.first.m_IsString ? 2 + static_cast<uint32_t>(Utf16FromUtf8( name.first.m_Name ).size()) * 2 : 0;
                leaf_count += name.second.size();
            }
        }

        uint32_t entries_start = tables;
        uint32_t strings_start = entries_start + static_cast<uint32_t>(leaf_count) * 16;
        uint32_t data_start = AlignUp( strings_start + strings, 8 );
        uint32_t data_size = 0;

        for (const ResourceItem & item : items)
        {
            data_size = AlignUp( data_size + static_cast<uint32_t>(item.m_Data.size()), 8 );
        }

        std::vector< uint8_t > out( data_start + data_size, 0 );
        uint32_t table_cursor = 0;
        uint32_t entry_cursor = entries_start;
        uint32_t string_cursor = strings_start;
        uint32_t data_cursor = data_start;

        auto write16 = [&] ( uint32_t at, uint16_t value )
        {
            out[at] = static_cast<uint8_t>(value); out[at + 1] = static_cast<uint8_t>(value >> 8);
        };

        auto write32 = [&] ( uint32_t at, uint32_t value )
        {
            write16( at, static_cast<uint16_t>(value) ); write16( at + 2, static_cast<uint16_t>(value >> 16) );
        };

        auto write_table = [&] ( uint16_t named, uint16_t ids ) -> uint32_t
        {
            uint32_t at = table_cursor;
            write16( at + 12, named );
            write16( at + 14, ids );
            table_cursor += 16 + (named + ids) * 8u;
            return at + 16;
        };

        auto write_name = [&] ( const NameKey & key ) -> uint32_t
        {
            if (!key.m_IsString)
            {
                return key.m_Id;
            }

            std::u16string text = Utf16FromUtf8( key.m_Name );
            uint32_t at = string_cursor;
            write16( at, static_cast<uint16_t>(text.size()) );

            for (size_t character = 0; character < text.size(); ++character)
            {
                write16( at + 2 + static_cast<uint32_t>( character ) * 2, text[character] );
            }

            string_cursor += 2 + static_cast<uint32_t>( text.size() ) * 2;
            return 0x80000000 | at;
        };

        auto count_named = [] ( const auto & map )
        {
            uint16_t named = 0;

            for (const auto & pair : map)
            {
                named += pair.first.m_IsString ? 1 : 0;
            }

            return named;
        };

        // Level 1. The child tables are laid out breadth first right after their parent table.
        uint16_t type_named = count_named( tree );
        uint32_t type_entries = write_table( type_named, static_cast<uint16_t>(tree.size() - type_named) );
        uint32_t type_slot = 0;

        for (const auto & type : tree)
        {
            uint32_t entry = type_entries + type_slot++ * 8;
            write32( entry, write_name( type.first ) );
            write32( entry + 4, 0x80000000 | table_cursor );
            uint16_t name_named = count_named( type.second );
            uint32_t name_entries = write_table( name_named, static_cast<uint16_t>(type.second.size() - name_named) );
            uint32_t name_slot = 0;

            for (const auto & name : type.second)
            {
                uint32_t name_entry = name_entries + name_slot++ * 8;
                write32( name_entry, write_name( name.first ) );
                write32( name_entry + 4, 0x80000000 | table_cursor );
                uint32_t language_entries = write_table( 0, static_cast<uint16_t>(name.second.size()) );
                uint32_t language_slot = 0;

                for (const auto & language : name.second)
                {
                    const ResourceItem & item = items[language.second];
                    uint32_t language_entry = language_entries + language_slot++ * 8;
                    write32( language_entry, language.first );
                    write32( language_entry + 4, entry_cursor );

                    // IMAGE_RESOURCE_DATA_ENTRY with the RVA of the data
                    write32( entry_cursor, rsrc_rva + data_cursor );
                    write32( entry_cursor + 4, static_cast<uint32_t>(item.m_Data.size()) );
                    write32( entry_cursor + 8, item.m_CodePage );
                    entry_cursor += 16;

                    std::copy( item.m_Data.begin(), item.m_Data.end(), out.begin() + data_cursor );
                    data_cursor = AlignUp( data_cursor + static_cast<uint32_t>(item.m_Data.size()), 8 );
                }
            }
        }

        return out;
    }

    std::string WriteResources(
        std::vector< uint8_t > & data,
        const std::vector< ResourceItem > & items
    )
    {
        PeImage image( data );
        Require( image.IsPe(), ErrorCode::ERR_NOT_PE, "The file is not a PE image" );
        uint32_t dir_offset = image.GetDataDirectoryOffset() + DIR_RESOURCE * 8;
        Require( image.GetNumberOfRvaAndSizes() > DIR_RESOURCE, ErrorCode::ERR_UNSUPPORTED, "The optional header has no resource directory entry" );
        DataDirectory current = image.GetDirectory( DIR_RESOURCE );
        size_t section_index = current.m_Rva != 0 ? image.FindSectionByRva( current.m_Rva ) : NO_SECTION;

        // The old section is reused when it starts with the resources and holds nothing else
        bool exclusive = false;

        if (section_index != NO_SECTION)
        {
            const PeLib::PELIB_SECTION_HEADER * section = image.GetLoader().getSectionHeader( section_index );
            exclusive = section->VirtualAddress == current.m_Rva;

            for (uint32_t directory = 0; directory < DIR_COUNT && exclusive; ++directory)
            {
                DataDirectory other = image.GetDirectory( directory );

                if (directory != DIR_RESOURCE && directory != DIR_SECURITY && other.m_Rva != 0 && image.FindSectionByRva( other.m_Rva ) == section_index)
                {
                    exclusive = false;
                }
            }

            if (exclusive && image.GetEntryPoint() != 0 && image.FindSectionByRva( image.GetEntryPoint() ) == section_index)
            {
                exclusive = false;
            }
        }

        if (exclusive)
        {
            const PeLib::PELIB_SECTION_HEADER * section = image.GetLoader().getSectionHeader( section_index );
            std::vector< uint8_t > built = BuildResourceSection( items, section->VirtualAddress );
            uint32_t next_virtual = UINT32_MAX;

            for (size_t index = 0; index < image.GetSectionCount(); ++index)
            {
                uint32_t address = image.GetLoader().getSectionHeader( index )->VirtualAddress;

                if (address > section->VirtualAddress)
                {
                    next_virtual = std::min( next_virtual, address );
                }
            }

            bool fits_raw = built.size() <= section->SizeOfRawData;
            bool fits_virtual = next_virtual == UINT32_MAX || section->VirtualAddress + built.size() <= next_virtual;

            if (fits_raw && fits_virtual)
            {
                uint64_t raw = section->PointerToRawData;
                std::fill( data.begin() + static_cast<ptrdiff_t>(raw), data.begin() + static_cast<ptrdiff_t>(raw + section->SizeOfRawData), 0 );
                std::copy( built.begin(), built.end(), data.begin() + static_cast<ptrdiff_t>(raw) );
                uint64_t header = image.GetSectionHeaderOffset( section_index );
                Poke( data, header + 8, std::max< uint64_t >( section->VirtualSize, built.size() ), 4 );
                Poke( data, dir_offset, section->VirtualAddress, 4 );
                Poke( data, dir_offset + 4, built.size(), 4 );
                return "Resources rebuilt in place in section " + image.GetSectionName( section_index ) + " (" + FormatBytes( built.size() ) + ")";
            }
        }

        // New section at the end of the image, the data entries hold RVAs so the address is predicted first.
        NewSection section;
        section.m_Name = ".rsrc";
        section.m_Characteristics = 0x40000040;

        uint32_t predicted = 0;
        {
            uint32_t alignment = image.GetSectionAlignment();
            predicted = AlignUp( image.GetSizeOfHeaders(), alignment );

            for (size_t index = 0; index < image.GetSectionCount(); ++index)
            {
                const PeLib::PELIB_SECTION_HEADER * header = image.GetLoader().getSectionHeader( index );
                predicted = std::max( predicted, AlignUp( header->VirtualAddress + std::max( header->VirtualSize, header->SizeOfRawData ), alignment ) );
            }
        }

        section.m_Data = BuildResourceSection( items, predicted );
        uint32_t size = static_cast<uint32_t>( section.m_Data.size() );
        uint32_t rva = AppendSection( data, section );

        if (rva != predicted)
        {
            // Headers grew and moved the virtual end, rebuild for the real address.
            PeImage updated( data );
            std::vector< uint8_t > rebuilt = BuildResourceSection( items, rva );
            uint64_t raw = updated.GetLoader().getSectionHeader( updated.GetSectionCount() - 1 )->PointerToRawData;
            std::copy( rebuilt.begin(), rebuilt.end(), data.begin() + static_cast<ptrdiff_t>(raw) );
        }

        PeImage updated( data );
        dir_offset = updated.GetDataDirectoryOffset() + DIR_RESOURCE * 8;
        Poke( data, dir_offset, rva, 4 );
        Poke( data, dir_offset + 4, size, 4 );
        return "Resources rebuilt in a new .rsrc section at RVA " + Hex( rva ) + " (" + FormatBytes( size ) + ")" +
            (section_index != NO_SECTION ? ", the old resource data is no longer referenced" : "");
    }

    std::string ExecuteResourceCommand(
        Document & document,
        const JsonValue & command
    )
    {
        std::string id = command.GetString( "command" );
        PeImage image( document.GetData() );
        Require( image.IsPe(), ErrorCode::ERR_NOT_PE, "The file is not a PE image" );
        std::vector< ResourceItem > items = EnumerateResources( image, true );
        uint32_t language = static_cast<uint32_t>(command.GetUnsigned( "language", 1033 ));

        auto selected = [&] () -> ResourceItem &
        {
            uint64_t index = command.GetUnsigned( "index", UINT64_MAX );
            Require( index < items.size(), ErrorCode::ERR_NOT_FOUND, "No such resource" );
            return items[index];
        };

        std::string summary;

        if (id == "resources.replace")
        {
            ResourceItem & item = selected();
            item.m_Data = ReadDataArgument( command );
            summary = "Replaced " + ResourcePath( item );
        }
        else if (id == "resources.remove")
        {
            ResourceItem item = selected();

            if (!item.m_TypeIsString && (item.m_TypeId == RT_GROUP_ICON || item.m_TypeId == RT_GROUP_CURSOR))
            {
                RemoveGroupMembers( items, item );
            }

            std::erase_if( items, [&] ( const ResourceItem & other )
            {
                return other.m_TypeIsString == item.m_TypeIsString && other.m_TypeId == item.m_TypeId && other.m_TypeName == item.m_TypeName &&
                    other.m_NameIsString == item.m_NameIsString && other.m_NameId == item.m_NameId && other.m_Name == item.m_Name && other.m_Language == item.m_Language;
            } );
            summary = "Removed " + ResourcePath( item );
        }
        else if (id == "resources.add")
        {
            Identifier type = ParseIdentifier( command, "type" );
            Identifier name = ParseIdentifier( command, "name" );
            ResourceItem item;
            item.m_TypeId = type.m_Id;
            item.m_TypeName = type.m_Name;
            item.m_TypeIsString = type.m_IsString;
            item.m_NameId = name.m_Id;
            item.m_Name = name.m_Name;
            item.m_NameIsString = name.m_IsString;
            item.m_Language = language;
            item.m_Data = ReadDataArgument( command );
            summary = "Added " + ResourcePath( item );

            // Same type, name and language replaces the existing resource.
            std::erase_if( items, [&] ( const ResourceItem & other )
            {
                return other.m_TypeIsString == item.m_TypeIsString && other.m_TypeId == item.m_TypeId && other.m_TypeName == item.m_TypeName &&
                    other.m_NameIsString == item.m_NameIsString && other.m_NameId == item.m_NameId && other.m_Name == item.m_Name && other.m_Language == item.m_Language;
            } );
            items.push_back( std::move( item ) );
        }
        else if (id == "resources.add_icon" || id == "resources.replace_icon")
        {
            std::vector< uint8_t > file = ReadDataArgument( command );
            bool cursor = !file.empty() && file.size() >= 4 && file[2] == 2;
            ResourceItem group;
            group.m_TypeId = cursor ? RT_GROUP_CURSOR : RT_GROUP_ICON;
            group.m_Language = language;

            if (id == "resources.replace_icon")
            {
                ResourceItem old = selected();
                Require( !old.m_TypeIsString && (old.m_TypeId == RT_GROUP_ICON || old.m_TypeId == RT_GROUP_CURSOR), ErrorCode::ERR_INVALID_ARGUMENT,
                    "Select an icon group or cursor group to replace" );
                RemoveGroupMembers( items, old );
                group.m_TypeId = old.m_TypeId;
                group.m_NameId = old.m_NameId;
                group.m_Name = old.m_Name;
                group.m_NameIsString = old.m_NameIsString;
                group.m_Language = old.m_Language;
                std::erase_if( items, [&] ( const ResourceItem & other )
                {
                    return !other.m_TypeIsString && other.m_TypeId == old.m_TypeId && other.m_NameIsString == old.m_NameIsString &&
                        other.m_NameId == old.m_NameId && other.m_Name == old.m_Name && other.m_Language == old.m_Language;
                } );
            }
            else if (command.Has( "name" ))
            {
                Identifier name = ParseIdentifier( command, "name" );
                group.m_NameId = name.m_Id;
                group.m_Name = name.m_Name;
                group.m_NameIsString = name.m_IsString;
            }
            else
            {
                group.m_NameId = NextFreeId( items, group.m_TypeId );
            }

            group.m_Data = ImportIconFile( items, file, group.m_TypeId == RT_GROUP_CURSOR, group.m_Language );
            summary = (id == "resources.add_icon" ? "Added " : "Replaced ") + ResourcePath( group );
            items.push_back( std::move( group ) );
        }
        else if (id == "resources.add_bitmap")
        {
            std::vector< uint8_t > file = ReadDataArgument( command );
            Require( file.size() > 14 && file[0] == 'B' && file[1] == 'M', ErrorCode::ERR_PARSE, "The file is not a .bmp file" );
            ResourceItem item;
            item.m_TypeId = RT_BITMAP;
            item.m_Language = language;

            if (command.Has( "name" ))
            {
                Identifier name = ParseIdentifier( command, "name" );
                item.m_NameId = name.m_Id;
                item.m_Name = name.m_Name;
                item.m_NameIsString = name.m_IsString;
            }
            else
            {
                item.m_NameId = NextFreeId( items, RT_BITMAP );
            }

            // 'RT_BITMAP' is the DIB without 'BITMAPFILEHEADER'.
            item.m_Data.assign( file.begin() + 14, file.end() );
            summary = "Added " + ResourcePath( item );
            items.push_back( std::move( item ) );
        }
        else if (id == "resources.set_version")
        {
            // From a command cell. "argument": "<index>:<target>", "value": text or number.
            std::string argument = command.GetString( "argument" );
            size_t colon = argument.find( ':' );
            uint64_t index = UINT64_MAX;
            Require( colon != std::string::npos && ParseNumber( "#" + argument.substr( 0, colon ), index ) && index < items.size(),
                ErrorCode::ERR_INVALID_ARGUMENT, "Invalid version information cell " + argument );
            ResourceItem & item = items[index];
            Require( !item.m_TypeIsString && item.m_TypeId == RT_VERSION, ErrorCode::ERR_INVALID_ARGUMENT, "The resource is not version information" );
            std::string target = argument.substr( colon + 1 );
            item.m_Data = EditVersionInfo( item.m_Data, target, command.GetString( "value" ), command.GetUnsigned( "value", UINT64_MAX ) );
            std::string name = target.substr( target.find( '.' ) + 1 );
            summary = "Version information: " + name + (target.rfind( "fixed.", 0 ) == 0 ? " (fixed)" : "") + " changed";
        }
        else if (id == "resources.import")
        {
            std::vector< uint8_t > other_bytes = ReadDataArgument( command );
            PeImage other( other_bytes );
            Require( other.IsPe(), ErrorCode::ERR_NOT_PE, "The source file is not a PE image" );
            std::vector< ResourceItem > imported = EnumerateResources( other, true );
            Require( !imported.empty(), ErrorCode::ERR_NOT_FOUND, "The source file has no resources" );

            if (command.GetBool( "replace" ))
            {
                items = std::move( imported );
                summary = "Resources replaced with the ones of the source file";
            }
            else
            {
                for (ResourceItem & item : imported)
                {
                    std::erase_if( items, [&] ( const ResourceItem & existing )
                    {
                        return existing.m_TypeIsString == item.m_TypeIsString && existing.m_TypeId == item.m_TypeId && existing.m_TypeName == item.m_TypeName &&
                            existing.m_NameIsString == item.m_NameIsString && existing.m_NameId == item.m_NameId && existing.m_Name == item.m_Name &&
                            existing.m_Language == item.m_Language;
                    } );
                    items.push_back( std::move( item ) );
                }

                summary = "Resources of the source file merged";
            }
        }
        else
        {
            throw Error( ErrorCode::ERR_NOT_FOUND, "Unknown command " + id );
        }

        std::vector< uint8_t > data = document.GetData();
        std::string written = WriteResources( data, items );
        document.SetData( std::move( data ), summary );
        return Answer( summary + ". " + written );
    }
}