#include "pecore/Platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <softpub.h>
#include <wintrust.h>
#include <winternl.h>
#endif

namespace PeCore
{
    namespace
    {
        std::string Lower(
            std::string text
        )
        {
            std::transform( text.begin(), text.end(), text.begin(), [] ( unsigned char character )
            {
                return static_cast<char>(std::tolower( character ));
            } );
            return text;
        }

        #ifdef _WIN32
        std::string FormatFileTime(
            const FILETIME & time
        )
        {
            FILETIME local;
            SYSTEMTIME system;

            if (!FileTimeToLocalFileTime( &time, &local ) || !FileTimeToSystemTime( &local, &system ))
            {
                return std::string();
            }

            char buffer[32];
            snprintf( buffer, sizeof( buffer ), "%04u-%02u-%02u %02u:%02u:%02u", system.wYear, system.wMonth, system.wDay, system.wHour, system.wMinute, system.wSecond );
            return buffer;
        }

        // ApiSetMap of the running process [schema version 6, Windows 10 and 11].
        struct ApiSetNamespace
        {
            ULONG m_Version;
            ULONG m_Size;
            ULONG m_Flags;
            ULONG m_Count;
            ULONG m_EntryOffset;
            ULONG m_HashOffset;
            ULONG m_HashFactor;
        };

        struct ApiSetNamespaceEntry
        {
            ULONG m_Flags;
            ULONG m_NameOffset;
            ULONG m_NameLength;
            ULONG m_HashedLength;
            ULONG m_ValueOffset;
            ULONG m_ValueCount;
        };

        struct ApiSetValueEntry
        {
            ULONG m_Flags;
            ULONG m_NameOffset;
            ULONG m_NameLength;
            ULONG m_ValueOffset;
            ULONG m_ValueLength;
        };

        const ApiSetNamespace * GetApiSetMap()
        {
            PEB * peb = NtCurrentTeb()->ProcessEnvironmentBlock;
            #ifdef _WIN64
            const size_t api_set_map_offset = 0x68;
            #else
            const size_t api_set_map_offset = 0x38;
            #endif
            auto map = *reinterpret_cast<const ApiSetNamespace * const *>(reinterpret_cast<const uint8_t *>(peb) + api_set_map_offset);
            return map != nullptr && map->m_Version == 6 ? map : nullptr;
        }

        std::filesystem::path SystemDirectory(
            bool is_64_bit
        )
        {
            wchar_t buffer[MAX_PATH];
            UINT length = 0;

            #ifdef _WIN64
            // A 64 bit process sees the native System32, 32 bit images load from SysWOW64.
            length = is_64_bit ? GetSystemDirectoryW( buffer, MAX_PATH ) : GetSystemWow64DirectoryW( buffer, MAX_PATH );
            #else
            length = GetSystemDirectoryW( buffer, MAX_PATH );
            #endif

            return length != 0 && length < MAX_PATH ? std::filesystem::path( buffer ) : std::filesystem::path();
        }

        std::filesystem::path WindowsDirectory()
        {
            wchar_t buffer[MAX_PATH];
            UINT length = GetWindowsDirectoryW( buffer, MAX_PATH );
            return length != 0 && length < MAX_PATH ? std::filesystem::path( buffer ) : std::filesystem::path();
        }

        // die_library exports used here. 'DIE_ScanFileW' does not terminate its result [QString::toWCharArray], so the
        // UTF-8 'DIE_ScanFileA' is preferred and 'DIE_VB_ScanFile' [caller buffer, returns the length] is the fallback.
        struct DieLibrary
        {
            HMODULE m_Module = nullptr;
            std::filesystem::path m_Path;
            char * (*m_ScanFileA)(char *, unsigned int, char *) = nullptr;

            // char * DIE_ScanMemoryA( char * memory, int size, unsigned int flags, char * database )
            char * (*m_ScanMemoryA)(char *, int, unsigned int, char *) = nullptr;
            void (*m_FreeA)(char *) = nullptr;
            int( __stdcall * m_ScanFileBuffer )(wchar_t *, unsigned int, wchar_t *, wchar_t *, int) = nullptr;
        };

        std::mutex g_DieMutex;
        DieLibrary g_Die;

