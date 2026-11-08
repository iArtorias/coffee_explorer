using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Mvvm;
using CoffeeExplorer.Services;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.ViewModels
{
    public enum CellEditor
    {
        EDITOR_NONE = 0,
        EDITOR_HEX,
        EDITOR_TEXT,
        EDITOR_FLAGS,
        EDITOR_ENUM
    }

    // Writes edited cells back into the document and resolves choice sets.
    public interface ICellHost
    {
        bool IsReadOnly { get; }

        ChoiceSetData? FindChoices(
            string? key
        );

        // Returns an error message or null.
        string? WriteValue(
            TableCell cell,
            ulong value
        );

        string? WriteText(
            TableCell cell,
            string text
        );

        void ReportFocus(
            TableCell? cell,
            TableRow? row
        );
    }

    // Host of cells built on a worker thread before their NodeViewModel exists, the node sets Target when it takes them.
    public sealed class CellHostProxy : ICellHost
    {
        public ICellHost? Target { get; set; }

        public bool IsReadOnly => Target?.IsReadOnly ?? true;

        public ChoiceSetData? FindChoices(
            string? key
        )
        {
            return Target?.FindChoices(key);
        }

        public string? WriteValue(
            TableCell cell,
            ulong value
        )
        {
            return Target != null ? Target.WriteValue(cell, value) : Tr("The view is not ready");
        }

        public string? WriteText(
            TableCell cell,
            string text
        )
        {
            return Target != null ? Target.WriteText(cell, text) : Tr("The view is not ready");
        }

        public void ReportFocus(
            TableCell? cell,
            TableRow? row
        )
        {
            Target?.ReportFocus(cell, row);
        }
    }

    public sealed class TableColumn
    {
        public string Header { get; init; } = string.Empty;

        public string? Tip { get; init; }

        // Hex values and offsets use the monospace font.
        public bool IsMono { get; init; }

        // Member and value columns of field grids get a fixed minimum width, the last column fills.
        public double MinWidth { get; init; } = 60;
    }

    public sealed class TableCell : ObservableObject
    {
        private string m_Text;
        private string? m_Meaning;
        private string? m_ToolTip;

        public TableCell(
            ICellHost? host,
            string text,
            ulong? offset = null,
            uint size = 0,
            CellEditor editor = CellEditor.EDITOR_NONE,
            string? choices = null,
            string? meaning = null,
            bool mono = false,
            string? command = null,
            string? argument = null
        )
        {
            Host = host;
            m_Text = text;
            Offset = offset;
            Size = size;
            Command = command;
            Argument = argument;
            Editor = offset == null && command == null ? CellEditor.EDITOR_NONE : editor;
            Choices = choices;
            m_Meaning = meaning;
            IsMono = mono;
        }

        public ICellHost? Host { get; }

        public string Text
        {
            get => m_Text;
            set => SetProperty(ref m_Text, value);
        }

        public string? Meaning
        {
            get => m_Meaning;
            set => SetProperty(ref m_Meaning, value);
        }

        public bool HasMeaning => !string.IsNullOrEmpty(m_Meaning);

        public ulong? Offset { get; }

        public uint Size { get; }

        public CellEditor Editor { get; }

        // Core command that applies an edit of this cell (values not stored as plain bytes), null for offset cells.
        public string? Command { get; }

        public string? Argument { get; }

        public string? Choices { get; }

        public bool IsMono { get; }

        // Offset cells describe their place in the file, made when the tooltip opens (large tables have many cells).
        public string? ToolTip
        {
            get => m_ToolTip ?? (Offset != null ? Tr("Offset {0}, {1}", Format.Offset(Offset.Value), Format.SizeName(Size)) : null);
            set => SetProperty(ref m_ToolTip, value);
        }

        // Set by the view (File info, field tips), not the offset text made on the fly.
        public bool HasCustomToolTip => m_ToolTip != null;

        // Member or column name, used for the undo text.
        public string Label { get; init; } = "value";

        public bool IsEditable => Editor != CellEditor.EDITOR_NONE && Host != null && !Host.IsReadOnly;

        public bool HasChoices => Editor == CellEditor.EDITOR_FLAGS || Editor == CellEditor.EDITOR_ENUM;

        public bool IsTextEditor => Editor == CellEditor.EDITOR_HEX || Editor == CellEditor.EDITOR_TEXT;

        public ulong NumericValue => Format.TryParseNumber(m_Text, out ulong value) ? value : 0;

        public string? Commit(
            string text
        )
        {
            if (Host == null || !IsEditable)
            {
                return Tr("The value is read only");
            }

            if (Editor == CellEditor.EDITOR_TEXT)
            {
                return Host.WriteText(this, text);
            }

            if (!Format.TryParseNumber(text, out ulong value))
            {
                return Tr("\"{0}\" is not a number. Values are hex, add \" d\" for decimal.", text);
            }

            if (value > Format.MaxValue(Size))
            {
                return Tr("The value does not fit into {0} ({1} bytes)", Format.SizeName(Size), Size);
            }

            return Host.WriteValue(this, value);
        }

        public string? CommitValue(
            ulong value
        )
        {
            return Host == null || !IsEditable ? Tr("The value is read only") : Host.WriteValue(this, value);
        }

        public static CellEditor ParseEditor(
            string? editor
        )
        {
            return editor switch
            {
                "hex" => CellEditor.EDITOR_HEX,
                "text" => CellEditor.EDITOR_TEXT,
                "flags" => CellEditor.EDITOR_FLAGS,
                "enum" => CellEditor.EDITOR_ENUM,
                _ => CellEditor.EDITOR_NONE
            };
        }
    }

    public sealed class TableRow : ObservableObject
    {
        public TableRow(
            List<TableCell> cells
        )
        {
            Cells = cells;
        }

        public List<TableCell> Cells { get; }

        public string? Detail { get; init; }

        public string? Navigate { get; init; }

        public ulong? Offset { get; init; }

        public ulong Size { get; init; }

        public int Index { get; init; }

        // Rows that must catch the eye (the entry point). Accent tint, bold text, marker on the left.
        public bool IsHighlighted { get; init; }

        // Extra payload (resource item, dependency module etc).
        public object? Tag { get; init; }

        public string Copy()
        {
            return string.Join("\t", Cells.Select(cell => cell.HasMeaning && !cell.Text.Contains(cell.Meaning!, StringComparison.Ordinal) ? $"{cell.Text} ({cell.Meaning})" : cell.Text));
        }
    }

    // A grid. Field list (Member, Offset, Size, Value, Meaning) or list (columns from the core).
    public sealed class TableViewModel : ObservableObject
    {
        private TableRow? m_SelectedRow;

        public TableViewModel(
            List<TableColumn> columns,
            List<TableRow> rows,
            bool is_fields
        )
        {
            Columns = columns;
            Rows = rows;
            IsFields = is_fields;
        }

        public List<TableColumn> Columns { get; }

        public List<TableRow> Rows { get; }

        public bool IsFields { get; }

        public bool IsEmpty => Rows.Count == 0;

        public TableRow? SelectedRow
        {
            get => m_SelectedRow;
            set
            {
                if (SetProperty(ref m_SelectedRow, value))
                {
                    SelectionChanged?.Invoke(this, EventArgs.Empty);
                }
            }
        }

        public event EventHandler? SelectionChanged;

        public string CopyAll(
            IEnumerable<TableRow>? rows = null
        )
        {
            var builder = new StringBuilder();
            builder.AppendLine(string.Join("\t", Columns.Select(column => column.Header)));

            foreach (TableRow row in rows ?? Rows)
            {
                builder.AppendLine(row.Copy());
            }

            return builder.ToString();
        }

        // Builders

        public static TableViewModel FromFields(
            ICellHost? host,
            IReadOnlyList<FieldData> fields,
            bool property_list = false
        )
        {
            var rows = new List<TableRow>();

            // Overview style property and value lists have no offsets.
            if (property_list || fields.All(field => field.Offset == null && field.Type == null))
            {
                for (int index = 0; index < fields.Count; ++index)
                {
                    FieldData field = fields[index];

                    // Command fields are editable (version information), the others are plain text.
                    TableCell value = field.Command != null
                        ? new TableCell(host, field.Value, null, field.Size, TableCell.ParseEditor(field.Editor), field.Choices, field.Meaning,
                               field.Editor != "text", field.Command, field.Argument)
                        { Label = field.Name }
                        : new TableCell(null, field.Value, meaning: field.Meaning);

                    rows.Add(new TableRow(new List<TableCell>
                    {
                        new( null, field.Name ) { ToolTip = field.Tip },
                        value
                    })
                    { Index = index, IsHighlighted = field.Highlight != null });
                }

                return new TableViewModel(new List<TableColumn>
                {
                    new() { Header = Tr( "Property" ), MinWidth = 160 },
                    new() { Header = Tr( "Value" ), MinWidth = 300 }
                }, rows, true);
            }

            for (int index = 0; index < fields.Count; ++index)
            {
                FieldData field = fields[index];
                CellEditor editor = TableCell.ParseEditor(field.Editor);
                string offset = field.Offset != null ? Format.Offset(field.Offset.Value) : string.Empty;

                rows.Add(new TableRow(new List<TableCell>
                {
                    new( null, field.Name ) { ToolTip = field.Tip },
                    new( null, offset, mono: true ),
                    new( null, field.Type ?? Format.SizeName( field.Size ) ),
                    new( host, field.Value, field.Offset, field.Size, editor, field.Choices, null, editor != CellEditor.EDITOR_TEXT ) { ToolTip = field.Tip, Label = field.Name },
                    new( null, field.Meaning ?? string.Empty )
                })
                {
                    Index = index,
                    Offset = field.Offset,
                    Size = field.Size,
                    IsHighlighted = field.Highlight != null
                });
            }

            return new TableViewModel(new List<TableColumn>
            {
                new() { Header = Tr( "Member" ), MinWidth = 200 },
                new() { Header = Tr( "Offset" ), IsMono = true, MinWidth = 80 },
                new() { Header = Tr( "Size" ), MinWidth = 60 },
                new() { Header = Tr( "Value" ), IsMono = true, MinWidth = 140 },
                new() { Header = Tr( "Meaning" ), MinWidth = 240 }
            }, rows, true);
        }

        public static TableViewModel FromList(
            ICellHost? host,
            IReadOnlyList<ColumnData> columns,
            IReadOnlyList<RowData> rows
        )
        {
            // A column is monospace when its editable cells hold hex values.
            var mono = new bool[columns.Count];

            foreach (RowData row in rows.Take(64))
            {
                for (int index = 0; index < row.Cells.Count && index < mono.Length; ++index)
                {
                    CellData cell = row.Cells[index];

                    if (cell.Editor == "hex" || cell.Editor == "flags" || cell.Editor == "enum" || LooksHex(cell.Text))
                    {
                        mono[index] = true;
                    }
                }
            }

            var table_columns = columns.Select((column, index) => new TableColumn { Header = column.Name, Tip = column.Tip, IsMono = mono[index] }).ToList();
            var table_rows = new List<TableRow>(rows.Count);

            for (int row_index = 0; row_index < rows.Count; ++row_index)
            {
                RowData row = rows[row_index];
                var cells = new List<TableCell>(row.Cells.Count);

                for (int index = 0; index < row.Cells.Count; ++index)
                {
                    CellData cell = row.Cells[index];
                    CellEditor editor = TableCell.ParseEditor(cell.Editor);
                    cells.Add(new TableCell(host, cell.Text, cell.Offset, cell.Size, editor, cell.Choices, cell.Meaning,
                                              index < mono.Length && mono[index] && editor != CellEditor.EDITOR_TEXT, cell.Command, cell.Argument)
                    {
                        Label = index < columns.Count ? columns[index].Name : "value"
                    });
                }

                // Short rows (ragged data) still get every column.
                while (cells.Count < table_columns.Count)
                {
                    cells.Add(new TableCell(null, string.Empty));
                }

                table_rows.Add(new TableRow(cells)
                {
                    Index = row_index,
                    Detail = row.Detail,
                    Navigate = row.Navigate,
                    Offset = row.Offset,
                    Size = row.Size
                });
            }

            return new TableViewModel(table_columns, table_rows, false);
        }

        private static bool LooksHex(
            string text
        )
        {
            return text.Length is 4 or 8 or 16 && text.All(Uri.IsHexDigit);
        }

        public static string HexText(
            ulong value,
            uint size
        )
        {
            return value.ToString("X" + Math.Max(1, Math.Min(16, size * 2)), CultureInfo.InvariantCulture);
        }
    }
}