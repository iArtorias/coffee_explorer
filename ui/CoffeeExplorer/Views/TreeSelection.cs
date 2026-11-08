using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace CoffeeExplorer.Views
{
    // 'TreeView.SelectedItem' is read only, this forwards selection changes to a command of the view model.
    public static class TreeSelection
    {
        public static readonly DependencyProperty CommandProperty = DependencyProperty.RegisterAttached(
            "Command", typeof(ICommand), typeof(TreeSelection), new PropertyMetadata(null, OnCommandChanged));

        public static ICommand? GetCommand(
            DependencyObject element
        )
        {
            return (ICommand?)element.GetValue(CommandProperty);
        }

        public static void SetCommand(
            DependencyObject element,
            ICommand? value
        )
        {
            element.SetValue(CommandProperty, value);
        }

        private static void OnCommandChanged(
            DependencyObject element,
            DependencyPropertyChangedEventArgs arguments
        )
        {
            if (element is not TreeView tree)
            {
                return;
            }

            tree.SelectedItemChanged -= OnSelectedItemChanged;
            tree.PreviewMouseRightButtonDown -= OnRightButtonDown;

            if (arguments.NewValue != null)
            {
                tree.SelectedItemChanged += OnSelectedItemChanged;
                tree.PreviewMouseRightButtonDown += OnRightButtonDown;
            }
        }

        // Context menus act on the selected item, so a right click selects first like in Explorer.
        private static void OnRightButtonDown(
            object sender,
            MouseButtonEventArgs arguments
        )
        {
            for (DependencyObject? element = arguments.OriginalSource as DependencyObject; element != null; element = element is Visual ? VisualTreeHelper.GetParent(element) : LogicalTreeHelper.GetParent(element))
            {
                if (element is TreeViewItem item)
                {
                    item.IsSelected = true;
                    item.Focus();
                    return;
                }
            }
        }

        private static void OnSelectedItemChanged(
            object sender,
            RoutedPropertyChangedEventArgs<object> arguments
        )
        {
            ICommand? command = GetCommand((TreeView)sender);

            if (command != null && arguments.NewValue != null && command.CanExecute(arguments.NewValue))
            {
                command.Execute(arguments.NewValue);
            }
        }

        // Keeps the selected item visible when the selection comes from the view model.
        public static readonly DependencyProperty BringIntoViewProperty = DependencyProperty.RegisterAttached(
            "BringIntoView", typeof(bool), typeof(TreeSelection), new PropertyMetadata(false, OnBringIntoViewChanged));

        public static bool GetBringIntoView(
            DependencyObject element
        )
        {
            return (bool)element.GetValue(BringIntoViewProperty);
        }

        public static void SetBringIntoView(
            DependencyObject element,
            bool value
        )
        {
            element.SetValue(BringIntoViewProperty, value);
        }

        private static void OnBringIntoViewChanged(
            DependencyObject element,
            DependencyPropertyChangedEventArgs arguments
        )
        {
            if (element is TreeViewItem item && arguments.NewValue is true)
            {
                item.Selected += (sender, selected) =>
                {
                    if (ReferenceEquals(selected.OriginalSource, sender))
                    {
                        ((TreeViewItem)sender).BringIntoView();
                    }
                };
            }
        }
    }
}