        bool LoadDie(
            const std::filesystem::path & dll_path
        )
        {
            if (g_Die.m_Module != nullptr)
            {
                return g_Die.m_Path == dll_path;
            }

            // Altered search path so the Qt libraries next to 'die.dll' resolve.
            HMODULE module = LoadLibraryExW( dll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH );

            if (module == nullptr)
            {
                return false;
            }

            g_Die.m_Module = module;
            g_Die.m_Path = dll_path;
            g_Die.m_ScanFileA = reinterpret_cast<char * (*)(char *, unsigned int, char *)>(GetProcAddress( module, "DIE_ScanFileA" ));
            g_Die.m_FreeA = reinterpret_cast<void (*)(char *)>(GetProcAddress( module, "DIE_FreeMemoryA" ));
            g_Die.m_ScanMemoryA = reinterpret_cast<char * (*)(char *, int, unsigned int, char *)>(GetProcAddress( module, "DIE_ScanMemoryA" ));
            g_Die.m_ScanFileBuffer = reinterpret_cast<int( __stdcall * )(wchar_t *, unsigned int, wchar_t *, wchar_t *, int)>(
                GetProcAddress( module, "DIE_VB_ScanFile" ));
            return (g_Die.m_ScanFileA != nullptr && g_Die.m_FreeA != nullptr) || g_Die.m_ScanFileBuffer != nullptr;
        }
        #endif
    }

    #ifdef _WIN32
    namespace
    {
        constexpr DWORD FAST_READ_BLOCK = 4 * 1024 * 1024;
        constexpr DWORD FAST_READ_SECTOR = 4096;

        // 'VirtualAlloc' gives page and so sector aligned memory as 'FILE_FLAG_NO_BUFFERING' requires.
        class AlignedBlock
        {
        private:

            void * m_Pointer = nullptr;

        public:

            explicit AlignedBlock(
                size_t size
            )
                : m_Pointer( VirtualAlloc( nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE ) )
            {}

            ~AlignedBlock()
            {
                if (m_Pointer != nullptr)
                {
                    VirtualFree( m_Pointer, 0, MEM_RELEASE );
                }
            }

            AlignedBlock(
                const AlignedBlock &
            ) = delete;

            AlignedBlock & operator=(
                const AlignedBlock &
                ) = delete;

            uint8_t * Get() const
            {
                return static_cast<uint8_t *>(m_Pointer);
            }
        };

        class Handle
        {
        private:

            HANDLE m_Handle = INVALID_HANDLE_VALUE;

        public:

            explicit Handle(
                HANDLE handle
            )
                : m_Handle( handle )
            {}

            ~Handle()
            {
                if (m_Handle != INVALID_HANDLE_VALUE && m_Handle != nullptr)
                {
                    CloseHandle( m_Handle );
                }
            }

            Handle(
                const Handle &
            ) = delete;

            Handle & operator=(
                const Handle &
                ) = delete;

            HANDLE Get() const
            {
                return m_Handle;
            }

            bool IsValid() const
            {
                return m_Handle != INVALID_HANDLE_VALUE && m_Handle != nullptr;
            }
        };

        [[noreturn]] void ThrowReadError(
            const std::filesystem::path & path,
            DWORD code
        )
        {
            ErrorCode error = code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION ? ErrorCode::ERR_ACCESS_DENIED : ErrorCode::ERR_IO;
            throw Error( error, "Cannot read " + Utf8FromUtf16( path.filename().u16string() ) + " (Windows error " + std::to_string( code ) + ")" );
        }

        void ReadSequential(
            const std::filesystem::path & path,
            std::vector< uint8_t > & data
        )
        {
            Handle file( CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_SEQUENTIAL_SCAN, nullptr ) );

            if (!file.IsValid())
            {
                ThrowReadError( path, GetLastError() );
            }

            size_t done = 0;

            while (done < data.size())
            {
                DWORD wanted = static_cast<DWORD>( std::min< size_t >( data.size() - done, FAST_READ_BLOCK * 16ull ) );
                DWORD got = 0;

                if (!ReadFile( file.Get(), data.data() + done, wanted, &got, nullptr ))
                {
                    ThrowReadError( path, GetLastError() );
                }

                if (got == 0)
                {
                    break;
                }

                done += got;
            }

