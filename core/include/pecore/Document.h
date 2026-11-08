#pragma once

#include "Hash.h"
#include "Types.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace PeCore
{
    // The whole file in memory with undo and redo. PE files are small enough and every view needs random access anyway.
    // Every change is recorded as 'replace [offset, offset + before] with after', structural operations that rebuild
    // the file go through SetData which stores only the changed middle part.
    class Document
    {
    private:
        struct Step
        {
            uint64_t m_Offset = 0;
            std::vector< uint8_t > m_Before;
            std::vector< uint8_t > m_After;
        };

        struct Group
        {
            std::string m_Description;
            std::vector< Step > m_Steps;
        };

        std::vector< uint8_t > m_Data;
        std::filesystem::path m_Path;
        std::string m_Name;
        std::vector< Group > m_UndoGroups;
        std::vector< Group > m_RedoGroups;
        Group m_OpenGroup;
        int m_GroupDepth = 0;

        // Undo depth at the last save, -1 when that state can no longer be reached.
        int64_t m_SavedDepth = 0;
        uint64_t m_Revision = 1;

        // Results of slow queries [hashes, Authenticode, trust, entropy], dropped when the revision changes. The lock
        // lets the overview compute several of them on worker threads at once.
        mutable std::mutex m_CacheLock;
        mutable std::map< std::string, std::shared_ptr< const void > > m_Cache;
        mutable uint64_t m_CacheRevision = 0;

        std::shared_ptr< const void > FindCached(
            const std::string & key
        ) const;

        std::shared_ptr< const void > StoreCached(
            const std::string & key,
            std::shared_ptr< const void > value
        ) const;

        void Apply(
            const Step & step,
            bool forward
        );

        void Record(
            Step step,
            const std::string & description
        );

        // Turns the steps of a group that is still open [typing in the hex view] into an undo step, so undo, redo and save
        // see them. The group itself stays open for the steps that follow.
        void CommitOpenGroup();

        // Undo steps including the open group, the depth 'IsModified' compares with the saved one.
        int64_t UndoDepth() const
        {
            return static_cast<int64_t>(m_UndoGroups.size()) + (m_OpenGroup.m_Steps.empty() ? 0 : 1);
        }

    public:

        static std::unique_ptr< Document > OpenFile(
            const std::filesystem::path & path,
            uint64_t size_limit
        );

        static std::unique_ptr< Document > FromBytes(
            std::vector< uint8_t > data,
            const std::string & name
        );

        std::unique_ptr< Document > Clone() const;

        const std::vector< uint8_t > & GetData() const
        {
            return m_Data;
        }

        uint64_t GetSize() const
        {
            return m_Data.size();
        }

        // Incremented by every change, undo and redo. Parsed views cache against it.
        uint64_t GetRevision() const
        {
            return m_Revision;
        }

        // MD5, SHA-1, SHA-256 and BLAKE3 of the current bytes, computed once per revision.
        const FileHashes & GetHashes() const;

        // Returns 'true' when the slow query of this key is cached for the current revision.
        bool HasCached(
            const std::string & key
        ) const;

        // Computes the text once per revision and key.
        const std::string & GetCached(
            const std::string & key,
            const std::function< std::string() > & compute
        ) const;

        // Computes the object once per revision and key. Safe to call from several threads while the document is not
        // changed. two threads asking for the same key may both compute it, the first result is kept.
        template< typename T >
        std::shared_ptr< const T > GetCachedObject(
            const std::string & key,
            const std::function< T() > & compute
        ) const
        {
            if (std::shared_ptr< const void > found = FindCached( key ))
            {
                return std::static_pointer_cast<const T>(found);
            }

            return std::static_pointer_cast<const T>(StoreCached( key, std::make_shared< const T >( compute() ) ));
        }

        const std::filesystem::path & GetPath() const
        {
            return m_Path;
        }

        const std::string & GetName() const
        {
            return m_Name;
        }

        size_t Read(
            uint64_t offset,
            std::span< uint8_t > out
        ) const;

        // Writes inside the current size, the file length never changes.
        void Overwrite(
            uint64_t offset,
            std::span< const uint8_t > data,
            const std::string & description
        );

        // Replaces length bytes at offset with data, the file grows or shrinks as needed.
        void Replace(
            uint64_t offset,
            uint64_t length,
            std::span< const uint8_t > data,
            const std::string & description
        );

        // Swaps in a rebuilt file, only the differing middle part is stored for undo.
        void SetData(
            std::vector< uint8_t > data,
            const std::string & description
        );

        void BeginGroup(
            const std::string & description
        );

        void EndGroup();

        // Also true while a group is open: bytes typed in the hex view can be undone before the view lets go of them.
        bool CanUndo() const
        {
            return !m_UndoGroups.empty() || !m_OpenGroup.m_Steps.empty();
        }

        bool CanRedo() const
        {
            return !m_RedoGroups.empty();
        }

        std::string GetUndoDescription() const;

        std::string GetRedoDescription() const;

        // Returns the offset of the first byte the step touched so the UI can show it.
        bool Undo(
            uint64_t & offset
        );

        bool Redo(
            uint64_t & offset
        );

        bool IsModified() const
        {
            return m_SavedDepth != UndoDepth();
        }

        // Writes to path [the current path when empty] through a temporary file, optionally keeping a .bak copy.
        void Save(
            const std::filesystem::path & path,
            bool make_backup
        );

        void Reload();
    };

    std::vector< uint8_t > ReadWholeFile(
        const std::filesystem::path & path,
        uint64_t size_limit
    );

    void WriteWholeFile(
        const std::filesystem::path & path,
        std::span< const uint8_t > data
    );
}