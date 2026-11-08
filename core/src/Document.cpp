#include "pecore/Document.h"

#include "pecore/Platform.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

namespace PeCore
{
    std::vector< uint8_t > ReadWholeFile(
        const std::filesystem::path & path,
        uint64_t size_limit
    )
    {
        std::error_code error;
        uint64_t size = std::filesystem::file_size( path, error );

        if (error)
        {
            throw Error( ErrorCode::ERR_IO, "Cannot open " + path.filename().string() + ": " + error.message() );
        }

        if (size_limit != 0 && size > size_limit)
        {
            throw Error( ErrorCode::ERR_OUT_OF_RANGE, "The file is larger than the configured limit (" + FormatBytes( size_limit ) + ")" );
        }

        return ReadFileContents( path, size );
    }

    void WriteWholeFile(
        const std::filesystem::path & path,
        std::span< const uint8_t > data
    )
    {
        std::ofstream stream( path, std::ios::binary | std::ios::trunc );

        if (!stream)
        {
            throw Error( ErrorCode::ERR_ACCESS_DENIED, "Cannot open " + path.filename().string() + " for writing" );
        }

        if (!data.empty() && !stream.write( reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()) ))
        {
            throw Error( ErrorCode::ERR_IO, "Cannot write " + path.filename().string() );
        }

        stream.close();

