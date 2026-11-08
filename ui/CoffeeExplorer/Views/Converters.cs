using System;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;

namespace CoffeeExplorer.Views
{
    public sealed class BoolToVisibilityConverter : IValueConverter
    {
        public bool Invert { get; set; }

        public object Convert(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            bool visible = value is bool flag && flag;
            return visible != Invert ? Visibility.Visible : Visibility.Collapsed;
        }

        public object ConvertBack(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            return value is Visibility visibility && (visibility == Visibility.Visible) != Invert;
        }
    }

    // Visible when a count is above zero, or when it is zero with ZeroVisible set
    public sealed class CountToVisibilityConverter : IValueConverter
    {
        public bool ZeroVisible { get; set; }

        public object Convert(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            bool positive = value is int count && count > 0;
            return positive != ZeroVisible ? Visibility.Visible : Visibility.Collapsed;
        }

        public object ConvertBack(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            throw new NotSupportedException();
        }
    }

    public sealed class CountToBoolConverter : IValueConverter
    {
        public object Convert(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            return value is int count && count > 0;
        }

        public object ConvertBack(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            throw new NotSupportedException();
        }
    }

    public sealed class NullToVisibilityConverter : IValueConverter
    {
        public bool Invert { get; set; }

        public object Convert(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            bool visible = value != null && !(value is string text && text.Length == 0);
            return visible != Invert ? Visibility.Visible : Visibility.Collapsed;
        }

        public object ConvertBack(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            throw new NotSupportedException();
        }
    }

    // True when an enum value equals the parameter name, used for radio style menu items.
    public sealed class EnumEqualsConverter : IValueConverter
    {
        public object Convert(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            return value != null && parameter != null && string.Equals(value.ToString(), parameter.ToString(), StringComparison.Ordinal);
        }

        public object ConvertBack(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            return Binding.DoNothing;
        }
    }

    // 'Image.Source' from an icon resource key that follows theme switches. A converter with
    // 'TryFindResource' would freeze the light or dark image that was current when it ran.
    public static class IconSource
    {
        public static readonly DependencyProperty KeyProperty = DependencyProperty.RegisterAttached(
            "Key", typeof(string), typeof(IconSource), new PropertyMetadata(null, OnKeyChanged));

        public static string? GetKey(
            DependencyObject element
        )
        {
            return (string?)element.GetValue(KeyProperty);
        }

        public static void SetKey(
            DependencyObject element,
            string? value
        )
        {
            element.SetValue(KeyProperty, value);
        }

        private static void OnKeyChanged(
            DependencyObject element,
            DependencyPropertyChangedEventArgs arguments
        )
        {
            if (element is not Image image)
            {
                return;
            }

            if (arguments.NewValue is string key && key.Length > 0)
            {
                image.SetResourceReference(Image.SourceProperty, key);
            }
            else
            {
                image.ClearValue(Image.SourceProperty);
            }
        }
    }

    // Left padding of a tree item from its depth, so the selection highlight spans the whole row like in Visual Studio.
    public sealed class TreeIndentConverter : IValueConverter
    {
        public double Indent { get; set; } = 16;

        public object Convert(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            int depth = 0;

            if (value is DependencyObject item)
            {
                for (DependencyObject? parent = ItemsControl.ItemsControlFromItemContainer(item); parent is TreeViewItem; parent = ItemsControl.ItemsControlFromItemContainer(parent))
                {
                    ++depth;
                }
            }

            return new Thickness(4 + depth * Indent, 0, 4, 0);
        }

        public object ConvertBack(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            throw new NotSupportedException();
        }
    }

    // Font weight of tree headings (file node, Tools group).
    public sealed class BoolToBoldConverter : IValueConverter
    {
        public object Convert(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            return value is bool bold && bold ? FontWeights.SemiBold : FontWeights.Normal;
        }

        public object ConvertBack(
            object value,
            Type target_type,
            object parameter,
            CultureInfo culture
        )
        {
            throw new NotSupportedException();
        }
    }
}