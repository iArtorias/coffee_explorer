/*
    Coffee Explorer core, flat C ABI for the WPF front end.

    Rules:

    - Paths and short texts are UTF-16 (char16_t), null terminated. Structured requests and answers are UTF-8 JSON.
    - Functions return 'PcResult'. 'PC_OK' is zero, errors are negative. 'PcGetLastError' returns the message of the last error on
      the calling thread.
    - Handles are opaque. A document belongs to one thread, use 'PcDocumentClone' for work on another thread.
    - Blobs returned by 'PcQuery', 'PcExecute' and 'PcExportResource' are released with 'PcBlobFree'.

    Queries:

      { "query": "tree" }                                      Structure tree, see 'Views.h'.
      { "query": "view", "node": id, "defer" }                 Node view, see 'Views.h' and 'Resources.h'. "defer": true
                                                               Leaves the whole file hashes out of the overview.
      { "query": "overview_hashes" }                           { "md5", "sha1", "sha256", "blake3", "signature", "matches" }
      { "query": "info" }                                      { "pe", "is64", "dotnet", "machine", "image_base", "entry_point" }
      { "query": "convert", "kind": "va|rva|offset", "value" } Address converter, see 'Tools.h'.
      { "query": "imports" }                                   Import summary for the dependency walker.
      { "query": "exports" }                                   Export summary for the 'Import Adder'.
      { "query": "disassemble", "offset", "size", "base", "bits", "max" }
      { "query": "resolve", "name", "directory", "is64" }      Module search, { "path", "found", "how" }
      { "query": "die", "dll", "database", "file", "flags" }   Detect It Easy result (DiE JSON text in "result").
        "memory": true scans the current bytes of the document instead (unsaved changes), the document stays
        unlocked during the scan.
      { "query": "die_available", "dll" }                      { "available" }
      { "query": "capabilities" }                              { "disassembler", "version" }

    Commands are listed in 'Operations.h' and 'Resources.h'. Every command is one undo step.
*/

#pragma once

#include <stdint.h>

#ifdef __cplusplus
#define PC_EXTERN extern "C"
#else
#define PC_EXTERN
#endif

#if defined( PECORE_STATIC )
#define PC_API PC_EXTERN
#elif defined( _WIN32 )
#ifdef PECORE_EXPORTS
#define PC_API PC_EXTERN __declspec( dllexport )
#else
#define PC_API PC_EXTERN __declspec( dllimport )
#endif
#else
#define PC_API PC_EXTERN __attribute__( ( visibility( "default" ) ) )
#endif

#define PC_VERSION 0x00010100

#define PC_OK 0
#define PC_ERR_INVALID_ARGUMENT -1
#define PC_ERR_IO -2
#define PC_ERR_ACCESS_DENIED -3
#define PC_ERR_NOT_FOUND -4
#define PC_ERR_READ_ONLY -5
#define PC_ERR_NOT_PE -6
#define PC_ERR_OUT_OF_RANGE -7
#define PC_ERR_PARSE -8
#define PC_ERR_UNSUPPORTED -9
#define PC_ERR_INTERNAL -10
#define PC_ERR_BUFFER_TOO_SMALL -11

typedef int32_t PcResult;
typedef struct PcDocumentHandle * PcDocument;
typedef struct PcBlobHandle * PcBlob;

typedef struct PcDocumentInfo
{
    uint64_t m_Size;
    uint64_t m_Revision;
    int32_t m_Modified;
    int32_t m_CanUndo;
    int32_t m_CanRedo;
    int32_t m_IsPe;
    int32_t m_Is64;
    int32_t m_IsDotNet;
    int32_t m_HasPath;
    int32_t m_Reserved;
} PcDocumentInfo;

// General.

PC_API uint32_t PcGetVersion( void );

PC_API PcResult PcGetLastError(
    char16_t * buffer,
    int32_t capacity
);

// Documents.

// 'size_limit' 0 means no limit.
PC_API PcResult PcDocumentOpen(
    const char16_t * path,
    uint64_t size_limit,
    PcDocument * document
);

PC_API PcResult PcDocumentOpenBytes(
    const uint8_t * data,
    uint64_t size,
    const char16_t * name,
    PcDocument * document
);

PC_API PcResult PcDocumentClone(
    PcDocument document,
    PcDocument * clone
);

PC_API void PcDocumentClose(
    PcDocument document
);

PC_API PcResult PcDocumentGetInfo(
    PcDocument document,
    PcDocumentInfo * info
);

// Size of the current bytes without parsing anything ['PcDocumentGetInfo' parses the image after a change of the headers].
PC_API PcResult PcDocumentGetSize(
    PcDocument document,
    uint64_t * size
);

// Full path for file documents, the display name otherwise.
PC_API PcResult PcDocumentGetPath(
    PcDocument document,
    char16_t * buffer,
    int32_t capacity
);

PC_API PcResult PcDocumentRead(
    PcDocument document,
    uint64_t offset,
    uint8_t * data,
    uint32_t length,
    uint32_t * read
);

// Overwrites bytes inside the file, one undo step [or part of the open group].
PC_API PcResult PcDocumentWrite(
    PcDocument document,
    uint64_t offset,
    const uint8_t * data,
    uint32_t length,
    const char16_t * description
);

PC_API PcResult PcDocumentBeginGroup(
    PcDocument document,
    const char16_t * description
);

PC_API PcResult PcDocumentEndGroup(
    PcDocument document
);

PC_API PcResult PcDocumentUndo(
    PcDocument document,
    uint64_t * offset
);

PC_API PcResult PcDocumentRedo(
    PcDocument document,
    uint64_t * offset
);

// Description of the next undo [redo = 0] or redo [redo = 1] step.
PC_API PcResult PcDocumentGetUndoText(
    PcDocument document,
    int32_t redo,
    char16_t * buffer,
    int32_t capacity
);

// 'path' null saves to the current file.
PC_API PcResult PcDocumentSave(
    PcDocument document,
    const char16_t * path,
    int32_t make_backup
);

PC_API PcResult PcDocumentReload(
    PcDocument document
);

// Structured requests.

PC_API PcResult PcQuery(
    PcDocument document,
    const char * request,
    PcBlob * result
);

PC_API PcResult PcExecute(
    PcDocument document,
    const char * command,
    PcBlob * result
);

// Resource as a complete file [.ico, .cur, .bmp or raw], extension receives '.ico' etc.
PC_API PcResult PcExportResource(
    PcDocument document,
    int32_t index,
    int32_t preview,
    PcBlob * data,
    char16_t * extension,
    int32_t capacity
);

PC_API PcResult PcBlobGet(
    PcBlob blob,
    const uint8_t ** data,
    uint64_t * size
);

PC_API void PcBlobFree(
    PcBlob blob
);