            // The file shrank while it was read.
            data.resize( done );
        }

        // Double buffered pipeline. Block n + 1 is read while block n is copied into the result.
        bool ReadOverlapped(
            const std::filesystem::path & path,
            std::vector< uint8_t > & data
        )
        {
            Handle file( CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED, nullptr ) );

            if (!file.IsValid())
            {
                return false;
            }

            AlignedBlock blocks[2] = { AlignedBlock( FAST_READ_BLOCK ), AlignedBlock( FAST_READ_BLOCK ) };
            Handle events[2] = { Handle( CreateEventW( nullptr, TRUE, FALSE, nullptr ) ), Handle( CreateEventW( nullptr, TRUE, FALSE, nullptr ) ) };

            if (blocks[0].Get() == nullptr || blocks[1].Get() == nullptr || !events[0].IsValid() || !events[1].IsValid())
            {
                return false;
            }

            OVERLAPPED requests[2] = {};
            size_t request_offset[2] = {};
            size_t request_length[2] = {};
            bool pending[2] = {};
            size_t next_offset = 0;

            auto issue = [&] ( int slot ) -> bool
            {
                request_length[slot] = 0;

                if (next_offset >= data.size())
                {
                    return true;
                }

                size_t logical = std::min< size_t >( FAST_READ_BLOCK, data.size() - next_offset );
                DWORD padded = static_cast<DWORD>((logical + FAST_READ_SECTOR - 1) & ~static_cast<size_t>(FAST_READ_SECTOR - 1));
                requests[slot] = OVERLAPPED {};
                requests[slot].Offset = static_cast<DWORD>(next_offset);
                requests[slot].OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(next_offset) >> 32);
                requests[slot].hEvent = events[slot].Get();

                if (!ReadFile( file.Get(), blocks[slot].Get(), padded, nullptr, &requests[slot] ) && GetLastError() != ERROR_IO_PENDING)
                {
                    return false;
                }

