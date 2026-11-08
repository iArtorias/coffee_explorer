using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Mvvm;
using CoffeeExplorer.Services;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.ViewModels
{
    // Context menu or toolbar entry of a view, filled from the commands of the core view.
    public sealed class CommandItem
    {
        public CommandItem(
            CommandData data,
            Action<CommandData> run
        )
        {
            Data = data;
            Command = new RelayCommand(() => run(data), () => data.Enabled);
        }

        public CommandData Data { get; }

        public string Label => Data.Label;

        public bool IsRowCommand => Data.Scope == "row";

        public string? ToolTip => Data.Enabled ? null : Data.Reason;

        public RelayCommand Command { get; }

        public string IconKey => Data.Id switch
        {
            "rich.encrypt" => "Icon.Lock",
            "rich.decrypt" => "Icon.Unlock",
            "symbols.load" => "Icon.PDBFile",
            "symbols.browse" => "Icon.OpenFile",
            "symbols.download" => "Icon.CloudDownload",
            var id when id.Contains("delete") || id.Contains("remove") || id.Contains("clear") => "Icon.Delete",
            var id when id.Contains("add") => "Icon.Add",
            var id when id.Contains("move_up") => "Icon.MoveUp",
            var id when id.Contains("move_down") => "Icon.MoveDown",
            var id when id.Contains("save") || id.Contains("dump") => "Icon.Save",
            var id when id.Contains("replace") => "Icon.Edit",
            var id when id.Contains("import") => "Icon.Import",
            var id when id.StartsWith("rebuild", StringComparison.Ordinal) => "Icon.BuildSolution",
            _ => "Icon.Run"
        };
    }

    // One frame of an icon, cursor or bitmap resource
    public sealed class PreviewImage
    {
        public ImageSource Source { get; init; } = null!;

        public string Caption { get; init; } = string.Empty;

        public double Width { get; init; }

        public double Height { get; init; }
    }

    // Content of a structure node. The view JSON of the core turned into tables, a hex region, a preview and
    // a detail part for list rows. Nested for master and detail lists (the detail is another NodeViewModel).
    public sealed class NodeViewModel : ObservableObject, ICellHost
    {
        private readonly DocumentViewModel m_Owner;
        private NodeViewModel? m_Detail;
        private string? m_Error;

        public NodeViewModel(
            DocumentViewModel owner,
            ViewData data,
            int depth = 0
        )
            : this(owner, data, depth, null, null)
        {
        }

        // List was built on a worker thread from data (large tables), its cells talk to this node through host.
        public NodeViewModel(
            DocumentViewModel owner,
            ViewData data,
            int depth,
            TableViewModel? list,
            CellHostProxy? host
        )
        {
            m_Owner = owner;
            Data = data;
            Depth = depth;

            if (host != null)
            {
                host.Target = this;
            }

            if (data.Fields != null && data.Fields.Count > 0)
            {
                Fields = TableViewModel.FromFields(this, data.Fields, data.Layout == "overview");
            }

            if (data.Version != null && data.Version.Count > 0)
            {
                Version = TableViewModel.FromFields(this, data.Version, true);
            }

            if (data.Columns != null && data.Rows != null)
            {
                List = list ?? TableViewModel.FromList(this, data.Columns, data.Rows);
                List.SelectionChanged += (_, _) => OnRowSelected();
            }

            if (Fields != null)
            {
                Fields.SelectionChanged += (_, _) => m_Owner.ReportFocus(null, Fields.SelectedRow);
            }

            Commands = (data.Commands ?? new List<CommandData>()).Select(command => new CommandItem(command, m_Owner.RunViewCommand(this))).ToList();
            Warnings = data.Warnings ?? new List<string>();
            LoadPreview();
        }

        public ViewData Data { get; }

        public int Depth { get; }

        public string Node => Data.Node;

        public string Title => Data.Title;

        public string Layout => Data.Layout;

        public DocumentViewModel Owner => m_Owner;

        public TableViewModel? Fields { get; }

        public TableViewModel? Version { get; }

        public TableViewModel? List { get; }

        public bool HasFields => Fields != null;

        public bool HasVersion => Version != null;

        public bool HasList => List != null;

        // Field grids above a list keep their natural height, a fields only view fills the pane.
        public bool FieldsOnly => Fields != null && List == null && !HasHex;

        public string? Info => Data.Info;

        public bool HasInfo => !string.IsNullOrEmpty(Data.Info);

        public bool HasHex => Data.Hex != null && Data.Hex.Length > 0;

        // Resources show the preview and the raw data in two tabs.
        public bool HasPreviewTabs => HasPreview || HasPreviewText;

        public ulong HexOffset => Data.Hex?.Offset ?? 0;

        public ulong HexLength => Data.Hex?.Length ?? 0;

        public string HexTitle => Data.Hex?.Title ?? Tr("Data");

        public PeDocument Document => m_Owner.Document;

        public bool IsReadOnly => m_Owner.IsReadOnly;

        public List<CommandItem> Commands { get; }

        public List<CommandItem> ViewCommands => Commands.Where(command => !command.IsRowCommand).ToList();

        public List<CommandItem> RowCommands => Commands.Where(command => command.IsRowCommand).ToList();

        public bool HasCommands => Commands.Count > 0;

        public List<string> Warnings { get; }

        public bool HasWarnings => Warnings.Count > 0;

        public string WarningText => string.Join("\n", Warnings);

        // Detail of the selected row (list rows with a "detail" node).
        public NodeViewModel? Detail
        {
            get => m_Detail;
            private set
            {
                if (SetProperty(ref m_Detail, value))
                {
                    OnPropertyChanged(nameof(HasDetail));
                }
            }
        }

        public bool HasDetail => m_Detail != null;

        // Rows of this list open a detail part, even before one is selected. Asked several times while the view is built,
        // computed once (a list without details scanned all its rows each time).
        private bool? m_HasDetailPart;

        public bool HasDetailPart => m_HasDetailPart ??= Data.Rows != null && Data.Rows.Any(row => row.Detail != null);

        public string? Error
        {
            get => m_Error;
            private set => SetProperty(ref m_Error, value);
        }

        public List<PreviewImage> Preview { get; } = new();

        public bool HasPreview => Preview.Count > 0;

        public string? PreviewText { get; private set; }

        public bool HasPreviewText => PreviewText != null;

        // ICellHost

        public ChoiceSetData? FindChoices(
            string? key
        )
        {
            return key != null && Data.ChoiceSets != null && Data.ChoiceSets.TryGetValue(key, out ChoiceSetData? set) ? set : null;
        }

        public string? WriteValue(
            TableCell cell,
            ulong value
        )
        {
            if (cell.Command != null)
            {
                return m_Owner.ExecuteEdit(cell.Command, cell.Argument, value);
            }

            if (cell.Offset == null)
            {
                return Tr("The value has no file offset");
            }

            return m_Owner.WriteBytes(cell.Offset.Value, Format.LittleEndian(value, cell.Size), Tr("Edit {0}", cell.Label));
        }

        public string? WriteText(
            TableCell cell,
            string text
        )
        {
            if (cell.Command != null)
            {
                return m_Owner.ExecuteEdit(cell.Command, cell.Argument, text);
            }

            if (cell.Offset == null)
            {
                return Tr("The value has no file offset");
            }

            if (text.Length > cell.Size)
            {
                return Tr("The text is longer than the field ({0} characters)", cell.Size);
            }

            if (text.Any(character => character > 0xFF))
            {
                return Tr("Only 8 bit characters fit into the field");
            }

            return m_Owner.WriteBytes(cell.Offset.Value, Format.FixedString(text, cell.Size), Tr("Edit {0}", cell.Label));
        }

        public void ReportFocus(
            TableCell? cell,
            TableRow? row
        )
        {
            m_Owner.ReportFocus(cell, row);
        }

        // Selection

        private void OnRowSelected()
        {
            TableRow? row = List?.SelectedRow;
            m_Owner.ReportFocus(null, row);

            if (row?.Detail == null)
            {
                Detail = null;
                return;
            }

            try
            {
                Detail = new NodeViewModel(m_Owner, m_Owner.LoadView(row.Detail), Depth + 1);
                Error = null;
            }
            catch (Exception error)
            {
                Detail = null;
                Error = error.Message;
            }
        }

        // Selects the first row so the detail part is never empty (CFF left it blank until a click).
        public void SelectInitialRow(
            int index = 0
        )
        {
            if (List != null && List.Rows.Count > 0 && HasDetailPart)
            {
                List.SelectedRow = List.Rows[Math.Clamp(index, 0, List.Rows.Count - 1)];
            }
        }

        public int SelectedIndex => List?.SelectedRow?.Index ?? -1;

        // Preview

        private void LoadPreview()
        {
            string? kind = Data.Preview;

            if (kind == null)
            {
                return;
            }

            if (kind == "text" || (Data.Text != null && kind != "image" && kind != "icon" && kind != "cursor"))
            {
                PreviewText = Data.Text;
                return;
            }

            if (kind != "image" && kind != "icon" && kind != "cursor")
            {
                return;
            }

            int slash = Data.Node.LastIndexOf('/');

            if (slash < 0 || !int.TryParse(Data.Node.AsSpan(slash + 1), out int index))
            {
                return;
            }

            try
            {
                byte[] file = Document.ExportResource(index, true, out _);
                using var stream = new MemoryStream(file);
                BitmapDecoder decoder = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);

                foreach (BitmapFrame frame in decoder.Frames.OrderByDescending(frame => frame.PixelWidth))
                {
                    frame.Freeze();
                    int bits = frame.Format.BitsPerPixel;
                    Preview.Add(new PreviewImage
                    {
                        Source = frame,
                        Width = frame.PixelWidth,
                        Height = frame.PixelHeight,
                        Caption = Tr("{0} x {1}, {2} bit", frame.PixelWidth, frame.PixelHeight, bits)
                    });
                }
            }
            catch (Exception error)
            {
                Warnings.Add(Tr("Preview not available: {0}", error.Message));
            }
        }
    }
}