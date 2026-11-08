using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Documents;
using System.Windows.Media;

namespace CoffeeExplorer.Controls
{
    // One instruction drawn in the token colors of x64dbg. The core sends the text with [class, length] spans,
    // the brushes come from 'Themes\Disassembly*.xaml'.
    public sealed class DisassemblyText : FrameworkElement
    {
        public static readonly DependencyProperty TextProperty = DependencyProperty.Register(
            nameof(Text), typeof(string), typeof(DisassemblyText),
            new FrameworkPropertyMetadata(string.Empty, FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsRender));

        public static readonly DependencyProperty SpansProperty = DependencyProperty.Register(
            nameof(Spans), typeof(IReadOnlyList<int>), typeof(DisassemblyText),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

        // Resource keys in the order of the DisassemblyClass values of the core.
        private static readonly string[] CLASS_KEYS =
        {
            "Text", "Mnemonic", "PushPop", "Call", "Ret", "ConditionalJump", "UnconditionalJump", "Nop", "Far", "Int3", "Unusual", "Prefix",
            "GeneralRegister", "FpuRegister", "MmxRegister", "XmmRegister", "YmmRegister", "ZmmRegister", "SegmentRegister", "Value", "Address",
            "MemorySize", "MemoryBrackets", "MemoryStackBrackets", "MemoryBaseRegister", "MemoryIndexRegister", "MemoryScale", "MemoryOperator",
            "Comma", "Decorator", "MaskRegister"
        };

        // Bound to a disassembly brush with DynamicResource in the template. A theme switch changes it and redraws the line.
        public static readonly DependencyProperty ThemeProbeProperty = DependencyProperty.Register(
            nameof(ThemeProbe), typeof(object), typeof(DisassemblyText),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

        // Brushes of the current theme, looked up again when the theme dictionary changed.
        private static Brush?[]? s_Brushes;
        private static object? s_Probe;

        private FormattedText? m_Formatted;

        public string Text
        {
            get => (string)GetValue(TextProperty);
            set => SetValue(TextProperty, value);
        }

        // The XAML compiler refuses array typed properties inside templates.
        public IReadOnlyList<int>? Spans
        {
            get => (IReadOnlyList<int>?)GetValue(SpansProperty);
            set => SetValue(SpansProperty, value);
        }

        public object? ThemeProbe
        {
            get => GetValue(ThemeProbeProperty);
            set => SetValue(ThemeProbeProperty, value);
        }

        private static Brush?[] ThemeBrushes()
        {
            Brush?[]? brushes = s_Brushes;
            object? probe = Application.Current?.TryFindResource("Brush.Disasm.Mnemonic");

            if (brushes == null || !ReferenceEquals(probe, s_Probe))
            {
                s_Probe = probe;

                brushes = new Brush?[CLASS_KEYS.Length];

                for (int index = 0; index < CLASS_KEYS.Length; ++index)
                {
                    brushes[index] = Application.Current?.TryFindResource("Brush.Disasm." + CLASS_KEYS[index]) as Brush;
                }

                s_Brushes = brushes;
            }

            return brushes;
        }

        private FormattedText Format()
        {
            var formatted = new FormattedText(
                Text ?? string.Empty,
                CultureInfo.InvariantCulture,
                FlowDirection.LeftToRight,
                new Typeface(TextElement.GetFontFamily(this), TextElement.GetFontStyle(this), TextElement.GetFontWeight(this), TextElement.GetFontStretch(this)),
                TextElement.GetFontSize(this),
                TextElement.GetForeground(this) ?? System.Windows.Media.Brushes.Gray,
                VisualTreeHelper.GetDpi(this).PixelsPerDip);

            IReadOnlyList<int>? spans = Spans;
            Brush?[] brushes = ThemeBrushes();
            int position = 0;
            int length = formatted.Text.Length;

            // The core counts UTF-8 bytes, Zydis writes ASCII only, so bytes and characters are the same here.
            for (int index = 0; spans != null && index + 1 < spans.Count && position < length; index += 2)
            {
                int count = Math.Min(spans[index + 1], length - position);
                int kind = spans[index];

                if (count > 0 && kind >= 0 && kind < brushes.Length && brushes[kind] is Brush brush)
                {
                    formatted.SetForegroundBrush(brush, position, count);
                }

                position += Math.Max(count, 0);
            }

            return formatted;
        }

        protected override Size MeasureOverride(
            Size available
        )
        {
            m_Formatted = Format();
            return new Size(m_Formatted.WidthIncludingTrailingWhitespace, m_Formatted.Height);
        }

        protected override void OnRender(
            DrawingContext context
        )
        {
            // Spans or the theme changed after the last measure.
            m_Formatted = Format();
            context.PushClip(new RectangleGeometry(new Rect(RenderSize)));
            context.DrawText(m_Formatted, new Point(0, Math.Max(0, (RenderSize.Height - m_Formatted.Height) / 2)));
            context.Pop();
        }

        protected override void OnPropertyChanged(
            DependencyPropertyChangedEventArgs arguments
        )
        {
            base.OnPropertyChanged(arguments);

            // Font and selection color come from the row, both are inherited.
            if (arguments.Property == TextElement.FontFamilyProperty || arguments.Property == TextElement.FontSizeProperty || arguments.Property == TextElement.ForegroundProperty)
            {
                InvalidateMeasure();
                InvalidateVisual();
            }
        }
    }
}