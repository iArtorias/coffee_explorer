#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl_core.h>
#include <shlguid.h>
#include <shldisp.h>
#include <exdisp.h>
#include <oleauto.h>

#include <new>
#include <string>
#include <vector>

/*
    'Open with Coffee Explorer' in the Windows 11 compact context menu.
*/

namespace
{
    // '{5EA954C8-DDE2-46E2-8BE3-200CAB988D11}', the same id is in 'AppxManifest.xml'.
    constexpr CLSID CLSID_OPEN_COMMAND = { 0x5ea954c8, 0xdde2, 0x46e2, { 0x8b, 0xe3, 0x20, 0x0c, 0xab, 0x98, 0x8d, 0x11 } };

    constexpr wchar_t TITLE[] = L"Open with Coffee Explorer";
    constexpr wchar_t EXECUTABLE[] = L"CoffeeExplorer.exe";

    HMODULE g_Module = nullptr;
    long g_Objects = 0;
    long g_Locks = 0;

    // Folder of this library with a trailing backslash, the executable sits next to it.
    std::wstring ModuleFolder()
    {
        std::wstring path( MAX_PATH, L'\0' );

        for (;; )
        {
            DWORD length = GetModuleFileNameW( g_Module, path.data(), static_cast<DWORD>(path.size()) );

            if (length == 0)
            {
                return std::wstring();
            }

            if (length < path.size())
            {
                path.resize( length );
                break;
            }

            path.resize( path.size() * 2 );
        }

        size_t slash = path.find_last_of( L"\\/" );
        return slash == std::wstring::npos ? std::wstring() : path.substr( 0, slash + 1 );
    }

    std::wstring ExecutablePath()
    {
        return ModuleFolder() + EXECUTABLE;
    }

    // File system paths of the selection, items without one (virtual folders, search results of libraries) are skipped.
    std::vector< std::wstring > SelectedPaths(
        IShellItemArray * items
    )
    {
        std::vector< std::wstring > paths;
        DWORD count = 0;

        if (items == nullptr || FAILED( items->GetCount( &count ) ))
        {
            return paths;
        }

        for (DWORD index = 0; index < count; ++index)
        {
            IShellItem * item = nullptr;

            if (FAILED( items->GetItemAt( index, &item ) ))
            {
                continue;
            }

            PWSTR path = nullptr;

            if (SUCCEEDED( item->GetDisplayName( SIGDN_FILESYSPATH, &path ) ))
            {
                paths.emplace_back( path );
                CoTaskMemFree( path );
            }

            item->Release();
        }

        return paths;
    }

    // Minimal owner of a COM interface pointer.
    template < typename Interface >
    class ComReference
    {
    private:

        Interface * m_Pointer = nullptr;

    public:

        ComReference() = default;
        ComReference( const ComReference & ) = delete;
        ComReference & operator=( const ComReference & ) = delete;

        ~ComReference()
        {
            if (m_Pointer != nullptr)
            {
                m_Pointer->Release();
            }
        }

        Interface ** operator&()
        {
            return &m_Pointer;
        }

        Interface * operator->() const
        {
            return m_Pointer;
        }

        Interface * Get() const
        {
            return m_Pointer;
        }
    };

    class BstrVariant
    {
    public:

        VARIANT m_Value;

        explicit BstrVariant(
            const wchar_t * text
        )
        {
            VariantInit( &m_Value );

            if (text != nullptr)
            {
                m_Value.vt = VT_BSTR;
                m_Value.bstrVal = SysAllocString( text );
            }
        }

        BstrVariant( const BstrVariant & ) = delete;
        BstrVariant & operator=( const BstrVariant & ) = delete;

        ~BstrVariant()
        {
            VariantClear( &m_Value );
        }
    };

