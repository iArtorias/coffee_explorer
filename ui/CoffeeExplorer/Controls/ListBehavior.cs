using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace CoffeeExplorer.Controls
{
    // Small ListView helpers for templates.
    public static class ListBehavior
    {
        public static readonly DependencyProperty DoubleClickCommandProperty = DependencyProperty.RegisterAttached(
            "DoubleClickCommand", typeof(ICommand), typeof(ListBehavior), new PropertyMetadata(null, OnDoubleClickCommandChanged));

        public static ICommand? GetDoubleClickCommand(
            DependencyObject element
        )
        {
            return (ICommand?)element.GetValue(DoubleClickCommandProperty);
        }

        public static void SetDoubleClickCommand(
            DependencyObject element,
            ICommand? value
        )
        {
            element.SetValue(DoubleClickCommandProperty, value);
        }

        private static void OnDoubleClickCommandChanged(
            DependencyObject element,
            DependencyPropertyChangedEventArgs arguments
        )
        {
            if (element is not ListView list)
            {
                return;
            }

            list.MouseDoubleClick -= OnMouseDoubleClick;

            if (arguments.NewValue != null)
            {
                list.MouseDoubleClick += OnMouseDoubleClick;
            }
        }

        private static void OnMouseDoubleClick(
            object sender,
            MouseButtonEventArgs arguments
        )
        {
            if (arguments.ChangedButton != MouseButton.Left || sender is not ListView list || GetDoubleClickCommand(list) is not ICommand command)
            {
                return;
            }

            // Rows only, not the column headers or the empty space below the last row.
            for (DependencyObject? element = arguments.OriginalSource as DependencyObject; element != null && element != list;
                  element = element is Visual ? VisualTreeHelper.GetParent(element) : LogicalTreeHelper.GetParent(element))
            {
                if (element is ListViewItem)
                {
                    if (command.CanExecute(null))
                    {
                        command.Execute(null);
                    }

                    arguments.Handled = true;
                    return;
                }
            }
        }

        public static readonly DependencyProperty ScrollSelectionIntoViewProperty = DependencyProperty.RegisterAttached(
            "ScrollSelectionIntoView", typeof(bool), typeof(ListBehavior), new PropertyMetadata(false, OnScrollSelectionIntoViewChanged));

        public static readonly DependencyProperty CollapsibleColumnProperty = DependencyProperty.RegisterAttached(
            "CollapsibleColumn", typeof(int), typeof(ListBehavior), new PropertyMetadata(-1, OnColumnChanged));

        public static readonly DependencyProperty ColumnVisibleProperty = DependencyProperty.RegisterAttached(
            "ColumnVisible", typeof(bool), typeof(ListBehavior), new PropertyMetadata(true, OnColumnChanged));

        // Width a hidden column had, restored when it is shown again.
        private static readonly ConditionalWeakTable<GridViewColumn, object> s_Widths = new();

        public static bool GetScrollSelectionIntoView(
            DependencyObject element
        )
        {
            return (bool)element.GetValue(ScrollSelectionIntoViewProperty);
        }

        public static void SetScrollSelectionIntoView(
            DependencyObject element,
            bool value
        )
        {
            element.SetValue(ScrollSelectionIntoViewProperty, value);
        }

        public static int GetCollapsibleColumn(
            DependencyObject element
        )
        {
            return (int)element.GetValue(CollapsibleColumnProperty);
        }

        public static void SetCollapsibleColumn(
            DependencyObject element,
            int value
        )
        {
            element.SetValue(CollapsibleColumnProperty, value);
        }

        public static bool GetColumnVisible(
            DependencyObject element
        )
        {
            return (bool)element.GetValue(ColumnVisibleProperty);
        }

        public static void SetColumnVisible(
            DependencyObject element,
            bool value
        )
        {
            element.SetValue(ColumnVisibleProperty, value);
        }

        private static void OnScrollSelectionIntoViewChanged(
            DependencyObject element,
            DependencyPropertyChangedEventArgs arguments
        )
        {
            if (element is not ListView list)
            {
                return;
            }

            list.SelectionChanged -= OnSelectionChanged;

            if ((bool)arguments.NewValue)
            {
                list.SelectionChanged += OnSelectionChanged;
            }
        }

        private static void OnSelectionChanged(
            object sender,
            SelectionChangedEventArgs arguments
        )
        {
            var list = (ListView)sender;

            // After the new items are laid out (Follow replaces the whole listing).
            list.Dispatcher.BeginInvoke(() =>
            {
                if (list.SelectedItem != null)
                {
                    list.ScrollIntoView(list.SelectedItem);
                }
            }, DispatcherPriority.Background);
        }

        private static void OnColumnChanged(
            DependencyObject element,
            DependencyPropertyChangedEventArgs arguments
        )
        {
            if (element is not ListView list)
            {
                return;
            }

            // The template sets View after the attached properties, apply once the list is loaded too.
            Apply(list);

            if (!list.IsLoaded)
            {
                list.Loaded -= OnLoaded;
                list.Loaded += OnLoaded;
            }
        }

        private static void OnLoaded(
            object sender,
            RoutedEventArgs arguments
        )
        {
            var list = (ListView)sender;
            list.Loaded -= OnLoaded;
            Apply(list);
        }

        private static void Apply(
            ListView list
        )
        {
            int index = GetCollapsibleColumn(list);

            if (list.View is not GridView view || index < 0 || index >= view.Columns.Count)
            {
                return;
            }

            GridViewColumn column = view.Columns[index];

            if (GetColumnVisible(list))
            {
                if (s_Widths.TryGetValue(column, out object? width))
                {
                    column.Width = (double)width;
                    s_Widths.Remove(column);
                }
            }
            else if (!s_Widths.TryGetValue(column, out _))
            {
                s_Widths.Add(column, column.Width);
                column.Width = 0;
            }
        }
    }
}