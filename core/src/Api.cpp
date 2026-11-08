#include "pecore/Api.h"

#include "pecore/Authenticode.h"
#include "pecore/Document.h"
#include "pecore/Json.h"
#include "pecore/Operations.h"
#include "pecore/PeImage.h"
#include "pecore/Platform.h"
#include "pecore/Resources.h"
#include "pecore/Symbols.h"
#include "pecore/Tools.h"
#include "pecore/Views.h"

#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>

// Opaque handle types of the C ABI.
struct PcDocumentHandle
{
    std::unique_ptr< PeCore::Document > m_Document;
    std::unique_ptr< PeCore::PeImage > m_Image;
    uint64_t m_ImageRevision = 0;

    // PDB symbols, loaded on request, they stay with the document across edits.
    std::shared_ptr< PeCore::SymbolTable > m_Symbols;
    std::string m_SymbolMessage;

    // The UI loads views on worker threads, every call on the document holds this lock.
    std::recursive_mutex m_Lock;

    // Header shape of the bytes the image was parsed from, and what the parse said about them [PcDocumentGetInfo].
    struct Shape
    {
        bool m_Pe = false;
        bool m_Is64 = false;
        bool m_DotNet = false;

        bool operator==( const Shape & ) const = default;
    };

    Shape m_ParsedProbe;
    Shape m_ParsedFlags;

    // Cheap look at the headers, no PeLib. MZ, the PE signature, the optional header magic and the COM descriptor.
    static Shape Probe(
        const std::vector< uint8_t > & data
    )
    {
        auto read = [&] ( uint64_t offset, size_t size ) -> uint64_t
        {
            uint64_t value = 0;

            for (size_t index = 0; index < size && offset + index < data.size(); ++index)
            {
                value |= static_cast<uint64_t>( data[static_cast<size_t>( offset + index )] ) << (8 * index);
            }

            return value;
        };

        Shape shape;

        if (data.size() < 0x40 || read( 0, 2 ) != 0x5A4D)
        {
            return shape;
        }

        uint64_t nt = read( 0x3C, 4 );

        if (nt + 24 > data.size() || read( nt, 4 ) != 0x00004550)
        {
            return shape;
        }

        uint64_t optional = nt + 24;
        uint64_t magic = read( optional, 2 );
        shape.m_Pe = magic == 0x10B || magic == 0x20B;
        shape.m_Is64 = magic == 0x20B;
        uint64_t directories = optional + (shape.m_Is64 ? 112 : 96);
        uint64_t count = read( optional + (shape.m_Is64 ? 108 : 92), 4 );
        shape.m_DotNet = shape.m_Pe && count > 14 && read( directories + 14 * 8, 4 ) != 0;
        return shape;
    }

    // Parsed image of the current bytes, rebuilt after every change.
    const PeCore::PeImage & GetImage()
    {
        if (m_Image == nullptr || m_ImageRevision != m_Document->GetRevision())
        {
            m_Image.reset();
            m_Image = std::make_unique< PeCore::PeImage >( m_Document->GetData() );
            m_ImageRevision = m_Document->GetRevision();
            m_ParsedProbe = Probe( m_Document->GetData() );
            m_ParsedFlags = { m_Image->IsPe(), m_Image->Is64(), m_Image->IsPe() && m_Image->IsDotNet() };
        }

        return *m_Image;
    }

    // PE, 64 bit and .NET flags of the current bytes. Typing in the hex view changes the revision with every key.
    // As long as the header shape stays the same the flags of the last parse still hold, the image is parsed again only when a
    // view needs it [it was parsed for every key before].
    Shape GetShape()
    {
        if (m_Image != nullptr && m_ImageRevision != m_Document->GetRevision() && Probe( m_Document->GetData() ) == m_ParsedProbe)
        {
            return m_ParsedFlags;
        }

        GetImage();
        return m_ParsedFlags;
    }
};

struct PcBlobHandle
{
    std::string m_Data;
};

namespace
{
    using namespace PeCore;

    thread_local std::u16string t_LastError;

