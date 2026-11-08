using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Http;
using System.Text.Json;
using System.Threading.Tasks;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Services;
using CoffeeExplorer.Views.Dialogs;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.ViewModels
{
    // PDB symbols of a document. The core reads the PDB and keeps it with the document handle, the UI finds files,
    // asks before network access and downloads into a symbol store below '%LOCALAPPDATA%\CoffeeExplorer\Symbols'.
    public sealed partial class DocumentViewModel
    {
        private const string SYMBOL_SERVER = "https://msdl.microsoft.com/download/symbols";

        private static readonly HttpClient s_Http = CreateHttpClient();

        private bool m_Downloading;

        public static string SymbolCache => System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CoffeeExplorer", "Symbols");

        private static HttpClient CreateHttpClient()
        {
            var client = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
            client.DefaultRequestHeaders.UserAgent.ParseAdd("Microsoft-Symbol-Server/10.0.0.0");
            return client;
        }

        private JsonDocument QuerySymbols(
            string query,
            Dictionary<string, object>? extra = null
        )
        {
            var request = new Dictionary<string, object> { ["query"] = query, ["cache"] = SymbolCache, ["server"] = SYMBOL_SERVER };

            if (extra != null)
            {
                foreach (var pair in extra)
                {
                    request[pair.Key] = pair.Value;
                }
            }

            return Document.Query(request);
        }

        // Local lookup at open. CodeView path, next to the file, symbol path, caches. Silent, no network. Runs on the
        // worker thread that opens the file, a large PDB is read before the document tab exists.
        public static void LoadLocalSymbols(
            PeDocument document
        )
        {
            try
            {
                using JsonDocument answer = document.Query(new Dictionary<string, object> { ["query"] = "symbols_load", ["cache"] = SymbolCache, ["server"] = SYMBOL_SERVER });
            }
            catch (CoreException)
            {
                // Not a PE file or no debug directory, nothing to load.
            }
        }

        // Returns true when symbols are loaded afterwards. 'explicit_path' null searches the usual places.
        public bool LoadSymbols(
            string? explicit_path,
            bool force = false
        )
        {
            try
            {
                var extra = new Dictionary<string, object> { ["force"] = force };

                if (explicit_path != null)
                {
                    extra["path"] = explicit_path;
                }

                using JsonDocument answer = QuerySymbols("symbols_load", extra);
                bool loaded = CoreJson.GetBool(answer.RootElement, "loaded");
                string message = CoreJson.GetString(answer.RootElement, "message");
                Refresh(true);

                if (loaded)
                {
                    m_Main.SetStatus(message);
                    return true;
                }

                // A PDB of another build is still useful for code that did not change, the user decides.
                if (explicit_path != null && !force && message.Contains("another", StringComparison.Ordinal) &&
                     MessageService.Confirm(m_Main.Window, Tr("PDB does not match"), message + "\n\n" + Tr("Load it anyway? Names can be wrong where the code changed."),
                                             Tr("Load anyway"), Tr("Cancel"), MessageKind.MESSAGE_WARNING))
                {
                    return LoadSymbols(explicit_path, true);
                }

                m_Main.ShowError(Tr("Symbols not loaded"), message);
                return false;
            }
            catch (CoreException error)
            {
                m_Main.ShowError(Tr("Symbols not loaded"), error.Message);
                return false;
            }
        }

        public void BrowseSymbols()
        {
            string? path = FileDialogs.Open(Tr("Load PDB"), Tr("Program databases") + " (*.pdb)|*.pdb|" + Tr("All files") + " (*.*)|*.*");

            if (path != null)
            {
                LoadSymbols(path);
            }
        }

        // Asks, downloads the PDB of the CodeView record into the symbol store and loads it.
        public async void DownloadSymbols()
        {
            if (m_Downloading)
            {
                m_Main.SetStatus(Tr("A symbol download is already running"));
                return;
            }

            string url;
            string store;
            string pdb;

            try
            {
                using JsonDocument state = QuerySymbols("symbols");
                url = CoreJson.GetString(state.RootElement, "url");
                store = CoreJson.GetString(state.RootElement, "store");
                pdb = CoreJson.GetString(state.RootElement, "pdb");
            }
            catch (CoreException error)
            {
                m_Main.ShowError(Tr("Symbols not loaded"), error.Message);
                return;
            }

            if (url.Length == 0 || store.Length == 0)
            {
                m_Main.ShowError(Tr("Symbols not loaded"), Tr("The file has no CodeView (RSDS) record, there is no PDB to download."));
                return;
            }

            if (File.Exists(store))
            {
                LoadSymbols(store);
                return;
            }

            if (!MessageService.Confirm(m_Main.Window, Tr("Download symbols"), Tr("Download {0} from the Microsoft symbol server?", System.IO.Path.GetFileName(pdb)) + $"\n\n{url}\n\n" + Tr("It is stored in {0}.", SymbolCache),
                                          Tr("Download"), Tr("Cancel")))
            {
                return;
            }

            m_Downloading = true;
            string partial = store + ".download";

            try
            {
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(store)!);
                m_Main.SetStatus(Tr("Downloading {0}", url));

                using (HttpResponseMessage response = await s_Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead))
                {
                    if (!response.IsSuccessStatusCode)
                    {
                        throw new IOException(response.StatusCode == System.Net.HttpStatusCode.NotFound ? Tr("The symbol server has no PDB for this build")
                                                                                                         : Tr("The symbol server answered {0} {1}", (int)response.StatusCode, response.ReasonPhrase));
                    }

                    long? total = response.Content.Headers.ContentLength;
                    await using Stream source = await response.Content.ReadAsStreamAsync();
                    await using (var target = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, true))
                    {
                        byte[] buffer = new byte[1 << 16];
                        long done = 0;
                        int read;
                        DateTime shown = DateTime.MinValue;

                        while ((read = await source.ReadAsync(buffer)) > 0)
                        {
                            await target.WriteAsync(buffer.AsMemory(0, read));
                            done += read;

                            if (DateTime.UtcNow - shown > TimeSpan.FromMilliseconds(250))
                            {
                                shown = DateTime.UtcNow;
                                m_Main.SetStatus(total is > 0 ? Tr("Downloading symbols {0} % of {1}", done * 100 / total.Value, Format.Bytes((ulong)total.Value)) :
                                                                 Tr("Downloading symbols, {0}", Format.Bytes((ulong)done)));
                            }
                        }
                    }
                }

                File.Move(partial, store, true);
                LoadSymbols(store);
            }
            catch (Exception error) when (error is IOException || error is HttpRequestException || error is TaskCanceledException || error is UnauthorizedAccessException)
            {
                TryDelete(partial);
                m_Main.ShowError(Tr("Symbol download failed"), error.Message);
            }
            finally
            {
                m_Downloading = false;
            }
        }

        private static void TryDelete(
            string path
        )
        {
            try
            {
                File.Delete(path);
            }
            catch (Exception)
            {
                // A leftover partial download is overwritten next time.
            }
        }
    }
}