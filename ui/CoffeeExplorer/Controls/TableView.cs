using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Services;
using CoffeeExplorer.ViewModels;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.Controls
{
    // Turns a ListView into the grid of a TableViewModel. One GridView column per table column, a ValueCell per
    // cell, sorting by header click (lists only), copy, F2 and F4 editing, navigation rows and the view commands
    // in the context menu. Every grid of the application goes through here so they all behave the same.
    public static class TableView
    {
        public static readonly DependencyProperty TableProperty = DependencyProperty.RegisterAttached(
            "Table", typeof(TableViewModel), typeof(TableView), new PropertyMetadata(null, OnTableChanged));

        public static readonly DependencyProperty NodeProperty = DependencyProperty.RegisterAttached(
            "Node", typeof(NodeViewModel), typeof(TableView), new PropertyMetadata(null));

        private static readonly DependencyProperty SortProperty = DependencyProperty.RegisterAttached(
            "Sort", typeof(SortState), typeof(TableView), new PropertyMetadata(null));

        private static readonly DependencyProperty ReorderProperty = DependencyProperty.RegisterAttached(
            "Reorder", typeof(ReorderState), typeof(TableView), new PropertyMetadata(null));

        private sealed class SortState
        {
            public int Column = -1;
            public bool Descending;
        }

        // Drag and drop reordering of rows (import descriptor move).
        private sealed class ReorderState
        {
            public TableRow? Row;
            public Point StartPoint;
            public bool Dragging;
        }

        public static TableViewModel? GetTable(
            DependencyObject element
        )
        {
            return (TableViewModel?)element.GetValue(TableProperty);
        }

        public static void SetTable(
            DependencyObject element,
            TableViewModel? value
        )
        {
            element.SetValue(TableProperty, value);
        }

        public static NodeViewModel? GetNode(
            DependencyObject element
        )
        {
            return (NodeViewModel?)element.GetValue(NodeProperty);
        }

        public static void SetNode(
            DependencyObject element,
            NodeViewModel? value
        )
        {
            element.SetValue(NodeProperty, value);
        }

        private static void OnTableChanged(
            DependencyObject element,
            DependencyPropertyChangedEventArgs arguments
        )
        {
            if (element is not ListView list)
            {
                return;
            }

            if (list.GetValue(SortProperty) == null)
            {
                list.SetValue(SortProperty, new SortState());
                list.PreviewKeyDown += OnKeyDown;
                list.MouseDoubleClick += OnDoubleClick;
                list.ContextMenuOpening += OnContextMenuOpening;
                list.ContextMenu = new ContextMenu();
                list.AddHandler(GridViewColumnHeader.ClickEvent, new RoutedEventHandler(OnHeaderClick));
                VirtualizingPanel.SetIsVirtualizing(list, true);
                VirtualizingPanel.SetVirtualizationMode(list, VirtualizationMode.Recycling);
                ScrollViewer.SetCanContentScroll(list, true);
                list.SelectionMode = SelectionMode.Extended;
                list.SetValue(ReorderProperty, new ReorderState());
                list.PreviewMouseLeftButtonDown += OnReorderMouseDown;
                list.PreviewMouseLeftButtonUp += OnReorderMouseUp;
                list.PreviewMouseMove += OnReorderMouseMove;
                list.DragOver += OnReorderDragOver;
                list.Drop += OnReorderDrop;
            }

            var table = arguments.NewValue as TableViewModel;
            BindingOperations.ClearBinding(list, Selector.SelectedItemProperty);

            if (table == null)
            {
                list.ItemsSource = null;
                list.View = null;
                return;
            }

            ((SortState)list.GetValue(SortProperty)).Column = -1;
            var view = new GridView { AllowsColumnReorder = false };

            for (int index = 0; index < table.Columns.Count; ++index)
            {
                TableColumn column = table.Columns[index];
                var factory = new FrameworkElementFactory(typeof(ValueCell));
                factory.SetBinding(FrameworkElement.DataContextProperty, new Binding($"Cells[{index}]"));

                var header = new GridViewColumnHeader
                {
                    Content = column.Header,
                    Tag = index,
                    ToolTip = column.Tip,
                    HorizontalContentAlignment = HorizontalAlignment.Left
                };

                view.Columns.Add(new GridViewColumn
                {
                    Header = header,
                    CellTemplate = new DataTemplate { VisualTree = factory },
                    Width = double.NaN
                });
            }

            list.View = view;
            list.ItemsSource = table.Rows;
            list.SetBinding(Selector.SelectedItemProperty, new Binding(nameof(TableViewModel.SelectedRow)) { Source = table, Mode = BindingMode.TwoWay });

            list.Dispatcher.BeginInvoke(() => FitColumns(list, table), System.Windows.Threading.DispatcherPriority.Loaded);

            // Values that arrive later (Detect It Easy result, edits) widen the columns again.
            PropertyChangedEventHandler refit = (_, change) =>
            {
                if (change.PropertyName == nameof(TableCell.Text) || change.PropertyName == nameof(TableCell.Meaning))
                {
                    list.Dispatcher.BeginInvoke(() =>
                    {
                        if (GetTable(list) == table)
                        {
                            FitColumns(list, table);
                        }
                    }, System.Windows.Threading.DispatcherPriority.Background);
                }
            };

            foreach (TableRow row in table.Rows.Take(FIT_SAMPLE_ROWS))
            {
                foreach (TableCell cell in row.Cells)
                {
                    cell.PropertyChanged += refit;
                }
            }
        }

        // Rows that are measured to size a column. Virtualized rows are not realized yet when the grid is shown, so
        // the widths come from the text itself instead of the auto sizing of GridView.
        private const int FIT_SAMPLE_ROWS = 500;
        private const double FIT_MAX_WIDTH = 640;
        private const double FIT_MAX_LAST_WIDTH = 1200;
        private const double FIT_CELL_PADDING = 22;
        private const double FIT_CHOICES_WIDTH = 26;
        // Longest cells of a column (by characters) that are measured, proportional fonts vary a little per character.
        private const int FIT_MEASURED_CELLS = 32;

        private static int CellLength(
            TableCell cell
        )
        {
            return cell.Text.Length + (cell.HasMeaning ? cell.Meaning!.Length + 1 : 0) + (cell.HasChoices && cell.IsEditable ? 3 : 0);
        }

        private static void FitColumns(
            ListView list,
            TableViewModel table
        )
        {
            if (list.View is not GridView view)
            {
                return;
            }

            double pixels_per_dip = VisualTreeHelper.GetDpi(list).PixelsPerDip;
            var ui_face = new Typeface(list.FontFamily, FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);
            var mono_face = new Typeface(list.TryFindResource("Font.Mono") as FontFamily ?? list.FontFamily, FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);
            double font_size = list.FontSize;
            int sample = Math.Min(table.Rows.Count, FIT_SAMPLE_ROWS);

            double Measure(
                string? text,
                Typeface face
            )
            {
                if (string.IsNullOrEmpty(text))
                {
                    return 0;
                }

                var formatted = new FormattedText(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight, face, font_size, Brushes.Black, pixels_per_dip);
                return formatted.WidthIncludingTrailingWhitespace;
            }

            // Only the longest texts of a column can be the widest.
            var candidates = new List<TableCell>(sample);

            for (int index = 0; index < view.Columns.Count && index < table.Columns.Count; ++index)
            {
                TableColumn column = table.Columns[index];
                double width = Measure(column.Header, ui_face);
                candidates.Clear();

                for (int row = 0; row < sample; ++row)
                {
                    List<TableCell> cells = table.Rows[row].Cells;

                    if (index < cells.Count)
                    {
                        candidates.Add(cells[index]);
                    }
                }

                if (candidates.Count > FIT_MEASURED_CELLS)
                {
                    candidates.Sort((left, right) => CellLength(right).CompareTo(CellLength(left)));
                    candidates.RemoveRange(FIT_MEASURED_CELLS, candidates.Count - FIT_MEASURED_CELLS);
                }

                foreach (TableCell cell in candidates)
                {
                    double cell_width = Measure(cell.Text, cell.IsMono ? mono_face : ui_face);

                    if (cell.HasMeaning)
                    {
                        cell_width += 8 + Measure(cell.Meaning, ui_face);
                    }

                    if (cell.HasChoices && cell.IsEditable)
                    {
                        cell_width += FIT_CHOICES_WIDTH;
                    }

                    width = Math.Max(width, cell_width);
                }

                bool last = index == table.Columns.Count - 1;
                width = Math.Clamp(Math.Ceiling(width + FIT_CELL_PADDING), column.MinWidth, last ? FIT_MAX_LAST_WIDTH : FIT_MAX_WIDTH);
                view.Columns[index].Width = width;
            }
        }

        private static IEnumerable<TableRow> SelectedRows(
            ListView list
        )
        {
            // In view order (sorting included).
            if (list.SelectedItems.Count == 1)
            {
                return list.SelectedItems.Cast<TableRow>().ToList();
            }

            var selected = new HashSet<TableRow>(list.SelectedItems.Cast<TableRow>());
            return list.Items.Cast<TableRow>().Where(selected.Contains).ToList();
        }

        private static void Copy(
            string text
        )
        {
            try
            {
                Clipboard.SetText(text);
            }
            catch (Exception)
            {
                // Clipboard locked by another process.
            }
        }

        private static ValueCell? FindEditableCell(
            ListView list
        )
        {
            if (list.SelectedItem == null || list.ItemContainerGenerator.ContainerFromItem(list.SelectedItem) is not DependencyObject container)
            {
                return null;
            }

            return FindDescendants<ValueCell>(container).FirstOrDefault(cell => cell.CanEdit);
        }

        private static IEnumerable<T> FindDescendants<T>(
            DependencyObject root
        ) where T : DependencyObject
        {
            int count = VisualTreeHelper.GetChildrenCount(root);

            for (int index = 0; index < count; ++index)
            {
                DependencyObject child = VisualTreeHelper.GetChild(root, index);

                if (child is T match)
                {
                    yield return match;
                }

                foreach (T nested in FindDescendants<T>(child))
                {
                    yield return nested;
                }
            }
        }

        private static void OnKeyDown(
            object sender,
            KeyEventArgs arguments
        )
        {
            var list = (ListView)sender;

            if (arguments.OriginalSource is TextBox)
            {
                return;
            }

            if (arguments.Key == Key.C && (Keyboard.Modifiers & ModifierKeys.Control) != 0)
            {
                TableViewModel? table = GetTable(list);

                if (table != null && list.SelectedItems.Count > 0)
                {
                    Copy(list.SelectedItems.Count == 1 ? ((TableRow)list.SelectedItem).Copy() : string.Join(Environment.NewLine, SelectedRows(list).Select(row => row.Copy())));
                }

                arguments.Handled = true;
            }
            else if (arguments.Key == Key.F2 || arguments.Key == Key.F4)
            {
                FindEditableCell(list)?.BeginEdit(arguments.Key == Key.F4);
                arguments.Handled = true;
            }
            else if (arguments.Key == Key.Enter && list.SelectedItem is TableRow row && row.Navigate != null)
            {
                GetNode(list)?.Owner.NavigateTo(row.Navigate);
                arguments.Handled = true;
            }
        }

        private static void OnDoubleClick(
            object sender,
            MouseButtonEventArgs arguments
        )
        {
            // Double clicks on editable cells are handled by the cell.
            if (arguments.Handled || sender is not ListView list || list.SelectedItem is not TableRow row)
            {
                return;
            }

            for (DependencyObject? element = arguments.OriginalSource as DependencyObject; element != null && element != list; element = element is Visual ? VisualTreeHelper.GetParent(element) : LogicalTreeHelper.GetParent(element))
            {
                if (element is ValueCell cell && cell.CanEdit)
                {
                    return;
                }
            }

            if (row.Navigate != null)
            {
                GetNode(list)?.Owner.NavigateTo(row.Navigate);
                arguments.Handled = true;
            }
        }

        private static void OnHeaderClick(
            object sender,
            RoutedEventArgs arguments
        )
        {
            if (sender is not ListView list || arguments.OriginalSource is not GridViewColumnHeader header || header.Tag is not int column)
            {
                return;
            }

            TableViewModel? table = GetTable(list);

            // Field grids keep the structure order.
            if (table == null || table.IsFields)
            {
                return;
            }

            var state = (SortState)list.GetValue(SortProperty);
            state.Descending = state.Column == column && !state.Descending;
            state.Column = column;
            bool mono = column < table.Columns.Count && table.Columns[column].IsMono;

            if (CollectionViewSource.GetDefaultView(list.ItemsSource) is ListCollectionView view)
            {
                view.CustomSort = new RowComparer(column, state.Descending, mono);
            }

            if (list.View is GridView grid)
            {
                for (int index = 0; index < grid.Columns.Count; ++index)
                {
                    if (grid.Columns[index].Header is GridViewColumnHeader other && index < table.Columns.Count)
                    {
                        other.Content = index == column ? table.Columns[index].Header + (state.Descending ? "  ▾" : "  ▴") : table.Columns[index].Header;
                    }
                }
            }
        }

        private sealed class RowComparer : IComparer
        {
            private readonly int m_Column;
            private readonly bool m_Descending;
            private readonly bool m_Numeric;

            public RowComparer(
                int column,
                bool descending,
                bool numeric
            )
            {
                m_Column = column;
                m_Descending = descending;
                m_Numeric = numeric;
            }

            public int Compare(
                object? left,
                object? right
            )
            {
                string a = left is TableRow row_a && m_Column < row_a.Cells.Count ? row_a.Cells[m_Column].Text : string.Empty;
                string b = right is TableRow row_b && m_Column < row_b.Cells.Count ? row_b.Cells[m_Column].Text : string.Empty;
                int result;

                if (m_Numeric && ulong.TryParse(a, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out ulong x) &&
                     ulong.TryParse(b, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out ulong y))
                {
                    result = x.CompareTo(y);
                }
                else if (double.TryParse(a, NumberStyles.Float, CultureInfo.InvariantCulture, out double p) &&
                          double.TryParse(b, NumberStyles.Float, CultureInfo.InvariantCulture, out double q))
                {
                    result = p.CompareTo(q);
                }
                else
                {
                    result = string.Compare(a, b, StringComparison.OrdinalIgnoreCase);
                }

                if (result == 0 && left is TableRow first && right is TableRow second)
                {
                    result = first.Index.CompareTo(second.Index);
                }

                return m_Descending ? -result : result;
            }
        }

        private static void OnContextMenuOpening(
            object sender,
            ContextMenuEventArgs arguments
        )
        {
            var list = (ListView)sender;
            ContextMenu menu = list.ContextMenu;
            menu.Items.Clear();
            TableViewModel? table = GetTable(list);
            NodeViewModel? node = GetNode(list);
            var row = list.SelectedItem as TableRow;

            if (table == null)
            {
                arguments.Handled = true;
                return;
            }

            ValueCell? editable = FindEditableCell(list);

            if (editable != null)
            {
                menu.Items.Add(MakeItem(Tr("Edit value"), "Icon.Edit", () => editable.BeginEdit(true), true, "F2"));
                menu.Items.Add(new Separator());
            }

            if (row?.Navigate != null && node != null)
            {
                string target = row.Navigate;
                menu.Items.Add(MakeItem(Tr("Go to directory"), "Icon.GoToCurrentLine", () => node.Owner.NavigateTo(target), true, "Enter"));
                menu.Items.Add(new Separator());
            }

            menu.Items.Add(MakeItem(Tr("Copy"), "Icon.Copy", () => Copy(string.Join(Environment.NewLine, SelectedRows(list).Select(item => item.Copy()))), row != null, "Ctrl+C"));

            if (row != null)
            {
                TableCell? value = row.Cells.FirstOrDefault(cell => cell.Offset != null) ?? (row.Cells.Count > 1 ? row.Cells[1] : null);

                if (value != null)
                {
                    menu.Items.Add(MakeItem(Tr("Copy value"), null, () => Copy(value.Text), true));
                }

                ulong? offset = value?.Offset ?? row.Offset;

                if (offset != null)
                {
                    ulong captured = offset.Value;
                    menu.Items.Add(MakeItem(Tr("Copy offset"), null, () => Copy(Format.Offset(captured)), true));
                }
            }

            menu.Items.Add(MakeItem(Tr("Copy all rows"), null, () => Copy(table.CopyAll()), !table.IsEmpty));

            if (node != null && row?.Offset != null)
            {
                ulong row_offset = row.Offset.Value;
                ulong row_size = Math.Max(1, row.Size);
                menu.Items.Add(new Separator());
                menu.Items.Add(MakeItem(Tr("Show in hex editor"), "Icon.Binary", () => node.Owner.ShowInHexEditor(row_offset, row_size), true));
            }

            // Commands of the view only belong to its main list, not to the header fields above it.
            if (node != null && table == node.List && node.HasCommands)
            {
                menu.Items.Add(new Separator());

                foreach (CommandItem command in node.RowCommands.Concat(node.ViewCommands))
                {
                    MenuItem item = MakeItem(command.Label, command.IconKey, () => command.Command.Execute(null), command.Data.Enabled && (!command.IsRowCommand || row != null));
                    item.ToolTip = command.ToolTip;
                    ToolTipService.SetShowOnDisabled(item, true);
                    menu.Items.Add(item);
                }
            }
        }

        // Drag and drop reordering of lists whose view can move rows (currently the import directory).

        private const string ROW_FORMAT = "CoffeeExplorer.TableRow";

        private static bool CanReorder(
            ListView list
        )
        {
            NodeViewModel? node = GetNode(list);
            TableViewModel? table = GetTable(list);

            if (node == null || table == null || table != node.List || table.Rows.Count < 2 || node.IsReadOnly)
            {
                return false;
            }

            // Only the natural order matches the order in the file, a sorted view would move to the wrong place.
            if (list.GetValue(SortProperty) is SortState sort && sort.Column != -1)
            {
                return false;
            }

            return node.Commands.Any(command => command.Data.Id == "imports.move_up" || command.Data.Id == "imports.move_down");
        }

        private static void OnReorderMouseDown(
            object sender,
            MouseButtonEventArgs arguments
        )
        {
            var list = (ListView)sender;
            var state = (ReorderState)list.GetValue(ReorderProperty)!;
            state.Row = null;
            state.Dragging = false;

            if (!CanReorder(list))
            {
                list.AllowDrop = false;
                return;
            }

            if (ItemsControl.ContainerFromElement(list, arguments.OriginalSource as DependencyObject) is ListViewItem container && container.Content is TableRow row)
            {
                state.Row = row;
                state.StartPoint = arguments.GetPosition(list);
                list.AllowDrop = true;
            }
        }

        private static void OnReorderMouseUp(
            object sender,
            MouseButtonEventArgs arguments
        )
        {
            var list = (ListView)sender;
            var state = (ReorderState)list.GetValue(ReorderProperty)!;
            state.Row = null;
            list.AllowDrop = false;
        }

        private static void OnReorderMouseMove(
            object sender,
            MouseEventArgs arguments
        )
        {
            var list = (ListView)sender;
            var state = (ReorderState)list.GetValue(ReorderProperty)!;

            if (state.Row == null || state.Dragging || arguments.LeftButton != MouseButtonState.Pressed)
            {
                return;
            }

            Point position = arguments.GetPosition(list);

            if (Math.Abs(position.X - state.StartPoint.X) < SystemParameters.MinimumHorizontalDragDistance &&
                Math.Abs(position.Y - state.StartPoint.Y) < SystemParameters.MinimumVerticalDragDistance)
            {
                return;
            }

            state.Dragging = true;
            list.AllowDrop = true;

            try
            {
                DragDrop.DoDragDrop(list, new DataObject(ROW_FORMAT, state.Row), DragDropEffects.Move);
            }
            finally
            {
                state.Dragging = false;
                state.Row = null;
                list.AllowDrop = false;
            }
        }

        private static void OnReorderDragOver(
            object sender,
            DragEventArgs arguments
        )
        {
            arguments.Handled = true;
            arguments.Effects = CanReorder((ListView)sender) && arguments.Data.GetDataPresent(ROW_FORMAT) ? DragDropEffects.Move : DragDropEffects.None;
        }

        private static void OnReorderDrop(
            object sender,
            DragEventArgs arguments
        )
        {
            arguments.Handled = true;
            var list = (ListView)sender;

            if (GetNode(list) is not NodeViewModel node || GetTable(list) is not TableViewModel table ||
                !CanReorder(list) || arguments.Data.GetData(ROW_FORMAT) is not TableRow dragged)
            {
                return;
            }

            int count = table.Rows.Count;
            int target;

            if (ItemsControl.ContainerFromElement(list, arguments.OriginalSource as DependencyObject) is ListViewItem container && container.Content is TableRow over)
            {
                // Upper half of the hovered row: take its place, lower half: below it.
                Point position = arguments.GetPosition(container);
                target = over.Index + (position.Y > container.ActualHeight / 2 ? 1 : 0);
            }
            else
            {
                target = arguments.GetPosition(list).Y < list.ActualHeight / 2 ? 0 : count;
            }

            // The dragged row leaves its own position, the indexes behind it shift up by one.
            if (target > dragged.Index)
            {
                --target;
            }

            if (target < 0 || target >= count || target == dragged.Index)
            {
                return;
            }

            CommandAnswer? answer = node.Owner.Execute(new Dictionary<string, object>
            {
                ["command"] = "imports.move",
                ["index"] = dragged.Index,
                ["target"] = target
            });

            // The view was rebuilt, keep the moved descriptor selected.
            if (answer != null && node.Owner.Content is NodeViewModel current && current.List != null)
            {
                TableRow? moved = current.List.Rows.FirstOrDefault(row => row.Index == target);

                if (moved != null)
                {
                    current.List.SelectedRow = moved;
                }
            }
        }

        private static MenuItem MakeItem(
            string header,
            string? icon,
            Action action,
            bool enabled,
            string? gesture = null
        )
        {
            var item = new MenuItem { Header = header, IsEnabled = enabled, InputGestureText = gesture ?? string.Empty };

            if (icon != null)
            {
                var image = new Image();
                image.SetResourceReference(FrameworkElement.StyleProperty, "Icon");
                image.SetResourceReference(Image.SourceProperty, icon);
                item.Icon = image;
            }

            item.Click += (_, _) => action();
            return item;
        }
    }
}