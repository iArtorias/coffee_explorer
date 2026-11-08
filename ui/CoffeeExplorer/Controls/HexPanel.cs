using System;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Services;
using CoffeeExplorer.ViewModels;
using CoffeeExplorer.Views.Dialogs;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.Controls
{
    // Hex view with its toolbar. Go to, find (hex or text), copy, paste write, fill, select all, disassemble.
    // Used by every node with raw data and by the 'Hex Editor' and 'Address Converter' tools.
    public sealed class HexPanel : DockPanel
    {
        private readonly HexView m_View = new();
        private readonly TextBox m_GoTo = new() { Width = 110, VerticalContentAlignment = VerticalAlignment.Center };
        private readonly TextBox m_Find = new() { Width = 180, VerticalContentAlignment = VerticalAlignment.Center };
        private readonly ComboBox m_FindMode = new() { Width = 70, Margin = new Thickness(4, 0, 0, 0) };
        private readonly CheckBox m_MatchCase = new() { Content = Tr("Match case"), Margin = new Thickness(8, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
        private readonly TextBlock m_Title = new() { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 12, 0), FontWeight = FontWeights.SemiBold };
        private readonly TextBlock m_Status = new() { VerticalAlignment = VerticalAlignment.Center };
        private DocumentViewModel? m_Document;

        public static readonly DependencyProperty DocumentProperty = DependencyProperty.Register(
            nameof(Document), typeof(DocumentViewModel), typeof(HexPanel), new PropertyMetadata(null, (d, _) => ((HexPanel)d).OnDocumentChanged()));

        public static readonly DependencyProperty RegionStartProperty = DependencyProperty.Register(
            nameof(RegionStart), typeof(ulong), typeof(HexPanel), new PropertyMetadata(0ul, (d, e) => ((HexPanel)d).m_View.RegionStart = (ulong)e.NewValue));

        public static readonly DependencyProperty RegionLengthProperty = DependencyProperty.Register(
            nameof(RegionLength), typeof(ulong), typeof(HexPanel), new PropertyMetadata(0ul, (d, e) => ((HexPanel)d).m_View.RegionLength = (ulong)e.NewValue));

        public static readonly DependencyProperty TitleProperty = DependencyProperty.Register(
            nameof(Title), typeof(string), typeof(HexPanel), new PropertyMetadata(null, (d, e) => ((HexPanel)d).m_Title.Text = e.NewValue as string ?? string.Empty));

        public static readonly DependencyProperty SelectionProperty = DependencyProperty.Register(
            nameof(Selection), typeof(HexSelectionRequest), typeof(HexPanel), new PropertyMetadata(null, (d, e) => ((HexPanel)d).OnSelectionRequested(e.NewValue as HexSelectionRequest)));

        public HexPanel()
        {
            m_FindMode.Items.Add(Tr("Text"));
            m_FindMode.Items.Add("Hex");
            m_FindMode.SelectedIndex = 0;
            m_GoTo.SetResourceReference(Control.FontFamilyProperty, "Font.Mono");
            m_GoTo.ToolTip = Tr("File offset, hex (add \" d\" for decimal)");
            m_Find.ToolTip = Tr("Text or hex bytes to find (Enter finds the next match)");
            m_Status.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");

            m_GoTo.KeyDown += (_, arguments) => { if (arguments.Key == Key.Enter) { GoTo(); arguments.Handled = true; } };
            m_Find.KeyDown += (_, arguments) => { if (arguments.Key == Key.Enter) { FindNext(); arguments.Handled = true; } };

            var toolbar = new WrapPanel { Margin = new Thickness(8, 4, 8, 4) };
            toolbar.Children.Add(m_Title);
            toolbar.Children.Add(Label(Tr("Go to")));
            toolbar.Children.Add(m_GoTo);
            toolbar.Children.Add(ToolButton("Icon.GoToCurrentLine", Tr("Go to offset (Enter)"), GoTo));
            toolbar.Children.Add(Separator());
            toolbar.Children.Add(Label(Tr("Find")));
            toolbar.Children.Add(m_Find);
            toolbar.Children.Add(m_FindMode);
            toolbar.Children.Add(m_MatchCase);
            toolbar.Children.Add(ToolButton("Icon.FindNext", Tr("Find next (F3)"), FindNext));
            toolbar.Children.Add(Separator());
            toolbar.Children.Add(ToolButton("Icon.Copy", Tr("Copy as hex, or as text in the text column (Ctrl+C)"), () => m_View.Copy()));
            toolbar.Children.Add(ToolButton("Icon.PasteReplace", Tr("Write the clipboard over the bytes at the caret (Ctrl+V)"), () => m_View.PasteWrite()));
            toolbar.Children.Add(ToolButton("Icon.Fill", Tr("Fill a range with a byte pattern"), Fill));
            toolbar.Children.Add(ToolButton("Icon.SelectAll", Tr("Select all (Ctrl+A)"), () => m_View.SelectAll()));
            toolbar.Children.Add(Separator());
            toolbar.Children.Add(ToolButton("Icon.DisassemblyWindow", Tr("Disassemble from the caret"), Disassemble));
            toolbar.Children.Add(ToolButton("Icon.Save", Tr("Save the selection to a file"), SaveSelection));

            var top = new Border { Child = toolbar, BorderThickness = new Thickness(0, 0, 0, 1) };
            top.SetResourceReference(Border.BackgroundProperty, "Brush.Panel.Background");
            top.SetResourceReference(Border.BorderBrushProperty, "Brush.Panel.Border");
            SetDock(top, Dock.Top);
            Children.Add(top);

            var bottom = new Border { Child = m_Status, Padding = new Thickness(8, 3, 8, 3), BorderThickness = new Thickness(0, 1, 0, 0) };
            bottom.SetResourceReference(Border.BackgroundProperty, "Brush.Panel.Background");
            bottom.SetResourceReference(Border.BorderBrushProperty, "Brush.Panel.Border");
            SetDock(bottom, Dock.Bottom);
            Children.Add(bottom);

            Children.Add(m_View);

            m_View.StateChanged += (_, _) => UpdateStatus();
            m_View.Edited += (_, _) => m_Document?.OnBytesEdited();
            m_View.EditorFontSize = SettingsStore.Current.HexFontSize;

            InputBindings.Add(new KeyBinding(new Mvvm.RelayCommand(FindNext), Key.F3, ModifierKeys.None));
            Loaded += (_, _) => Attach();
            Unloaded += (_, _) => Detach();
        }

        public DocumentViewModel? Document
        {
            get => (DocumentViewModel?)GetValue(DocumentProperty);
            set => SetValue(DocumentProperty, value);
        }

        public ulong RegionStart
        {
            get => (ulong)GetValue(RegionStartProperty);
            set => SetValue(RegionStartProperty, value);
        }

        public ulong RegionLength
        {
            get => (ulong)GetValue(RegionLengthProperty);
            set => SetValue(RegionLengthProperty, value);
        }

        public string? Title
        {
            get => (string?)GetValue(TitleProperty);
            set => SetValue(TitleProperty, value);
        }

        public HexSelectionRequest? Selection
        {
            get => (HexSelectionRequest?)GetValue(SelectionProperty);
            set => SetValue(SelectionProperty, value);
        }

        public HexView View => m_View;

        private void OnDocumentChanged()
        {
            Detach();
            m_Document = Document;
            m_View.Source = m_Document?.Document;
            m_View.IsReadOnly = m_Document?.IsReadOnly ?? true;
            Attach();
            UpdateStatus();
        }

        private void Attach()
        {
            if (m_Document != null && IsLoaded)
            {
                m_Document.PropertyChanged -= OnDocumentPropertyChanged;
                m_Document.PropertyChanged += OnDocumentPropertyChanged;
            }

            m_View.EditorFontSize = SettingsStore.Current.HexFontSize;
            m_View.EditorFontFamily = SettingsStore.Current.HexFontFamily;
        }

        private void Detach()
        {
            if (m_Document != null)
            {
                m_Document.PropertyChanged -= OnDocumentPropertyChanged;
            }
        }

        // Undo, redo and structural edits change the bytes under the view.
        private void OnDocumentPropertyChanged(
            object? sender,
            System.ComponentModel.PropertyChangedEventArgs arguments
        )
        {
            if (arguments.PropertyName == nameof(DocumentViewModel.Info))
            {
                m_View.RefreshData();
            }
        }

        private void OnSelectionRequested(
            HexSelectionRequest? request
        )
        {
            if (request == null)
            {
                return;
            }

            // The request can arrive before the view knows its size.
            Dispatcher.BeginInvoke(() => m_View.SetSelection(request.Offset, request.Length), System.Windows.Threading.DispatcherPriority.Loaded);
        }

        private void UpdateStatus()
        {
            ulong begin = m_View.RegionBegin;
            ulong end = m_View.RegionEnd;
            string region = Tr("Range {0} - {1} ({2} bytes)", Format.Offset(begin), Format.Offset(end == 0 ? 0 : end - 1), (end - begin).ToString("N0"));

            if (m_View.HasSelection)
            {
                m_Status.Text = Tr("Offset {0}   Selected {1} - {2} ({3} bytes)", Format.Offset(m_View.CaretOffset), Format.Offset(m_View.SelectionStart),
                                    Format.Offset(m_View.SelectionStart + m_View.SelectionLength - 1), m_View.SelectionLength.ToString("N0")) + "   " + region;
            }
            else
            {
                m_Status.Text = Tr("Offset {0}", Format.Offset(m_View.CaretOffset)) + "   " + region;
            }

            m_Document?.ShowHexFocus(m_View.CaretOffset, m_View.SelectionStart, m_View.SelectionLength);
        }

        private void GoTo()
        {
            if (!Format.TryParseNumber(m_GoTo.Text, out ulong offset))
            {
                m_Status.Text = Tr("\"{0}\" is not an offset", m_GoTo.Text);
                return;
            }

            if (offset < m_View.RegionBegin || offset >= m_View.RegionEnd)
            {
                m_Status.Text = Tr("{0} is outside this range, use the Hex Editor tool for the whole file", Format.Offset(offset));
                return;
            }

            m_View.SetSelection(offset, 0);
            m_View.Focus();
        }

        private void FindNext()
        {
            string text = m_Find.Text;

            if (text.Length == 0)
            {
                m_Find.Focus();
                return;
            }

            byte[] pattern;

            if (m_FindMode.SelectedIndex == 1)
            {
                if (!Format.TryParseHexBytes(text, out pattern) || pattern.Length == 0)
                {
                    m_Status.Text = Tr("Type hex bytes such as 4D 5A 90");
                    return;
                }
            }
            else
            {
                pattern = Encoding.Latin1.GetBytes(text);
            }

            if (!m_View.Find(pattern, m_FindMode.SelectedIndex == 0 && m_MatchCase.IsChecked != true))
            {
                m_Status.Text = Tr("No match found");
            }
        }

        private void Fill()
        {
            if (m_Document == null || m_Document.IsReadOnly)
            {
                return;
            }

            (ulong start, ulong length) = m_View.SelectionOrCaret;
            var dialog = new FillDialog(start, Math.Max(1, length));

            if (dialog.ShowFor(Window.GetWindow(this)))
            {
                if (dialog.Start < m_View.RegionBegin || dialog.Start + dialog.Length > m_View.RegionEnd)
                {
                    MessageService.ShowError(Window.GetWindow(this), Tr("Fill"), Tr("The range is outside the bytes shown here."));
                    return;
                }

                m_View.Fill(dialog.Pattern, dialog.Start, dialog.Length);
            }
        }

        private void Disassemble()
        {
            m_Document?.ShowInDisassembler(m_View.CaretOffset);
        }

        private void SaveSelection()
        {
            if (m_Document == null)
            {
                return;
            }

            (ulong start, ulong length) = m_View.HasSelection ? (m_View.SelectionStart, m_View.SelectionLength) : (m_View.RegionBegin, m_View.RegionEnd - m_View.RegionBegin);

            if (length == 0)
            {
                return;
            }

            string? path = FileDialogs.Save(Tr("Save bytes"), $"{System.IO.Path.GetFileNameWithoutExtension(m_Document.Path)}_{start:X}.bin", Tr("Binary files") + " (*.bin)|*.bin|" + Tr("All files") + " (*.*)|*.*");

            if (path != null)
            {
                m_Document.SaveBytes(start, length, path);
            }
        }

        private static TextBlock Label(
            string text
        )
        {
            return new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0) };
        }

        private static Border Separator()
        {
            var line = new Border { Width = 1, Margin = new Thickness(8, 3, 8, 3) };
            line.SetResourceReference(Border.BackgroundProperty, "Brush.Panel.Border");
            return line;
        }

        private static Button ToolButton(
            string icon,
            string tip,
            Action action
        )
        {
            var image = new Image();
            image.SetResourceReference(StyleProperty, "Icon");
            image.SetResourceReference(Image.SourceProperty, icon);
            var button = new Button { Content = image, ToolTip = tip, Margin = new Thickness(2, 0, 0, 0) };
            button.SetResourceReference(StyleProperty, "Button.Tool");
            button.Click += (_, _) => action();
            return button;
        }
    }
}