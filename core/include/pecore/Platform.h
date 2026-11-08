#pragma once

#include "Types.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace PeCore
{
    struct FileTimes
    {
        std::string m_Created;
        std::string m_Modified;
        std::string m_Accessed;
    };

    bool GetFileTimes(
        const std::filesystem::path & path,
        FileTimes & times
    );

    // Puts temporary in the place of target. On Windows through 'ReplaceFileW', so the saved file keeps what the original
    // had besides its bytes like attributes, ACL, creation time, alternate data streams and the object ID. Falls back to a
    // plain rename [no target yet, or a file system without 'ReplaceFileW']. Throws 'Error' when the target stays unchanged.
    void ReplaceFileKeepingMetadata(
        const std::filesystem::path & target,
        const std::filesystem::path & temporary
    );

    // Deletes the previous versions of saved files that could not be deleted while they were in use [a loaded DLL or a
    // running program, 'pecore.dll' of Coffee Explorer itself]. Kept in '%LOCALAPPDATA%\CoffeeExplorer\pending_deletes.txt',
    // retried on every save and on startup. Returns how many are still waiting.
    size_t DeletePendingReplacedFiles();

    // Reads the whole file. On Windows files of 'FAST_READ_THRESHOLD' and more go through unbuffered overlapped I/O with
    // two sector aligned buffers [the read of the next block is in flight while the previous one is copied].
    // Smaller files use one sequential read. Throws 'Error' on failure.
    constexpr uint64_t FAST_READ_THRESHOLD = 64ull * 1024 * 1024;

    std::vector< uint8_t > ReadFileContents(
        const std::filesystem::path & path,
        uint64_t size
    );

    // 'WinVerifyTrust' [Authenticode policy] for a file on disk. The signature itself, the chain to a trusted root and the
    // file digest. No UI, no revocation check and no network download [only the local URL cache].
    struct TrustResult
    {
        bool m_Available = false;
        bool m_Trusted = false;
        uint32_t m_Code = 0;
        std::string m_Message;
    };

    TrustResult VerifyFileTrust(
        const std::filesystem::path & path
    );

    bool IsTrustVerificationAvailable();

    // Environment variable as UTF-8 [wide API on Windows, so non ASCII folders survive], empty when it is not set.
    std::string GetEnvironmentText(
        const char * name
    );

    // '?Open@Document@PeCore@@...' -> 'PeCore::Document::Open' [DbgHelp on Windows], the name itself elsewhere. Complete
    // gives the whole declaration: 'public: void __cdecl PeCore::Document::Open(...)', like CFF Explorer shows it.
    std::string UndecorateName(
        const std::string & name,
        bool complete = false
    );

    // Host library of an API set contract [api-ms-win-core-file-l1-1-0.dll -> kernelbase.dll], empty when the name is not
    // an API set or the schema is not available (non Windows).
    std::string ResolveApiSet(
        const std::string & name
    );

    struct ResolvedModule
    {
        std::filesystem::path m_Path;

        // 'application directory', 'system directory', 'API set -> kernelbase.dll', 'PATH' etc.
        std::string m_How;
        bool m_Found = false;
    };

    // Standard library search order for a module imported by a file in 'importer_directory'. The application directory,
    // the system directory of the right bitness, the Windows directory, the current directory and PATH.
    ResolvedModule ResolveModule(
        const std::string & name,
        const std::filesystem::path & importer_directory,
        bool is_64_bit
    );

    // Detect It Easy through 'die.dll' [die_library]. 'dll_path' is the full path of 'die.dll', database the db folder.
    bool IsDieAvailable(
        const std::filesystem::path & dll_path
    );

    std::string DieScanFile(
        const std::filesystem::path & dll_path,
        const std::filesystem::path & database,
        const std::filesystem::path & file,
        uint32_t flags
    );

    // Scans bytes that are not on disk [a document with unsaved changes]. 'DIE_ScanMemoryA', or a temporary file when
    // 'die.dll' lacks it or the data is larger than 2 GB.
    std::string DieScanMemory(
        const std::filesystem::path & dll_path,
        const std::filesystem::path & database,
        std::span< const uint8_t > data,
        uint32_t flags
    );
}