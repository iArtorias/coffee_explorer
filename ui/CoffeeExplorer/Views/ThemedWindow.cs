using System;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using CoffeeExplorer.Services;

namespace CoffeeExplorer.Views
{
    // Base of every window. Theme brushes, UI font and device pixel snapping.
    public class ThemedWindow : Window
    {
        public ThemedWindow()
        {
            SetResourceReference(BackgroundProperty, "Brush.Window.Background");
            SetResourceReference(ForegroundProperty, "Brush.Text.Foreground");
            SetResourceReference(FontFamilyProperty, "Font.Ui");
            SetResourceReference(FontSizeProperty, "Font.Size");
            UseLayoutRounding = true;
            SnapsToDevicePixels = true;
            TextOptions.SetTextFormattingMode(this, TextFormattingMode.Ideal);
        }

        // The handle exists from here on, the title bar gets the theme before the window is first drawn.
        protected override void OnSourceInitialized(
            EventArgs arguments
        )
        {
            base.OnSourceInitialized(arguments);
            ThemeService.ApplyTitleBar(this);
        }
    }

    // Modal dialog. Centered on the owner, fixed size, Escape closes.
    public class DialogWindow : ThemedWindow
    {
        public DialogWindow()
        {
            WindowStartupLocation = WindowStartupLocation.CenterOwner;
            ResizeMode = ResizeMode.NoResize;
            ShowInTaskbar = false;
            SizeToContent = SizeToContent.WidthAndHeight;

            Window? main = Application.Current?.MainWindow;

            if (main != null && main != this && main.IsLoaded)
            {
                Owner = main;
            }
        }

        protected override void OnPreviewKeyDown(
            KeyEventArgs arguments
        )
        {
            base.OnPreviewKeyDown(arguments);

            if (arguments.Key == Key.Escape && !arguments.Handled)
            {
                DialogResult = false;
                arguments.Handled = true;
            }
        }

        // Shows the dialog over the given owner (or the main window) and returns true when confirmed.
        public bool ShowFor(
            Window? owner
        )
        {
            if (owner != null && owner != this)
            {
                Owner = owner;
            }

            return ShowDialog() == true;
        }
    }
}