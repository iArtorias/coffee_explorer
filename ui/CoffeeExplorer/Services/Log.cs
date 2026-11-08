using System;
using System.IO;
using System.Text;

namespace CoffeeExplorer.Services
{
    // Plain text error log, next to the executable when that folder is writable (portable use), otherwise in
    // '%LOCALAPPDATA%\CoffeeExplorer'. Only errors are written, the log is meant for bug reports.
    public static class Log
    {
        private static readonly object m_Lock = new();
        private static string? m_Path;

        public static string Path => m_Path ??= ResolvePath();

        public static void Error(
            Exception? error
        )
        {
            if (error == null)
            {
                return;
            }

            Write(error.ToString());
        }

        public static void Write(
            string message
        )
        {
            try
            {
                lock (m_Lock)
                {
                    File.AppendAllText(Path, $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {message}{Environment.NewLine}{Environment.NewLine}", Encoding.UTF8);
                }
            }
            catch (Exception)
            {
                // Logging must never take the application down.
            }
        }

        private static string ResolvePath()
        {
            string portable = System.IO.Path.Combine(AppContext.BaseDirectory, "CoffeeExplorer.log");

            try
            {
                using (new FileStream(portable, FileMode.Append, FileAccess.Write, FileShare.ReadWrite))
                {
                }

                return portable;
            }
            catch (Exception)
            {
                string folder = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CoffeeExplorer");
                Directory.CreateDirectory(folder);
                return System.IO.Path.Combine(folder, "CoffeeExplorer.log");
            }
        }
    }
}