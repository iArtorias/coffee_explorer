using System.Runtime.InteropServices;

namespace CoffeeExplorer.Interop
{
    // Invoke declarations for 'pecore.dll'.
    internal static unsafe partial class Native
    {
        private const string LIBRARY = "pecore.dll";

        public const int PC_OK = 0;
        public const int PC_ERR_INVALID_ARGUMENT = -1;
        public const int PC_ERR_IO = -2;
        public const int PC_ERR_ACCESS_DENIED = -3;
        public const int PC_ERR_NOT_FOUND = -4;
        public const int PC_ERR_READ_ONLY = -5;
        public const int PC_ERR_NOT_PE = -6;
        public const int PC_ERR_OUT_OF_RANGE = -7;
        public const int PC_ERR_PARSE = -8;
        public const int PC_ERR_UNSUPPORTED = -9;
        public const int PC_ERR_INTERNAL = -10;
        public const int PC_ERR_BUFFER_TOO_SMALL = -11;

        [StructLayout(LayoutKind.Sequential)]
        public struct PcDocumentInfo
        {
            public ulong m_Size;
            public ulong m_Revision;
            public int m_Modified;
            public int m_CanUndo;
            public int m_CanRedo;
            public int m_IsPe;
            public int m_Is64;
            public int m_IsDotNet;
            public int m_HasPath;
            public int m_Reserved;
        }

        [LibraryImport(LIBRARY)]
        public static partial uint PcGetVersion();

        [LibraryImport(LIBRARY)]
        public static partial int PcGetLastError(
            char* buffer,
            int capacity
        );

        [LibraryImport(LIBRARY, StringMarshalling = StringMarshalling.Utf16)]
        public static partial int PcDocumentOpen(
            string path,
            ulong size_limit,
            out nint document
        );

        [LibraryImport(LIBRARY, StringMarshalling = StringMarshalling.Utf16)]
        public static partial int PcDocumentOpenBytes(
            byte* data,
            ulong size,
            string name,
            out nint document
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcDocumentClone(
            nint document,
            out nint clone
        );

        [LibraryImport(LIBRARY)]
        public static partial void PcDocumentClose(
            nint document
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcDocumentGetInfo(
            nint document,
            out PcDocumentInfo info
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcDocumentGetSize(
            nint document,
            out ulong size
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcDocumentGetPath(
            nint document,
            char* buffer,
            int capacity
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcDocumentRead(
            nint document,
            ulong offset,
            byte* data,
            uint length,
            out uint read
        );

        [LibraryImport(LIBRARY, StringMarshalling = StringMarshalling.Utf16)]
        public static partial int PcDocumentWrite(
            nint document,
            ulong offset,
            byte* data,
            uint length,
            string description
        );

        [LibraryImport(LIBRARY, StringMarshalling = StringMarshalling.Utf16)]
        public static partial int PcDocumentBeginGroup(
            nint document,
            string description
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcDocumentEndGroup(
            nint document
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcDocumentUndo(
            nint document,
            out ulong offset
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcDocumentRedo(
            nint document,
            out ulong offset
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcDocumentGetUndoText(
            nint document,
            int redo,
            char* buffer,
            int capacity
        );

        [LibraryImport(LIBRARY, StringMarshalling = StringMarshalling.Utf16)]
        public static partial int PcDocumentSave(
            nint document,
            string? path,
            int make_backup
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcDocumentReload(
            nint document
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcQuery(
            nint document,
            byte* request,
            out nint result
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcExecute(
            nint document,
            byte* command,
            out nint result
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcExportResource(
            nint document,
            int index,
            int preview,
            out nint data,
            char* extension,
            int capacity
        );

        [LibraryImport(LIBRARY)]
        public static partial int PcBlobGet(
            nint blob,
            out byte* data,
            out ulong size
        );

        [LibraryImport(LIBRARY)]
        public static partial void PcBlobFree(
            nint blob
        );
    }
}