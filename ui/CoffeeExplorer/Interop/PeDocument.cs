using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace CoffeeExplorer.Interop
{
    // Error of a 'pecore.dll' call.
    public sealed class CoreException : Exception
    {
        public int Code { get; }

        public CoreException(
            int code,
            string message
        )
            : base(Services.LanguageService.TrText(message))
        {
            Code = code;
        }
    }

    public readonly struct DocumentInfo
    {
        public ulong Size { get; init; }

        public ulong Revision { get; init; }

        public bool IsModified { get; init; }

        public bool CanUndo { get; init; }

        public bool CanRedo { get; init; }

        public bool IsPe { get; init; }

        public bool Is64 { get; init; }

        public bool IsDotNet { get; init; }

        public bool HasPath { get; init; }
    }

    // Byte access used by the hex view.
    public interface IByteSource
    {
        ulong Length { get; }

        int Read(
            ulong offset,
            Span<byte> buffer
        );

        void Write(
            ulong offset,
            byte[] data,
            string description
        );

        // Typing in the hex view collects many writes in one undo step.
        void BeginGroup(
            string description
        );

        void EndGroup();

        bool IsReadOnly { get; }
    }
    public sealed unsafe class PeDocument : IDisposable, IByteSource
    {
        private readonly object m_Gate = new();
        private nint m_Handle;
        private int m_Users;
        private bool m_Closing;

        private PeDocument(
            nint handle,
            bool read_only
        )
        {
            m_Handle = handle;
            IsReadOnly = read_only;
        }

        ~PeDocument()
        {
            // Unreachable, so no call can be running.
            if (m_Handle != 0)
            {
                Native.PcDocumentClose(m_Handle);
                m_Handle = 0;
            }
        }

        public bool IsReadOnly { get; }

        public static uint CoreVersion => Native.PcGetVersion();

        public static PeDocument Open(
            string path,
            ulong size_limit,
            bool read_only = false
        )
        {
            Check(Native.PcDocumentOpen(path, size_limit, out nint handle));
            return new PeDocument(handle, read_only);
        }

        public static PeDocument OpenBytes(
            byte[] data,
            string name
        )
        {
            fixed (byte* pointer = data)
            {
                Check(Native.PcDocumentOpenBytes(pointer, (ulong)data.Length, name, out nint handle));
                return new PeDocument(handle, true);
            }
        }

        // Independent copy for work on another thread.
        public PeDocument Clone()
        {
            nint handle = Acquire();

            try
            {
                Check(Native.PcDocumentClone(handle, out nint clone));
                return new PeDocument(clone, true);
            }
            finally
            {
                Release();
            }
        }

        public void Dispose()
        {
            nint close = 0;

            lock (m_Gate)
            {
                if (m_Handle == 0 || m_Closing)
                {
                    return;
                }

                m_Closing = true;

                if (m_Users == 0)
                {
                    close = m_Handle;
                    m_Handle = 0;
                }
            }

            // Otherwise the last running call closes it.
            if (close != 0)
            {
                Native.PcDocumentClose(close);
            }

            GC.SuppressFinalize(this);
        }

        // The handle for one call, 'ObjectDisposedException' once the document is closed or closing.
        private nint Acquire()
        {
            lock (m_Gate)
            {
                if (m_Handle == 0 || m_Closing)
                {
                    throw new ObjectDisposedException(nameof(PeDocument));
                }

                ++m_Users;
                return m_Handle;
            }
        }

        private void Release()
        {
            nint close = 0;

            lock (m_Gate)
            {
                if (--m_Users == 0 && m_Closing)
                {
                    close = m_Handle;
                    m_Handle = 0;
                }
            }

            if (close != 0)
            {
                Native.PcDocumentClose(close);
            }
        }

        public DocumentInfo GetInfo()
        {
            Native.PcDocumentInfo info;
            nint handle = Acquire();

            try
            {
                Check(Native.PcDocumentGetInfo(handle, out info));
            }
            finally
            {
                Release();
            }

            return new DocumentInfo
            {
                Size = info.m_Size,
                Revision = info.m_Revision,
                IsModified = info.m_Modified != 0,
                CanUndo = info.m_CanUndo != 0,
                CanRedo = info.m_CanRedo != 0,
                IsPe = info.m_IsPe != 0,
                Is64 = info.m_Is64 != 0,
                IsDotNet = info.m_IsDotNet != 0,
                HasPath = info.m_HasPath != 0
            };
        }

        // The hex view asks for the length all the time, 'PcDocumentGetSize' never parses the image.
        public ulong Length
        {
            get
            {
                nint handle = Acquire();

                try
                {
                    Check(Native.PcDocumentGetSize(handle, out ulong size));
                    return size;
                }
                finally
                {
                    Release();
                }
            }
        }

        public string Path
        {
            get
            {
                char* buffer = stackalloc char[32768];
                nint handle = Acquire();

                try
                {
                    Check(Native.PcDocumentGetPath(handle, buffer, 32768));
                }
                finally
                {
                    Release();
                }

                return new string(buffer);
            }
        }

        public int Read(
            ulong offset,
            Span<byte> buffer
        )
        {
            if (buffer.Length == 0)
            {
                return 0;
            }

            nint handle = Acquire();

            try
            {
                fixed (byte* pointer = buffer)
                {
                    Check(Native.PcDocumentRead(handle, offset, pointer, (uint)buffer.Length, out uint read));
                    return (int)read;
                }
            }
            finally
            {
                Release();
            }
        }

        public byte[] ReadBytes(
            ulong offset,
            int length
        )
        {
            var data = new byte[length];
            int read = Read(offset, data);
            return read == length ? data : data.AsSpan(0, read).ToArray();
        }

        public void Write(
            ulong offset,
            byte[] data,
            string description
        )
        {
            if (IsReadOnly)
            {
                throw new CoreException(Native.PC_ERR_READ_ONLY, "The document is read only");
            }

            nint handle = Acquire();

            try
            {
                fixed (byte* pointer = data)
                {
                    Check(Native.PcDocumentWrite(handle, offset, pointer, (uint)data.Length, description));
                }
            }
            finally
            {
                Release();
            }
        }

        public void BeginGroup(
            string description
        )
        {
            nint handle = Acquire();

            try
            {
                Check(Native.PcDocumentBeginGroup(handle, description));
            }
            finally
            {
                Release();
            }
        }

        public void EndGroup()
        {
            nint handle = Acquire();

            try
            {
                Check(Native.PcDocumentEndGroup(handle));
            }
            finally
            {
                Release();
            }
        }

        public bool Undo(
            out ulong offset
        )
        {
            nint handle = Acquire();

            try
            {
                return Native.PcDocumentUndo(handle, out offset) == Native.PC_OK;
            }
            finally
            {
                Release();
            }
        }

        public bool Redo(
            out ulong offset
        )
        {
            nint handle = Acquire();

            try
            {
                return Native.PcDocumentRedo(handle, out offset) == Native.PC_OK;
            }
            finally
            {
                Release();
            }
        }

        public string GetUndoText(
            bool redo
        )
        {
            char* buffer = stackalloc char[512];
            nint handle = Acquire();

            try
            {
                return Native.PcDocumentGetUndoText(handle, redo ? 1 : 0, buffer, 512) == Native.PC_OK ? new string(buffer) : string.Empty;
            }
            finally
            {
                Release();
            }
        }

        public void Save(
            string? path,
            bool make_backup
        )
        {
            nint handle = Acquire();

            try
            {
                Check(Native.PcDocumentSave(handle, path, make_backup ? 1 : 0));
            }
            finally
            {
                Release();
            }
        }

        public void Reload()
        {
            nint handle = Acquire();

            try
            {
                Check(Native.PcDocumentReload(handle));
            }
            finally
            {
                Release();
            }
        }

        // Structured requests

        public string QueryText(
            string request
        )
        {
            return Encoding.UTF8.GetString(QueryBytes(request));
        }

        // The answer as UTF-8 bytes, for answers parsed straight into objects (large views).
        public byte[] QueryBytes(
            string request
        )
        {
            nint handle = Acquire();

            try
            {
                return CallBytes(handle, request, false);
            }
            finally
            {
                Release();
            }
        }

        public JsonDocument Query(
            string request
        )
        {
            return JsonDocument.Parse(QueryBytes(request));
        }

        public JsonDocument Query(
            object request
        )
        {
            return Query(JsonSerializer.Serialize(request));
        }

        // Queries that do not need a document (resolve, die, capabilities).
        public static JsonDocument QueryStatic(
            object request
        )
        {
            return JsonDocument.Parse(CallBytes(0, JsonSerializer.Serialize(request), false));
        }

        public JsonDocument Execute(
            object command
        )
        {
            if (IsReadOnly)
            {
                throw new CoreException(Native.PC_ERR_READ_ONLY, "The document is read only");
            }

            nint handle = Acquire();

            try
            {
                return JsonDocument.Parse(CallBytes(handle, JsonSerializer.Serialize(command), true));
            }
            finally
            {
                Release();
            }
        }

        // A resource as a complete file (.ico, .cur, .bmp or raw), with the suggested extension.
        public byte[] ExportResource(
            int index,
            bool preview,
            out string extension
        )
        {
            char* buffer = stackalloc char[32];
            nint blob;
            nint handle = Acquire();

            try
            {
                Check(Native.PcExportResource(handle, index, preview ? 1 : 0, out blob, buffer, 32));
            }
            finally
            {
                Release();
            }

            extension = new string(buffer);
            return TakeBlob(blob);
        }

        private static byte[] CallBytes(
            nint handle,
            string request,
            bool execute
        )
        {
            byte[] bytes = Encoding.UTF8.GetBytes(request + "\0");
            nint blob;

            fixed (byte* pointer = bytes)
            {
                Check(execute ? Native.PcExecute(handle, pointer, out blob) : Native.PcQuery(handle, pointer, out blob));
            }

            return TakeBlob(blob);
        }

        private static byte[] TakeBlob(
            nint blob
        )
        {
            try
            {
                Check(Native.PcBlobGet(blob, out byte* data, out ulong size));
                return new ReadOnlySpan<byte>(data, checked((int)size)).ToArray();
            }
            finally
            {
                Native.PcBlobFree(blob);
            }
        }

        private static void Check(
            int result
        )
        {
            if (result >= Native.PC_OK)
            {
                return;
            }

            char* buffer = stackalloc char[1024];
            string message = Native.PcGetLastError(buffer, 1024) == Native.PC_OK ? new string(buffer) : string.Empty;

            if (message.Length == 0)
            {
                message = $"pecore error {result}";
            }

            throw new CoreException(result, message);
        }
    }
}