    PcResult ToResult(
        ErrorCode code
    )
    {
        switch (code)
        {
            case ErrorCode::ERR_NONE:
                return PC_OK;
            case ErrorCode::ERR_INVALID_ARGUMENT:
                return PC_ERR_INVALID_ARGUMENT;
            case ErrorCode::ERR_IO:
                return PC_ERR_IO;
            case ErrorCode::ERR_ACCESS_DENIED:
                return PC_ERR_ACCESS_DENIED;
            case ErrorCode::ERR_NOT_FOUND:
                return PC_ERR_NOT_FOUND;
            case ErrorCode::ERR_READ_ONLY:
                return PC_ERR_READ_ONLY;
            case ErrorCode::ERR_NOT_PE:
                return PC_ERR_NOT_PE;
            case ErrorCode::ERR_OUT_OF_RANGE:
                return PC_ERR_OUT_OF_RANGE;
            case ErrorCode::ERR_PARSE:
                return PC_ERR_PARSE;
            case ErrorCode::ERR_UNSUPPORTED:
                return PC_ERR_UNSUPPORTED;
            default:
                return PC_ERR_INTERNAL;
        }
    }

    // Runs body and converts every exception into a result code plus the thread local message.
    template< typename BODY >
    PcResult Guard(
        BODY && body
    )
    {
        try
        {
            t_LastError.clear();
            return body();
        }
        catch (const Error & error)
        {
            t_LastError = Utf16FromUtf8( error.what() );
            return ToResult( error.GetCode() );
        }
        catch (const std::filesystem::filesystem_error & error)
        {
            t_LastError = Utf16FromUtf8( error.what() );
            return error.code() == std::errc::permission_denied ? PC_ERR_ACCESS_DENIED : PC_ERR_IO;
        }
        catch (const std::bad_alloc &)
        {
            t_LastError = u"Out of memory";
            return PC_ERR_INTERNAL;
        }
        catch (const std::exception & error)
        {
            t_LastError = Utf16FromUtf8( error.what() );
            return PC_ERR_INTERNAL;
        }
        catch (...)
        {
            t_LastError = u"Unknown error";
            return PC_ERR_INTERNAL;
        }
    }

    void RequireArgument(
        bool condition,
        const char * message
    )
    {
        if (!condition)
        {
            throw Error( ErrorCode::ERR_INVALID_ARGUMENT, message );
        }
    }

    PcDocumentHandle & GetHandle(
        PcDocument document
    )
    {
        RequireArgument( document != nullptr && document->m_Document != nullptr, "Invalid document handle" );
        return *document;
    }

    // Serializes the calls on one document (no-op for a null handle, GetHandle reports that)
    class DocumentLock
    {
    private:

        std::unique_lock< std::recursive_mutex > m_Lock;

    public:

        explicit DocumentLock(
            PcDocument document
        )
        {
            if (document != nullptr)
            {
                m_Lock = std::unique_lock< std::recursive_mutex >( document->m_Lock );
            }
        }

        // Long calls that work on a copy of the bytes let the UI keep editing.
        void Release()
        {
            if (m_Lock.owns_lock())
            {
                m_Lock.unlock();
            }
        }
    };

    std::string DieAnswer(
        const std::string & result
    )
    {
        JsonWriter writer;
        writer.BeginObject();
        writer.Member( "result", result );
        writer.EndObject();
        return writer.TakeText();
    }

    std::u16string ToString(
        const char16_t * text
    )
    {
        return text != nullptr ? std::u16string( text ) : std::u16string();
    }

    PcResult CopyString(
        const std::u16string & text,
        char16_t * buffer,
        int32_t capacity
    )
    {
        if (buffer == nullptr || capacity <= 0)
        {
            return PC_ERR_INVALID_ARGUMENT;
        }

        size_t count = std::min< size_t >( text.size(), static_cast<size_t>(capacity - 1) );
        std::memcpy( buffer, text.data(), count * sizeof( char16_t ) );
        buffer[count] = 0;
        return text.size() < static_cast<size_t>(capacity) ? PC_OK : PC_ERR_BUFFER_TOO_SMALL;
    }

    PcResult WrapDocument(
        std::unique_ptr< Document > document,
        PcDocument * out
    )
    {
        auto handle = std::make_unique< PcDocumentHandle >();
        handle->m_Document = std::move( document );
        *out = handle.release();
        return PC_OK;
    }

    PcResult WrapBlob(
        std::string data,
        PcBlob * out
    )
    {
        auto blob = std::make_unique< PcBlobHandle >();
        blob->m_Data = std::move( data );
        *out = blob.release();
        return PC_OK;
    }

    std::filesystem::path PathOf(
        const std::string & utf8
    )
    {
        return std::filesystem::path( Utf16FromUtf8( utf8 ) );
    }

