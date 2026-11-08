using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using CoffeeExplorer.Services;

namespace CoffeeExplorer.Views.Dialogs
{
    // Shared dialog layout. Body with labelled fields, an error line and a footer with verb buttons.
    // Every Coffee Explorer dialog uses it so spacing, wording and keyboard behaviour stay consistent.
    public abstract class FormDialog : DialogWindow
    {
        private readonly StackPanel m_Body = new() { Margin = new Thickness(20, 18, 20, 8) };
        private readonly TextBlock m_Error = new() { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(20, 0, 20, 10), Visibility = Visibility.Collapsed };
        private readonly StackPanel m_Buttons = new() { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
        private readonly DockPanel m_Footer = new();

        protected Button AcceptButton { get; }

        protected FormDialog(
            string title,
            string accept_text,
            double width = 440
        )
        {
            Title = title;
            Width = width;
            SizeToContent = SizeToContent.Height;
            m_Error.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Error");

            AcceptButton = new Button { Content = accept_text, IsDefault = true, Margin = new Thickness(8, 0, 0, 0) };
            AcceptButton.Click += (_, _) => TryAccept();
            var cancel = new Button { Content = LanguageService.Tr("Cancel"), IsCancel = true, Margin = new Thickness(8, 0, 0, 0) };

            m_Buttons.Children.Add(AcceptButton);
            m_Buttons.Children.Add(cancel);
            DockPanel.SetDock(m_Buttons, Dock.Right);
            m_Footer.Children.Add(m_Buttons);

            var footer = new Border { Padding = new Thickness(20, 12, 20, 12), Child = m_Footer, BorderThickness = new Thickness(0, 1, 0, 0) };
            footer.SetResourceReference(Border.BackgroundProperty, "Brush.Panel.Background");
            footer.SetResourceReference(Border.BorderBrushProperty, "Brush.Panel.Border");

            var root = new DockPanel();
            DockPanel.SetDock(footer, Dock.Bottom);
            root.Children.Add(footer);
            DockPanel.SetDock(m_Error, Dock.Bottom);
            root.Children.Add(m_Error);
            root.Children.Add(m_Body);
            Content = root;

            Loaded += (_, _) => FocusFirstInput();
        }

        // Accept runs Validate, which returns an error message or null.
        protected abstract string? Validate();

        protected void TryAccept()
        {
            string? error = Validate();

            if (error != null)
            {
                ShowError(error);
                return;
            }

            DialogResult = true;
        }

        protected void ShowError(
            string? error
        )
        {
            m_Error.Text = error ?? string.Empty;
            m_Error.Visibility = string.IsNullOrEmpty(error) ? Visibility.Collapsed : Visibility.Visible;
        }

        protected virtual void FocusFirstInput()
        {
            foreach (UIElement child in m_Body.Children)
            {
                if (FindInput(child) is Control control)
                {
                    control.Focus();

                    if (control is TextBox box)
                    {
                        box.SelectAll();
                    }

                    return;
                }
            }
        }

        private static Control? FindInput(
            DependencyObject element
        )
        {
            if (element is TextBox || element is ComboBox || element is ListBox)
            {
                return (Control)element;
            }

            int count = VisualTreeHelper.GetChildrenCount(element);

            for (int index = 0; index < count; ++index)
            {
                if (FindInput(VisualTreeHelper.GetChild(element, index)) is Control found)
                {
                    return found;
                }
            }

            if (element is Panel panel)
            {
                foreach (UIElement child in panel.Children)
                {
                    if (FindInput(child) is Control nested)
                    {
                        return nested;
                    }
                }
            }

            if (element is Decorator decorator && decorator.Child != null)
            {
                return FindInput(decorator.Child);
            }

            return null;
        }

        // Builders.

        protected T Add<T>(
            T element
        ) where T : UIElement
        {
            m_Body.Children.Add(element);
            return element;
        }

        protected void AddFooterLeft(
            UIElement element
        )
        {
            m_Footer.Children.Insert(0, element);
        }

        protected TextBlock AddText(
            string text,
            bool muted = true
        )
        {
            var block = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 10) };

            if (muted)
            {
                block.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");
            }

            return Add(block);
        }

        protected TextBlock AddSection(
            string text
        )
        {
            return Add(new TextBlock { Text = text, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 6, 0, 6) });
        }

        protected TextBox AddTextBox(
            string label,
            string value,
            bool mono = false
        )
        {
            var box = new TextBox { Text = value };

            if (mono)
            {
                box.FontFamily = new FontFamily("Cascadia Mono, Consolas");
            }

            AddField(label, box);
            return box;
        }

        protected void AddField(
            string label,
            UIElement input
        )
        {
            var panel = new StackPanel { Margin = new Thickness(0, 0, 0, 12) };
            var text = new Label { Content = label, Target = input, Padding = new Thickness(0, 0, 0, 4) };
            text.SetResourceReference(Label.ForegroundProperty, "Brush.Text.Foreground");
            panel.Children.Add(text);
            panel.Children.Add(input);
            Add(panel);
        }

        protected List<RadioButton> AddRadioRow(
            string? label,
            params string[] options
        )
        {
            var row = new WrapPanel { Margin = new Thickness(0, 0, 0, 12) };
            var buttons = new List<RadioButton>();
            string group = Guid.NewGuid().ToString("N");

            foreach (string option in options)
            {
                var button = new RadioButton { Content = option, GroupName = group };
                buttons.Add(button);
                row.Children.Add(button);
            }

            if (buttons.Count > 0)
            {
                buttons[0].IsChecked = true;
            }

            if (label != null)
            {
                var panel = new StackPanel { Margin = new Thickness(0, 0, 0, 0) };
                panel.Children.Add(new TextBlock { Text = label, Margin = new Thickness(0, 0, 0, 4) });
                panel.Children.Add(row);
                Add(panel);
            }
            else
            {
                Add(row);
            }

            return buttons;
        }

        protected CheckBox AddCheckBox(
            string text,
            bool value
        )
        {
            return Add(new CheckBox { Content = text, IsChecked = value, Margin = new Thickness(0, 0, 0, 10) });
        }

        protected static Grid MakeRow(
            params (UIElement Element, GridLength Width)[] cells
        )
        {
            var grid = new Grid();

            for (int index = 0; index < cells.Length; ++index)
            {
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = cells[index].Width });
                Grid.SetColumn(cells[index].Element, index);
                grid.Children.Add(cells[index].Element);
            }

            return grid;
        }

        protected static Button MakeBrowseButton(
            RoutedEventHandler click
        )
        {
            var button = new Button { Content = LanguageService.Tr("Browse..."), MinWidth = 0, Margin = new Thickness(6, 0, 0, 0) };
            button.Click += click;
            return button;
        }

        protected static Image MakeIcon(
            string key,
            double size = 16
        )
        {
            var image = new Image { Width = size, Height = size };
            image.SetResourceReference(Image.SourceProperty, key);
            return image;
        }

        protected static bool TryParseNumber(
            string text,
            out ulong value
        )
        {
            return Format.TryParseNumber(text, out value);
        }
    }
}