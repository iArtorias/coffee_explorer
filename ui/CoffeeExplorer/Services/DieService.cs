using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using CoffeeExplorer.Interop;

namespace CoffeeExplorer.Services
{
    public sealed class DieDetection
    {
        public string FileType { get; init; } = string.Empty;

        public string Type { get; init; } = string.Empty;

        public string Name { get; init; } = string.Empty;

        public string Version { get; init; } = string.Empty;

        public string Info { get; init; } = string.Empty;

        // Full line as Detect It Easy prints it, "Compiler: Microsoft Visual C/C++(19.36)[C++]".
        public string Text { get; init; } = string.Empty;
    }

    public sealed class DieResult
    {
        public List<DieDetection> Detections { get; } = new();

        public string Raw { get; init; } = string.Empty;

        public string Summary => Detections.Count == 0 ? LanguageService.Tr("No match found") : string.Join(", ", Detections.Select(detection => detection.Text));

        // One detection per line, the tooltip of the File info row.
        public string Lines => Detections.Count == 0 ? Summary : string.Join("\n", Detections.Select(detection => detection.Text));
    }

    // Detect It Easy through die_library.
    // The core loads and calls the dynamic library, scans run on a worker thread.
    public static class DieService
    {
        public const uint DIE_DEEPSCAN = 0x00000001;
        public const uint DIE_HEURISTICSCAN = 0x00000002;
        public const uint DIE_ALLTYPESSCAN = 0x00000004;
        public const uint DIE_RECURSIVESCAN = 0x00000008;
        public const uint DIE_VERBOSE = 0x00000010;
        public const uint DIE_RESULTASJSON = 0x00020000;

        // '0' not checked yet, '1' die.dll loaded, '2' missing or not loadable.
        private static int s_State;

        public static string Folder => Path.Combine(AppContext.BaseDirectory, "die");

        public static string DllPath => Path.Combine(Folder, "die.dll");

        public static string DatabasePath => Path.Combine(Folder, "db");

        // Never loads die.dll on the calling thread.
        public static bool IsAvailable
        {
            get
            {
                int state = Volatile.Read(ref s_State);
                return state != 0 ? state == 1 : File.Exists(DllPath) && Directory.Exists(DatabasePath);
            }
        }

        // Loads die.dll on a worker thread once, called when the window is idle after startup.
        public static void WarmUp()
        {
            if (Volatile.Read(ref s_State) != 0)
            {
                return;
            }

            if (!File.Exists(DllPath) || !Directory.Exists(DatabasePath))
            {
                Volatile.Write(ref s_State, 2);
                return;
            }

            Task.Run(() =>
            {
                bool loaded;

                try
                {
                    using JsonDocument answer = PeDocument.QueryStatic(new Dictionary<string, object> { ["query"] = "die_available", ["dll"] = DllPath });
                    loaded = CoreJson.GetBool(answer.RootElement, "available");
                }
                catch (Exception)
                {
                    loaded = false;
                }

                Volatile.Write(ref s_State, loaded ? 1 : 2);
            });
        }

        public static string UnavailableText =>
            LanguageService.Tr("Detect It Easy is not installed. Run tools\\build_die.ps1 (downloads die_library) and build.ps1, or copy die.dll, its Qt runtime and db\\ into {0}.", Folder);

        public static uint FlagsFromSettings(
            DieSettings settings
        )
        {
            uint flags = DIE_RESULTASJSON;

            if (settings.DeepScan)
            {
                flags |= DIE_DEEPSCAN;
            }

            if (settings.HeuristicScan)
            {
                flags |= DIE_HEURISTICSCAN;
            }

            if (settings.RecursiveScan)
            {
                flags |= DIE_RECURSIVESCAN;
            }

            if (settings.AllTypes)
            {
                flags |= DIE_ALLTYPESSCAN;
            }

            if (settings.Verbose)
            {
                flags |= DIE_VERBOSE;
            }

            return flags;
        }

        public static Task<DieResult> ScanAsync(
            string path,
            uint flags
        )
        {
            return Task.Run(() =>
            {
                using JsonDocument answer = PeDocument.QueryStatic(new Dictionary<string, object>
                {
                    ["query"] = "die",
                    ["dll"] = DllPath,
                    ["database"] = DatabasePath,
                    ["file"] = path,
                    ["flags"] = flags | DIE_RESULTASJSON
                });

                return Parse(CoreJson.GetString(answer.RootElement, "result"));
            });
        }

        // Scans the document. The file on disk while it has no unsaved changes, otherwise its current bytes (an edited
        // entry point changes what Detect It Easy finds, like the CFF Explorer overview).
        public static Task<DieResult> ScanDocumentAsync(
            PeDocument document,
            string path,
            bool from_memory,
            uint flags
        )
        {
            if (!from_memory)
            {
                return ScanAsync(path, flags);
            }

            return Task.Run(() =>
            {
                using JsonDocument answer = document.Query(new Dictionary<string, object>
                {
                    ["query"] = "die",
                    ["dll"] = DllPath,
                    ["database"] = DatabasePath,
                    ["memory"] = true,
                    ["flags"] = flags | DIE_RESULTASJSON
                });

                return Parse(CoreJson.GetString(answer.RootElement, "result"));
            });
        }

        // die_library JSON: { "detects": [ { "filetype", "parentfilepart", "values": [ { "type", "name", "version", "info", "string" } ] } ] }
        // Recursive scans nest more values. Every object with a string member is one detection.
        public static DieResult Parse(
            string text
        )
        {
            var result = new DieResult { Raw = text };

            if (string.IsNullOrWhiteSpace(text))
            {
                return result;
            }

            try
            {
                using JsonDocument document = JsonDocument.Parse(text);
                Collect(document.RootElement, string.Empty, result.Detections);
            }
            catch (JsonException)
            {
                // Plain text answer.
                foreach (string line in text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
                {
                    result.Detections.Add(new DieDetection { Text = line });
                }
            }

            return result;
        }

        private static void Collect(
            JsonElement element,
            string file_type,
            List<DieDetection> detections
        )
        {
            if (element.ValueKind == JsonValueKind.Array)
            {
                foreach (JsonElement item in element.EnumerateArray())
                {
                    Collect(item, file_type, detections);
                }

                return;
            }

            if (element.ValueKind != JsonValueKind.Object)
            {
                return;
            }

            string type_here = CoreJson.GetString(element, "filetype", file_type);

            if (element.TryGetProperty("string", out JsonElement line) && line.ValueKind == JsonValueKind.String)
            {
                detections.Add(new DieDetection
                {
                    FileType = type_here,
                    Type = CoreJson.GetString(element, "type"),
                    Name = CoreJson.GetString(element, "name"),
                    Version = CoreJson.GetString(element, "version"),
                    Info = CoreJson.GetString(element, "info"),
                    Text = line.GetString() ?? string.Empty
                });
            }

            foreach (JsonProperty property in element.EnumerateObject())
            {
                if (property.Value.ValueKind == JsonValueKind.Array || property.Value.ValueKind == JsonValueKind.Object)
                {
                    Collect(property.Value, type_here, detections);
                }
            }
        }
    }
}