                pending[slot] = true;
                request_offset[slot] = next_offset;
                request_length[slot] = logical;
                next_offset += logical;
                return true;
            };

            auto complete = [&] ( int slot ) -> bool
            {
                if (!pending[slot])
                {
                    return true;
                }

                DWORD got = 0;
                BOOL ok = GetOverlappedResult( file.Get(), &requests[slot], &got, TRUE );
                pending[slot] = false;

                if (!ok)
                {
                    return false;
                }

                size_t length = std::min< size_t >( got, request_length[slot] );
                std::memcpy( data.data() + request_offset[slot], blocks[slot].Get(), length );
                return length == request_length[slot];
            };

            // A request must be finished before its buffer and event go away.
            auto drain = [&] ()
            {
                for (int slot = 0; slot < 2; ++slot)
                {
                    if (pending[slot])
                    {
                        DWORD ignored = 0;
                        CancelIoEx( file.Get(), &requests[slot] );
                        GetOverlappedResult( file.Get(), &requests[slot], &ignored, TRUE );
                        pending[slot] = false;
                    }
                }
            };

            if (!issue( 0 ))
            {
                return false;
            }

            for (int current = 0; pending[current]; current = 1 - current)
            {
                if (!issue( 1 - current ) || !complete( current ))
                {
                    drain();
                    return false;
                }
            }

            return true;
        }
    }
    #endif

    #ifdef _WIN32
    namespace
    {
        std::mutex g_PendingLock;

        std::filesystem::path PendingListPath()
        {
            std::string local = GetEnvironmentText( "LOCALAPPDATA" );

            if (local.empty())
            {
                return {};
            }

            return std::filesystem::path( Utf16FromUtf8( local ) ) / L"CoffeeExplorer" / L"pending_deletes.txt";
        }

        std::vector< std::wstring > ReadPendingList(
            const std::filesystem::path & list
        )
        {
            std::vector< std::wstring > paths;
            std::ifstream stream( list, std::ios::binary );
            std::string line;

            while (std::getline( stream, line ))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }

                if (!line.empty())
                {
                    std::u16string wide = Utf16FromUtf8( line );
                    paths.emplace_back( wide.begin(), wide.end() );
                }
            }

            return paths;
        }

        void WritePendingList(
            const std::filesystem::path & list,
            const std::vector< std::wstring > & paths
        )
        {
            std::error_code error;

            if (paths.empty())
            {
                std::filesystem::remove( list, error );
                return;
            }

            std::filesystem::create_directories( list.parent_path(), error );
            std::ofstream stream( list, std::ios::binary | std::ios::trunc );

            for (const std::wstring & path : paths)
            {
                stream << Utf8FromUtf16( std::u16string( path.begin(), path.end() ) ) << "\n";
            }
        }

        // DeleteFileW, then a POSIX delete [the name goes at once even while another process keeps the file open].
        // A mapped image [loaded DLL, running program] refuses both until the last process lets go of it.
        bool TryDelete(
            const std::wstring & path
        )
        {
            SetFileAttributesW( path.c_str(), FILE_ATTRIBUTE_NORMAL );

            if (DeleteFileW( path.c_str() ) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)
            {
                return true;
            }

            HANDLE file = CreateFileW( path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr );

            if (file == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            // FILE_DISPOSITION_INFO_EX [Windows 10 1607], spelled out: the SDK only declares it for newer NTDDI versions.
            struct DispositionEx
            {
                DWORD m_Flags;
            };

            constexpr DWORD DISPOSITION_DELETE = 0x00000001;
            constexpr DWORD DISPOSITION_POSIX_SEMANTICS = 0x00000002;
            constexpr DWORD DISPOSITION_IGNORE_READONLY = 0x00000010;
            constexpr auto DISPOSITION_INFO_EX_CLASS = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);

            DispositionEx disposition = { DISPOSITION_DELETE | DISPOSITION_POSIX_SEMANTICS | DISPOSITION_IGNORE_READONLY };
            bool deleted = SetFileInformationByHandle( file, DISPOSITION_INFO_EX_CLASS, &disposition, sizeof( disposition ) ) != FALSE;
            CloseHandle( file );
            return deleted;
        }

        // The previous version still in use: hidden until it can go, remembered for the next save or start.
        void RememberPending(
            const std::wstring & path
        )
        {
            DWORD attributes = GetFileAttributesW( path.c_str() );

            if (attributes != INVALID_FILE_ATTRIBUTES)
            {
                SetFileAttributesW( path.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN );
            }

            // Only succeeds elevated [HKLM PendingFileRenameOperations], the list below covers everyone else.
            MoveFileExW( path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT );

            std::filesystem::path list = PendingListPath();

            if (list.empty())
            {
                return;
            }

            std::lock_guard< std::mutex > lock( g_PendingLock );
            std::vector< std::wstring > paths = ReadPendingList( list );

            if (std::find( paths.begin(), paths.end(), path ) == paths.end())
            {
                paths.push_back( path );
            }

            WritePendingList( list, paths );
        }

        // '<name>.~ce<pid>-<count>.tmp' next to the target. Unique, an earlier one of this session may still be in use.
        std::filesystem::path PreviousVersionPath(
            const std::filesystem::path & target
        )
        {
            static std::atomic< unsigned > counter { 0 };
            std::filesystem::path previous = target;
            previous += L".~ce" + std::to_wstring( GetCurrentProcessId() ) + L"-" + std::to_wstring( ++counter ) + L".tmp";
            return previous;
        }
    }
    #endif

    size_t DeletePendingReplacedFiles()
    {
        #ifdef _WIN32
        std::filesystem::path list = PendingListPath();

        if (list.empty())
        {
            return 0;
        }

        std::lock_guard< std::mutex > lock( g_PendingLock );
        std::error_code error;

        if (!std::filesystem::exists( list, error ))
        {
            return 0;
        }

        std::vector< std::wstring > remaining;

        for (const std::wstring & path : ReadPendingList( list ))
        {
            if (!TryDelete( path ))
            {
                remaining.push_back( path );
            }
        }

        WritePendingList( list, remaining );
        return remaining.size();
        #else
        return 0;
        #endif
    }

    void ReplaceFileKeepingMetadata(
        const std::filesystem::path & target,
        const std::filesystem::path & temporary
    )
    {
        std::error_code error;

        #ifdef _WIN32
        // Without a backup name 'ReplaceFileW' renames a target that is in use [a loaded dynamic library, a running program] to
        // '<name>~RF<hex>.TMP' and leaves it behind when it cannot delete it. The previous version gets a name of ours
        // instead and is deleted here, or later once it is free.
        if (std::filesystem::exists( target, error ))
        {
            DeletePendingReplacedFiles();
            std::filesystem::path previous = PreviousVersionPath( target );

            if (ReplaceFileW( target.c_str(), temporary.c_str(), previous.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr ))
            {
                if (!TryDelete( previous.wstring() ))
                {
                    RememberPending( previous.wstring() );
                }

                return;
            }

            // The target already has the previous name, the replacement could not be moved in. The target is put back so the rename below finds it.
            if (GetLastError() == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 && std::filesystem::exists( previous, error ) && !std::filesystem::exists( target, error ))
            {
                MoveFileExW( previous.c_str(), target.c_str(), 0 );
            }
        }

        // 'ReplaceFileW' failed. The target is unchanged, or gone with the replacement still under its temporary name. Either
        // way the rename below finishes the job [or reports why it cannot, a read only file for example].
        #endif

        std::filesystem::rename( temporary, target, error );

        if (error)
        {
            std::error_code ignored;
            std::filesystem::remove( temporary, ignored );
            throw Error( error == std::errc::permission_denied ? ErrorCode::ERR_ACCESS_DENIED : ErrorCode::ERR_IO,
                "Cannot replace " + target.filename().string() + ": " + error.message() );
        }
    }

    std::vector< uint8_t > ReadFileContents(
        const std::filesystem::path & path,
        uint64_t size
    )
    {
        std::vector< uint8_t > data( static_cast<size_t>(size) );

        if (size == 0)
        {
            return data;
        }

        #ifdef _WIN32
        // Unbuffered reads can fail on some file systems [network shares, odd sector sizes], the plain read follows.
        if (size >= FAST_READ_THRESHOLD && ReadOverlapped( path, data ))
        {
            return data;
        }

        ReadSequential( path, data );
        return data;
        #else
        std::ifstream stream( path, std::ios::binary );

        if (!stream || !stream.read( reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size) ))
        {
            throw Error( ErrorCode::ERR_IO, "Cannot read " + path.filename().string() );
        }

        return data;
        #endif
    }

    std::string GetEnvironmentText(
        const char * name
    )
    {
        #ifdef _WIN32
        std::wstring wide_name;

        for (const char * character = name; *character != 0; ++character)
        {
            wide_name.push_back( static_cast<wchar_t>(static_cast<unsigned char>(*character)) );
        }

        DWORD length = GetEnvironmentVariableW( wide_name.c_str(), nullptr, 0 );

        if (length == 0)
        {
            return {};
        }

        std::wstring value( length, L'\0' );
        length = GetEnvironmentVariableW( wide_name.c_str(), value.data(), length );
        value.resize( length );
        return Utf8FromUtf16( std::u16string( value.begin(), value.end() ) );
        #else
        const char * value = std::getenv( name );
        return value != nullptr ? std::string( value ) : std::string();
        #endif
    }

    std::string UndecorateName(
        const std::string & name,
        bool complete
    )
    {
        #ifdef _WIN32
        if (name.size() > 1 && name[0] == '?')
        {
            // DbgHelp is not thread safe, the core can run on worker threads.
            static std::mutex lock;
            std::lock_guard< std::mutex > guard( lock );
            char buffer[2048];

            // Some names [dynamic initializers of templates in anonymous namespaces] only undecorate completely.
            for (DWORD flags : { static_cast<DWORD>(complete ? UNDNAME_COMPLETE : UNDNAME_NAME_ONLY), static_cast<DWORD>(UNDNAME_COMPLETE) })
            {
                if (UnDecorateSymbolName( name.c_str(), buffer, sizeof( buffer ), flags ) != 0 && buffer[0] != '?')
                {
                    return buffer;
                }
            }
        }
        #else
        (void)complete;
        #endif
        return name;
    }

    bool IsTrustVerificationAvailable()
    {
        #ifdef _WIN32
        return true;
        #else
        return false;
        #endif
    }

    TrustResult VerifyFileTrust(
        const std::filesystem::path & path
    )
    {
        TrustResult result;
        #ifdef _WIN32
        result.m_Available = true;
        std::wstring file_name = path.wstring();

        WINTRUST_FILE_INFO file_info = {};
        file_info.cbStruct = sizeof( file_info );
        file_info.pcwszFilePath = file_name.c_str();

        WINTRUST_DATA trust_data = {};
        trust_data.cbStruct = sizeof( trust_data );
        trust_data.dwUIChoice = WTD_UI_NONE;
        trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
        trust_data.dwUnionChoice = WTD_CHOICE_FILE;
        trust_data.pFile = &file_info;
        trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
        trust_data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;

        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        LONG status = WinVerifyTrust( static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust_data );
        trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust( static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust_data );

        result.m_Code = static_cast<uint32_t>(status);
        result.m_Trusted = status == ERROR_SUCCESS;

        switch (static_cast<HRESULT>(status))
        {
            case ERROR_SUCCESS:
                result.m_Message = "Trusted, the signature and the chain verify";
                break;
            case TRUST_E_NOSIGNATURE:
                result.m_Message = "Not signed, or the signature cannot be read";
                break;
            case TRUST_E_BAD_DIGEST:
                result.m_Message = "The file was modified after signing (digest mismatch)";
                break;
            case CERT_E_UNTRUSTEDROOT:
                result.m_Message = "The chain ends in a root that is not trusted";
                break;
            case CERT_E_CHAINING:
                result.m_Message = "The chain to a root certificate cannot be built";
                break;
            case CERT_E_EXPIRED:
                result.m_Message = "A certificate expired and the signature has no valid timestamp";
                break;
            case CERT_E_REVOKED:
                result.m_Message = "A certificate was revoked";
                break;
            case TRUST_E_EXPLICIT_DISTRUST:
                result.m_Message = "The signer is explicitly distrusted";
                break;
            case TRUST_E_SUBJECT_NOT_TRUSTED:
                result.m_Message = "The subject is not trusted";
                break;
            case CRYPT_E_SECURITY_SETTINGS:
                result.m_Message = "Blocked by the local security policy";
                break;
            case TRUST_E_SUBJECT_FORM_UNKNOWN:
                result.m_Message = "Windows does not know how to verify this file";
                break;
            default:
            {
                char code[16];
                std::snprintf( code, sizeof( code ), "%08X", static_cast<unsigned int>(status) );
                result.m_Message = std::string( "Not trusted, WinVerifyTrust error 0x" ) + code;
                break;
            }
        }
        #else
        (void)path;
        result.m_Message = "Windows trust verification is not available on this platform";
        #endif
        return result;
    }

    bool GetFileTimes(
        const std::filesystem::path & path,
        FileTimes & times
    )
    {
        #ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA attributes;

        if (!GetFileAttributesExW( path.c_str(), GetFileExInfoStandard, &attributes ))
        {
            return false;
        }

        times.m_Created = FormatFileTime( attributes.ftCreationTime );
        times.m_Modified = FormatFileTime( attributes.ftLastWriteTime );
        times.m_Accessed = FormatFileTime( attributes.ftLastAccessTime );
        return true;
        #else
        std::error_code error;
        auto time = std::filesystem::last_write_time( path, error );

        if (error)
        {
            return false;
        }

        auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
        std::time_t value = std::chrono::system_clock::to_time_t( system );
        char buffer[32];
        std::strftime( buffer, sizeof( buffer ), "%Y-%m-%d %H:%M:%S", std::localtime( &value ) );
        times.m_Modified = buffer;
        times.m_Created = "Not available";
        times.m_Accessed = "Not available";
        return true;
        #endif
    }

    std::string ResolveApiSet(
        const std::string & name
    )
    {
        std::string lower = Lower( name );

        if (lower.rfind( "api-", 0 ) != 0 && lower.rfind( "ext-", 0 ) != 0)
        {
            return std::string();
        }

        #ifdef _WIN32
        const ApiSetNamespace * map = GetApiSetMap();

        if (map == nullptr)
        {
            return std::string();
        }

        if (lower.size() > 4 && lower.compare( lower.size() - 4, 4, ".dll" ) == 0)
        {
            lower.resize( lower.size() - 4 );
        }

        // The hashed part of an API set name ends before the last hyphen [the version is ignored].
        std::u16string query = Utf16FromUtf8( lower.substr( 0, lower.rfind( '-' ) ) );
        const uint8_t * base = reinterpret_cast<const uint8_t *>(map);
        auto entries = reinterpret_cast<const ApiSetNamespaceEntry *>(base + map->m_EntryOffset);

        for (ULONG index = 0; index < map->m_Count; ++index)
        {
            const ApiSetNamespaceEntry & entry = entries[index];
            auto entry_name = reinterpret_cast<const char16_t *>( base + entry.m_NameOffset );
            size_t hashed = entry.m_HashedLength / 2;

            if (hashed != query.size())
            {
                continue;
            }

            bool equal = true;

            for (size_t character = 0; character < hashed && equal; ++character)
            {
                equal = std::towlower( static_cast<wint_t>( entry_name[character] ) ) == static_cast<wint_t>( query[character] );
            }

            if (!equal || entry.m_ValueCount == 0)
            {
                continue;
            }

            // The first value is the default host, the others are importer specific overrides.
            auto values = reinterpret_cast<const ApiSetValueEntry *>(base + entry.m_ValueOffset);
            const ApiSetValueEntry & value = values[0];

            if (value.m_ValueLength == 0)
            {
                return std::string();
            }

            std::u16string host( reinterpret_cast<const char16_t *>(base + value.m_ValueOffset), value.m_ValueLength / 2 );
            return Utf8FromUtf16( host );
        }

        return std::string();
        #else
        return std::string();
        #endif
    }

    ResolvedModule ResolveModule(
        const std::string & name,
        const std::filesystem::path & importer_directory,
        bool is_64_bit
    )
    {
        ResolvedModule result;
        std::string file = name;
        std::string host = ResolveApiSet( name );

        if (!host.empty())
        {
            file = host;
            result.m_How = "API set -> " + host;
        }
        else if (Lower( name ).rfind( "api-", 0 ) == 0 || Lower( name ).rfind( "ext-", 0 ) == 0)
        {
            result.m_How = "API set (no host on this system)";
            return result;
        }

        std::filesystem::path file_path( Utf16FromUtf8( file ) );
        std::vector< std::pair< std::filesystem::path, std::string > > candidates;

        if (!importer_directory.empty() && host.empty())
        {
            candidates.emplace_back( importer_directory, "application directory" );
        }

        #ifdef _WIN32
        candidates.emplace_back( SystemDirectory( is_64_bit ), is_64_bit ? "system directory" : "system directory (SysWOW64)" );
        candidates.emplace_back( WindowsDirectory(), "Windows directory" );

        if (host.empty())
        {
            wchar_t * path = nullptr;
            size_t length = 0;

            if (_wdupenv_s( &path, &length, L"PATH" ) == 0 && path != nullptr)
            {
                std::wstring list( path );
                free( path );
                size_t start = 0;

                while (start <= list.size())
                {
                    size_t end = list.find( L';', start );
                    std::wstring entry = list.substr( start, end == std::wstring::npos ? std::wstring::npos : end - start );

                    if (!entry.empty())
                    {
                        candidates.emplace_back( std::filesystem::path( entry ), "PATH" );
                    }

                    if (end == std::wstring::npos)
                    {
                        break;
                    }

                    start = end + 1;
                }
            }
        }
        #endif

        for (const auto & candidate : candidates)
        {
            if (candidate.first.empty())
            {
                continue;
            }

            std::error_code error;
            std::filesystem::path full = candidate.first / file_path;

            if (std::filesystem::is_regular_file( full, error ))
            {
                result.m_Path = full;
                result.m_Found = true;
                result.m_How = result.m_How.empty() ? candidate.second : result.m_How + ", " + candidate.second;
                return result;
            }
        }

        if (result.m_How.empty())
        {
            result.m_How = "not found";
        }

        return result;
    }

    bool IsDieAvailable(
        const std::filesystem::path & dll_path
    )
    {
        #ifdef _WIN32
        std::lock_guard< std::mutex > lock( g_DieMutex );
        std::error_code error;
        return std::filesystem::is_regular_file( dll_path, error ) && LoadDie( dll_path );
        #else
        return false;
        #endif
    }

    std::string DieScanFile(
        const std::filesystem::path & dll_path,
        const std::filesystem::path & database,
        const std::filesystem::path & file,
        uint32_t flags
    )
    {
        #ifdef _WIN32
        std::lock_guard< std::mutex > lock( g_DieMutex );
        Require( LoadDie( dll_path ), ErrorCode::ERR_UNSUPPORTED, "Detect It Easy (die.dll) could not be loaded from " + dll_path.string() );

        if (g_Die.m_ScanFileA != nullptr && g_Die.m_FreeA != nullptr)
        {
            // Qt 5 reads char * arguments as UTF-8.
            std::u8string file_text = file.u8string();
            std::u8string database_text = database.u8string();
            char * result = g_Die.m_ScanFileA( reinterpret_cast<char *>(file_text.data()), flags, reinterpret_cast<char *>(database_text.data()) );
            Require( result != nullptr, ErrorCode::ERR_INTERNAL, "Detect It Easy returned no result" );
            std::string text( result );
            g_Die.m_FreeA( result );
            return text;
        }

        // The buffer variant reports 0 when the result does not fit, retry with a larger buffer.
        std::wstring file_text = file.wstring();
        std::wstring database_text = database.wstring();

        for (int size = 1 << 16; size <= (1 << 24); size <<= 2)
        {
            std::vector< wchar_t > buffer( static_cast<size_t>(size), L'\0' );
            int length = g_Die.m_ScanFileBuffer( file_text.data(), flags, database_text.data(), buffer.data(), size );

            if (length > 0 && length < size)
            {
                std::u16string text( reinterpret_cast<const char16_t *>( buffer.data() ), static_cast<size_t>( length ) );
                return Utf8FromUtf16( text );
            }
        }

        return std::string();
        #else
        throw Error( ErrorCode::ERR_UNSUPPORTED, "Detect It Easy is only available on Windows" );
        #endif
    }

    std::string DieScanMemory(
        const std::filesystem::path & dll_path,
        const std::filesystem::path & database,
        std::span< const uint8_t > data,
        uint32_t flags
    )
    {
        #ifdef _WIN32
        {
            std::lock_guard< std::mutex > lock( g_DieMutex );
            Require( LoadDie( dll_path ), ErrorCode::ERR_UNSUPPORTED, "Detect It Easy (die.dll) could not be loaded from " + dll_path.string() );

            if (g_Die.m_ScanMemoryA != nullptr && g_Die.m_FreeA != nullptr && data.size() <= 0x7FFFFFFF)
            {
                std::u8string database_text = database.u8string();

                // die_library only reads the buffer, the signature is not const.
                char * result = g_Die.m_ScanMemoryA( reinterpret_cast<char *>(const_cast<uint8_t *>(data.data())), static_cast<int>(data.size()), flags,
                    reinterpret_cast<char *>(database_text.data()) );
                Require( result != nullptr, ErrorCode::ERR_INTERNAL, "Detect It Easy returned no result" );
                std::string text( result );
                g_Die.m_FreeA( result );
                return text;
            }
        }

        // Fallback. The bytes in a temporary file.
        std::error_code error;
        std::filesystem::path folder = std::filesystem::temp_directory_path( error ) / L"CoffeeExplorer";
        std::filesystem::create_directories( folder, error );
        std::filesystem::path file = folder / (L"die_" + std::to_wstring( GetCurrentProcessId() ) + L"_" + std::to_wstring( GetTickCount64() ) + L".bin");

        {
            std::ofstream stream( file, std::ios::binary | std::ios::trunc );
            Require( stream.good(), ErrorCode::ERR_IO, "The temporary file for Detect It Easy could not be created" );
            stream.write( reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()) );
            Require( stream.good(), ErrorCode::ERR_IO, "The temporary file for Detect It Easy could not be written" );
        }

        try
        {
            std::string text = DieScanFile( dll_path, database, file, flags );
            std::filesystem::remove( file, error );
            return text;
        }
        catch (...)
        {
            std::filesystem::remove( file, error );
            throw;
        }
        #else
        (void)data;
        throw Error( ErrorCode::ERR_UNSUPPORTED, "Detect It Easy is only available on Windows" );
        #endif
    }
}