    std::string RunQuery(
        PcDocument document,
        const JsonValue & request
    )
    {
        std::string query = request.GetString( "query" );

        // Queries that work without a document.
        if (query == "resolve")
        {
            ResolvedModule module = ResolveModule( request.GetString( "name" ), PathOf( request.GetString( "directory" ) ), request.GetBool( "is64", true ) );
            JsonWriter writer;
            writer.BeginObject();
            writer.Member( "path", Utf8FromUtf16( module.m_Path.u16string() ) );
            writer.MemberBool( "found", module.m_Found );
            writer.Member( "how", module.m_How );
            writer.EndObject();
            return writer.TakeText();
        }

        // Previous versions of saved files that were in use when they were replaced [ReplaceFileKeepingMetadata].
        if (query == "cleanup_replaced")
        {
            JsonWriter writer;
            writer.BeginObject();
            writer.MemberUnsigned( "remaining", static_cast<uint64_t>(DeletePendingReplacedFiles()) );
            writer.EndObject();
            return writer.TakeText();
        }

        if (query == "die_available")
        {
            JsonWriter writer;
            writer.BeginObject();
            writer.MemberBool( "available", IsDieAvailable( PathOf( request.GetString( "dll" ) ) ) );
            writer.EndObject();
            return writer.TakeText();
        }

        if (query == "die")
        {
            std::string file = request.GetString( "file" );

            if (file.empty() && document != nullptr)
            {
                file = Utf8FromUtf16( GetHandle( document ).m_Document->GetPath().u16string() );
            }

            RequireArgument( !file.empty(), "Detect It Easy needs a file on disk, save the document first" );
            return DieAnswer( DieScanFile( PathOf( request.GetString( "dll" ) ), PathOf( request.GetString( "database" ) ), PathOf( file ),
                static_cast<uint32_t>(request.GetUnsigned( "flags", 0x00020000 )) ) );
        }

        if (query == "capabilities")
        {
            JsonWriter writer;
            writer.BeginObject();
            writer.MemberBool( "disassembler", IsDisassemblerAvailable() );
            writer.MemberUnsigned( "version", PC_VERSION );
            writer.EndObject();
            return writer.TakeText();
        }

        PcDocumentHandle & handle = GetHandle( document );
        const PeImage & image = handle.GetImage();
        ViewContext context { *handle.m_Document, image, handle.m_Symbols };

        if (query == "tree")
        {
            return BuildTree( context );
        }

        if (query == "view")
        {
            context.m_DeferSlow = request.GetBool( "defer", false );
            return BuildView( context, request.GetString( "node", "overview" ) );
        }

        if (query == "overview_hashes")
        {
            return BuildOverviewHashes( *handle.m_Document, image );
        }

        if (query == "info")
        {
            JsonWriter writer;
            writer.BeginObject();
            writer.MemberBool( "pe", image.IsPe() );
            writer.MemberBool( "is64", image.Is64() );
            writer.MemberBool( "dotnet", image.IsPe() && image.IsDotNet() );

            if (image.IsPe())
            {
                writer.Member( "machine", Hex( image.GetMachine(), 4 ) );
                writer.Member( "image_base", Hex( image.GetImageBase(), image.Is64() ? 16 : 8 ) );
                writer.Member( "entry_point", Hex( image.GetEntryPoint() ) );
                uint64_t entry_offset = image.RvaToOffset( image.GetEntryPoint() );

                if (entry_offset != NO_OFFSET)
                {
                    writer.MemberUnsigned( "entry_offset", entry_offset );
                }
            }

            writer.EndObject();
            return writer.TakeText();
        }

        if (query == "convert")
        {
            uint64_t value = 0;
            RequireArgument( ParseNumber( request.GetString( "value" ), value ), "Enter a hex number" );
            AddressNames names( image, handle.m_Symbols );
            return ConvertAddress( image, request.GetString( "kind" ), value, &names );
        }

        // { "signed", "matches", "text" }, cached per revision, the UI asks after edits of signed files.
        if (query == "signature")
        {
            AuthenticodeSummary summary = GetAuthenticodeSummary( *handle.m_Document, image );
            JsonWriter writer;
            writer.BeginObject();
            writer.MemberBool( "signed", summary.m_Signed );
            writer.MemberBool( "matches", summary.m_Matches );
            writer.Member( "text", summary.m_Text );
            writer.EndObject();
            return writer.TakeText();
        }

        // Symbol state: { "codeview", "pdb", "guid", "age", "key", "loaded", "source", "count", "functions", "publics",
        // "message", "url", "store", "candidates" }. 'symbols_load { path, cache, force }' tries an explicit path or the
        // local candidates, then answers the same. Downloads are done by the UI into 'store' [symbol store path in cache].
        if (query == "symbols" || query == "symbols_load")
        {
            CodeViewInfo code_view = ReadCodeView( image );
            std::filesystem::path cache( Utf16FromUtf8( request.GetString( "cache" ) ) );
            std::string server = request.GetString( "server" );

            if (query == "symbols_load")
            {
                std::string error;
                std::string explicit_path = request.GetString( "path" );
                bool force = request.GetBool( "force", false );
                std::shared_ptr< SymbolTable > loaded;
                std::vector< std::string > reasons;

                auto attempt = [&] ( const std::filesystem::path & candidate )
                {
                    std::error_code code;

                    if (loaded != nullptr || !std::filesystem::is_regular_file( candidate, code ))
                    {
                        return;
                    }

                    loaded = SymbolTable::Load( candidate, image, code_view, !force, error );

                    if (loaded == nullptr)
                    {
                        reasons.push_back( Utf8FromUtf16( candidate.u16string() ) + ": " + error );
                    }
                };

                if (!explicit_path.empty())
                {
                    attempt( std::filesystem::path( Utf16FromUtf8( explicit_path ) ) );
                }
                else
                {
                    for (const std::filesystem::path & candidate : PdbCandidates( code_view, handle.m_Document->GetPath(), cache ))
                    {
                        attempt( candidate );
                    }
                }

                if (loaded != nullptr)
                {
                    handle.m_Symbols = loaded;
                    handle.m_SymbolMessage = "Loaded " + std::to_string( loaded->GetCount() ) + " symbols from " + Utf8FromUtf16( loaded->GetSource().u16string() );
                }
                else
                {
                    handle.m_SymbolMessage = !code_view.m_Present ? "The file has no CodeView (RSDS) record, there is no PDB to look for" :
                        reasons.empty() ? code_view.PdbFileName() + " was not found in the PDB path, next to the file or in the symbol caches" :
                        reasons.back();
                }
            }

            JsonWriter writer;
            writer.BeginObject();
            writer.MemberBool( "codeview", code_view.m_Present );
            writer.Member( "pdb", code_view.m_PdbPath );
            writer.Member( "guid", code_view.m_Present ? code_view.GuidText() : std::string() );
            writer.MemberUnsigned( "age", code_view.m_Age );
            writer.Member( "key", code_view.m_Present ? code_view.SearchKey() : std::string() );
            writer.MemberBool( "loaded", handle.m_Symbols != nullptr );
            writer.Member( "source", handle.m_Symbols != nullptr ? Utf8FromUtf16( handle.m_Symbols->GetSource().u16string() ) : std::string() );
            writer.MemberUnsigned( "count", handle.m_Symbols != nullptr ? handle.m_Symbols->GetCount() : 0 );
            writer.MemberUnsigned( "functions", handle.m_Symbols != nullptr ? handle.m_Symbols->GetFunctionCount() : 0 );
            writer.MemberUnsigned( "publics", handle.m_Symbols != nullptr ? handle.m_Symbols->GetPublicCount() : 0 );
            writer.Member( "message", handle.m_SymbolMessage );
            writer.Member( "url", code_view.m_Present ? SymbolServerUrl( code_view, server ) : std::string() );
            writer.Member( "store", code_view.m_Present && !cache.empty() ? Utf8FromUtf16( SymbolStorePath( code_view, cache ).u16string() ) : std::string() );
            writer.Key( "candidates" ).BeginArray();

            for (const std::filesystem::path & candidate : PdbCandidates( code_view, handle.m_Document->GetPath(), cache ))
            {
                writer.String( Utf8FromUtf16( candidate.u16string() ) );
            }

            writer.EndArray();
            writer.EndObject();
            return writer.TakeText();
        }

        if (query == "entropy")
        {
            return handle.m_Document->GetCached( "entropy", [&]
            {
                return BuildEntropyMap( image );
            } );
        }

        if (query == "imports")
        {
            return BuildImportSummary( image );
        }

        if (query == "exports")
        {
            return BuildExportSummary( image );
        }

        if (query == "disassemble")
        {
            AddressNames names( image, handle.m_Symbols );
            return Disassemble( image, request.GetUnsigned( "offset" ), request.GetUnsigned( "size", 0x1000 ), request.GetUnsigned( "base" ),
                static_cast<int>(request.GetInteger( "bits", image.Is64() ? 64 : 32 )), static_cast<size_t>(request.GetUnsigned( "max", 20000 )),
                &names );
        }

        throw Error( ErrorCode::ERR_NOT_FOUND, "Unknown query " + query );
    }
}

