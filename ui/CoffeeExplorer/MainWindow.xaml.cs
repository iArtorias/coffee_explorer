using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using CoffeeExplorer.Services;
using CoffeeExplorer.ViewModels;
using CoffeeExplorer.Views;

namespace CoffeeExplorer
{
    public partial class MainWindow : ThemedWindow
    {
        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(
            IntPtr window
        );

        public MainWindow()
        {
            ViewModel = new MainViewModel { Window = this };
            DataContext = ViewModel;
            InitializeComponent();
            RestorePlacement();
        }

        public MainViewModel ViewModel { get; }

        public void BringToFront()
        {
            if (WindowState == WindowState.Minimized)
            {
                WindowState = WindowState.Normal;
            }

            Activate();
            SetForegroundWindow(new WindowInteropHelper(this).Handle);
        }

        protected override void OnClosing(
            CancelEventArgs arguments
        )
        {
            if (!ViewModel.ConfirmExit())
            {
                arguments.Cancel = true;
                return;
            }

            SavePlacement();
            base.OnClosing(arguments);
        }

        private void RestorePlacement()
        {
            WindowPlacement placement = SettingsStore.Current.Window;
            Width = Math.Max(MinWidth, placement.Width);
            Height = Math.Max(MinHeight, placement.Height);

            // Only restore a position that is still on a screen.
            if (!double.IsNaN(placement.Left) && !double.IsNaN(placement.Top) &&
                 placement.Left >= SystemParameters.VirtualScreenLeft && placement.Top >= SystemParameters.VirtualScreenTop &&
                 placement.Left + 100 <= SystemParameters.VirtualScreenLeft + SystemParameters.VirtualScreenWidth &&
                 placement.Top + 100 <= SystemParameters.VirtualScreenTop + SystemParameters.VirtualScreenHeight)
            {
                WindowStartupLocation = WindowStartupLocation.Manual;
                Left = placement.Left;
                Top = placement.Top;
            }
            else
            {
                WindowStartupLocation = WindowStartupLocation.CenterScreen;
            }

            if (placement.Maximized)
            {
                WindowState = WindowState.Maximized;
            }
        }

        private void SavePlacement()
        {
            WindowPlacement placement = SettingsStore.Current.Window;
            Rect bounds = WindowState == WindowState.Normal ? new Rect(Left, Top, Width, Height) : RestoreBounds;
            placement.Left = bounds.Left;
            placement.Top = bounds.Top;
            placement.Width = bounds.Width;
            placement.Height = bounds.Height;
            placement.Maximized = WindowState == WindowState.Maximized;
        }

        // Drag and drop.

        private void OnDragOver(
            object sender,
            DragEventArgs arguments
        )
        {
            bool files = arguments.Data.GetDataPresent(DataFormats.FileDrop);
            arguments.Effects = files ? DragDropEffects.Copy : DragDropEffects.None;
            DropOverlay.Visibility = files ? Visibility.Visible : Visibility.Collapsed;
            arguments.Handled = true;
        }

        private void OnDragLeave(
            object sender,
            DragEventArgs arguments
        )
        {
            DropOverlay.Visibility = Visibility.Collapsed;
        }

        private void OnDrop(
            object sender,
            DragEventArgs arguments
        )
        {
            DropOverlay.Visibility = Visibility.Collapsed;

            if (arguments.Data.GetData(DataFormats.FileDrop) is not string[] paths)
            {
                return;
            }

            foreach (string path in paths.Where(File.Exists))
            {
                ViewModel.OpenFile(path);
            }

            arguments.Handled = true;
        }

        // Tab context menu.

        private static DocumentViewModel? MenuDocument(
            object sender
        )
        {
            return (sender as FrameworkElement)?.DataContext as DocumentViewModel;
        }

        private void OnCopyPath(
            object sender,
            RoutedEventArgs arguments
        )
        {
            if (MenuDocument(sender) is DocumentViewModel document)
            {
                try
                {
                    Clipboard.SetText(document.Path);
                }
                catch (Exception)
                {
                    // Clipboard locked by another process.
                }
            }
        }

        private void OnOpenFolder(
            object sender,
            RoutedEventArgs arguments
        )
        {
            if (MenuDocument(sender) is DocumentViewModel document && File.Exists(document.Path))
            {
                Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{document.Path}\"") { UseShellExecute = true });
            }
        }
    }
}