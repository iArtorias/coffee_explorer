using System;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Services;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.Controls
{
    // Entropy of the whole file as a strip (one column per block) above a band per region (headers, sections, overlay,
    // certificates). Low entropy is blue, around 4 bits per byte neutral gray, packed or encrypted data red. Hover shows
    // the block, click raises 'OffsetClicked' with the block start.
    public sealed class EntropyMap : FrameworkElement
    {
        private const double STRIP_HEIGHT = 26;
        private const double BAND_HEIGHT = 18;
        private const double GAP = 3;

        // Cool to warm ramp, readable on the light and the dark theme (the middle is a gray, not white).
        private static readonly (double Position, Color Color)[] RAMP =
        {
            (0.0, Color.FromRgb( 0x3B, 0x4C, 0xC0 )),
            (0.5, Color.FromRgb( 0x8E, 0x95, 0xA3 )),
            (0.75, Color.FromRgb( 0xE8, 0x9B, 0x6E )),
            (1.0, Color.FromRgb( 0xC0, 0x28, 0x2D ))
        };

        private readonly TextBlock m_Tip = new();
        private EntropyData? m_Data;
        private double m_HoverX = -1;
        private Brush[]? m_Brushes;

        public event Action<ulong, ulong>? OffsetClicked;

        public EntropyMap()
        {
            Height = STRIP_HEIGHT + GAP + BAND_HEIGHT;
            Cursor = Cursors.Hand;
            SnapsToDevicePixels = true;
            ToolTip = new ToolTip { Content = m_Tip };
            ToolTipService.SetInitialShowDelay(this, 0);
            ToolTipService.SetBetweenShowDelay(this, 0);
            ToolTipService.SetPlacement(this, System.Windows.Controls.Primitives.PlacementMode.Mouse);

            // The bands use theme brushes looked up while rendering.
            EventHandler repaint = (_, _) => InvalidateVisual();
            Loaded += (_, _) => ThemeService.ThemeChanged += repaint;
            Unloaded += (_, _) => ThemeService.ThemeChanged -= repaint;
        }

        public EntropyData? Data
        {
            get => m_Data;
            set
            {
                m_Data = value;
                m_Brushes = value?.Values.Select(entropy => (Brush)Frozen(new SolidColorBrush(EntropyColor(entropy)))).ToArray();
                InvalidateVisual();
            }
        }

        public static Color EntropyColor(
            double entropy
        )
        {
            double position = Math.Clamp(entropy / 8.0, 0.0, 1.0);

            for (int index = 1; index < RAMP.Length; ++index)
            {
                if (position <= RAMP[index].Position)
                {
                    var (from_position, from) = RAMP[index - 1];
                    var (to_position, to) = RAMP[index];
                    double amount = (position - from_position) / (to_position - from_position);
                    return Color.FromRgb(Mix(from.R, to.R, amount), Mix(from.G, to.G, amount), Mix(from.B, to.B, amount));
                }
            }

            return RAMP[^1].Color;
        }

        // A gradient brush of the ramp for the legend.
        public static Brush LegendBrush()
        {
            var brush = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 0) };

            foreach (var (position, color) in RAMP)
            {
                brush.GradientStops.Add(new GradientStop(color, position));
            }

            brush.Freeze();
            return brush;
        }

        private static byte Mix(
            byte from,
            byte to,
            double amount
        )
        {
            return (byte)Math.Round(from + (to - from) * amount);
        }

        private static Freezable Frozen(
            Freezable value
        )
        {
            value.Freeze();
            return value;
        }

        private Brush ResourceBrush(
            string key,
            Brush fallback
        )
        {
            return TryFindResource(key) as Brush ?? fallback;
        }

        private ulong OffsetAt(
            double x
        )
        {
            if (m_Data == null || m_Data.Size == 0 || ActualWidth <= 0)
            {
                return 0;
            }

            double fraction = Math.Clamp(x / ActualWidth, 0.0, 1.0);
            ulong offset = (ulong)(fraction * m_Data.Size);
            return Math.Min(offset - offset % Math.Max(m_Data.Block, 1), m_Data.Size - 1);
        }

        protected override void OnRender(
            DrawingContext context
        )
        {
            double width = ActualWidth;
            Brush border = ResourceBrush("Brush.Panel.Border", Brushes.Gray);
            context.DrawRectangle(ResourceBrush("Brush.Panel.Background", Brushes.Transparent), null, new Rect(0, 0, width, ActualHeight));

            if (m_Data == null || m_Brushes == null || m_Data.Values.Length == 0 || width <= 0)
            {
                return;
            }

            // One rectangle per device pixel column at most, neighbors with the same block reuse the brush.
            double block_width = width / m_Data.Values.Length;

            for (int index = 0; index < m_Data.Values.Length; ++index)
            {
                double x = index * block_width;
                context.DrawRectangle(m_Brushes[index], null, new Rect(x, 0, block_width + 0.5, STRIP_HEIGHT));
            }

            context.DrawRectangle(null, new Pen(border, 1), new Rect(0.5, 0.5, width - 1, STRIP_HEIGHT - 1));

            // Region bands, sections alternate between two tints so neighbors stay apart.
            double band_top = STRIP_HEIGHT + GAP;
            Brush text_brush = ResourceBrush("Brush.Text.Foreground", Brushes.Black);
            Brush[] fills = { ResourceBrush("Brush.Control.Hover", Brushes.LightGray), ResourceBrush("Brush.Control.Pressed", Brushes.Gray) };
            Brush special = ResourceBrush("Brush.Accent.Background", Brushes.SteelBlue);
            var typeface = new Typeface(TextElement.GetFontFamily(this), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);
            double pixels_per_dip = VisualTreeHelper.GetDpi(this).PixelsPerDip;
            int section = 0;

            foreach (EntropyRegion region in m_Data.Regions)
            {
                double left = width * region.Start / m_Data.Size;
                double right = width * Math.Min(region.End, m_Data.Size) / m_Data.Size;

                if (right - left < 1)
                {
                    right = left + 1;
                }

                bool is_section = region.Kind == "section";
                Brush fill = is_section ? fills[section++ % 2] : region.Kind == "headers" ? fills[1] : special;
                var rectangle = new Rect(left, band_top, right - left, BAND_HEIGHT);
                context.DrawRectangle(fill, new Pen(border, 1), rectangle);

                var label = new FormattedText(region.Name, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight, typeface, 11,
                                               region.Kind == "overlay" || region.Kind == "certificates" ? Brushes.White : text_brush, pixels_per_dip);

                // Names only where they fit, the tooltip has them all.
                if (label.Width + 6 <= rectangle.Width)
                {
                    context.DrawText(label, new Point(left + 3, band_top + (BAND_HEIGHT - label.Height) / 2));
                }
            }

            if (m_HoverX >= 0)
            {
                context.DrawRectangle(text_brush, null, new Rect(Math.Floor(m_HoverX), 0, 1, ActualHeight));
            }
        }

        protected override void OnMouseMove(
            MouseEventArgs arguments
        )
        {
            base.OnMouseMove(arguments);

            if (m_Data == null || m_Data.Values.Length == 0)
            {
                return;
            }

            m_HoverX = arguments.GetPosition(this).X;
            ulong offset = OffsetAt(m_HoverX);
            int block = (int)Math.Min((ulong)m_Data.Values.Length - 1, offset / Math.Max(m_Data.Block, 1));
            double entropy = m_Data.Values[block];
            ulong end = Math.Min(offset + m_Data.Block, m_Data.Size);
            string region = string.Join(", ", m_Data.Regions.Where(item => offset >= item.Start && offset < item.End).Select(item => item.Name));
            string meaning = entropy < 1 ? Tr("padding or zeros") : entropy < 5 ? Tr("text, tables or sparse data") : entropy < 6.8 ? Tr("code or structured data") :
                             entropy < 7.5 ? Tr("compressed data, resources") : Tr("packed or encrypted data");

            m_Tip.Text = Tr("Offset {0} - {1}", Format.Offset(offset), Format.Offset(end - 1)) + "\n" +
                         Tr("Entropy {0} bits per byte, {1}", entropy.ToString("0.00", CultureInfo.InvariantCulture), meaning) +
                         (region.Length > 0 ? $"\n{region}" : string.Empty) + "\n" + Tr("Click to open in the hex editor");
            InvalidateVisual();
        }

        protected override void OnMouseLeave(
            MouseEventArgs arguments
        )
        {
            base.OnMouseLeave(arguments);
            m_HoverX = -1;
            InvalidateVisual();
        }

        protected override void OnMouseLeftButtonUp(
            MouseButtonEventArgs arguments
        )
        {
            base.OnMouseLeftButtonUp(arguments);

            if (m_Data != null && m_Data.Size != 0)
            {
                ulong offset = OffsetAt(arguments.GetPosition(this).X);
                OffsetClicked?.Invoke(offset, Math.Min(m_Data.Block, m_Data.Size - offset));
            }
        }
    }
}