PC_API uint32_t PcGetVersion( void )
{
    return PC_VERSION;
}

PC_API PcResult PcGetLastError(
    char16_t * buffer,
    int32_t capacity
)
{
    return CopyString( t_LastError, buffer, capacity );
}

PC_API PcResult PcDocumentOpen(
    const char16_t * path,
    uint64_t size_limit,
    PcDocument * document
)
{
    return Guard( [&]
    {
        RequireArgument( path != nullptr && document != nullptr, "Invalid argument" );
        return WrapDocument( Document::OpenFile( std::filesystem::path( ToString( path ) ), size_limit ), document );
    } );
}

PC_API PcResult PcDocumentOpenBytes(
    const uint8_t * data,
    uint64_t size,
    const char16_t * name,
    PcDocument * document
)
{
    return Guard( [&]
    {
        RequireArgument( (data != nullptr || size == 0) && document != nullptr, "Invalid argument" );
        std::vector< uint8_t > bytes( data, data + size );
        return WrapDocument( Document::FromBytes( std::move( bytes ), Utf8FromUtf16( ToString( name ) ) ), document );
    } );
}

PC_API PcResult PcDocumentClone(
    PcDocument document,
    PcDocument * clone
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        RequireArgument( clone != nullptr, "Invalid argument" );
        return WrapDocument( GetHandle( document ).m_Document->Clone(), clone );
    } );
}

