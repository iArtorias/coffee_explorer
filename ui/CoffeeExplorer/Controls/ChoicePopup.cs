using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Services;
using CoffeeExplorer.ViewModels;
using TableCell = CoffeeExplorer.ViewModels.TableCell;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.Controls
{
    // Drop down editor for flags (a check box per bit, radio groups for masked multi bit fields) and enums
    // (a list). The raw value stays editable at the top and follows every click.
    public static class ChoicePopup
    {
        private sealed class Choice
        {
            public ulong Value { get; init; }

            public ulong Mask { get; init; }

            public string Name { get; init; } = string.Empty;
        }

        public static void Show(
            FrameworkElement target,
            TableCell cell,
            ChoiceSetData set
        )
        {
            var choices = set.Items.Select(item => new Choice
            {
                Value = Parse(item.Value),
                Mask = item.Mask != null ? Parse(item.Mask) : 0,
                Name = item.Name
            }).ToList();

            ulong value = cell.NumericValue;
            bool updating = false;

            var popup = new Popup
            {
                PlacementTarget = target,
                Placement = PlacementMode.Bottom,
                StaysOpen = false,
                AllowsTransparency = true,
                Focusable = true
            };

            var value_box = new TextBox { Text = TableViewModel.HexText(value, cell.Size), MinWidth = 120 };
            value_box.SetResourceReference(TextBox.FontFamilyProperty, "Font.Mono");
            var header = new DockPanel { Margin = new Thickness(0, 0, 0, 8) };
            var label = new TextBlock { Text = Tr("Value"), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 8, 0) };
            DockPanel.SetDock(label, Dock.Left);
            header.Children.Add(label);
            header.Children.Add(value_box);

            var list = new StackPanel();
            var toggles = new List<(ToggleButton Box, Choice Choice)>();
            ListBox? enum_list = null;

            void Sync()
            {
                updating = true;
                value_box.Text = TableViewModel.HexText(value, cell.Size);

                foreach ((ToggleButton box, Choice choice) in toggles)
                {
                    ulong mask = choice.Mask != 0 ? choice.Mask : choice.Value;
                    box.IsChecked = mask != 0 ? (value & mask) == choice.Value : value == 0;
                }

                if (enum_list != null)
                {
                    enum_list.SelectedItem = enum_list.Items.Cast<ListBoxItem>().FirstOrDefault(item => ((Choice)item.Tag).Value == value);
                }

                updating = false;
            }

            if (set.Flags)
            {
                foreach (IGrouping<ulong, Choice> group in choices.GroupBy(choice => choice.Mask))
                {
                    if (group.Key != 0)
                    {
                        var caption = new TextBlock { Text = Tr("Mask {0}", group.Key.ToString("X")), Margin = new Thickness(0, 6, 0, 2) };
                        caption.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");
                        list.Children.Add(caption);
                    }

                    string radio_group = Guid.NewGuid().ToString("N");

                    foreach (Choice choice in group)
                    {
                        ToggleButton box = group.Key != 0
                            ? new RadioButton { GroupName = radio_group }
                            : new CheckBox();
                        box.Content = $"{choice.Name}  ({choice.Value:X})";
                        box.Margin = new Thickness(0, 0, 0, 4);
                        Choice captured = choice;

                        box.Click += (_, _) =>
                        {
                            if (updating)
                            {
                                return;
                            }

                            if (captured.Mask != 0)
                            {
                                value = (value & ~captured.Mask) | captured.Value;
                            }
                            else if (box.IsChecked == true)
                            {
                                value |= captured.Value;
                            }
                            else
                            {
                                value &= ~captured.Value;
                            }

                            Sync();
                        };

                        toggles.Add((box, choice));
                        list.Children.Add(box);
                    }
                }
            }
            else
            {
                enum_list = new ListBox { MaxHeight = 360, MinWidth = 260, BorderThickness = new Thickness(0) };

                foreach (Choice choice in choices)
                {
                    enum_list.Items.Add(new ListBoxItem { Content = $"{choice.Value:X}  {choice.Name}", Tag = choice });
                }

                enum_list.SelectionChanged += (_, _) =>
                {
                    if (!updating && enum_list.SelectedItem is ListBoxItem item)
                    {
                        value = ((Choice)item.Tag).Value;
                        Sync();
                    }
                };

                list.Children.Add(enum_list);
            }

            value_box.TextChanged += (_, _) =>
            {
                if (!updating && Format.TryParseNumber(value_box.Text, out ulong typed) && typed <= Format.MaxValue(cell.Size))
                {
                    value = typed;
                    int caret = value_box.CaretIndex;
                    Sync();
                    value_box.CaretIndex = Math.Min(caret, value_box.Text.Length);
                }
            };

            void Apply()
            {
                popup.IsOpen = false;

                if (value == cell.NumericValue)
                {
                    return;
                }

                string? error = cell.CommitValue(value);

                if (error != null)
                {
                    MessageService.ShowError(Window.GetWindow(target), Tr("Invalid value"), error);
                }
            }

            if (enum_list != null)
            {
                enum_list.MouseDoubleClick += (_, _) => Apply();
            }

            var apply = new Button { Content = Tr("Apply"), IsDefault = true, Margin = new Thickness(8, 0, 0, 0) };
            apply.SetResourceReference(FrameworkElement.StyleProperty, "Button.Accent");
            apply.Click += (_, _) => Apply();
            var cancel = new Button { Content = Tr("Cancel"), Margin = new Thickness(8, 0, 0, 0) };
            cancel.Click += (_, _) => popup.IsOpen = false;
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 10, 0, 0) };
            buttons.Children.Add(apply);
            buttons.Children.Add(cancel);

            var body = new DockPanel();
            DockPanel.SetDock(header, Dock.Top);
            DockPanel.SetDock(buttons, Dock.Bottom);
            body.Children.Add(header);
            body.Children.Add(buttons);
            body.Children.Add(new ScrollViewer { Content = list, MaxHeight = 420, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });

            var border = new Border
            {
                Child = body,
                Padding = new Thickness(12),
                BorderThickness = new Thickness(1),
                Margin = new Thickness(0, 2, 8, 8),
                MinWidth = 300
            };

            border.SetResourceReference(Border.BackgroundProperty, "Brush.Menu.Background");
            border.SetResourceReference(Border.BorderBrushProperty, "Brush.Menu.Border");
            border.SetResourceReference(Border.CornerRadiusProperty, "Radius.Popup");
            border.SetResourceReference(TextElement.ForegroundProperty, "Brush.Menu.Foreground");
            border.Effect = new System.Windows.Media.Effects.DropShadowEffect { BlurRadius = 12, ShadowDepth = 2, Opacity = 0.25 };

            border.PreviewKeyDown += (_, arguments) =>
            {
                if (arguments.Key == Key.Escape)
                {
                    popup.IsOpen = false;
                    arguments.Handled = true;
                }
                else if (arguments.Key == Key.Enter)
                {
                    Apply();
                    arguments.Handled = true;
                }
            };

            popup.Child = border;
            Sync();
            popup.IsOpen = true;
            popup.Dispatcher.BeginInvoke(() =>
            {
                if (enum_list != null)
                {
                    (enum_list.SelectedItem as ListBoxItem ?? enum_list.Items.OfType<ListBoxItem>().FirstOrDefault())?.Focus();
                }
                else
                {
                    value_box.Focus();
                    value_box.SelectAll();
                }
            }, System.Windows.Threading.DispatcherPriority.Input);
        }

        private static ulong Parse(
            string text
        )
        {
            return ulong.TryParse(text, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out ulong value) ? value : 0;
        }
    }
}