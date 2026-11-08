using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Media;
using CoffeeExplorer.Controls;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Services;
using CoffeeExplorer.ViewModels;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.Views
{
    // Content of a structure node, built from the parts the 'NodeViewModel' has:
    //
    // header (title and view commands), warnings, info text, field grid, list grid, detail of the selected row,
    // hex panel, and for resources a preview tab next to the raw data.
    //
    // Field grids stretch, list grids take the remaining height, the detail sits under a splitter.
    public sealed class NodeView : ContentControl
    {
        public NodeView()
        {
            Focusable = false;

            // Content of a tab inherits from the TabItem (logical parent), set the text color here
            SetResourceReference(ForegroundProperty, "Brush.Text.Foreground");
            DataContextChanged += (_, _) => Build();
        }

        private void Build()
        {
            if (DataContext is not NodeViewModel node)
            {
                Content = null;
                return;
            }

            var grid = new Grid();
            var rows = new List<(UIElement Element, GridLength Height)>();

            if (node.Depth == 0)
            {
                rows.Add((MakeHeader(node.Title, node.ViewCommands, true), GridLength.Auto));
            }
            else if (!string.IsNullOrEmpty(node.Title))
            {
                rows.Add((MakeHeader(node.Title, new List<CommandItem>(), false), GridLength.Auto));
            }

            if (node.HasWarnings)
            {
                var warning = new Border { Child = new TextBlock { Text = node.WarningText, TextWrapping = TextWrapping.Wrap }, Margin = new Thickness(8, 8, 8, 0) };
                warning.SetResourceReference(StyleProperty, "InfoBar");
                ((TextBlock)warning.Child).SetResourceReference(TextBlock.ForegroundProperty, "Brush.Warning.Foreground");
                rows.Add((warning, GridLength.Auto));
            }

            if (node.HasInfo)
            {
                var info = new TextBox
                {
                    Text = node.Info,
                    IsReadOnly = true,
                    TextWrapping = TextWrapping.Wrap,
                    BorderThickness = new Thickness(0),
                    Background = Brushes.Transparent,
                    Padding = new Thickness(10, 8, 10, 8),
                    MaxHeight = 160
                };
                info.SetResourceReference(ForegroundProperty, "Brush.Text.Foreground");
                rows.Add((info, GridLength.Auto));
            }

            if (node.Layout == "overview")
            {
                rows.Add((MakeOverview(node), new GridLength(1, GridUnitType.Star)));
                Assemble(grid, rows);
                Content = grid;
                return;
            }

            bool has_lower = node.HasList || node.HasHex || node.HasPreviewTabs;

            if (node.HasFields)
            {
                ListView fields = MakeGrid(node.Fields!, node);

                if (has_lower)
                {
                    // Header fields above a list or hex region show all their rows when the pane is tall enough and
                    // never take more than about half of the pane from the part below.
                    double share = node.HasList ? 0.5 : 0.6;
                    fields.MaxHeight = 360;
                    grid.SizeChanged += (_, size) =>
                    {
                        if (size.HeightChanged && size.NewSize.Height > 0)
                        {
                            fields.MaxHeight = Math.Max(160, Math.Floor(size.NewSize.Height * share));
                        }
                    };
                    rows.Add((fields, GridLength.Auto));
                }
                else
                {
                    rows.Add((fields, new GridLength(1, GridUnitType.Star)));
                }
            }

            if (node.HasList)
            {
                if (node.HasFields)
                {
                    rows.Add((MakeSectionLabel(node.List!.Rows.Count == 1 ? Tr("1 entry") : Tr("{0} entries", node.List!.Rows.Count.ToString("N0"))), GridLength.Auto));
                }

                ListView list = MakeGrid(node.List!, node);

                if (node.List!.IsEmpty)
                {
                    rows.Add((MakeEmpty(Tr("The list is empty")), new GridLength(1, GridUnitType.Star)));
                }
                else
                {
                    rows.Add((list, new GridLength(node.HasDetailPart ? 1.2 : 1, GridUnitType.Star)));
                }

                if (node.HasDetailPart)
                {
                    rows.Add((MakeSplitter(), new GridLength(5)));
                    var detail = new ContentControl { Focusable = false };
                    detail.SetBinding(ContentProperty, new Binding(nameof(NodeViewModel.Detail)) { Source = node });
                    var holder = new Grid();
                    holder.Children.Add(MakeEmpty(Tr("Select a row to see its details")));
                    holder.Children.Add(detail);
                    rows.Add((holder, new GridLength(1, GridUnitType.Star)));
                }
            }

            if (node.HasPreviewTabs)
            {
                rows.Add((MakePreviewTabs(node), new GridLength(1.4, GridUnitType.Star)));
            }
            else if (node.HasHex)
            {
                if (node.HasList || node.HasFields)
                {
                    rows.Add((MakeSplitter(), new GridLength(5)));
                }

                rows.Add((MakeHex(node), new GridLength(1.4, GridUnitType.Star)));
            }

            if (!node.HasFields && !node.HasList && !node.HasHex && !node.HasPreviewTabs && !node.HasInfo)
            {
                rows.Add((MakeEmpty(Tr("Nothing to show")), new GridLength(1, GridUnitType.Star)));
            }

            Assemble(grid, rows);
            Content = grid;
        }

        private static void Assemble(
            Grid grid,
            List<(UIElement Element, GridLength Height)> rows
        )
        {
            for (int index = 0; index < rows.Count; ++index)
            {
                grid.RowDefinitions.Add(new RowDefinition { Height = rows[index].Height, MinHeight = rows[index].Height.IsStar ? 60 : 0 });
                Grid.SetRow(rows[index].Element, index);
                grid.Children.Add(rows[index].Element);
            }
        }

        public static Border MakeHeader(
            string title,
            IReadOnlyList<CommandItem> commands,
            bool primary
        )
        {
            var panel = new DockPanel();
            var actions = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };

            foreach (CommandItem command in commands)
            {
                actions.Children.Add(MakeCommandButton(command));
            }

            DockPanel.SetDock(actions, Dock.Right);
            panel.Children.Add(actions);

            var text = new TextBlock { Text = title, TextTrimming = TextTrimming.CharacterEllipsis };
            text.SetResourceReference(StyleProperty, "Text.Title");

            if (!primary)
            {
                text.FontSize = 12;
            }

            panel.Children.Add(text);
            var header = new Border { Child = panel };
            header.SetResourceReference(StyleProperty, "Pane.Header");

            if (!primary)
            {
                header.MinHeight = 28;
                header.Padding = new Thickness(10, 4, 8, 4);
            }

            return header;
        }

        public static Button MakeCommandButton(
            CommandItem command
        )
        {
            var image = new Image();
            image.SetResourceReference(StyleProperty, "Icon");
            image.SetResourceReference(Image.SourceProperty, command.IconKey);
            var content = new StackPanel { Orientation = Orientation.Horizontal };
            content.Children.Add(image);
            content.Children.Add(new TextBlock { Text = command.Label.TrimEnd('.'), Margin = new Thickness(6, 0, 2, 0), VerticalAlignment = VerticalAlignment.Center });

            var button = new Button { Content = content, Command = command.Command, ToolTip = command.ToolTip, Margin = new Thickness(2, 0, 0, 0) };
            button.SetResourceReference(StyleProperty, "Button.Tool");
            ToolTipService.SetShowOnDisabled(button, true);
            return button;
        }

        public static ListView MakeGrid(
            TableViewModel table,
            NodeViewModel? node
        )
        {
            var list = new ListView();
            list.SetResourceReference(StyleProperty, "ListView.Grid");
            TableView.SetNode(list, node);
            TableView.SetTable(list, table);
            return list;
        }

        private static TextBlock MakeSectionLabel(
            string text
        )
        {
            var label = new TextBlock { Text = text, Margin = new Thickness(12, 8, 12, 4), FontWeight = FontWeights.SemiBold };
            label.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");
            return label;
        }

        private static TextBlock MakeEmpty(
            string text
        )
        {
            var label = new TextBlock { Text = text, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(16) };
            label.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");
            return label;
        }

        private static GridSplitter MakeSplitter()
        {
            return new GridSplitter { HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch, ResizeDirection = GridResizeDirection.Rows, Height = 5 };
        }

        private static HexPanel MakeHex(
            NodeViewModel node
        )
        {
            return new HexPanel
            {
                Document = node.Owner,
                RegionStart = node.HexOffset,
                RegionLength = node.HexLength,
                Title = node.HexTitle
            };
        }

        private static UIElement MakeOverview(
            NodeViewModel node
        )
        {
            var stack = new StackPanel { Margin = new Thickness(0, 0, 0, 12) };

            if (node.Fields != null)
            {
                ListView fields = MakeGrid(node.Fields, node);
                ScrollViewer.SetVerticalScrollBarVisibility(fields, ScrollBarVisibility.Disabled);
                VirtualizingPanel.SetIsVirtualizing(fields, false);
                stack.Children.Add(fields);
            }

            UIElement? entropy = MakeEntropyMap(node);

            if (entropy != null)
            {
                stack.Children.Add(MakeSectionLabel(Tr("Entropy")));
                stack.Children.Add(entropy);
            }

            if (node.Version != null)
            {
                stack.Children.Add(MakeSectionLabel(Tr("Version information")));
                ListView version = MakeGrid(node.Version, node);
                ScrollViewer.SetVerticalScrollBarVisibility(version, ScrollBarVisibility.Disabled);
                VirtualizingPanel.SetIsVirtualizing(version, false);
                stack.Children.Add(version);
            }

            var scroll = new ScrollViewer { Content = stack, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
            scroll.SetResourceReference(BackgroundProperty, "Brush.Editor.Background");

            // The grids inside would swallow the mouse wheel
            scroll.PreviewMouseWheel += (_, arguments) =>
            {
                scroll.ScrollToVerticalOffset(scroll.VerticalOffset - arguments.Delta / 3.0);
                arguments.Handled = true;
            };

            return scroll;
        }

        // Strip of the whole file with a legend, click opens the hex editor at the block. The map reads the whole file, it
        // is computed on a worker thread (cached per revision in the core) and appears when ready.
        private static UIElement? MakeEntropyMap(
            NodeViewModel node
        )
        {
            if (!node.Owner.IsPe && node.Owner.Info.Size == 0)
            {
                return null;
            }

            var holder = new ContentControl { Focusable = false, MinHeight = 40 };
            var waiting = new TextBlock { Text = Tr("Computing..."), Margin = new Thickness(12, 2, 12, 8), FontSize = 11 };
            waiting.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");
            holder.Content = waiting;
            FillEntropyMap(node, holder);
            return holder;
        }

        private static async void FillEntropyMap(
            NodeViewModel node,
            ContentControl holder
        )
        {
            EntropyData data;

            try
            {
                byte[] answer = await System.Threading.Tasks.Task.Run(() => node.Document.QueryBytes("{\"query\":\"entropy\"}"));
                data = CoreJson.Parse<EntropyData>(answer);
            }
            catch (Exception error) when (error is CoreException || error is ObjectDisposedException || error is JsonException)
            {
                holder.Content = null;
                return;
            }

            holder.MinHeight = 0;
            holder.Content = BuildEntropyMap(node, data);
        }

        private static UIElement BuildEntropyMap(
            NodeViewModel node,
            EntropyData data
        )
        {
            var map = new EntropyMap { Data = data, Margin = new Thickness(12, 2, 12, 4) };
            map.OffsetClicked += (offset, length) => node.Owner.ShowInHexEditor(offset, length);

            var legend = new DockPanel { Margin = new Thickness(12, 2, 12, 8), LastChildFill = false };
            var gradient = new Border { Width = 160, Height = 8, CornerRadius = new CornerRadius(2), Background = EntropyMap.LegendBrush(), VerticalAlignment = VerticalAlignment.Center };
            var low = new TextBlock { Text = Tr("0  zeros"), Margin = new Thickness(0, 0, 6, 0), FontSize = 11 };
            var high = new TextBlock { Text = Tr("8  packed or encrypted"), Margin = new Thickness(6, 0, 0, 0), FontSize = 11 };
            var block = new TextBlock { Text = Tr("{0} blocks of {1}", data.Values.Length.ToString("N0"), Format.Bytes(data.Block)), FontSize = 11 };
            low.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");
            high.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");
            block.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");
            legend.Children.Add(low);
            legend.Children.Add(gradient);
            legend.Children.Add(high);
            DockPanel.SetDock(block, Dock.Right);
            legend.Children.Add(block);

            var panel = new StackPanel();
            panel.Children.Add(map);
            panel.Children.Add(legend);
            return panel;
        }

        private static UIElement MakePreviewTabs(
            NodeViewModel node
        )
        {
            var tabs = new TabControl();
            tabs.SetResourceReference(StyleProperty, "TabControl.Tool");
            tabs.SetResourceReference(ItemsControl.ItemContainerStyleProperty, "TabItem.Tool");

            UIElement preview;

            if (node.HasPreview)
            {
                preview = new ResourcePreview(node.Preview);
            }
            else
            {
                var text = new TextBox
                {
                    Text = node.PreviewText ?? string.Empty,
                    IsReadOnly = true,
                    AcceptsReturn = true,
                    TextWrapping = TextWrapping.NoWrap,
                    BorderThickness = new Thickness(0),
                    VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                    HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
                    Padding = new Thickness(8)
                };
                text.SetResourceReference(FontFamilyProperty, "Font.Mono");
                text.SetResourceReference(BackgroundProperty, "Brush.Editor.Background");
                preview = text;
            }

            tabs.Items.Add(new TabItem { Header = Tr("Preview"), Content = preview });

            if (node.HasHex)
            {
                tabs.Items.Add(new TabItem { Header = Tr("Raw data"), Content = MakeHex(node) });
            }

            tabs.SelectedIndex = 0;
            return tabs;
        }
    }

    // Icon, cursor and bitmap frames on a checkerboard, zoomable with nearest neighbour scaling.
    public sealed class ResourcePreview : DockPanel
    {
        private readonly IReadOnlyList<PreviewImage> m_Images;
        private readonly WrapPanel m_Frames = new() { Margin = new Thickness(12) };
        private int m_Zoom = 1;

        public ResourcePreview(
            IReadOnlyList<PreviewImage> images
        )
        {
            m_Images = images;
            SetResourceReference(BackgroundProperty, "Brush.Editor.Background");

            var bar = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(12, 8, 12, 0) };
            bar.Children.Add(new TextBlock { Text = Tr("Zoom"), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 8, 0) });
            var zoom = new ComboBox { Width = 80 };

            foreach (int factor in new[] { 1, 2, 4, 8 })
            {
                zoom.Items.Add($"{factor * 100} %");
            }

            // Small icons start at 200 % so they are readable on high DPI screens
            m_Zoom = images.Count > 0 && images.Max(image => image.Width) <= 48 ? 2 : 1;
            zoom.SelectedIndex = m_Zoom == 2 ? 1 : 0;
            zoom.SelectionChanged += (_, _) => { m_Zoom = 1 << zoom.SelectedIndex; Render(); };
            bar.Children.Add(zoom);
            SetDock(bar, Dock.Top);
            Children.Add(bar);
            Children.Add(new ScrollViewer { Content = m_Frames, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });

            Loaded += (_, _) => Render();
        }

        protected override void OnDpiChanged(
            DpiScale old_dpi,
            DpiScale new_dpi
        )
        {
            base.OnDpiChanged(old_dpi, new_dpi);
            Render();
        }

        private void Render()
        {
            m_Frames.Children.Clear();
            foreach (PreviewImage image in m_Images)
            {
                // Zoom is relative to the UI scale, 100% shows the image as large as a 96 DPI screen would, so it
                // keeps its size next to the rest of the UI on high DPI monitors.
                double width = image.Width * m_Zoom;
                double height = image.Height * m_Zoom;
                var picture = new Image { Source = image.Source, Width = width, Height = height, Stretch = Stretch.Fill };
                RenderOptions.SetBitmapScalingMode(picture, BitmapScalingMode.NearestNeighbor);

                var frame = new Border { Child = picture, Background = Checkerboard(), BorderThickness = new Thickness(1), HorizontalAlignment = HorizontalAlignment.Left };
                frame.SetResourceReference(Border.BorderBrushProperty, "Brush.Panel.Border");

                var caption = new TextBlock { Text = image.Caption, Margin = new Thickness(0, 4, 0, 0) };
                caption.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");

                var cell = new StackPanel { Margin = new Thickness(0, 0, 20, 20), VerticalAlignment = VerticalAlignment.Bottom };
                cell.Children.Add(frame);
                cell.Children.Add(caption);
                m_Frames.Children.Add(cell);
            }
        }

        private static Brush Checkerboard()
        {
            var light = new SolidColorBrush(Color.FromRgb(0xF4, 0xF4, 0xF4));
            var dark = new SolidColorBrush(Color.FromRgb(0xDD, 0xDD, 0xDD));
            var drawing = new DrawingGroup();
            drawing.Children.Add(new GeometryDrawing(light, null, new RectangleGeometry(new Rect(0, 0, 16, 16))));
            drawing.Children.Add(new GeometryDrawing(dark, null, new RectangleGeometry(new Rect(0, 0, 8, 8))));
            drawing.Children.Add(new GeometryDrawing(dark, null, new RectangleGeometry(new Rect(8, 8, 8, 8))));
            var brush = new DrawingBrush(drawing) { TileMode = TileMode.Tile, Viewport = new Rect(0, 0, 16, 16), ViewportUnits = BrushMappingMode.Absolute };
            brush.Freeze();
            return brush;
        }
    }
}