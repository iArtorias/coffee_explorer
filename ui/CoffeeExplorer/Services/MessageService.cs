using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using CoffeeExplorer.Views;

namespace CoffeeExplorer.Services
{
    public enum MessageKind
    {
        MESSAGE_INFORMATION,
        MESSAGE_WARNING,
        MESSAGE_ERROR,
        MESSAGE_QUESTION
    }

    public enum SaveChoice
    {
        SAVE_CHOICE_SAVE,
        SAVE_CHOICE_DISCARD,
        SAVE_CHOICE_CANCEL
    }

    // Themed replacement for MessageBox, so prompts follow the light and dark theme.
    public static class MessageService
    {
        private static int Show(
            Window? owner,
            MessageKind kind,
            string title,
            string message,
            string[] buttons,
            int default_button,
            int cancel_button
        )
        {
            var dialog = new DialogWindow { Title = title, MinWidth = 380, MaxWidth = 560 };
            int result = cancel_button;

            string icon = kind switch
            {
                MessageKind.MESSAGE_ERROR => "Icon.StatusError",
                MessageKind.MESSAGE_WARNING => "Icon.StatusWarning",
                MessageKind.MESSAGE_QUESTION => "Icon.StatusHelp",
                _ => "Icon.StatusInformation"
            };

            var image = new Image { Width = 32, Height = 32, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 2, 16, 0) };
            image.SetResourceReference(Image.SourceProperty, icon);

            var heading = new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 6) };
            var body = new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap };
            body.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");

            var text = new StackPanel();
            text.Children.Add(heading);
            text.Children.Add(body);

            var content = new DockPanel { Margin = new Thickness(20, 20, 20, 16) };
            DockPanel.SetDock(image, Dock.Left);
            content.Children.Add(image);
            content.Children.Add(text);

            var buttons_panel = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };

            for (int index = 0; index < buttons.Length; ++index)
            {
                int value = index;
                var button = new Button { Content = buttons[index], Margin = new Thickness(8, 0, 0, 0), IsDefault = index == default_button, IsCancel = index == cancel_button };
                button.Click += (_, _) => { result = value; dialog.DialogResult = value != cancel_button || buttons.Length == 1; };
                buttons_panel.Children.Add(button);
            }

            var footer = new Border { Padding = new Thickness(20, 12, 20, 12), Child = buttons_panel, BorderThickness = new Thickness(0, 1, 0, 0) };
            footer.SetResourceReference(Border.BackgroundProperty, "Brush.Panel.Background");
            footer.SetResourceReference(Border.BorderBrushProperty, "Brush.Panel.Border");

            var root = new DockPanel();
            DockPanel.SetDock(footer, Dock.Bottom);
            root.Children.Add(footer);
            root.Children.Add(content);
            dialog.Content = root;

            if (owner != null && owner.IsLoaded)
            {
                dialog.Owner = owner;
            }

            dialog.ShowDialog();
            return result;
        }

        public static void ShowError(
            Window? owner,
            string title,
            string message
        )
        {
            Show(owner, MessageKind.MESSAGE_ERROR, title, message, new[] { LanguageService.Tr("OK") }, 0, 0);
        }

        public static void ShowInfo(
            Window? owner,
            string title,
            string message
        )
        {
            Show(owner, MessageKind.MESSAGE_INFORMATION, title, message, new[] { LanguageService.Tr("OK") }, 0, 0);
        }

        public static bool Confirm(
            Window? owner,
            string title,
            string message,
            string? accept = null,
            string? decline = null,
            MessageKind kind = MessageKind.MESSAGE_QUESTION
        )
        {
            return Show(owner, kind, title, message, new[] { accept ?? LanguageService.Tr("OK"), decline ?? LanguageService.Tr("Cancel") }, 0, 1) == 0;
        }

        public static SaveChoice AskSave(
            Window? owner,
            string document_name
        )
        {
            int choice = Show(owner, MessageKind.MESSAGE_QUESTION, LanguageService.Tr("Save changes?"), LanguageService.Tr("\"{0}\" has unsaved changes.", document_name),
                              new[] { LanguageService.Tr("Save"), LanguageService.Tr("Don't save"), LanguageService.Tr("Cancel") }, 0, 2);
            return choice == 0 ? SaveChoice.SAVE_CHOICE_SAVE : choice == 1 ? SaveChoice.SAVE_CHOICE_DISCARD : SaveChoice.SAVE_CHOICE_CANCEL;
        }
    }
}