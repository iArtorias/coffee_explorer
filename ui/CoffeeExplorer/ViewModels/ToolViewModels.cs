using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows.Input;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Mvvm;
using CoffeeExplorer.Services;
using CoffeeExplorer.Views.Dialogs;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.ViewModels
{
    public static class ToolFactory
    {
        public static object Create(
            string id,
            DocumentViewModel document
        )
        {
            return id switch
            {
                "tool.address" => new AddressConverterViewModel(document),
                "tool.dependencies" => new DependencyWalkerViewModel(document),
                "tool.hex" => new HexEditorViewModel(document),
                "tool.die" => new DieViewModel(document),
                "tool.importadder" => new ImportAdderViewModel(document),
                "tool.disassembler" => new DisassemblerViewModel(document),
                "tool.rebuilder" => new RebuilderViewModel(document),
                _ => new MessageViewModel(id, Tr("Unknown tool"))
            };
        }
    }

    // Asks a hex panel to select a range, a new instance per request so repeated requests still arrive.
    public sealed class HexSelectionRequest
    {
        public HexSelectionRequest(
            ulong offset,
            ulong length
        )
        {
            Offset = offset;
            Length = length;
        }

        public ulong Offset { get; }

        public ulong Length { get; }
    }

    public abstract class ToolViewModel : ObservableObject
    {
        protected ToolViewModel(
            DocumentViewModel document,
            string title,
            string description
        )
        {
            Document = document;
            Title = title;
            Description = description;
        }

        public DocumentViewModel Document { get; }

        public string Title { get; }

        public string Description { get; }

        public PeDocument Core => Document.Document;

        protected JsonDocument Query(
            Dictionary<string, object> request
        )
        {
            return Core.Query(request);
        }
    }

    // Address converter

    public sealed class AddressConverterViewModel : ToolViewModel, IDocumentTool
    {
        private string m_Va = string.Empty;
        private string m_Rva = string.Empty;
        private string m_Offset = string.Empty;
        private string m_Section = string.Empty;
        private string m_Symbol = string.Empty;
        private string m_Message = string.Empty;
        private bool m_Updating;
        private HexSelectionRequest? m_Selection;

        public AddressConverterViewModel(
            DocumentViewModel document
        )
            : base(document, Tr("Address Converter"), Tr("Converts between virtual addresses, relative virtual addresses and file offsets."))
        {
            if (document.IsPe)
            {
                // Start at the image base so all three fields show something.
                m_Rva = "00000000";
                Convert("rva", m_Rva);
            }
        }

        public string Va
        {
            get => m_Va;
            set
            {
                if (SetProperty(ref m_Va, value) && !m_Updating)
                {
                    Convert("va", value);
                }
            }
        }

        public string Rva
        {
            get => m_Rva;
            set
            {
                if (SetProperty(ref m_Rva, value) && !m_Updating)
                {
                    Convert("rva", value);
                }
            }
        }

        public string Offset
        {
            get => m_Offset;
            set
            {
                if (SetProperty(ref m_Offset, value) && !m_Updating)
                {
                    Convert("offset", value);
                }
            }
        }

        public string Section
        {
            get => m_Section;
            private set => SetProperty(ref m_Section, value);
        }

        public string Message
        {
            get => m_Message;
            private set => SetProperty(ref m_Message, value);
        }

        // Function or import at the address, '-' without (PDB symbols, exports, IAT slots).
        public string Symbol
        {
            get => m_Symbol;
            private set => SetProperty(ref m_Symbol, value);
        }

        public HexSelectionRequest? Selection
        {
            get => m_Selection;
            private set => SetProperty(ref m_Selection, value);
        }

        public void OnDocumentChanged()
        {
            if (Format.TryParseNumber(m_Rva, out _))
            {
                Convert("rva", m_Rva);
            }
        }

        private void Convert(
            string kind,
            string text
        )
        {
            if (!Format.TryParseNumber(text, out ulong value))
            {
                Message = text.Length == 0 ? Tr("Type a value in any of the fields") : Tr("\"{0}\" is not a hex number", text);
                return;
            }

            try
            {
                using JsonDocument answer = Query(new Dictionary<string, object> { ["query"] = "convert", ["kind"] = kind, ["value"] = value.ToString("X", CultureInfo.InvariantCulture) });
                JsonElement root = answer.RootElement;
                m_Updating = true;

                if (kind != "va")
                {
                    Va = CoreJson.GetString(root, "va");
                }

                if (kind != "rva")
                {
                    Rva = CoreJson.GetString(root, "rva");
                }

                if (kind != "offset")
                {
                    Offset = CoreJson.GetString(root, "offset");
                }

                m_Updating = false;
                bool valid = CoreJson.GetBool(root, "valid");
                string section = CoreJson.GetString(root, "section");
                Section = section.Length > 0 ? TrText(section) : valid ? Tr("PE headers") : "-";
                string symbol = CoreJson.GetString(root, "symbol");
                Symbol = symbol.Length > 0 ? symbol : "-";

                // The section is shown next to the fields, the message line is only for problems.
                string message = CoreJson.GetString(root, "message");
                Message = message.Length > 0 ? TrText(message) : valid ? string.Empty : Tr("Outside the image");

                if (root.TryGetProperty("file_offset", out JsonElement file_offset) && file_offset.TryGetUInt64(out ulong position))
                {
                    Selection = new HexSelectionRequest(position, 1);
                }
            }
            catch (CoreException error)
            {
                m_Updating = false;
                Message = error.Message;
            }
        }
    }

    // Hex editor

    public sealed class HexEditorViewModel : ToolViewModel
    {
        private HexSelectionRequest? m_Selection;

        public HexEditorViewModel(
            DocumentViewModel document
        )
            : base(document, Tr("Hex Editor"), Tr("The whole file. Changes are part of the undo history and saved with the document."))
        {
        }

        public HexSelectionRequest? Selection
        {
            get => m_Selection;
            private set => SetProperty(ref m_Selection, value);
        }

        public void Select(
            ulong offset,
            ulong length
        )
        {
            Selection = new HexSelectionRequest(offset, length);
        }
    }

    // Detect It Easy

    public sealed class DieViewModel : ToolViewModel
    {
        private bool m_IsScanning;
        private string m_Status = string.Empty;
        private string m_Raw = string.Empty;

        public DieViewModel(
            DocumentViewModel document
        )
            : base(document, Tr("Detect It Easy"), Tr("Identifies compilers, linkers, packers, protectors and installers with the Detect It Easy signature database."))
        {
            ScanCommand = new RelayCommand(() => _ = Scan(), () => !m_IsScanning && IsAvailable);

            if (document.DieResult != null)
            {
                Show(document.DieResult);
            }
            else
            {
                Status = IsAvailable ? Tr("Press Scan to identify the file") : DieService.UnavailableText;
            }
        }

        // The settings object is replaced when the Options dialog is confirmed.
        public DieSettings Options => SettingsStore.Current.Die;

        public bool IsAvailable => DieService.IsAvailable;

        public ObservableCollection<DieDetection> Results { get; } = new();

        public ICommand ScanCommand { get; }

        public bool IsScanning
        {
            get => m_IsScanning;
            private set => SetProperty(ref m_IsScanning, value);
        }

        public string Status
        {
            get => m_Status;
            private set => SetProperty(ref m_Status, value);
        }

        public string Raw
        {
            get => m_Raw;
            private set => SetProperty(ref m_Raw, value);
        }

        private async Task Scan()
        {
            // Unsaved changes are scanned from memory, so the result matches what the editor shows.
            bool from_memory = Document.IsModified;

            if (!from_memory && !File.Exists(Document.Path))
            {
                Status = Tr("Save the file first, Detect It Easy scans the file on disk");
                return;
            }

            IsScanning = true;
            Results.Clear();

            try
            {
                DieResult result = await DieService.ScanDocumentAsync(Core, Document.Path, from_memory, DieService.FlagsFromSettings(Options));
                Document.SetDieResult(result);
                Show(result);
            }
            catch (Exception error)
            {
                Status = Tr("Scan failed: {0}", error.Message);
            }
            finally
            {
                IsScanning = false;
                CommandManager.InvalidateRequerySuggested();
            }
        }

        private void Show(
            DieResult result
        )
        {
            Results.Clear();

            foreach (DieDetection detection in result.Detections)
            {
                Results.Add(detection);
            }

            Raw = result.Raw;
            Status = result.Detections.Count == 0 ? Tr("Scan completed, no match found") :
                     result.Detections.Count == 1 ? Tr("Scan completed, 1 detection") : Tr("Scan completed, {0} detections", result.Detections.Count.ToString("N0"));
        }
    }

    // Disassembler

    public sealed class DisassemblyLine
    {
        public string Address { get; init; } = string.Empty;

        public ulong Offset { get; init; }

        public string Bytes { get; init; } = string.Empty;

        public string Text { get; init; } = string.Empty;

        // Symbol starting at this address (PDB, export or import slot).
        public string Label { get; init; } = string.Empty;

        // Name of the branch target or of the RIP relative operand ('kernel32.dll!CreateFileW').
        public string Comment { get; init; } = string.Empty;

        // x64dbg token colors of Text: [class, length, class, length etc] ('DisassemblyClass' of the core).
        public int[]? Spans { get; init; }

        // Long C++ names are cut by the column, the tooltip has them in full (null means no tooltip).
        public string? LabelToolTip => Label.Length > 0 ? Label : null;

        public string? CommentToolTip => Comment.Length > 0 ? Comment : null;

        // The entry point, marked like the instruction pointer of x64dbg ("RIP" or "EIP" in the marker column).
        public bool IsEntry { get; init; }

        public string EntryText { get; init; } = string.Empty;

        // Direct branch. "jcc", "jmp" or "call", null for other instructions
        public string? Branch { get; init; }

        // Address of the branch target as the core writes it.
        public string Target { get; init; } = string.Empty;

        // File offset of the target, null when it has no bytes in the file (the branch cannot be followed).
        public ulong? TargetOffset { get; init; }

        public bool CanFollow => TargetOffset != null;

        // Arrow in the marker column. Up for a branch backwards, down for a branch forwards, a loop for a branch to itself.
        public string BranchGlyph { get; init; } = string.Empty;

        public string? MarkerToolTip { get; init; }
    }

    public sealed class DisassemblerViewModel : ToolViewModel
    {
        private string m_OffsetText = string.Empty;
        private string m_SizeText = "1000";
        private string m_BaseText = string.Empty;
        private int m_Bits;
        private bool m_ShowBytes = true;
        private string m_Status = string.Empty;
        private DisassemblyLine? m_SelectedLine;
        private ObservableCollection<DisassemblyLine> m_Lines = new();
        // Incremented by every request, a result that arrives after a newer request is dropped.
        private int m_Ticket;
        // Line to select once the running request has its lines (follow and back).
        private ulong? m_PendingSelection;

        // Where Follow came from, Back returns there like the '-' key of x64dbg.
        private sealed class Place
        {
            public string OffsetText { get; init; } = string.Empty;

            public string BaseText { get; init; } = string.Empty;

            public string SizeText { get; init; } = string.Empty;

            public int Bits { get; init; }

            public ulong? Selected { get; init; }
        }

        private readonly Stack<Place> m_History = new();

        public DisassemblerViewModel(
            DocumentViewModel document
        )
            : base(document, Tr("Disassembler"), Tr("x86 and x64 disassembly of any part of the file (Zydis)."))
        {
            m_Bits = document.Info.Is64 ? 64 : 32;
            DisassembleCommand = new RelayCommand(Disassemble, () => IsAvailable);
            EntryPointCommand = new RelayCommand(GoToEntryPoint, () => Document.IsPe);
            ShowInHexCommand = new RelayCommand(() => Document.ShowInHexEditor(m_SelectedLine!.Offset, 1), () => m_SelectedLine != null);
            FollowCommand = new RelayCommand(Follow, () => m_SelectedLine?.CanFollow == true);
            BackCommand = new RelayCommand(Back, () => m_History.Count > 0);

            try
            {
                using JsonDocument capabilities = PeDocument.QueryStatic(new Dictionary<string, object> { ["query"] = "capabilities" });
                IsAvailable = CoreJson.GetBool(capabilities.RootElement, "disassembler");
            }
            catch (CoreException)
            {
                IsAvailable = false;
            }

            if (!IsAvailable)
            {
                m_Status = Tr("pecore.dll was built without the disassembler (build.ps1 -NoZydis)");
            }
            else if (document.IsPe)
            {
                GoToEntryPoint();
            }
        }

        public bool IsAvailable { get; }

        public int[] BitOptions { get; } = { 16, 32, 64 };

        public int Bits
        {
            get => m_Bits;
            set => SetProperty(ref m_Bits, value);
        }

        public string OffsetText
        {
            get => m_OffsetText;
            set
            {
                if (SetProperty(ref m_OffsetText, value))
                {
                    UpdateBase();
                }
            }
        }

        public string SizeText
        {
            get => m_SizeText;
            set => SetProperty(ref m_SizeText, value);
        }

        public string BaseText
        {
            get => m_BaseText;
            set => SetProperty(ref m_BaseText, value);
        }

        public bool ShowBytes
        {
            get => m_ShowBytes;
            set => SetProperty(ref m_ShowBytes, value);
        }

        public string Status
        {
            get => m_Status;
            private set => SetProperty(ref m_Status, value);
        }

        // Replaced as a whole.
        public ObservableCollection<DisassemblyLine> Lines
        {
            get => m_Lines;
            private set => SetProperty(ref m_Lines, value);
        }

        public DisassemblyLine? SelectedLine
        {
            get => m_SelectedLine;
            set
            {
                // Follow and Back change the selection from code, their buttons and keys must notice.
                if (SetProperty(ref m_SelectedLine, value))
                {
                    CommandManager.InvalidateRequerySuggested();
                }
            }
        }

        public ICommand DisassembleCommand { get; }

        public ICommand EntryPointCommand { get; }

        public ICommand ShowInHexCommand { get; }

        // Enter or double click. The target of the selected jcc, jmp or call.
        public ICommand FollowCommand { get; }

        // Backspace or '-'. Back to where the last Follow started.
        public ICommand BackCommand { get; }

        public bool CanGoBack => m_History.Count > 0;

        private Place CurrentPlace()
        {
            return new Place { OffsetText = m_OffsetText, BaseText = m_BaseText, SizeText = m_SizeText, Bits = m_Bits, Selected = m_SelectedLine?.Offset };
        }

        private void Follow()
        {
            DisassemblyLine? line = m_SelectedLine;

            if (line?.TargetOffset is not ulong target)
            {
                return;
            }

            m_History.Push(CurrentPlace());
            OnPropertyChanged(nameof(CanGoBack));

            // A target inside the listing is only selected, like x64dbg follows a jump on screen.
            DisassemblyLine? visible = m_Lines.FirstOrDefault(candidate => candidate.Offset == target);

            if (visible != null)
            {
                SelectedLine = visible;
                Status = Tr("Followed {0} to {1}", line.Branch ?? string.Empty, line.Target);
                return;
            }

            m_PendingSelection = target;
            OffsetText = Format.Offset(target);

            // The base follows the offset for image addresses, a listing with an own base keeps the address of the target.
            if (!string.IsNullOrEmpty(line.Target))
            {
                BaseText = line.Target;
            }

            Disassemble();
        }

        private void Back()
        {
            if (m_History.Count == 0)
            {
                return;
            }

            Place place = m_History.Pop();
            OnPropertyChanged(nameof(CanGoBack));

            if (place.OffsetText == m_OffsetText && place.BaseText == m_BaseText && place.SizeText == m_SizeText && place.Bits == m_Bits)
            {
                SelectedLine = m_Lines.FirstOrDefault(candidate => candidate.Offset == place.Selected) ?? m_SelectedLine;
                return;
            }

            m_PendingSelection = place.Selected;
            m_OffsetText = place.OffsetText;
            OnPropertyChanged(nameof(OffsetText));
            BaseText = place.BaseText;
            SizeText = place.SizeText;
            Bits = place.Bits;
            Disassemble();
        }

        public void DisassembleAt(
            ulong offset
        )
        {
            OffsetText = Format.Offset(offset);
            Disassemble();
        }

        private void GoToEntryPoint()
        {
            try
            {
                using JsonDocument info = Query(new Dictionary<string, object> { ["query"] = "info" });
                ulong offset = CoreJson.GetUnsigned(info.RootElement, "entry_offset", ulong.MaxValue);

                if (offset == ulong.MaxValue)
                {
                    Status = Tr("The entry point is not inside the file");
                    return;
                }

                DisassembleAt(offset);
            }
            catch (CoreException error)
            {
                Status = error.Message;
            }
        }

        // The base address follows the offset: the VA of the first byte when it maps into the image.
        private void UpdateBase()
        {
            if (!Document.IsPe || !Format.TryParseNumber(m_OffsetText, out ulong offset))
            {
                return;
            }

            try
            {
                using JsonDocument answer = Query(new Dictionary<string, object> { ["query"] = "convert", ["kind"] = "offset", ["value"] = offset.ToString("X", CultureInfo.InvariantCulture) });

                if (CoreJson.GetBool(answer.RootElement, "valid"))
                {
                    BaseText = CoreJson.GetString(answer.RootElement, "va");
                }
            }
            catch (CoreException)
            {
                // Offsets in the overlay have no address, the base stays as typed.
            }
        }

        // The core decodes on a worker thread (up to 1 MB and 50000 lines), the window stays responsive meanwhile.
        private async void Disassemble()
        {
            if (!Format.TryParseNumber(m_OffsetText, out ulong offset))
            {
                Status = Tr("The offset is not a hex number");
                return;
            }

            if (!Format.TryParseNumber(m_SizeText, out ulong size) || size == 0)
            {
                Status = Tr("The size is not a hex number");
                return;
            }

            ulong base_address = Format.TryParseNumber(m_BaseText, out ulong parsed) ? parsed : offset;
            int ticket = ++m_Ticket;
            string request = JsonSerializer.Serialize(new Dictionary<string, object>
            {
                ["query"] = "disassemble",
                ["offset"] = offset,
                ["size"] = Math.Min(size, 0x100000ul),
                ["base"] = base_address,
                ["bits"] = m_Bits,
                ["max"] = 50000
            });

            PeDocument core = Core;
            Status = Tr("Disassembling...");

            try
            {
                string entry_text = m_Bits == 64 ? "RIP" : m_Bits == 32 ? "EIP" : "IP";
                List<DisassemblyLine> lines = await Task.Run(() => ParseLines(core.QueryBytes(request), entry_text));

                if (ticket != m_Ticket)
                {
                    return;
                }

                SelectedLine = null;
                Lines = new ObservableCollection<DisassemblyLine>(lines);
                Status = Tr("{0} instructions from {1}", lines.Count.ToString("N0"), Format.Offset(offset));

                // Follow and Back select their line, a fresh listing starts at its entry point line or its first line.
                ulong? select = m_PendingSelection;
                m_PendingSelection = null;
                SelectedLine = (select != null ? lines.FirstOrDefault(line => line.Offset == select) : null) ??
                               lines.FirstOrDefault(line => line.IsEntry) ?? lines.FirstOrDefault();
            }
            catch (Exception error) when (error is CoreException || error is ObjectDisposedException || error is JsonException)
            {
                if (ticket == m_Ticket)
                {
                    Status = error.Message;
                }
            }
        }

        private static List<DisassemblyLine> ParseLines(
            byte[] answer,
            string entry_text
        )
        {
            using JsonDocument document = JsonDocument.Parse(answer);
            var result = new List<DisassemblyLine>();

            if (!document.RootElement.TryGetProperty("lines", out JsonElement lines))
            {
                return result;
            }

            result.Capacity = lines.GetArrayLength();

            foreach (JsonElement line in lines.EnumerateArray())
            {
                int[]? spans = null;

                if (line.TryGetProperty("spans", out JsonElement items) && items.ValueKind == JsonValueKind.Array)
                {
                    spans = new int[items.GetArrayLength()];
                    int index = 0;

                    foreach (JsonElement item in items.EnumerateArray())
                    {
                        spans[index++] = item.TryGetInt32(out int value) ? value : 0;
                    }
                }

                bool entry = CoreJson.GetBool(line, "entry");
                string? branch = CoreJson.GetString(line, "branch") is { Length: > 0 } kind ? kind : null;
                string target = CoreJson.GetString(line, "target");
                ulong? target_offset = line.TryGetProperty("target_offset", out JsonElement found) && found.TryGetUInt64(out ulong value_offset) ? value_offset : null;
                ulong address = Format.TryParseNumber(CoreJson.GetString(line, "address"), out ulong parsed_address) ? parsed_address : 0;
                ulong target_address = Format.TryParseNumber(target, out ulong parsed_target) ? parsed_target : 0;
                string glyph = branch == null ? string.Empty : branch == "call" ? "→" : target_address == address ? "↻" : target_address < address ? "▲" : "▼";

                result.Add(new DisassemblyLine
                {
                    Address = CoreJson.GetString(line, "address"),
                    Offset = CoreJson.GetUnsigned(line, "offset"),
                    Bytes = CoreJson.GetString(line, "bytes"),
                    Text = CoreJson.GetString(line, "text"),
                    Label = CoreJson.GetString(line, "label"),
                    Comment = CoreJson.GetString(line, "comment"),
                    Spans = spans,
                    IsEntry = entry,
                    EntryText = entry ? entry_text : string.Empty,
                    Branch = branch,
                    Target = target,
                    TargetOffset = target_offset,
                    BranchGlyph = glyph,
                    MarkerToolTip = entry ? Tr("Entry point (AddressOfEntryPoint), where {0} starts", entry_text) :
                                    branch == null ? null :
                                    target_offset != null ? Tr("{0} to {1}, Enter or double click follows it", branch, target) :
                                                            Tr("{0} to {1}, outside of the file", branch, target)
                });
            }

            return result;
        }
    }

    // Rebuilder

    public sealed class RebuilderOption : ObservableObject
    {
        private bool m_IsChecked;

        public string Key { get; init; } = string.Empty;

        public string Label { get; init; } = string.Empty;

        public string Description { get; init; } = string.Empty;

        public bool IsEnabled { get; init; } = true;

        public string? DisabledReason { get; init; }

        public string? ToolTip => IsEnabled ? Description : DisabledReason;

        public bool IsChecked
        {
            get => m_IsChecked;
            set => SetProperty(ref m_IsChecked, value);
        }
    }

    public sealed class RebuilderViewModel : ToolViewModel, IDocumentTool
    {
        private string m_Log = string.Empty;
        private string m_NewImageBase = string.Empty;
        private bool m_ChangeImageBase;
        private int m_Realign;

        public RebuilderViewModel(
            DocumentViewModel document
        )
            : base(document, Tr("Rebuilder"), Tr("Repairs and shrinks the image. The options run in the listed order as one undo step."))
        {
            RebuildCommand = new RelayCommand(Rebuild, () => !Document.IsReadOnly && Document.IsPe);
            BuildOptions();
        }

        public ObservableCollection<RebuilderOption> Options { get; } = new();

        // '0' keeps the alignment, '512' or '4096' realigns the raw data.
        public int[] RealignOptions { get; } = { 0, 512, 4096 };

        public int Realign
        {
            get => m_Realign;
            set => SetProperty(ref m_Realign, value);
        }

        public bool ChangeImageBase
        {
            get => m_ChangeImageBase;
            set => SetProperty(ref m_ChangeImageBase, value);
        }

        public string NewImageBase
        {
            get => m_NewImageBase;
            set => SetProperty(ref m_NewImageBase, value);
        }

        public bool CanRebase { get; private set; }

        public string? RebaseReason { get; private set; }

        public string Log
        {
            get => m_Log;
            private set => SetProperty(ref m_Log, value);
        }

        public ICommand RebuildCommand { get; }

        public void OnDocumentChanged()
        {
            var checked_keys = Options.Where(option => option.IsChecked && option.IsEnabled).Select(option => option.Key).ToHashSet();
            BuildOptions();

            foreach (RebuilderOption option in Options)
            {
                option.IsChecked = option.IsEnabled && checked_keys.Contains(option.Key);
            }
        }

        private bool HasNode(
            string id
        )
        {
            return Document.Nodes.Any(root => root.Find(id) != null);
        }

        private void BuildOptions()
        {
            Options.Clear();
            bool relocations = HasNode("relocs");
            bool debug = HasNode("debug");
            bool bound = HasNode("bound");
            bool dotnet = HasNode("dotnet");

            Options.Add(new RebuilderOption { Key = "rebuild_header", Label = Tr("Rebuild PE header"), Description = Tr("Recomputes NumberOfSections, SizeOfHeaders, SizeOfCode, SizeOfInitializedData and SizeOfImage"), IsChecked = true });
            Options.Add(new RebuilderOption { Key = "update_checksum", Label = Tr("Update checksum"), Description = Tr("Writes the correct CheckSum, drivers and boot files need it"), IsChecked = true });
            Options.Add(new RebuilderOption { Key = "remove_relocations", Label = Tr("Remove base relocations"), Description = Tr("Drops the relocation directory and sets RELOCS_STRIPPED, the image then needs its preferred base"), IsEnabled = relocations, DisabledReason = Tr("The file has no base relocations") });
            Options.Add(new RebuilderOption { Key = "remove_debug", Label = Tr("Remove debug directory"), Description = Tr("Clears the debug directory (CodeView, POGO ...) and its data"), IsEnabled = debug, DisabledReason = Tr("The file has no debug directory") });
            Options.Add(new RebuilderOption { Key = "remove_bound_imports", Label = Tr("Remove bound import table"), Description = Tr("Bound imports are ignored by ASLR enabled loaders and go stale after every Windows update"), IsEnabled = bound, DisabledReason = Tr("The file has no bound import table") });
            Options.Add(new RebuilderOption { Key = "remove_strong_name", Label = Tr("Remove strong name signature"), Description = Tr("Clears the StrongNameSigned flag and the signature directory of a .NET assembly"), IsEnabled = dotnet, DisabledReason = Tr("The file is not a .NET assembly") });

            CanRebase = relocations;
            RebaseReason = relocations ? Tr("Applies the base relocations for the new image base") : Tr("Rebasing needs base relocations");
            OnPropertyChanged(nameof(CanRebase));
            OnPropertyChanged(nameof(RebaseReason));
        }

        private void Rebuild()
        {
            var request = new Dictionary<string, object> { ["command"] = "rebuild" };

            foreach (RebuilderOption option in Options)
            {
                request[option.Key] = option.IsEnabled && option.IsChecked;
            }

            if (m_Realign != 0)
            {
                request["realign"] = m_Realign;
            }

            if (m_ChangeImageBase && CanRebase)
            {
                if (!Format.TryParseNumber(m_NewImageBase, out ulong image_base) || image_base % 0x10000 != 0)
                {
                    Log = Tr("The new image base must be a hex number aligned to 64 KB (10000h)");
                    return;
                }

                request["new_image_base"] = image_base.ToString("X", CultureInfo.InvariantCulture);
            }

            CommandAnswer? answer = Document.Execute(request);

            if (answer != null)
            {
                Log = $"{DateTime.Now:HH:mm:ss}\n{answer.Message}\n\n{Log}".TrimEnd();
            }
        }
    }

    // Import Adder

    public sealed class ExportEntry : ObservableObject
    {
        private bool m_IsChecked;

        public string Name { get; init; } = string.Empty;

        public uint Ordinal { get; init; }

        public string Rva { get; init; } = string.Empty;

        public string Forwarder { get; init; } = string.Empty;

        public string Display => Name.Length > 0 ? Name : Tr("Ordinal {0}", Ordinal);

        public bool IsChecked
        {
            get => m_IsChecked;
            set => SetProperty(ref m_IsChecked, value);
        }
    }

    public sealed class ImportModuleEntry
    {
        public string Name { get; init; } = string.Empty;

        public string Path { get; init; } = string.Empty;

        public List<ExportEntry> Exports { get; init; } = new();
    }

    public sealed class PendingImport : ObservableObject
    {
        private bool m_ByOrdinal;

        public string Module { get; init; } = string.Empty;

        public string Name { get; init; } = string.Empty;

        public uint Ordinal { get; init; }

        public bool CanImportByName => Name.Length > 0;

        public bool ByOrdinal
        {
            get => m_ByOrdinal;
            set
            {
                if (SetProperty(ref m_ByOrdinal, value || !CanImportByName))
                {
                    OnPropertyChanged(nameof(Method));
                }
            }
        }

        public string Method => m_ByOrdinal ? Tr("Ordinal {0}", Ordinal) : Tr("Name");

        public string Display => Name.Length > 0 ? Name : Tr("Ordinal {0}", Ordinal);
    }

    public sealed class ImportAdderViewModel : ToolViewModel
    {
        private ImportModuleEntry? m_SelectedModule;
        private PendingImport? m_SelectedImport;
        private string m_Filter = string.Empty;
        private string m_SectionName = ".imports";
        private bool m_CreateSection;
        private bool m_UseOft = true;
        private string m_Status = string.Empty;

        public ImportAdderViewModel(
            DocumentViewModel document
        )
            : base(document, Tr("Import Adder"), Tr("Adds imports to the file. The import table is rebuilt at the end of the last section (or in a new section), the existing imports keep working."))
        {
            AddModuleCommand = new RelayCommand(AddModule);
            RemoveModuleCommand = new RelayCommand(() => { Modules.Remove(m_SelectedModule!); SelectedModule = Modules.FirstOrDefault(); }, () => m_SelectedModule != null);
            ImportByNameCommand = new RelayCommand(() => ImportChecked(false), () => m_SelectedModule != null);
            ImportByOrdinalCommand = new RelayCommand(() => ImportChecked(true), () => m_SelectedModule != null);
            RemoveImportCommand = new RelayCommand(() => Imports.Remove(m_SelectedImport!), () => m_SelectedImport != null);
            ToggleMethodCommand = new RelayCommand(() => m_SelectedImport!.ByOrdinal = !m_SelectedImport.ByOrdinal, () => m_SelectedImport != null && m_SelectedImport.CanImportByName);
            RebuildCommand = new RelayCommand(Rebuild, () => Imports.Count > 0 && !Document.IsReadOnly);
            Status = Tr("Add the module to import from (its DLL file), check functions and import them by name or ordinal");
        }

        public ObservableCollection<ImportModuleEntry> Modules { get; } = new();

        public ObservableCollection<PendingImport> Imports { get; } = new();

        public ImportModuleEntry? SelectedModule
        {
            get => m_SelectedModule;
            set
            {
                if (SetProperty(ref m_SelectedModule, value))
                {
                    OnPropertyChanged(nameof(FilteredExports));
                }
            }
        }

        public PendingImport? SelectedImport
        {
            get => m_SelectedImport;
            set => SetProperty(ref m_SelectedImport, value);
        }

        public string Filter
        {
            get => m_Filter;
            set
            {
                if (SetProperty(ref m_Filter, value))
                {
                    OnPropertyChanged(nameof(FilteredExports));
                }
            }
        }

        public IEnumerable<ExportEntry> FilteredExports =>
            m_SelectedModule == null ? Enumerable.Empty<ExportEntry>() :
            m_Filter.Length == 0 ? m_SelectedModule.Exports : m_SelectedModule.Exports.Where(entry => entry.Display.Contains(m_Filter, StringComparison.OrdinalIgnoreCase));

        public string SectionName
        {
            get => m_SectionName;
            set => SetProperty(ref m_SectionName, value);
        }

        // Off like in CFF Explorer. The table is appended to the last section, no new section header.
        public bool CreateSection
        {
            get => m_CreateSection;
            set => SetProperty(ref m_CreateSection, value);
        }

        public bool UseOft
        {
            get => m_UseOft;
            set => SetProperty(ref m_UseOft, value);
        }

        public string Status
        {
            get => m_Status;
            private set => SetProperty(ref m_Status, value);
        }

        public ICommand AddModuleCommand { get; }

        public ICommand RemoveModuleCommand { get; }

        public ICommand ImportByNameCommand { get; }

        public ICommand ImportByOrdinalCommand { get; }

        public ICommand RemoveImportCommand { get; }

        public ICommand ToggleMethodCommand { get; }

        public ICommand RebuildCommand { get; }

        private void AddModule()
        {
            string? path = FileDialogs.Open(Tr("Add module"), Tr("Libraries") + " (*.dll;*.exe;*.sys;*.ocx;*.drv)|*.dll;*.exe;*.sys;*.ocx;*.drv|" + Tr("All files") + " (*.*)|*.*");

            if (path == null)
            {
                return;
            }

            try
            {
                using PeDocument module = PeDocument.Open(path, 0, true);
                using JsonDocument answer = module.Query(new Dictionary<string, object> { ["query"] = "exports" });
                var exports = new List<ExportEntry>();

                if (answer.RootElement.TryGetProperty("functions", out JsonElement functions))
                {
                    foreach (JsonElement function in functions.EnumerateArray())
                    {
                        exports.Add(new ExportEntry
                        {
                            Name = CoreJson.GetString(function, "name"),
                            Ordinal = (uint)CoreJson.GetUnsigned(function, "ordinal"),
                            Rva = CoreJson.GetString(function, "rva"),
                            Forwarder = CoreJson.GetString(function, "forwarder")
                        });
                    }
                }

                if (exports.Count == 0)
                {
                    Status = Tr("{0} exports no functions", System.IO.Path.GetFileName(path));
                    return;
                }

                var entry = new ImportModuleEntry { Name = System.IO.Path.GetFileName(path).ToLowerInvariant(), Path = path, Exports = exports };
                Modules.Add(entry);
                SelectedModule = entry;
                Status = Tr("{0}: {1} exported functions", entry.Name, exports.Count.ToString("N0"));
            }
            catch (CoreException error)
            {
                Document.Main.ShowError(Tr("The module could not be read"), error.Message);
            }
        }

        private void ImportChecked(
            bool by_ordinal
        )
        {
            if (m_SelectedModule == null)
            {
                return;
            }

            var picked = m_SelectedModule.Exports.Where(entry => entry.IsChecked).ToList();

            if (picked.Count == 0)
            {
                Status = Tr("Check the functions to import first");
                return;
            }

            foreach (ExportEntry entry in picked)
            {
                bool exists = Imports.Any(import => import.Module == m_SelectedModule.Name && import.Ordinal == entry.Ordinal);

                if (!exists)
                {
                    Imports.Add(new PendingImport { Module = m_SelectedModule.Name, Name = entry.Name, Ordinal = entry.Ordinal, ByOrdinal = by_ordinal || entry.Name.Length == 0 });
                }

                entry.IsChecked = false;
            }

            Status = Imports.Count == 1 ? Tr("1 function to import") : Tr("{0} functions to import", Imports.Count.ToString("N0"));
        }

        private void Rebuild()
        {
            var modules = Imports.GroupBy(import => import.Module).Select(group => new Dictionary<string, object>
            {
                ["name"] = group.Key,
                ["functions"] = group.Select(import => import.ByOrdinal
                    ? new Dictionary<string, object> { ["ordinal"] = import.Ordinal }
                    : new Dictionary<string, object> { ["name"] = import.Name }).ToList()
            }).ToList();

            CommandAnswer? answer = Document.Execute(new Dictionary<string, object>
            {
                ["command"] = "imports.add",
                ["modules"] = modules,
                ["use_oft"] = m_UseOft,
                ["create_section"] = m_CreateSection,
                ["section_name"] = m_SectionName.Length > 0 ? m_SectionName : ".imports"
            });

            if (answer != null)
            {
                Imports.Clear();
                Status = answer.Message ?? Tr("Import table rebuilt");
            }
        }
    }
}