    // Lets the desktop Explorer start the program ('IShellDispatch2::ShellExecute' of the desktop folder view), so the new
    // process is a child of Explorer and not of the calling process. Fails when Explorer is not the shell.
    HRESULT ShellExecuteFromExplorer(
        const wchar_t * file,
        const wchar_t * arguments,
        const wchar_t * folder
    )
    {
        ComReference< IShellWindows > windows;
        HRESULT result = CoCreateInstance( CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS( &windows ) );

        if (FAILED( result ))
        {
            return result;
        }

        VARIANT empty;
        VariantInit( &empty );
        long window = 0;
        ComReference< IDispatch > desktop;
        result = windows->FindWindowSW( &empty, &empty, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, &desktop );

        // 'S_FALSE', no desktop window.
        if (result != S_OK || desktop.Get() == nullptr)
        {
            return FAILED( result ) ? result : E_FAIL;
        }

        ComReference< IShellBrowser > browser;
        result = IUnknown_QueryService( desktop.Get(), SID_STopLevelBrowser, IID_PPV_ARGS( &browser ) );

        if (FAILED( result ))
        {
            return result;
        }

        ComReference< IShellView > view;
        result = browser->QueryActiveShellView( &view );

        if (FAILED( result ))
        {
            return result;
        }

        ComReference< IDispatch > background;
        result = view->GetItemObject( SVGIO_BACKGROUND, IID_PPV_ARGS( &background ) );

        if (FAILED( result ))
        {
            return result;
        }

        ComReference< IShellFolderViewDual > folder_view;
        result = background->QueryInterface( IID_PPV_ARGS( &folder_view ) );

        if (FAILED( result ))
        {
            return result;
        }

        ComReference< IDispatch > application;
        result = folder_view->get_Application( &application );

        if (FAILED( result ))
        {
            return result;
        }

        ComReference< IShellDispatch2 > shell;
        result = application->QueryInterface( IID_PPV_ARGS( &shell ) );

        if (FAILED( result ))
        {
            return result;
        }

        BSTR file_text = SysAllocString( file );

        if (file_text == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        BstrVariant argument_value( arguments );
        BstrVariant folder_value( folder );
        BstrVariant operation( nullptr );
        VARIANT show;
        VariantInit( &show );
        show.vt = VT_I4;
        show.lVal = SW_SHOWNORMAL;

        result = shell->ShellExecute( file_text, argument_value.m_Value, folder_value.m_Value, operation.m_Value, show );
        SysFreeString( file_text );
        return result;
    }

    std::wstring QuoteArgument(
        const std::wstring & text
    )
    {
        // Paths cannot contain quotes, only a trailing backslash would escape the closing quote.
        std::wstring quoted = L"\"" + text;

        if (!text.empty() && text.back() == L'\\')
        {
            quoted += L'\\';
        }

        return quoted + L"\"";
    }

    class OpenCommand final : public IExplorerCommand
    {
    private:

        long m_References = 1;

        ~OpenCommand()
        {
            InterlockedDecrement( &g_Objects );
        }

    public:
    
        OpenCommand()
        {
            InterlockedIncrement( &g_Objects );
        }

        // IUnknown

        IFACEMETHODIMP QueryInterface(
            REFIID riid,
            void ** object
        ) override
        {
            static const QITAB TABLE[] = {
                QITABENT( OpenCommand, IExplorerCommand ),
                { nullptr, 0 }
            };

            return QISearch( this, TABLE, riid, object );
        }

        IFACEMETHODIMP_( ULONG ) AddRef() override
        {
            return static_cast<ULONG>(InterlockedIncrement( &m_References ));
        }

        IFACEMETHODIMP_( ULONG ) Release() override
        {
            long references = InterlockedDecrement( &m_References );

            if (references == 0)
            {
                delete this;
            }

            return static_cast<ULONG>(references);
        }

        // IExplorerCommand

        IFACEMETHODIMP GetTitle(
            IShellItemArray *,
            LPWSTR * name
        ) override
        {
            return SHStrDupW( TITLE, name );
        }

        IFACEMETHODIMP GetIcon(
            IShellItemArray *,
            LPWSTR * icon
        ) override
        {
            return SHStrDupW( (ExecutablePath() + L",0").c_str(), icon );
        }

        IFACEMETHODIMP GetToolTip(
            IShellItemArray *,
            LPWSTR * tip
        ) override
        {
            *tip = nullptr;
            return E_NOTIMPL;
        }

        IFACEMETHODIMP GetCanonicalName(
            GUID * name
        ) override
        {
            *name = CLSID_OPEN_COMMAND;
            return S_OK;
        }

        IFACEMETHODIMP GetState(
            IShellItemArray *,
            BOOL,
            EXPCMDSTATE * state
        ) override
        {
            *state = ECS_ENABLED;
            return S_OK;
        }

        IFACEMETHODIMP Invoke(
            IShellItemArray * items,
            IBindCtx *
        ) override
        {
            std::vector< std::wstring > paths = SelectedPaths( items );

            if (paths.empty())
            {
                return S_OK;
            }

            std::wstring executable = ExecutablePath();
            std::wstring arguments;

            for (const std::wstring & path : paths)
            {
                arguments += (arguments.empty() ? L"" : L" ") + QuoteArgument( path );
            }

            std::wstring folder = ModuleFolder();

            // Explorer starts the editor outside of the package [see the top of this file].
            if (SUCCEEDED( ShellExecuteFromExplorer( executable.c_str(), arguments.c_str(), folder.empty() ? nullptr : folder.c_str() ) ))
            {
                return S_OK;
            }

            // Without Explorer as the shell the editor runs inside the package, Coffee Explorer restarts itself before
            // it changes the package.
            std::wstring command_line = QuoteArgument( executable ) + L" " + arguments;
            STARTUPINFOW startup = { sizeof( startup ) };
            PROCESS_INFORMATION process = {};

            if (!CreateProcessW( executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.empty() ? nullptr : folder.c_str(),
                &startup, &process ))
            {
                return HRESULT_FROM_WIN32( GetLastError() );
            }

            // Lets the editor take the foreground, Explorer owns it while the menu runs.
            AllowSetForegroundWindow( process.dwProcessId );
            CloseHandle( process.hThread );
            CloseHandle( process.hProcess );
            return S_OK;
        }

        IFACEMETHODIMP GetFlags(
            EXPCMDFLAGS * flags
        ) override
        {
            *flags = ECF_DEFAULT;
            return S_OK;
        }

        IFACEMETHODIMP EnumSubCommands(
            IEnumExplorerCommand ** commands
        ) override
        {
            *commands = nullptr;
            return E_NOTIMPL;
        }
    };

    class ClassFactory final : public IClassFactory
    {
    public:
        IFACEMETHODIMP QueryInterface(
            REFIID riid,
            void ** object
        ) override
        {
            static const QITAB TABLE[] = {
                QITABENT( ClassFactory, IClassFactory ),
                { nullptr, 0 }
            };

            return QISearch( this, TABLE, riid, object );
        }

        // The factory is a static object, its lifetime is the module.
        IFACEMETHODIMP_( ULONG ) AddRef() override
        {
            return 2;
        }

        IFACEMETHODIMP_( ULONG ) Release() override
        {
            return 1;
        }

        IFACEMETHODIMP CreateInstance(
            IUnknown * outer,
            REFIID riid,
            void ** object
        ) override
        {
            *object = nullptr;

            if (outer != nullptr)
            {
                return CLASS_E_NOAGGREGATION;
            }

            OpenCommand * command = new (std::nothrow) OpenCommand();

            if (command == nullptr)
            {
                return E_OUTOFMEMORY;
            }

            HRESULT result = command->QueryInterface( riid, object );
            command->Release();
            return result;
        }

        IFACEMETHODIMP LockServer(
            BOOL lock
        ) override
        {
            if (lock)
            {
                InterlockedIncrement( &g_Locks );
            }
            else
            {
                InterlockedDecrement( &g_Locks );
            }

            return S_OK;
        }
    };

    ClassFactory g_Factory;
}

BOOL APIENTRY DllMain(
    HMODULE module,
    DWORD reason,
    LPVOID
)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_Module = module;
        DisableThreadLibraryCalls( module );
    }

    return TRUE;
}

STDAPI DllGetClassObject(
    REFCLSID clsid,
    REFIID riid,
    LPVOID * object
)
{
    *object = nullptr;

    if (!IsEqualCLSID( clsid, CLSID_OPEN_COMMAND ))
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    return g_Factory.QueryInterface( riid, object );
}

// Used by CoffeeExplorer.exe: starts a program through Explorer, outside of the package of the caller.
STDAPI LaunchOutsidePackage(
    LPCWSTR file,
    LPCWSTR arguments,
    LPCWSTR folder
)
{
    if (file == nullptr)
    {
        return E_INVALIDARG;
    }

    // 'RPC_E_CHANGED_MODE', the thread already is in the MTA, the shell objects still work through their proxies.
    HRESULT initialized = CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
    HRESULT result = ShellExecuteFromExplorer( file, arguments, folder );

    if (SUCCEEDED( initialized ))
    {
        CoUninitialize();
    }

    return result;
}

STDAPI DllCanUnloadNow()
{
    return g_Objects == 0 && g_Locks == 0 ? S_OK : S_FALSE;
}