using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Mvvm;
using CoffeeExplorer.Services;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.ViewModels
{
    // Tools keep their state while the user moves around the tree, and hear about edits.
    public interface IDocumentTool
    {
        void OnDocumentChanged();
    }

    // One open file. The core document, its structure tree and the content of the selected node.
    public sealed partial class DocumentViewModel : ObservableObject, IDisposable
    {
        private readonly MainViewModel m_Main;
        private readonly Dictionary<string, object> m_Tools = new();
        private readonly DispatcherTimer m_RefreshTimer;
        private TreeNodeViewModel? m_SelectedNode;
        private object? m_Content;
        private string m_FocusText = string.Empty;
        private DocumentInfo m_Info;
        private string m_Path;
        private string m_FileInfoText = Tr("Not scanned");
        private DieResult? m_DieResult;
        private TableCell? m_FileInfoCell;
        // Revision of the bytes the last scan saw (null: never scanned), edits after it start a new scan after a pause.
        private ulong? m_DieRevision;
        private int m_DieTicket;
        private readonly DispatcherTimer m_DieTimer;
        private bool m_Refreshing;
        // The file had a valid Authenticode signature when it was opened, edits are checked against it.
        private bool m_SignatureValid;
        private bool m_SignatureWarned;
        private bool m_SignatureChecked;
        // Incremented by every content request, a view that finishes loading after the user moved on is dropped.
        private int m_ContentTicket;

        // Views that take longer than this show a loading placeholder instead of freezing the window.
        private const int LOADING_PLACEHOLDER_MS = 120;
        // Views that take longer than this are written to the log with their timings.
        private const long SLOW_VIEW_MS = 500;

        public DocumentViewModel(
            MainViewModel main,
            PeDocument document,
            string path,
            bool read_only
        )
        {
            m_Main = main;
            Document = document;
            m_Path = path;
            IsReadOnly = read_only;
            m_Info = document.GetInfo();

            m_RefreshTimer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(400) };
            m_RefreshTimer.Tick += (_, _) => { m_RefreshTimer.Stop(); Refresh(false); };
            m_DieTimer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(800) };
            m_DieTimer.Tick += (_, _) => StartDieScan();

            SelectCommand = new RelayCommand(parameter =>
            {
                if (parameter is TreeNodeViewModel node)
                {
                    SelectedNode = node;
                }
            });

            // Local symbols were loaded on the worker thread that opened the file (MainViewModel.OpenFileCore), so the
            // first view and the disassembler already name functions. The signature state is read after the first view
            // on a worker thread, it reuses the digest the overview hashes computed.
            BuildTree();
            SelectedNode = Nodes.FirstOrDefault();
        }

        // Tree selection from the view.
        public RelayCommand SelectCommand { get; }

        public PeDocument Document { get; }

        public MainViewModel Main => m_Main;

        public bool IsReadOnly { get; }

        public string Path
        {
            get => m_Path;
            private set
            {
                if (SetProperty(ref m_Path, value))
                {
                    OnPropertyChanged(nameof(FileName));
                    OnPropertyChanged(nameof(ToolTip));
                }
            }
        }

        public string FileName => System.IO.Path.GetFileName(m_Path);

        public string ToolTip => m_Path;

        public DocumentInfo Info => m_Info;

        public bool IsModified => m_Info.IsModified;

        public bool CanUndo => m_Info.CanUndo && !IsReadOnly;

        public bool CanRedo => m_Info.CanRedo && !IsReadOnly;

        public bool IsPe => m_Info.IsPe;

        public string IconKey => m_Info.IsDotNet ? "Icon.Assembly" : m_Info.IsPe ? "Icon.BinaryFile" : "Icon.Document";

        public string FileTypeText
        {
            get
            {
                if (!m_Info.IsPe)
                {
                    return Tr("Not a PE file");
                }

                string kind = m_Info.Is64 ? "PE32+" : "PE32";
                return m_Info.IsDotNet ? kind + " .NET" : kind;
            }
        }

        public ObservableCollection<TreeNodeViewModel> Nodes { get; } = new();

        public TreeNodeViewModel? SelectedNode
        {
            get => m_SelectedNode;
            set
            {
                if (value == null || value == m_SelectedNode)
                {
                    return;
                }

                if (m_SelectedNode != null)
                {
                    m_SelectedNode.IsSelected = false;
                }

                m_SelectedNode = value;
                value.IsSelected = true;
                OnPropertyChanged();

                if (!m_Refreshing)
                {
                    ShowContent(value.Id);
                }
            }
        }

        public object? Content
        {
            get => m_Content;
            private set => SetProperty(ref m_Content, value);
        }

        // Offset, size and type of the focused value, shown in the status bar.
        public string FocusText
        {
            get => m_FocusText;
            set => SetProperty(ref m_FocusText, value);
        }

        public string FileInfoText
        {
            get => m_FileInfoText;
            private set
            {
                if (SetProperty(ref m_FileInfoText, value) && m_FileInfoCell != null)
                {
                    m_FileInfoCell.Text = value;
                    m_FileInfoCell.ToolTip = FileInfoToolTip;
                }
            }
        }

        public DieResult? DieResult => m_DieResult;

        // Every detection on its own line, the row itself only shows what fits.
        private string FileInfoToolTip => m_DieResult != null && m_FileInfoText == m_DieResult.Summary ? m_DieResult.Lines : m_FileInfoText;

        // Views

        // The core leaves out what reads the whole file (overview hashes and signature) unless it is cached, those
        // fields are pending and 'FillPendingFields' completes them on a worker thread.
        public ViewData LoadView(
            string node,
            bool defer = false
        )
        {
            var request = new Dictionary<string, object> { ["query"] = "view", ["node"] = node };

            if (defer)
            {
                request["defer"] = true;
            }

            return CoreJson.Parse<ViewData>(Document.QueryBytes(JsonSerializer.Serialize(request)));
        }

        public JsonDocument Query(
            Dictionary<string, object> request
        )
        {
            return Document.Query(request);
        }

        private void BuildTree()
        {
            var tree = CoreJson.Parse<TreeData>(Document.QueryText("{\"query\":\"tree\"}"));
            Nodes.Clear();

            foreach (TreeNodeData node in tree.Nodes)
            {
                Nodes.Add(new TreeNodeViewModel(node, null, m_Info));
            }
        }

        private TreeNodeViewModel? FindNode(
            string id
        )
        {
            foreach (TreeNodeViewModel root in Nodes)
            {
                if (root.Find(id) is TreeNodeViewModel found)
                {
                    return found;
                }
            }

            return null;
        }

        private static bool IsCoreView(
            string id
        )
        {
            return id != "tools" && (!id.StartsWith("tool.", StringComparison.Ordinal) || id == "tool.resources");
        }

        // Core view result of a worker thread. The parsed view or the error.
        private sealed class LoadedView
        {
            public ViewData? Data { get; init; }

            public Exception? Error { get; init; }

            public long Milliseconds { get; set; }

            // Rows of a large list, built on the worker thread too.
            public TableViewModel? List { get; set; }

            public CellHostProxy? ListHost { get; set; }
        }

        // Lists from this many rows on are turned into view models on the worker thread.
        private const int WORKER_LIST_ROWS = 2000;

        private LoadedView LoadViewSafe(
            string id
        )
        {
            var watch = Stopwatch.StartNew();

            try
            {
                // The overview shows at once, the hashes of a large file follow.
                return new LoadedView { Data = LoadView(id, id == "overview"), Milliseconds = watch.ElapsedMilliseconds };
            }
            catch (Exception error)
            {
                return new LoadedView { Error = error, Milliseconds = watch.ElapsedMilliseconds };
            }
        }

        // Selection of a tree node. Core views load on a worker thread (the core serializes the calls on a document),
        // a view that is not ready within 'LOADING_PLACEHOLDER_MS' shows a loading placeholder meanwhile.
        private async void ShowContent(
            string id
        )
        {
            int ticket = ++m_ContentTicket;

            if (!IsCoreView(id))
            {
                Content = CreateContent(id);
                return;
            }

            Task<LoadedView> task = Task.Run(() =>
            {
                LoadedView result = LoadViewSafe(id);

                if (result.Data is { Layout: not "resources", Columns: not null, Rows: not null } view && view.Rows.Count >= WORKER_LIST_ROWS)
                {
                    var rows_watch = Stopwatch.StartNew();
                    result.ListHost = new CellHostProxy();
                    result.List = TableViewModel.FromList(result.ListHost, view.Columns, view.Rows);
                    result.Milliseconds += rows_watch.ElapsedMilliseconds;
                }

                // The entropy strip of the overview asks the core on the UI thread, the core caches the map.
                if (id == "overview" && result.Error == null && IsPe)
                {
                    try
                    {
                        Document.QueryText("{\"query\":\"entropy\"}");
                    }
                    catch (Exception error) when (error is CoreException || error is ObjectDisposedException)
                    {
                        // The overview shows the strip without it, or the tab was closed meanwhile.
                    }
                }

                return result;
            });

            // Waits without blocking the window: the old view stays until the new one is ready or the placeholder is due.
            if (await Task.WhenAny(task, Task.Delay(LOADING_PLACEHOLDER_MS)) != task)
            {
                if (ticket != m_ContentTicket)
                {
                    return;
                }

                FocusText = string.Empty;
                Content = new LoadingViewModel(FindNode(id)?.Name ?? id);
            }

            LoadedView loaded = await task;

            if (ticket != m_ContentTicket || m_SelectedNode?.Id != id)
            {
                return;
            }

            var watch = Stopwatch.StartNew();
            Content = BuildContent(id, loaded);

            if (loaded.Milliseconds + watch.ElapsedMilliseconds > SLOW_VIEW_MS)
            {
                Log.Write($"View {id} of {FileName}: worker {loaded.Milliseconds} ms (core, parse{(loaded.List != null ? ", rows" : string.Empty)}), window {watch.ElapsedMilliseconds} ms");
            }

            CheckInitialSignature();
        }

        // Whether the file was validly signed when it was opened, on a worker thread. The digest reads the whole file
        // (the overview hashes compute it at the same time and the core caches it per revision).
        private async void CheckInitialSignature()
        {
            if (m_SignatureChecked)
            {
                return;
            }

            m_SignatureChecked = true;

            if (IsPe)
            {
                m_SignatureValid = await Task.Run(() => QuerySignatureMatches());
            }
        }

        private object? CreateContent(
            string id
        )
        {
            FocusText = string.Empty;

            if (id == "tools")
            {
                return new GroupViewModel(FindNode(id)!);
            }

            if (id.StartsWith("tool.", StringComparison.Ordinal) && id != "tool.resources")
            {
                if (!m_Tools.TryGetValue(id, out object? tool))
                {
                    tool = ToolFactory.Create(id, this);
                    m_Tools[id] = tool;
                }

                return tool;
            }

            return BuildContent(id, LoadViewSafe(id));
        }

        private object? BuildContent(
            string id,
            LoadedView loaded
        )
        {
            FocusText = string.Empty;

            try
            {
                if (loaded.Error != null)
                {
                    throw loaded.Error;
                }

                ViewData data = loaded.Data!;

                if (data.Layout == "resources")
                {
                    var resources = new ResourcesViewModel(this, data);
                    resources.SelectByIndex(0);
                    return resources;
                }

                var node = new NodeViewModel(this, data, 0, loaded.List, loaded.ListHost);

                if (id == "overview")
                {
                    AddFileInfoRow(node);
                    FillPendingFields(node, data);
                }

                node.SelectInitialRow();
                return node;
            }
            catch (Exception error)
            {
                return new MessageViewModel(FindNode(id)?.Name ?? id, error.Message);
            }
        }

        // Deferred overview fields ('Computing...'). The hashes and the signature line come from one worker query. The cells
        // are filled even when the user moved on, the next visit finds the values cached in the core.
        private async void FillPendingFields(
            NodeViewModel node,
            ViewData data
        )
        {
            if (node.Fields == null || data.Fields == null)
            {
                return;
            }

            var pending = new List<(string Key, TableCell Cell)>();

            foreach (TableRow row in node.Fields.Rows)
            {
                // Property rows keep the index of their field, the File info row (-1) is ours
                if (row.Index >= 0 && row.Index < data.Fields.Count && data.Fields[row.Index].Pending is string key && row.Cells.Count > 1)
                {
                    pending.Add((key, row.Cells[1]));
                }
            }

            if (pending.Count == 0)
            {
                return;
            }

            string answer;

            try
            {
                answer = await Task.Run(() => Document.QueryText("{\"query\":\"overview_hashes\"}"));
            }
            catch (Exception error) when (error is CoreException || error is ObjectDisposedException)
            {
                foreach (var (_, cell) in pending)
                {
                    cell.Text = error.Message;
                }

                return;
            }

            using JsonDocument values = JsonDocument.Parse(answer);

            foreach (var (key, cell) in pending)
            {
                string value = CoreJson.GetString(values.RootElement, key);

                // The signature line is text of the core ('Signed by ..., file digest matches'), the hashes are data.
                cell.Text = key == "signature" ? LanguageService.TrList(LanguageService.TrText(value)) : value;
            }
        }

        // Detect It Easy result as 'File info' under the file type, like the CFF overview.
        private void AddFileInfoRow(
            NodeViewModel node
        )
        {
            if (node.Fields == null)
            {
                return;
            }

            var cell = new TableCell(null, FileInfoText) { ToolTip = FileInfoToolTip };
            var row = new TableRow(new List<TableCell> { new(null, Tr("File info")) { ToolTip = "Detect It Easy" }, cell }) { Index = -1 };
            int position = Math.Min(2, node.Fields.Rows.Count);
            node.Fields.Rows.Insert(position, row);
            m_FileInfoCell = cell;
        }

        public void ReportFocus(
            TableCell? cell,
            TableRow? row
        )
        {
            if (cell?.Offset != null)
            {
                FocusText = Tr("Offset {0}   Size {1} ({2})", Format.Offset(cell.Offset.Value), cell.Size, Format.SizeName(cell.Size));
            }
            else if (row?.Offset != null)
            {
                FocusText = Tr("Offset {0}   Size {1}", Format.Offset(row.Offset.Value), row.Size.ToString("N0"));
            }
            else
            {
                FocusText = string.Empty;
            }
        }

        public void ShowHexFocus(
            ulong caret,
            ulong selection_start,
            ulong selection_length
        )
        {
            FocusText = selection_length > 0
                ? Tr("Offset {0}   Selection {1} - {2} ({3} bytes)", Format.Offset(caret), Format.Offset(selection_start), Format.Offset(selection_start + selection_length - 1), selection_length.ToString("N0"))
                : Tr("Offset {0}", Format.Offset(caret));
        }

        // Navigation

        public bool NavigateTo(
            string id
        )
        {
            TreeNodeViewModel? node = FindNode(id);

            if (node == null)
            {
                return false;
            }

            node.ExpandParents();
            SelectedNode = node;
            return true;
        }

        // Opens the hex editor tool at a file offset.
        public void ShowInHexEditor(
            ulong offset,
            ulong length
        )
        {
            if (NavigateTo("tool.hex") && Content is HexEditorViewModel hex)
            {
                hex.Select(offset, length);
            }
        }

        public void ShowInDisassembler(
            ulong offset
        )
        {
            if (NavigateTo("tool.disassembler") && Content is DisassemblerViewModel disassembler)
            {
                disassembler.DisassembleAt(offset);
            }
        }

        // Edits

        // Returns an error message or null.
        public string? WriteBytes(
            ulong offset,
            byte[] data,
            string description
        )
        {
            if (IsReadOnly)
            {
                return Tr("The file is open read only");
            }

            try
            {
                Document.Write(offset, data, description);
            }
            catch (CoreException error)
            {
                return error.Message;
            }

            Refresh(true);
            m_Main.SetStatus(description);
            CheckSignature();
            return null;
        }

        // Edit of a command cell, returns an error message or null like 'WriteBytes' (the cell shows it inline).
        public string? ExecuteEdit(
            string command,
            string? argument,
            object value
        )
        {
            if (IsReadOnly)
            {
                return Tr("The file is open read only");
            }

            try
            {
                using JsonDocument answer_json = Document.Execute(new Dictionary<string, object>
                {
                    ["command"] = command,
                    ["argument"] = argument ?? string.Empty,
                    ["value"] = value
                });

                CommandAnswer answer = CoreJson.Parse<CommandAnswer>(answer_json);
                Refresh(true);
                m_Main.SetStatus(answer.Message ?? Tr("Done"));
                CheckSignature();
                return null;
            }
            catch (CoreException error)
            {
                return error.Message;
            }
        }

        // Runs a core command, refreshes and selects the node the answer names.
        public CommandAnswer? Execute(
            Dictionary<string, object> command
        )
        {
            if (IsReadOnly)
            {
                m_Main.ShowError(Tr("The file is open read only"), Tr("Open the file normally to change it."));
                return null;
            }

            try
            {
                using JsonDocument answer_json = Document.Execute(command);
                CommandAnswer answer = CoreJson.Parse<CommandAnswer>(answer_json);
                Refresh(true);

                if (!string.IsNullOrEmpty(answer.Select) && SelectedNode?.Id != answer.Select)
                {
                    NavigateTo(answer.Select);
                }

                m_Main.SetStatus(answer.Message ?? Tr("Done"));
                CheckSignature();
                return answer;
            }
            catch (CoreException error)
            {
                m_Main.ShowError(Tr("The operation failed"), error.Message);
                return null;
            }
        }

        // Thread safe. Called on worker threads, the tab may close meanwhile.
        private bool QuerySignatureMatches()
        {
            try
            {
                using JsonDocument answer = Document.Query(new Dictionary<string, object> { ["query"] = "signature" });
                return CoreJson.GetBool(answer.RootElement, "matches");
            }
            catch (Exception error) when (error is CoreException || error is ObjectDisposedException)
            {
                return false;
            }
        }

        // Once per break. The first edit that makes the file digest differ from the signed one says so, an undo back to
        // the signed bytes arms the warning again. The core caches the digest per revision, a new revision recomputes it
        // on a worker thread so editing a large signed file does not stall the window.
        private async void CheckSignature()
        {
            if (!m_SignatureValid)
            {
                return;
            }

            if (await Task.Run(() => QuerySignatureMatches()))
            {
                m_SignatureWarned = false;
                return;
            }

            if (!m_SignatureWarned)
            {
                m_SignatureWarned = true;
                m_Main.SetStatus(Tr("This change invalidates the Authenticode signature (the file digest no longer matches), re-sign the file after editing"));
            }
        }

        public void Undo()
        {
            if (Document.Undo(out ulong offset))
            {
                Refresh(true);
                m_Main.SetStatus(Tr("Undone at {0}", Format.Offset(offset)));
                CheckSignature();
            }
        }

        public void Redo()
        {
            if (Document.Redo(out ulong offset))
            {
                Refresh(true);
                m_Main.SetStatus(Tr("Redone at {0}", Format.Offset(offset)));
                CheckSignature();
            }
        }

        // Typing in a hex view. Refresh the tree and the state after a short pause, the view itself is current.
        public void OnBytesEdited()
        {
            UpdateInfo();
            m_RefreshTimer.Stop();
            m_RefreshTimer.Start();
        }

        public void UpdateInfo()
        {
            m_Info = Document.GetInfo();
            OnPropertyChanged(nameof(Info));

            // Detect It Easy follows the edits (a new entry point changes the result), once the typing pauses.
            if (m_DieRevision != null && m_DieRevision != m_Info.Revision)
            {
                m_DieTimer.Stop();
                m_DieTimer.Start();
            }

            OnPropertyChanged(nameof(IsModified));
            OnPropertyChanged(nameof(CanUndo));
            OnPropertyChanged(nameof(CanRedo));
            OnPropertyChanged(nameof(FileTypeText));
            OnPropertyChanged(nameof(IconKey));
            m_Main.OnDocumentStateChanged(this);
        }

        // Rebuilds the tree (sections or directories may have appeared) and, with content set, the current view.
        // Expanded nodes, the selected node and the selected rows survive.
        public void Refresh(
            bool content
        )
        {
            UpdateInfo();
            var expanded = new HashSet<string>();
            CollectExpanded(Nodes, expanded);
            string? selected = m_SelectedNode?.Id;

            m_Refreshing = true;

            try
            {
                BuildTree();
                ApplyExpanded(Nodes, expanded);
                TreeNodeViewModel? node = (selected != null ? FindNode(selected) : null) ?? Nodes.FirstOrDefault();

                if (node != null)
                {
                    node.ExpandParents();
                    m_SelectedNode = null;
                    SelectedNode = node;
                }
            }
            finally
            {
                m_Refreshing = false;
            }

            OnPropertyChanged(nameof(SelectedNode));

            if (m_SelectedNode == null)
            {
                return;
            }

            if (content || m_SelectedNode.Id != selected)
            {
                // A view still loading for the old bytes must not replace this one.
                ++m_ContentTicket;
                object? old = m_Content;
                object? fresh = CreateContent(m_SelectedNode.Id);
                RestoreState(old, fresh);
                Content = fresh;
            }

            foreach (object tool in m_Tools.Values)
            {
                if (tool is IDocumentTool listener)
                {
                    listener.OnDocumentChanged();
                }
            }
        }

        private static void RestoreState(
            object? old,
            object? fresh
        )
        {
            if (old is NodeViewModel old_node && fresh is NodeViewModel new_node)
            {
                RestoreNode(old_node, new_node);
            }
            else if (old is ResourcesViewModel old_resources && fresh is ResourcesViewModel new_resources && old_resources.SelectedItem != null)
            {
                new_resources.SelectByIndex(old_resources.SelectedItem.Index);
            }
        }

        private static void RestoreNode(
            NodeViewModel old_node,
            NodeViewModel new_node
        )
        {
            if (old_node.List != null && new_node.List != null && old_node.SelectedIndex >= 0 && old_node.SelectedIndex < new_node.List.Rows.Count)
            {
                new_node.List.SelectedRow = new_node.List.Rows[old_node.SelectedIndex];
            }

            if (old_node.Fields != null && new_node.Fields != null && old_node.Fields.SelectedRow != null)
            {
                int index = old_node.Fields.Rows.IndexOf(old_node.Fields.SelectedRow);

                if (index >= 0 && index < new_node.Fields.Rows.Count)
                {
                    new_node.Fields.SelectedRow = new_node.Fields.Rows[index];
                }
            }

            if (old_node.Detail != null && new_node.Detail != null)
            {
                RestoreNode(old_node.Detail, new_node.Detail);
            }
        }

        private static void CollectExpanded(
            IEnumerable<TreeNodeViewModel> nodes,
            HashSet<string> expanded
        )
        {
            foreach (TreeNodeViewModel node in nodes)
            {
                if (node.IsExpanded)
                {
                    expanded.Add(node.Id);
                }

                CollectExpanded(node.Children, expanded);
            }
        }

        private static void ApplyExpanded(
            IEnumerable<TreeNodeViewModel> nodes,
            HashSet<string> expanded
        )
        {
            foreach (TreeNodeViewModel node in nodes)
            {
                if (expanded.Contains(node.Id))
                {
                    node.IsExpanded = true;
                }

                ApplyExpanded(node.Children, expanded);
            }
        }

        // Files

        public bool Save(
            string? path
        )
        {
            try
            {
                Document.Save(path, SettingsStore.Current.MakeBackups);

                if (path != null)
                {
                    Path = path;
                    SettingsStore.AddRecentFile(path);
                }

                UpdateInfo();
                m_Main.SetStatus(Tr("Saved {0}", FileName));
                return true;
            }
            catch (CoreException error)
            {
                m_Main.ShowError(Tr("The file could not be saved"), error.Message);
                return false;
            }
        }

        public void Reload()
        {
            try
            {
                Document.Reload();
                Refresh(true);
                StartDieScan();
                m_Main.SetStatus(Tr("Reloaded {0}", FileName));
            }
            catch (CoreException error)
            {
                m_Main.ShowError(Tr("The file could not be reloaded"), error.Message);
            }
        }

        // Detect It Easy

        // Scans the file on disk, or the current bytes while there are unsaved changes. A scan that finishes after a newer
        // one started is dropped.
        public async void StartDieScan()
        {
            m_DieTimer.Stop();
            bool from_memory = m_Info.IsModified;

            if (!from_memory && !File.Exists(m_Path))
            {
                FileInfoText = Tr("Save the file to scan it with Detect It Easy");
                return;
            }

            if (!DieService.IsAvailable)
            {
                FileInfoText = Tr("Detect It Easy is not installed");
                return;
            }

            int ticket = ++m_DieTicket;
            m_DieRevision = m_Info.Revision;

            // A rescan after an edit keeps the old result visible until the new one arrives.
            if (m_DieResult == null)
            {
                FileInfoText = Tr("Scanning with Detect It Easy...");
            }

            try
            {
                DieResult result = await DieService.ScanDocumentAsync(Document, m_Path, from_memory, DieService.FlagsFromSettings(SettingsStore.Current.Die));

                if (ticket != m_DieTicket)
                {
                    return;
                }

                m_DieResult = result;
                FileInfoText = result.Summary;

                if (m_FileInfoCell != null)
                {
                    m_FileInfoCell.ToolTip = FileInfoToolTip;
                }

                OnPropertyChanged(nameof(DieResult));
            }
            catch (Exception error)
            {
                if (ticket == m_DieTicket)
                {
                    FileInfoText = Tr("Detect It Easy failed: {0}", error.Message);
                }
            }
        }

        public void SetDieResult(
            DieResult result
        )
        {
            ++m_DieTicket;
            m_DieRevision = m_Info.Revision;
            m_DieResult = result;
            FileInfoText = result.Summary;

            if (m_FileInfoCell != null)
            {
                m_FileInfoCell.ToolTip = FileInfoToolTip;
            }

            OnPropertyChanged(nameof(DieResult));
        }

        // Tools build part of their text in code. They start over in the new language, the views reload translated.
        public void OnLanguageChanged()
        {
            foreach (object tool in m_Tools.Values)
            {
                (tool as IDisposable)?.Dispose();
            }

            m_Tools.Clear();
            Refresh(true);
        }

        public void Dispose()
        {
            m_RefreshTimer.Stop();
            m_DieTimer.Stop();
            ++m_DieTicket;

            foreach (object tool in m_Tools.Values)
            {
                (tool as IDisposable)?.Dispose();
            }

            m_Tools.Clear();
            Document.Dispose();
        }
    }

    // Content of the Tools group node. A short list of the tools.
    public sealed class GroupViewModel
    {
        public GroupViewModel(
            TreeNodeViewModel node
        )
        {
            Node = node;
        }

        public TreeNodeViewModel Node { get; }

        public string Title => Node.Name;
    }

    // A view that could not be built (broken directory). Title and the error of the core
    // Placeholder while a large view loads.
    public sealed class LoadingViewModel
    {
        public LoadingViewModel(
            string title
        )
        {
            Title = title;
        }

        public string Title { get; }
    }

    public sealed class MessageViewModel
    {
        public MessageViewModel(
            string title,
            string message
        )
        {
            Title = title;
            Message = message;
        }

        public string Title { get; }

        public string Message { get; }
    }
}