        if (!stream)
        {
            throw Error( ErrorCode::ERR_IO, "Cannot write " + path.filename().string() );
        }
    }

    std::shared_ptr< const void > Document::FindCached(
        const std::string & key
    ) const
    {
        std::lock_guard< std::mutex > guard( m_CacheLock );

        if (m_CacheRevision != m_Revision)
        {
            m_Cache.clear();
            m_CacheRevision = m_Revision;
        }

        auto found = m_Cache.find( key );
        return found != m_Cache.end() ? found->second : nullptr;
    }

    std::shared_ptr< const void > Document::StoreCached(
        const std::string & key,
        std::shared_ptr< const void > value
    ) const
    {
        std::lock_guard< std::mutex > guard( m_CacheLock );

        if (m_CacheRevision != m_Revision)
        {
            m_Cache.clear();
            m_CacheRevision = m_Revision;
        }

        return m_Cache.emplace( key, std::move( value ) ).first->second;
    }

    bool Document::HasCached(
        const std::string & key
    ) const
    {
        return FindCached( key ) != nullptr;
    }

    const FileHashes & Document::GetHashes() const
    {
        return *GetCachedObject< FileHashes >( "hashes", [this]
        {
            return ComputeFileHashes( m_Data );
        } );
    }

    const std::string & Document::GetCached(
        const std::string & key,
        const std::function< std::string() > & compute
    ) const
    {
        // The map keeps the string alive until the next change.
        return *GetCachedObject< std::string >( key, compute );
    }

    std::unique_ptr< Document > Document::OpenFile(
        const std::filesystem::path & path,
        uint64_t size_limit
    )
    {
        auto document = std::make_unique< Document >();
        document->m_Data = ReadWholeFile( path, size_limit );
        document->m_Path = std::filesystem::absolute( path );
        document->m_Name = Utf8FromUtf16( document->m_Path.filename().u16string() );
        return document;
    }

    std::unique_ptr< Document > Document::FromBytes(
        std::vector< uint8_t > data,
        const std::string & name
    )
    {
        auto document = std::make_unique< Document >();
        document->m_Data = std::move( data );
        document->m_Name = name;
        return document;
    }

    std::unique_ptr< Document > Document::Clone() const
    {
        auto document = std::make_unique< Document >();
        document->m_Data = m_Data;
        document->m_Path = m_Path;
        document->m_Name = m_Name;
        document->m_Revision = m_Revision;
        return document;
    }

    size_t Document::Read(
        uint64_t offset,
        std::span< uint8_t > out
    ) const
    {
        if (offset >= m_Data.size())
        {
            return 0;
        }

        size_t count = static_cast<size_t>(std::min< uint64_t >( out.size(), m_Data.size() - offset ));
        std::memcpy( out.data(), m_Data.data() + offset, count );
        return count;
    }

    void Document::Apply(
        const Step & step,
        bool forward
    )
    {
        const std::vector< uint8_t > & remove = forward ? step.m_Before : step.m_After;
        const std::vector< uint8_t > & insert = forward ? step.m_After : step.m_Before;
        auto position = m_Data.begin() + static_cast<ptrdiff_t>(step.m_Offset);

        if (remove.size() == insert.size())
        {
            std::copy( insert.begin(), insert.end(), position );
        }
        else
        {
            position = m_Data.erase( position, position + static_cast<ptrdiff_t>(remove.size()) );
            m_Data.insert( position, insert.begin(), insert.end() );
        }

        ++m_Revision;
    }

    void Document::Record(
        Step step,
        const std::string & description
    )
    {
        Apply( step, true );

        // A new change after undo drops the redo branch, if the saved state lived there it is gone for good
        if (!m_RedoGroups.empty())
        {
            if (m_SavedDepth > static_cast<int64_t>(m_UndoGroups.size()))
            {
                m_SavedDepth = -1;
            }

            m_RedoGroups.clear();
        }

        if (m_GroupDepth > 0)
        {
            m_OpenGroup.m_Steps.push_back( std::move( step ) );
            return;
        }

        Group group;
        group.m_Description = description;
        group.m_Steps.push_back( std::move( step ) );
        m_UndoGroups.push_back( std::move( group ) );
    }

    void Document::Overwrite(
        uint64_t offset,
        std::span< const uint8_t > data,
        const std::string & description
    )
    {
        Require( offset <= m_Data.size() && data.size() <= m_Data.size() - offset, ErrorCode::ERR_OUT_OF_RANGE, "Write outside of the file" );

        if (data.empty() || std::equal( data.begin(), data.end(), m_Data.begin() + static_cast<ptrdiff_t>(offset) ))
        {
            return;
        }

        Step step;
        step.m_Offset = offset;
        step.m_Before.assign( m_Data.begin() + static_cast<ptrdiff_t>(offset), m_Data.begin() + static_cast<ptrdiff_t>(offset + data.size()) );
        step.m_After.assign( data.begin(), data.end() );
        Record( std::move( step ), description );
    }

    void Document::Replace(
        uint64_t offset,
        uint64_t length,
        std::span< const uint8_t > data,
        const std::string & description
    )
    {
        Require( offset <= m_Data.size() && length <= m_Data.size() - offset, ErrorCode::ERR_OUT_OF_RANGE, "Replace outside of the file" );

        Step step;
        step.m_Offset = offset;
        step.m_Before.assign( m_Data.begin() + static_cast<ptrdiff_t>(offset), m_Data.begin() + static_cast<ptrdiff_t>(offset + length) );
        step.m_After.assign( data.begin(), data.end() );

        if (step.m_Before == step.m_After)
        {
            return;
        }

        Record( std::move( step ), description );
    }

    void Document::SetData(
        std::vector< uint8_t > data,
        const std::string & description
    )
    {
        size_t prefix = 0;
        size_t limit = std::min( data.size(), m_Data.size() );

        while (prefix < limit && data[prefix] == m_Data[prefix])
        {
            ++prefix;
        }

        if (prefix == limit && data.size() == m_Data.size())
        {
            return;
        }

        size_t suffix = 0;

        while (suffix < limit - prefix && data[data.size() - 1 - suffix] == m_Data[m_Data.size() - 1 - suffix])
        {
            ++suffix;
        }

        std::span< const uint8_t > middle( data.data() + prefix, data.size() - prefix - suffix );
        Replace( prefix, m_Data.size() - prefix - suffix, middle, description );
    }

    void Document::BeginGroup(
        const std::string & description
    )
    {
        if (m_GroupDepth++ == 0)
        {
            m_OpenGroup = Group();
            m_OpenGroup.m_Description = description;
        }
    }

    void Document::EndGroup()
    {
        if (m_GroupDepth == 0 || --m_GroupDepth > 0)
        {
            return;
        }

        if (!m_OpenGroup.m_Steps.empty())
        {
            m_UndoGroups.push_back( std::move( m_OpenGroup ) );
        }

        m_OpenGroup = Group();
    }

    void Document::CommitOpenGroup()
    {
        if (m_GroupDepth == 0 || m_OpenGroup.m_Steps.empty())
        {
            return;
        }

        // The group stays open with its description. The hex view keeps typing into it and closes it as usual, the
        // bytes typed from now on become the next undo step.
        std::string description = m_OpenGroup.m_Description;
        m_UndoGroups.push_back( std::move( m_OpenGroup ) );
        m_OpenGroup = Group();
        m_OpenGroup.m_Description = description;
    }

    std::string Document::GetUndoDescription() const
    {
        if (!m_OpenGroup.m_Steps.empty())
        {
            return m_OpenGroup.m_Description;
        }

        return m_UndoGroups.empty() ? std::string() : m_UndoGroups.back().m_Description;
    }

    std::string Document::GetRedoDescription() const
    {
        return m_RedoGroups.empty() ? std::string() : m_RedoGroups.back().m_Description;
    }

    bool Document::Undo(
        uint64_t & offset
    )
    {
        // Bytes typed in the hex view sit in an open group until the view loses focus, 'Ctrl+Z' must undo them right away
        // [it did nothing, and the Undo button was disabled, while the group was open]. The view starts a new group with
        // following keys.
        CommitOpenGroup();

        if (m_UndoGroups.empty())
        {
            return false;
        }

        Group group = std::move( m_UndoGroups.back() );
        m_UndoGroups.pop_back();

        for (auto step = group.m_Steps.rbegin(); step != group.m_Steps.rend(); ++step)
        {
            Apply( *step, false );
        }

        offset = group.m_Steps.empty() ? 0 : group.m_Steps.front().m_Offset;
        m_RedoGroups.push_back( std::move( group ) );
        return true;
    }

    bool Document::Redo(
        uint64_t & offset
    )
    {
        CommitOpenGroup();

        if (m_RedoGroups.empty())
        {
            return false;
        }

        Group group = std::move( m_RedoGroups.back() );
        m_RedoGroups.pop_back();

        for (const Step & step : group.m_Steps)
        {
            Apply( step, true );
        }

        offset = group.m_Steps.empty() ? 0 : group.m_Steps.front().m_Offset;
        m_UndoGroups.push_back( std::move( group ) );
        return true;
    }

    void Document::Save(
        const std::filesystem::path & path,
        bool make_backup
    )
    {
        // Typed bytes still in an open group belong to the saved state.
        CommitOpenGroup();
        std::filesystem::path target = path.empty() ? m_Path : std::filesystem::absolute( path );
        Require( !target.empty(), ErrorCode::ERR_INVALID_ARGUMENT, "The document has no file name, use Save as" );

        std::filesystem::path temporary = target;
        temporary += ".coffeeexplorer.tmp";
        WriteWholeFile( temporary, m_Data );

        std::error_code error;

        if (make_backup && std::filesystem::exists( target, error ))
        {
            std::filesystem::path backup = target;
            backup += ".bak";
            std::filesystem::copy_file( target, backup, std::filesystem::copy_options::overwrite_existing, error );

            if (error)
            {
                std::filesystem::remove( temporary, error );
                throw Error( ErrorCode::ERR_IO, "Cannot create the backup " + backup.filename().string() );
            }
        }

        // Keeps the attributes, ACL and creation time of the original [a plain rename gave the file new ones].
        ReplaceFileKeepingMetadata( target, temporary );

        m_Path = target;
        m_Name = Utf8FromUtf16( target.filename().u16string() );
        m_SavedDepth = static_cast<int64_t>(m_UndoGroups.size());
    }

    void Document::Reload()
    {
        Require( !m_Path.empty(), ErrorCode::ERR_INVALID_ARGUMENT, "The document has no file name" );
        m_Data = ReadWholeFile( m_Path, 0 );
        m_UndoGroups.clear();
        m_RedoGroups.clear();

        // A group still open [typing in the hex view] belongs to the old bytes, its EndGroup must not record it.
        m_OpenGroup = Group();
        m_GroupDepth = 0;
        m_SavedDepth = 0;
        ++m_Revision;
    }
}