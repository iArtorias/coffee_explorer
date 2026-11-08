using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using CoffeeExplorer.Services;
using CoffeeExplorer.ViewModels;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.Controls
{
    // One grid cell. The value, an optional muted meaning and, for flags and enums, a drop down button.
    // Double click or F2 edits hex and text values in place, flags and enums open the choice popup.
    // Replaces the 'Click here' links and the two different flag and enum dialogs of CFF Explorer.
    public sealed class ValueCell : Border
    {
        private readonly DockPanel m_Panel = new() { LastChildFill = true };
        private readonly TextBlock m_Text = new() { VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
        private readonly TextBlock m_Meaning = new() { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(8, 0, 0, 0), TextTrimming = TextTrimming.CharacterEllipsis };
        private readonly Button m_Choices;
        private TextBox? m_Editor;
        private bool m_Committing;

        // Muted meaning, in the selection text color while its row is the active selection (high contrast themes invert
        // the selected row, a muted gray would disappear on it).
        private static readonly Style MEANING_STYLE = CreateMeaningStyle();

        private static Style CreateMeaningStyle()
        {
            var row = new RelativeSource(RelativeSourceMode.FindAncestor, typeof(ListViewItem), 1);
            var style = new Style(typeof(TextBlock));
            style.Setters.Add(new Setter(TextBlock.ForegroundProperty, new DynamicResourceExtension("Brush.Text.Muted")));

            var selected = new MultiDataTrigger();
            selected.Conditions.Add(new Condition(new Binding(nameof(ListViewItem.IsSelected)) { RelativeSource = row }, true));
            selected.Conditions.Add(new Condition(new Binding { Path = new PropertyPath(Selector.IsSelectionActiveProperty), RelativeSource = row }, true));
            selected.Setters.Add(new Setter(TextBlock.ForegroundProperty, new DynamicResourceExtension("Brush.Editor.SelectionForeground")));
            selected.Setters.Add(new Setter(UIElement.OpacityProperty, 0.8));
            style.Triggers.Add(selected);
            style.Seal();
            return style;
        }

        public ValueCell()
        {
            Background = Brushes.Transparent;
            Padding = new Thickness(0);
            MinHeight = 18;
            m_Meaning.Style = MEANING_STYLE;
            m_Text.SetBinding(TextBlock.TextProperty, new Binding(nameof(TableCell.Text)));
            m_Meaning.SetBinding(TextBlock.TextProperty, new Binding(nameof(TableCell.Meaning)));

            var chevron = new Path
            {
                Data = Geometry.Parse("M0,0 L4,4 L8,0"),
                StrokeThickness = 1.2,
                Width = 8,
                Height = 5,
                Stretch = Stretch.None,
                VerticalAlignment = VerticalAlignment.Center
            };
            chevron.SetResourceReference(Shape.StrokeProperty, "Brush.Text.Foreground");

            m_Choices = new Button
            {
                Content = chevron,
                Padding = new Thickness(4, 2, 4, 2),
                MinWidth = 0,
                MinHeight = 0,
                Margin = new Thickness(6, 0, 0, 0),
                Focusable = false,
                ToolTip = Tr("Edit the flags (F4)"),
                Visibility = Visibility.Collapsed,
                VerticalAlignment = VerticalAlignment.Center
            };
            m_Choices.SetResourceReference(StyleProperty, "Button.Tool");
            m_Choices.Click += (_, _) => OpenChoices();

            DockPanel.SetDock(m_Choices, Dock.Right);
            m_Panel.Children.Add(m_Choices);
            m_Panel.Children.Add(m_Text);
            m_Panel.Children.Add(m_Meaning);
            Child = m_Panel;

            DataContextChanged += OnDataContextChanged;
            MouseLeftButtonDown += OnMouseDown;

            // The tooltip is made when it opens. Large tables have many cells, and a value cut off by the column shows
            // in full there. A placeholder keeps 'ToolTipOpening' coming, the handler replaces it or cancels.
            ToolTip = TOOLTIP_PLACEHOLDER;
            ToolTipOpening += OnToolTipOpening;
        }

        private static readonly object TOOLTIP_PLACEHOLDER = string.Empty;

        // Long tooltips (Detect It Easy lines of File info, undecorated names, signature text) wrap at this width.
        private const double TOOLTIP_MAX_WIDTH = 640;

        private TableCell? Cell => DataContext as TableCell;

        private void OnDataContextChanged(
            object sender,
            DependencyPropertyChangedEventArgs arguments
        )
        {
            if (arguments.OldValue is TableCell old)
            {
                old.PropertyChanged -= OnCellPropertyChanged;
            }

            if (arguments.NewValue is TableCell cell)
            {
                cell.PropertyChanged += OnCellPropertyChanged;
            }

            Update();
        }

        // Cells whose text or tooltip changes while shown (File info gets the Detect It Easy lines, hashes arrive later).
        private void OnCellPropertyChanged(
            object? sender,
            System.ComponentModel.PropertyChangedEventArgs arguments
        )
        {
            if ((arguments.PropertyName == nameof(TableCell.ToolTip) || arguments.PropertyName == nameof(TableCell.Text)) && ToolTip is System.Windows.Controls.ToolTip { IsOpen: true } open)
            {
                open.Content = MakeToolTipContent();
            }
        }

        public bool IsEditing => m_Editor != null;

        private void Update()
        {
            TableCell? cell = Cell;
            CancelEdit();

            if (cell == null)
            {
                return;
            }

            if (cell.IsMono)
            {
                m_Text.SetResourceReference(TextBlock.FontFamilyProperty, "Font.Mono");
            }
            else
            {
                m_Text.ClearValue(TextBlock.FontFamilyProperty);
            }

            m_Meaning.Visibility = cell.HasMeaning ? Visibility.Visible : Visibility.Collapsed;
            m_Choices.Visibility = cell.HasChoices && cell.IsEditable ? Visibility.Visible : Visibility.Collapsed;
            m_Choices.ToolTip = cell.Editor == CellEditor.EDITOR_FLAGS ? Tr("Edit the flags (F4)") : Tr("Choose a value (F4)");

            if (ToolTip is not System.Windows.Controls.ToolTip)
            {
                ToolTip = TOOLTIP_PLACEHOLDER;
            }
        }

        private void OnToolTipOpening(
            object sender,
            ToolTipEventArgs arguments
        )
        {
            UIElement? content = MakeToolTipContent();

            if (content == null)
            {
                // Nothing to say. No tooltip, the placeholder stays for the next time.
                arguments.Handled = true;
                return;
            }

            ToolTip = new System.Windows.Controls.ToolTip { Content = content };
        }

        // The cell's own tooltip, the full value when the column cuts it off, and the edit hint, wrapped.
        private UIElement? MakeToolTipContent()
        {
            TableCell? cell = Cell;

            if (cell == null)
            {
                return null;
            }

            var lines = new List<string>();
            string? tip = cell.ToolTip;

            // A cell with a tooltip of its own says it there already.
            if (!cell.HasCustomToolTip && IsTrimmed(m_Text) && !string.IsNullOrEmpty(cell.Text))
            {
                lines.Add(cell.Text);
            }

            if (cell.HasMeaning && IsTrimmed(m_Meaning) && (tip == null || !tip.Contains(cell.Meaning!, StringComparison.Ordinal)))
            {
                lines.Add(cell.Meaning!);
            }

            if (!string.IsNullOrEmpty(tip))
            {
                lines.Add(tip);
            }

            if (cell.IsEditable)
            {
                lines.Add(cell.HasChoices ? Tr("Double click or F4 to change") : Tr("Double click or F2 to edit"));
            }

            if (lines.Count == 0)
            {
                return null;
            }

            return new TextBlock { Text = string.Join("\n", lines), TextWrapping = TextWrapping.Wrap, MaxWidth = TOOLTIP_MAX_WIDTH };
        }

        // Whether the ellipsis cut the text. Its full width is larger than what the cell gives it.
        private static bool IsTrimmed(
            TextBlock text
        )
        {
            if (text.Visibility != Visibility.Visible || string.IsNullOrEmpty(text.Text) || text.ActualWidth <= 0)
            {
                return false;
            }

            var formatted = new FormattedText(text.Text, System.Globalization.CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                                               new Typeface(text.FontFamily, text.FontStyle, text.FontWeight, text.FontStretch), text.FontSize, Brushes.Black,
                                               VisualTreeHelper.GetDpi(text).PixelsPerDip);
            return formatted.WidthIncludingTrailingWhitespace > text.ActualWidth + 0.5;
        }

        private void OnMouseDown(
            object sender,
            MouseButtonEventArgs arguments
        )
        {
            TableCell? cell = Cell;
            cell?.Host?.ReportFocus(cell, null);

            if (arguments.ClickCount == 2 && cell != null && cell.IsEditable)
            {
                if (cell.HasChoices)
                {
                    OpenChoices();
                }
                else
                {
                    BeginEdit();
                }

                arguments.Handled = true;
            }
        }

        public bool CanEdit => Cell?.IsEditable == true;

        // F2, text edit, F4, choices (falls back to the text edit for hex values).
        public void BeginEdit(
            bool prefer_choices = false
        )
        {
            TableCell? cell = Cell;

            if (cell == null || !cell.IsEditable || m_Editor != null)
            {
                return;
            }

            if (cell.HasChoices && prefer_choices)
            {
                OpenChoices();
                return;
            }

            m_Editor = new TextBox
            {
                Text = cell.Text,
                MinWidth = 80,
                Padding = new Thickness(2, 0, 2, 0),
                VerticalContentAlignment = VerticalAlignment.Center
            };

            if (cell.IsMono)
            {
                m_Editor.SetResourceReference(TextBox.FontFamilyProperty, "Font.Mono");
            }

            if (cell.Editor == CellEditor.EDITOR_TEXT)
            {
                m_Editor.MaxLength = (int)cell.Size;
            }

            m_Editor.PreviewKeyDown += OnEditorKeyDown;
            m_Editor.LostKeyboardFocus += (_, _) => Commit(false);
            m_Text.Visibility = Visibility.Collapsed;
            m_Panel.Children.Add(m_Editor);
            m_Editor.Loaded += (_, _) => { m_Editor?.Focus(); m_Editor?.SelectAll(); };
        }

        private void OnEditorKeyDown(
            object sender,
            KeyEventArgs arguments
        )
        {
            if (arguments.Key == Key.Enter)
            {
                Commit(true);
                arguments.Handled = true;
            }
            else if (arguments.Key == Key.Escape)
            {
                CancelEdit();
                arguments.Handled = true;
            }
        }

        private void Commit(
            bool keep_open_on_error
        )
        {
            if (m_Editor == null || m_Committing)
            {
                return;
            }

            TableCell? cell = Cell;
            string text = m_Editor.Text;

            if (cell == null || text == cell.Text)
            {
                CancelEdit();
                return;
            }

            m_Committing = true;
            string? error;

            try
            {
                error = cell.Commit(text);
            }
            finally
            {
                m_Committing = false;
            }

            if (error == null)
            {
                CancelEdit();
                return;
            }

            if (keep_open_on_error)
            {
                MessageService.ShowError(Window.GetWindow(this), Tr("Invalid value"), error);
                m_Editor?.Focus();
                m_Editor?.SelectAll();
            }
            else
            {
                CancelEdit();
                MessageService.ShowError(Window.GetWindow(this), Tr("Invalid value"), error);
            }
        }

        public void CancelEdit()
        {
            if (m_Editor == null)
            {
                return;
            }

            TextBox editor = m_Editor;
            m_Editor = null;
            m_Panel.Children.Remove(editor);
            m_Text.Visibility = Visibility.Visible;
        }

        public void OpenChoices()
        {
            TableCell? cell = Cell;

            if (cell == null || !cell.IsEditable || !cell.HasChoices || cell.Host == null)
            {
                return;
            }

            var set = cell.Host.FindChoices(cell.Choices);

            if (set == null)
            {
                BeginEdit();
                return;
            }

            ChoicePopup.Show(this, cell, set);
        }
    }
}