PC_API void PcDocumentClose(
    PcDocument document
)
{
    if (document != nullptr)
    {
        // Waits for a call still running on a worker thread [a view that was loading when the tab closed].
        std::lock_guard< std::recursive_mutex > wait( document->m_Lock );
    }

    delete document;
}

PC_API PcResult PcDocumentGetInfo(
    PcDocument document,
    PcDocumentInfo * info
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        RequireArgument( info != nullptr, "Invalid argument" );
        PcDocumentHandle & handle = GetHandle( document );
        const Document & doc = *handle.m_Document;
        PcDocumentHandle::Shape shape = handle.GetShape();
        *info = PcDocumentInfo {};
        info->m_Size = doc.GetSize();
        info->m_Revision = doc.GetRevision();
        info->m_Modified = doc.IsModified() ? 1 : 0;
        info->m_CanUndo = doc.CanUndo() ? 1 : 0;
        info->m_CanRedo = doc.CanRedo() ? 1 : 0;
        info->m_IsPe = shape.m_Pe ? 1 : 0;
        info->m_Is64 = shape.m_Is64 ? 1 : 0;
        info->m_IsDotNet = shape.m_DotNet ? 1 : 0;
        info->m_HasPath = doc.GetPath().empty() ? 0 : 1;
        return PC_OK;
    } );
}

PC_API PcResult PcDocumentGetSize(
    PcDocument document,
    uint64_t * size
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        RequireArgument( size != nullptr, "Invalid argument" );
        *size = GetHandle( document ).m_Document->GetSize();
        return PC_OK;
    } );
}

PC_API PcResult PcDocumentGetPath(
    PcDocument document,
    char16_t * buffer,
    int32_t capacity
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        const Document & doc = *GetHandle( document ).m_Document;
        return CopyString( doc.GetPath().empty() ? Utf16FromUtf8( doc.GetName() ) : doc.GetPath().u16string(), buffer, capacity );
    } );
}

PC_API PcResult PcDocumentRead(
    PcDocument document,
    uint64_t offset,
    uint8_t * data,
    uint32_t length,
    uint32_t * read
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        RequireArgument( data != nullptr || length == 0, "Invalid argument" );
        size_t count = GetHandle( document ).m_Document->Read( offset, std::span< uint8_t >( data, length ) );

        if (read != nullptr)
        {
            *read = static_cast<uint32_t>(count);
        }

        return PC_OK;
    } );
}

PC_API PcResult PcDocumentWrite(
    PcDocument document,
    uint64_t offset,
    const uint8_t * data,
    uint32_t length,
    const char16_t * description
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        RequireArgument( data != nullptr || length == 0, "Invalid argument" );
        std::string text = Utf8FromUtf16( ToString( description ) );
        GetHandle( document ).m_Document->Overwrite( offset, std::span< const uint8_t >( data, length ), text.empty() ? "Edit" : text );
        return PC_OK;
    } );
}

