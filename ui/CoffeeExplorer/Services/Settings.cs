using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoffeeExplorer.Services
{
    public enum ThemeMode
    {
        THEME_SYSTEM = 0,
        THEME_LIGHT = 1,
        THEME_DARK = 2,
        // A VS Code color theme from the themes folder, 'AppSettings.CustomTheme' names it.
        THEME_CUSTOM = 3
    }

    public sealed class WindowPlacement
    {
        public double Left { get; set; } = double.NaN;

        public double Top { get; set; } = double.NaN;

        public double Width { get; set; } = 1360;

        public double Height { get; set; } = 860;

        public bool Maximized { get; set; }

        public double TreeWidth { get; set; } = 300;

        public bool ToolbarVisible { get; set; } = true;

        public bool StatusBarVisible { get; set; } = true;
    }

    public sealed class DieSettings
    {
        // Scan every opened file and show the result as File info in the overview.
        public bool ScanOnOpen { get; set; } = true;

        public bool DeepScan { get; set; } = true;

        public bool HeuristicScan { get; set; }

        public bool RecursiveScan { get; set; }

        public bool AllTypes { get; set; }

        public bool Verbose { get; set; }
    }

    public sealed class AppSettings
    {
        public ThemeMode Theme { get; set; } = ThemeMode.THEME_SYSTEM;

        // File name without extension of the custom theme ('themes\<name>.jsonc'), used with 'THEME_CUSTOM'.
        public string? CustomTheme { get; set; }

        // Interface language. 'English' or the name of a 'translations\<Language>.lang' file.
        public string Language { get; set; } = LanguageService.ENGLISH;

        // Keep a .bak copy of the original file on the first save.
        public bool MakeBackups { get; set; }

        // Files are loaded into memory, bigger files ask first. '0' disables the question.
        public int LargeFileMegabytes { get; set; } = 1024;

        // A second 'CoffeeExplorer.exe' started by the shell sends its files to the running instance.
        public bool SingleInstance { get; set; } = true;

        // Font of the whole interface (menus, trees, grids, dialogs). The hex view has its own.
        public string UiFontFamily { get; set; } = DEFAULT_UI_FONT;

        public double UiFontSize { get; set; } = 12.0;

        public const string DEFAULT_UI_FONT = "Segoe UI Variable Text, Segoe UI";

        public string HexFontFamily { get; set; } = "Cascadia Mono, Consolas, Courier New";

        public double HexFontSize { get; set; } = 13.0;

        public DieSettings Die { get; set; } = new();

        public WindowPlacement Window { get; set; } = new();

        public List<string> RecentFiles { get; set; } = new();

        public AppSettings Clone()
        {
            string text = JsonSerializer.Serialize(this);
            return JsonSerializer.Deserialize<AppSettings>(text) ?? new AppSettings();
        }
    }

    [JsonSerializable(typeof(AppSettings))]
    internal sealed partial class SettingsJsonContext : JsonSerializerContext
    {
    }

    // Loads and saves 'settings.json'. By default the file lives next to the executable (portable).
    // 'Store settings in AppData' moves it to '%APPDATA%\CoffeeExplorer'.
    public static class SettingsStore
    {
        private const int MAX_RECENT = 12;
        private const string FILE_NAME = "settings.json";

        // Source generated metadata, no reflection over the settings types at startup.
        private static readonly JsonSerializerOptions s_Options = new()
        {
            WriteIndented = true,
            Converters = { new JsonStringEnumConverter() },
            TypeInfoResolver = SettingsJsonContext.Default
        };

        private static string? m_FilePath;

        public static AppSettings Current { get; set; } = new();

        public static event EventHandler? RecentFilesChanged;

        public static string PortablePath => Path.Combine(AppContext.BaseDirectory, FILE_NAME);

        public static string AppDataPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "CoffeeExplorer", FILE_NAME);

        public static string FilePath => m_FilePath ??= ResolvePath();

        public static bool UsesAppData => string.Equals(FilePath, AppDataPath, StringComparison.OrdinalIgnoreCase);

        private static string ResolvePath()
        {
            if (File.Exists(PortablePath))
            {
                return PortablePath;
            }

            if (File.Exists(AppDataPath))
            {
                return AppDataPath;
            }

            return IsWritable(AppContext.BaseDirectory) ? PortablePath : AppDataPath;
        }

        private static bool IsWritable(
            string folder
        )
        {
            try
            {
                string probe = Path.Combine(folder, $".write_test_{Environment.ProcessId}");
                File.WriteAllText(probe, string.Empty);
                File.Delete(probe);
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }

        public static void Load()
        {
            try
            {
                if (File.Exists(FilePath))
                {
                    Current = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(FilePath), s_Options) ?? new AppSettings();

                    // A hand edited file with null members must not crash later.
                    Current.Die ??= new DieSettings();
                    Current.Window ??= new WindowPlacement();
                    Current.RecentFiles ??= new List<string>();
                    Current.Language ??= LanguageService.ENGLISH;
                    Current.UiFontFamily ??= AppSettings.DEFAULT_UI_FONT;
                }
            }
            catch (Exception)
            {
                // A broken settings file must never prevent the editor from starting.
                Current = new AppSettings();
            }
        }

        public static void Save()
        {
            try
            {
                string path = FilePath;
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);

                // Written next to it and swapped in. A crash or a full disk while writing used to leave a truncated file,
                // and the next start fell back to the defaults.
                string temporary = path + ".tmp";
                File.WriteAllText(temporary, JsonSerializer.Serialize(Current, s_Options));
                File.Move(temporary, path, true);
            }
            catch (Exception)
            {
                // Read only install folders or roaming profile issues are not worth an error dialog.
            }
        }

        // Moves the settings file between the program folder and AppData, returns an error message or null.
        public static string? SetStoreInAppData(
            bool app_data
        )
        {
            if (app_data == UsesAppData)
            {
                return null;
            }

            string old_path = FilePath;
            string new_path = app_data ? AppDataPath : PortablePath;

            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(new_path)!);
                File.WriteAllText(new_path, JsonSerializer.Serialize(Current, s_Options));
                m_FilePath = new_path;

                if (File.Exists(old_path))
                {
                    File.Delete(old_path);
                }

                return null;
            }
            catch (Exception error)
            {
                return error.Message;
            }
        }

        public static void AddRecentFile(
            string path
        )
        {
            List<string> list = Current.RecentFiles;
            list.RemoveAll(item => string.Equals(item, path, StringComparison.OrdinalIgnoreCase));
            list.Insert(0, path);

            if (list.Count > MAX_RECENT)
            {
                list.RemoveRange(MAX_RECENT, list.Count - MAX_RECENT);
            }

            RecentFilesChanged?.Invoke(null, EventArgs.Empty);
        }

        public static void RemoveRecentFile(
            string path
        )
        {
            Current.RecentFiles.RemoveAll(item => string.Equals(item, path, StringComparison.OrdinalIgnoreCase));
            RecentFilesChanged?.Invoke(null, EventArgs.Empty);
        }

        public static void ClearRecentFiles()
        {
            Current.RecentFiles.Clear();
            RecentFilesChanged?.Invoke(null, EventArgs.Empty);
        }
    }
}