PC_API PcResult PcDocumentBeginGroup(
    PcDocument document,
    const char16_t * description
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        GetHandle( document ).m_Document->BeginGroup( Utf8FromUtf16( ToString( description ) ) );
        return PC_OK;
    } );
}

PC_API PcResult PcDocumentEndGroup(
    PcDocument document
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        GetHandle( document ).m_Document->EndGroup();
        return PC_OK;
    } );
}

PC_API PcResult PcDocumentUndo(
    PcDocument document,
    uint64_t * offset
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        uint64_t position = 0;

        if (!GetHandle( document ).m_Document->Undo( position ))
        {
            throw Error( ErrorCode::ERR_NOT_FOUND, "Nothing to undo" );
        }

        if (offset != nullptr)
        {
            *offset = position;
        }

        return PC_OK;
    } );
}

PC_API PcResult PcDocumentRedo(
    PcDocument document,
    uint64_t * offset
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        uint64_t position = 0;

        if (!GetHandle( document ).m_Document->Redo( position ))
        {
            throw Error( ErrorCode::ERR_NOT_FOUND, "Nothing to redo" );
        }

        if (offset != nullptr)
        {
            *offset = position;
        }

        return PC_OK;
    } );
}

PC_API PcResult PcDocumentGetUndoText(
    PcDocument document,
    int32_t redo,
    char16_t * buffer,
    int32_t capacity
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        const Document & doc = *GetHandle( document ).m_Document;
        return CopyString( Utf16FromUtf8( redo ? doc.GetRedoDescription() : doc.GetUndoDescription() ), buffer, capacity );
    } );
}

PC_API PcResult PcDocumentSave(
    PcDocument document,
    const char16_t * path,
    int32_t make_backup
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        GetHandle( document ).m_Document->Save( std::filesystem::path( ToString( path ) ), make_backup != 0 );
        return PC_OK;
    } );
}

PC_API PcResult PcDocumentReload(
    PcDocument document
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        GetHandle( document ).m_Document->Reload();
        return PC_OK;
    } );
}

PC_API PcResult PcQuery(
    PcDocument document,
    const char * request,
    PcBlob * result
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        RequireArgument( request != nullptr && result != nullptr, "Invalid argument" );
        JsonValue parsed = JsonValue::Parse( request );

        // 'Detect It Easy' on the current bytes. Copy them, then scan without holding the document.
        if (document != nullptr && parsed.GetString( "query" ) == "die" && parsed.GetBool( "memory" ))
        {
            std::vector< uint8_t > data = GetHandle( document ).m_Document->GetData();
            lock.Release();
            return WrapBlob( DieAnswer( DieScanMemory( PathOf( parsed.GetString( "dll" ) ), PathOf( parsed.GetString( "database" ) ), data,
                static_cast<uint32_t>(parsed.GetUnsigned( "flags", 0x00020000 )) ) ), result );
        }

        return WrapBlob( RunQuery( document, parsed ), result );
    } );
}

PC_API PcResult PcExecute(
    PcDocument document,
    const char * command,
    PcBlob * result
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        RequireArgument( command != nullptr && result != nullptr, "Invalid argument" );
        PcDocumentHandle & handle = GetHandle( document );
        return WrapBlob( ExecuteCommand( *handle.m_Document, JsonValue::Parse( command ) ), result );
    } );
}

PC_API PcResult PcExportResource(
    PcDocument document,
    int32_t index,
    int32_t preview,
    PcBlob * data,
    char16_t * extension,
    int32_t capacity
)
{
    DocumentLock lock( document );
    return Guard( [&]
    {
        RequireArgument( data != nullptr && index >= 0, "Invalid argument" );
        std::string suffix;
        std::vector< uint8_t > bytes = ExportResource( GetHandle( document ).GetImage(), std::to_string( index ), preview != 0, suffix );

        if (extension != nullptr && capacity > 0)
        {
            CopyString( Utf16FromUtf8( suffix ), extension, capacity );
        }

        return WrapBlob( std::string( bytes.begin(), bytes.end() ), data );
    } );
}

PC_API PcResult PcBlobGet(
    PcBlob blob,
    const uint8_t ** data,
    uint64_t * size
)
{
    if (blob == nullptr || data == nullptr || size == nullptr)
    {
        return PC_ERR_INVALID_ARGUMENT;
    }

    *data = reinterpret_cast<const uint8_t *>(blob->m_Data.data());
    *size = blob->m_Data.size();
    return PC_OK;
}

PC_API void PcBlobFree(
    PcBlob blob
)
{
    delete blob;
}