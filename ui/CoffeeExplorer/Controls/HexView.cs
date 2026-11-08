using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Services;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.Controls
{
    public enum HexColumn
    {
        COLUMN_HEX = 0,
        COLUMN_TEXT = 1
    }

    // Hex view over a region of a document. Offset, 16 bytes per row and the Latin-1 text.
    // Overwrite editing only, a PE editor never inserts bytes through the hex view. Rows stay aligned to file
    // offsets (a region that starts at 0x208 shows its first row from 0x200, bytes outside the region are blank).
    // Drawn with glyph runs snapped to device pixels.
    public sealed class HexView : FrameworkElement
    {
        private const int BYTES_PER_ROW = 16;
        private const double PADDING = 8;

        private static readonly char[] HEX_DIGITS = "0123456789ABCDEF".ToCharArray();

        private readonly ScrollBar m_VerticalBar;
        private readonly ScrollBar m_HorizontalBar;
        private readonly VisualCollection m_Children;
        private readonly DispatcherTimer m_CaretTimer;
        private readonly ushort[] m_GlyphCache = new ushort[256];
        private readonly Dictionary<char, ushort> m_ExtraGlyphs = new();

        private IByteSource? m_Source;
        private GlyphTypeface? m_Typeface;
        private double m_CharWidth = 8;
        private double m_LineHeight = 16;
        private double m_Baseline = 12;
        private double m_PixelsPerDip = 1;
        private int m_OffsetDigits = 8;
        private double m_HexStart;
        private double m_TextStart;
        private double m_ContentWidth;
        private ulong m_TopRow;
        private int m_VisibleRows;
        private double m_ScrollX;
        private ulong m_Caret;
        private ulong m_Anchor;
        private bool m_LowNibble;
        private bool m_CaretOn = true;
        private bool m_Selecting;
        private bool m_GroupOpen;
        private byte[] m_Bytes = new byte[4096];

        public static readonly DependencyProperty SourceProperty = DependencyProperty.Register(
            nameof(Source), typeof(IByteSource), typeof(HexView), new FrameworkPropertyMetadata(null, (d, _) => ((HexView)d).OnSourceChanged()));

        public static readonly DependencyProperty RegionStartProperty = DependencyProperty.Register(
            nameof(RegionStart), typeof(ulong), typeof(HexView), new FrameworkPropertyMetadata(0ul, (d, _) => ((HexView)d).OnSourceChanged()));

        // Zero shows the whole source.
        public static readonly DependencyProperty RegionLengthProperty = DependencyProperty.Register(
            nameof(RegionLength), typeof(ulong), typeof(HexView), new FrameworkPropertyMetadata(0ul, (d, _) => ((HexView)d).OnSourceChanged()));

        public static readonly DependencyProperty IsReadOnlyProperty = DependencyProperty.Register(
            nameof(IsReadOnly), typeof(bool), typeof(HexView), new FrameworkPropertyMetadata(false));

        public static readonly DependencyProperty EditorFontFamilyProperty = DependencyProperty.Register(
            nameof(EditorFontFamily), typeof(string), typeof(HexView), new FrameworkPropertyMetadata("Cascadia Mono, Consolas, Courier New", (d, _) => ((HexView)d).UpdateMetrics()));

        public static readonly DependencyProperty EditorFontSizeProperty = DependencyProperty.Register(
            nameof(EditorFontSize), typeof(double), typeof(HexView), new FrameworkPropertyMetadata(13.0, (d, _) => ((HexView)d).UpdateMetrics()));

        // Caret or selection changed.
        public event EventHandler? StateChanged;

        // Bytes were written through the view.
        public event EventHandler? Edited;

        public HexView()
        {
            m_VerticalBar = new ScrollBar { Orientation = Orientation.Vertical, Minimum = 0, SmallChange = 1 };
            m_HorizontalBar = new ScrollBar { Orientation = Orientation.Horizontal, Minimum = 0, SmallChange = 16 };
            m_VerticalBar.ValueChanged += (_, _) => { m_TopRow = (ulong)Math.Max(0, Math.Round(m_VerticalBar.Value)); InvalidateVisual(); };
            m_HorizontalBar.ValueChanged += (_, _) => { m_ScrollX = Math.Round(m_HorizontalBar.Value); InvalidateVisual(); };
            m_Children = new VisualCollection(this) { m_VerticalBar, m_HorizontalBar };

            m_CaretTimer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(530) };
            m_CaretTimer.Tick += (_, _) => { m_CaretOn = !m_CaretOn; InvalidateVisual(); };

            Focusable = true;
            FocusVisualStyle = null;
            ClipToBounds = true;
            Cursor = Cursors.IBeam;
            SnapsToDevicePixels = true;
            TextOptions.SetTextFormattingMode(this, TextFormattingMode.Display);

            Loaded += (_, _) => { ThemeService.ThemeChanged += OnThemeChanged; UpdateMetrics(); };
            Unloaded += (_, _) => { ThemeService.ThemeChanged -= OnThemeChanged; m_CaretTimer.Stop(); CloseGroup(); };
        }

        // Properties.

        public IByteSource? Source
        {
            get => (IByteSource?)GetValue(SourceProperty);
            set => SetValue(SourceProperty, value);
        }

        public ulong RegionStart
        {
            get => (ulong)GetValue(RegionStartProperty);
            set => SetValue(RegionStartProperty, value);
        }

        public ulong RegionLength
        {
            get => (ulong)GetValue(RegionLengthProperty);
            set => SetValue(RegionLengthProperty, value);
        }

        public bool IsReadOnly
        {
            get => (bool)GetValue(IsReadOnlyProperty);
            set => SetValue(IsReadOnlyProperty, value);
        }

        public string EditorFontFamily
        {
            get => (string)GetValue(EditorFontFamilyProperty);
            set => SetValue(EditorFontFamilyProperty, value);
        }

        public double EditorFontSize
        {
            get => (double)GetValue(EditorFontSizeProperty);
            set => SetValue(EditorFontSizeProperty, value);
        }

        public HexColumn ActiveColumn { get; private set; } = HexColumn.COLUMN_HEX;

        public ulong CaretOffset => m_Caret;

        public ulong SelectionStart => Math.Min(m_Caret, m_Anchor);

        public ulong SelectionLength => m_Caret > m_Anchor ? m_Caret - m_Anchor : m_Anchor - m_Caret;

        public bool HasSelection => m_Caret != m_Anchor;

        // First and one past the last file offset of the region.
        public ulong RegionBegin => m_Source == null ? 0 : Math.Min(RegionStart, m_Source.Length);

        public ulong RegionEnd
        {
            get
            {
                if (m_Source == null)
                {
                    return 0;
                }

                ulong length = m_Source.Length;
                ulong end = RegionLength == 0 ? length : RegionStart + RegionLength;
                return Math.Min(end, length);
            }
        }

        private ulong FirstRow => RegionBegin / BYTES_PER_ROW;

        private ulong RowCount
        {
            get
            {
                ulong end = RegionEnd;
                ulong last = end == 0 ? 0 : (end - 1) / BYTES_PER_ROW;
                return end <= RegionBegin ? 1 : last - FirstRow + 1;
            }
        }

        private double HeaderHeight => m_LineHeight + 4;

        private double ScrollBarSize => SystemParameters.VerticalScrollBarWidth;

        // Selection, or the byte under the caret when nothing is selected.
        public (ulong Start, ulong Length) SelectionOrCaret => HasSelection ? (SelectionStart, SelectionLength) : (m_Caret, m_Caret < RegionEnd ? 1ul : 0ul);

        // Public operations

        public void RefreshData()
        {
            UpdateScrollBars();
            InvalidateVisual();
        }

        public void SetSelection(
            ulong start,
            ulong length
        )
        {
            CloseGroup();
            ulong begin = RegionBegin;
            ulong end = RegionEnd;
            start = Math.Clamp(start, begin, Math.Max(begin, end));
            m_Anchor = start;
            m_Caret = Math.Min(start + length, end);
            m_LowNibble = false;
            ScrollToOffset(start);
            OnStateChanged();
        }

        public void ScrollToOffset(
            ulong offset
        )
        {
            ulong row = offset / BYTES_PER_ROW;
            ulong first = FirstRow;
            ulong relative = row > first ? row - first : 0;

            if (relative < m_TopRow || relative >= m_TopRow + (ulong)Math.Max(1, m_VisibleRows - 1))
            {
                ulong target = relative > (ulong)(m_VisibleRows / 3) ? relative - (ulong)(m_VisibleRows / 3) : 0;
                SetTopRow(target);
            }

            InvalidateVisual();
        }

        public void SelectAll()
        {
            CloseGroup();
            m_Anchor = RegionBegin;
            m_Caret = RegionEnd;
            InvalidateVisual();
            OnStateChanged();
        }

        public byte[] GetSelectedBytes()
        {
            if (m_Source == null || !HasSelection)
            {
                return Array.Empty<byte>();
            }

            ulong length = Math.Min(SelectionLength, 64ul * 1024 * 1024);
            var data = new byte[length];
            int read = m_Source.Read(SelectionStart, data);
            return read == data.Length ? data : data.AsSpan(0, read).ToArray();
        }

        public void Copy()
        {
            byte[] data = GetSelectedBytes();

            if (data.Length == 0)
            {
                return;
            }

            string text = ActiveColumn == HexColumn.COLUMN_TEXT ? Encoding.Latin1.GetString(data) : Format.HexString(data);

            try
            {
                Clipboard.SetText(text);
            }
            catch (Exception)
            {
                // Clipboard locked by another process.
            }
        }

        // Overwrites from the caret (or the selection start) with hex text or plain text from the clipboard.
        public bool PasteWrite()
        {
            if (m_Source == null || IsReadOnly || m_Source.IsReadOnly)
            {
                return false;
            }

            string text;

            try
            {
                text = Clipboard.GetText();
            }
            catch (Exception)
            {
                return false;
            }

            if (string.IsNullOrEmpty(text))
            {
                return false;
            }

            byte[] data = ActiveColumn == HexColumn.COLUMN_HEX && Format.TryParseHexBytes(text, out byte[] parsed) ? parsed : Encoding.Latin1.GetBytes(text);
            ulong start = HasSelection ? SelectionStart : m_Caret;
            ulong room = RegionEnd > start ? RegionEnd - start : 0;

            if (room == 0 || data.Length == 0)
            {
                return false;
            }

            if ((ulong)data.Length > room)
            {
                Array.Resize(ref data, (int)room);
            }

            CloseGroup();
            m_Source.Write(start, data, "Paste");
            m_Anchor = start;
            m_Caret = start + (ulong)data.Length;
            OnEdited();
            return true;
        }

        public void Fill(
            byte[] pattern,
            ulong start,
            ulong length
        )
        {
            if (m_Source == null || pattern.Length == 0 || length == 0)
            {
                return;
            }

            var data = new byte[length];

            for (ulong index = 0; index < length; ++index)
            {
                data[index] = pattern[index % (ulong)pattern.Length];
            }

            CloseGroup();
            m_Source.Write(start, data, "Fill");
            OnEdited();
        }

        // Next occurrence of the pattern after the caret, wraps once inside the region.
        public bool Find(
            byte[] pattern,
            bool ignore_case
        )
        {
            if (m_Source == null || pattern.Length == 0)
            {
                return false;
            }

            ulong begin = RegionBegin;
            ulong end = RegionEnd;
            ulong from = HasSelection ? SelectionStart + 1 : m_Caret;

            ulong? hit = Search(pattern, ignore_case, Math.Max(from, begin), end) ?? Search(pattern, ignore_case, begin, Math.Min(end, from + (ulong)pattern.Length));

            if (hit == null)
            {
                return false;
            }

            SetSelection(hit.Value, (ulong)pattern.Length);
            return true;
        }

        private ulong? Search(
            byte[] pattern,
            bool ignore_case,
            ulong start,
            ulong end
        )
        {
            const int CHUNK = 1 << 20;
            var buffer = new byte[CHUNK + pattern.Length];
            ulong position = start;

            while (position < end)
            {
                int wanted = (int)Math.Min((ulong)buffer.Length, end - position);
                int read = m_Source!.Read(position, buffer.AsSpan(0, wanted));

                if (read < pattern.Length)
                {
                    break;
                }

                for (int index = 0; index + pattern.Length <= read; ++index)
                {
                    bool match = true;

                    for (int inner = 0; inner < pattern.Length; ++inner)
                    {
                        byte left = buffer[index + inner];
                        byte right = pattern[inner];

                        if (left != right && !(ignore_case && char.ToLowerInvariant((char)left) == char.ToLowerInvariant((char)right)))
                        {
                            match = false;
                            break;
                        }
                    }

                    if (match)
                    {
                        return position + (ulong)index;
                    }
                }

                if ((ulong)read < (ulong)wanted || read <= pattern.Length)
                {
                    break;
                }

                position += (ulong)(read - pattern.Length + 1);
            }

            return null;
        }

        // Layout

        protected override int VisualChildrenCount => m_Children.Count;

        protected override Visual GetVisualChild(
            int index
        )
        {
            return m_Children[index];
        }

        protected override Size MeasureOverride(
            Size available
        )
        {
            m_VerticalBar.Measure(available);
            m_HorizontalBar.Measure(available);
            double width = double.IsInfinity(available.Width) ? m_ContentWidth + ScrollBarSize : available.Width;
            double height = double.IsInfinity(available.Height) ? HeaderHeight + m_LineHeight * Math.Min(32, RowCount) : available.Height;
            return new Size(width, height);
        }

        protected override Size ArrangeOverride(
            Size final_size
        )
        {
            double bar = ScrollBarSize;
            bool horizontal = m_ContentWidth > final_size.Width - bar;
            m_HorizontalBar.Visibility = horizontal ? Visibility.Visible : Visibility.Collapsed;
            double content_height = final_size.Height - (horizontal ? bar : 0);

            m_VerticalBar.Arrange(new Rect(Math.Max(0, final_size.Width - bar), 0, bar, Math.Max(0, content_height)));
            m_HorizontalBar.Arrange(horizontal ? new Rect(0, Math.Max(0, final_size.Height - bar), Math.Max(0, final_size.Width - bar), bar) : new Rect());

            m_VisibleRows = Math.Max(1, (int)Math.Floor((content_height - HeaderHeight) / m_LineHeight));
            UpdateScrollBars();
            return final_size;
        }

        private void UpdateScrollBars()
        {
            ulong rows = RowCount;
            double maximum = rows > (ulong)m_VisibleRows ? rows - (ulong)m_VisibleRows : 0;
            m_VerticalBar.Maximum = maximum;
            m_VerticalBar.ViewportSize = m_VisibleRows;
            m_VerticalBar.LargeChange = Math.Max(1, m_VisibleRows - 1);

            if (m_TopRow > maximum)
            {
                m_TopRow = (ulong)maximum;
            }

            m_VerticalBar.Value = m_TopRow;

            double viewport = Math.Max(0, ActualWidth - ScrollBarSize);
            m_HorizontalBar.Maximum = Math.Max(0, m_ContentWidth - viewport);
            m_HorizontalBar.ViewportSize = viewport;
            m_HorizontalBar.LargeChange = Math.Max(1, viewport);

            if (m_ScrollX > m_HorizontalBar.Maximum)
            {
                m_ScrollX = m_HorizontalBar.Maximum;
            }
        }

        private void SetTopRow(
            ulong row
        )
        {
            ulong rows = RowCount;
            ulong maximum = rows > (ulong)m_VisibleRows ? rows - (ulong)m_VisibleRows : 0;
            m_TopRow = Math.Min(row, maximum);
            m_VerticalBar.Value = m_TopRow;
            InvalidateVisual();
        }

        private void UpdateMetrics()
        {
            m_PixelsPerDip = VisualTreeHelper.GetDpi(this).PixelsPerDip;
            var typeface = new Typeface(new FontFamily(EditorFontFamily), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);

            if (!typeface.TryGetGlyphTypeface(out m_Typeface))
            {
                new Typeface("Consolas").TryGetGlyphTypeface(out m_Typeface);
            }

            Array.Clear(m_GlyphCache);
            m_ExtraGlyphs.Clear();

            if (m_Typeface != null)
            {
                for (int character = 0x20; character < 0x7F; ++character)
                {
                    m_GlyphCache[character] = m_Typeface.CharacterToGlyphMap.TryGetValue(character, out ushort glyph) ? glyph : (ushort)0;
                }

                ushort zero = m_GlyphCache['0'];
                double advance = m_Typeface.AdvanceWidths[zero] * EditorFontSize;
                m_CharWidth = Math.Round(advance * m_PixelsPerDip) / m_PixelsPerDip;
                m_LineHeight = Math.Ceiling(m_Typeface.Height * EditorFontSize * 1.25 * m_PixelsPerDip) / m_PixelsPerDip;
                m_Baseline = Math.Round((m_Typeface.Baseline * EditorFontSize + (m_LineHeight - m_Typeface.Height * EditorFontSize) / 2) * m_PixelsPerDip) / m_PixelsPerDip;
            }

            m_OffsetDigits = m_Source != null && m_Source.Length > 0xFFFFFFFFul ? 16 : 8;
            m_HexStart = PADDING + (m_OffsetDigits + 2) * m_CharWidth;
            m_TextStart = m_HexStart + (BYTES_PER_ROW * 3 + 2) * m_CharWidth;
            m_ContentWidth = m_TextStart + BYTES_PER_ROW * m_CharWidth + PADDING * 2;

            InvalidateMeasure();
            InvalidateArrange();
            InvalidateVisual();
        }

        protected override void OnDpiChanged(
            DpiScale old_dpi,
            DpiScale new_dpi
        )
        {
            base.OnDpiChanged(old_dpi, new_dpi);
            UpdateMetrics();
        }

        private void OnThemeChanged(
            object? sender,
            EventArgs arguments
        )
        {
            InvalidateVisual();
        }

        private void OnSourceChanged()
        {
            CloseGroup();
            m_Source = Source;
            m_TopRow = 0;
            m_ScrollX = 0;
            m_Caret = RegionBegin;
            m_Anchor = m_Caret;
            m_LowNibble = false;
            UpdateMetrics();
            UpdateScrollBars();
            OnStateChanged();
        }

        private double CellX(
            int column,
            HexColumn kind
        )
        {
            if (kind == HexColumn.COLUMN_TEXT)
            {
                return m_TextStart + column * m_CharWidth;
            }

            // An extra space after the eighth byte splits the row in two halves.
            return m_HexStart + column * 3 * m_CharWidth + (column >= 8 ? m_CharWidth : 0);
        }

        // Mouse and keyboard

        private bool HitTest(
            Point point,
            out ulong offset,
            out HexColumn column,
            out bool low_nibble
        )
        {
            offset = 0;
            column = HexColumn.COLUMN_HEX;
            low_nibble = false;

            if (m_Source == null)
            {
                return false;
            }

            double x = point.X + m_ScrollX;
            double y = Math.Max(point.Y, HeaderHeight);
            long row = (long)Math.Floor((y - HeaderHeight) / m_LineHeight);
            ulong absolute_row = FirstRow + m_TopRow + (ulong)Math.Max(0, row);
            int cell;

            if (x >= m_TextStart - m_CharWidth / 2)
            {
                column = HexColumn.COLUMN_TEXT;
                cell = (int)Math.Floor((x - m_TextStart) / m_CharWidth);
            }
            else
            {
                double relative = x - m_HexStart;
                double half = 8 * 3 * m_CharWidth;

                if (relative >= half)
                {
                    relative -= m_CharWidth;
                }

                cell = (int)Math.Floor(relative / (3 * m_CharWidth));
                low_nibble = relative - cell * 3 * m_CharWidth >= m_CharWidth;
            }

            cell = Math.Clamp(cell, 0, BYTES_PER_ROW - 1);
            offset = Math.Clamp(absolute_row * BYTES_PER_ROW + (ulong)cell, RegionBegin, Math.Max(RegionBegin, RegionEnd == 0 ? 0 : RegionEnd - 1));
            return true;
        }

        protected override void OnMouseDown(
            MouseButtonEventArgs arguments
        )
        {
            base.OnMouseDown(arguments);
            Focus();

            if (arguments.ChangedButton != MouseButton.Left || !HitTest(arguments.GetPosition(this), out ulong offset, out HexColumn column, out bool low))
            {
                return;
            }

            CloseGroup();
            ActiveColumn = column;
            bool extend = (Keyboard.Modifiers & ModifierKeys.Shift) != 0;
            m_Caret = offset;
            m_LowNibble = column == HexColumn.COLUMN_HEX && low && !extend;

            if (!extend)
            {
                m_Anchor = offset;
            }

            m_Selecting = true;
            CaptureMouse();
            ResetCaretBlink();
            OnStateChanged();
            arguments.Handled = true;
        }

        protected override void OnMouseMove(
            MouseEventArgs arguments
        )
        {
            base.OnMouseMove(arguments);

            if (!m_Selecting || !HitTest(arguments.GetPosition(this), out ulong offset, out _, out _))
            {
                return;
            }

            // Dragging selects whole bytes, the caret sits after the last one.
            m_Caret = offset >= m_Anchor ? Math.Min(offset + 1, RegionEnd) : offset;
            m_LowNibble = false;

            Point point = arguments.GetPosition(this);

            if (point.Y < HeaderHeight && m_TopRow > 0)
            {
                SetTopRow(m_TopRow - 1);
            }
            else if (point.Y > ActualHeight - ScrollBarSize)
            {
                SetTopRow(m_TopRow + 1);
            }

            InvalidateVisual();
            OnStateChanged();
        }

        protected override void OnMouseUp(
            MouseButtonEventArgs arguments
        )
        {
            base.OnMouseUp(arguments);

            if (m_Selecting)
            {
                m_Selecting = false;
                ReleaseMouseCapture();
            }
        }

        protected override void OnMouseWheel(
            MouseWheelEventArgs arguments
        )
        {
            base.OnMouseWheel(arguments);
            long delta = -arguments.Delta / 40;
            long target = (long)m_TopRow + delta;
            SetTopRow((ulong)Math.Max(0, target));
            arguments.Handled = true;
        }

        protected override void OnGotKeyboardFocus(
            KeyboardFocusChangedEventArgs arguments
        )
        {
            base.OnGotKeyboardFocus(arguments);
            ResetCaretBlink();
        }

        protected override void OnLostKeyboardFocus(
            KeyboardFocusChangedEventArgs arguments
        )
        {
            base.OnLostKeyboardFocus(arguments);
            m_CaretTimer.Stop();
            CloseGroup();
            InvalidateVisual();
        }

        protected override void OnKeyDown(
            KeyEventArgs arguments
        )
        {
            base.OnKeyDown(arguments);

            if (m_Source == null)
            {
                return;
            }

            bool shift = (Keyboard.Modifiers & ModifierKeys.Shift) != 0;
            bool control = (Keyboard.Modifiers & ModifierKeys.Control) != 0;
            long caret = (long)m_Caret;
            long page = (long)Math.Max(1, m_VisibleRows - 1) * BYTES_PER_ROW;
            long? target = null;

            switch (arguments.Key)
            {
                case Key.Left:
                    target = caret - 1;
                    break;
                case Key.Right:
                    target = caret + 1;
                    break;
                case Key.Up:
                    target = caret - BYTES_PER_ROW;
                    break;
                case Key.Down:
                    target = caret + BYTES_PER_ROW;
                    break;
                case Key.PageUp:
                    target = caret - page;
                    break;
                case Key.PageDown:
                    target = caret + page;
                    break;
                case Key.Home:
                    target = control ? (long)RegionBegin : caret - caret % BYTES_PER_ROW;
                    break;
                case Key.End:
                    target = control ? (long)RegionEnd : Math.Min(caret - caret % BYTES_PER_ROW + BYTES_PER_ROW - 1, (long)RegionEnd);
                    break;
                case Key.Tab:
                    ActiveColumn = ActiveColumn == HexColumn.COLUMN_HEX ? HexColumn.COLUMN_TEXT : HexColumn.COLUMN_HEX;
                    m_LowNibble = false;
                    InvalidateVisual();
                    arguments.Handled = true;
                    return;
                case Key.A when control:
                    SelectAll();
                    arguments.Handled = true;
                    return;
                case Key.C when control:
                case Key.Insert when control:
                    Copy();
                    arguments.Handled = true;
                    return;
                case Key.V when control:
                    PasteWrite();
                    arguments.Handled = true;
                    return;
            }

            if (target == null)
            {
                return;
            }

            CloseGroup();
            ulong clamped = (ulong)Math.Clamp(target.Value, (long)RegionBegin, (long)RegionEnd);
            m_Caret = clamped;
            m_LowNibble = false;

            if (!shift)
            {
                m_Anchor = clamped;
            }

            ScrollToOffset(clamped);
            ResetCaretBlink();
            OnStateChanged();
            arguments.Handled = true;
        }

        protected override void OnTextInput(
            TextCompositionEventArgs arguments
        )
        {
            base.OnTextInput(arguments);

            if (m_Source == null || IsReadOnly || m_Source.IsReadOnly || string.IsNullOrEmpty(arguments.Text) ||
                 (Keyboard.Modifiers & (ModifierKeys.Control | ModifierKeys.Alt)) != 0)
            {
                return;
            }

            foreach (char character in arguments.Text)
            {
                if (!TypeCharacter(character))
                {
                    System.Media.SystemSounds.Beep.Play();
                    break;
                }
            }

            arguments.Handled = true;
        }

        private bool TypeCharacter(
            char character
        )
        {
            if (m_Caret >= RegionEnd || character < 0x20)
            {
                return false;
            }

            if (HasSelection)
            {
                m_Caret = SelectionStart;
                m_Anchor = m_Caret;
                m_LowNibble = false;
            }

            OpenGroup();
            byte value = ReadByte(m_Caret);

            if (ActiveColumn == HexColumn.COLUMN_HEX)
            {
                int nibble = Uri.IsHexDigit(character) ? Convert.ToInt32(character.ToString(), 16) : -1;

                if (nibble < 0)
                {
                    return false;
                }

                value = m_LowNibble ? (byte)((value & 0xF0) | nibble) : (byte)((value & 0x0F) | (nibble << 4));
                m_Source!.Write(m_Caret, new[] { value }, "Edit bytes");

                if (m_LowNibble)
                {
                    m_LowNibble = false;
                    m_Caret = Math.Min(m_Caret + 1, RegionEnd);
                }
                else
                {
                    m_LowNibble = true;
                }
            }
            else
            {
                if (character > 0xFF)
                {
                    return false;
                }

                m_Source!.Write(m_Caret, new[] { (byte)character }, "Edit bytes");
                m_Caret = Math.Min(m_Caret + 1, RegionEnd);
            }

            m_Anchor = m_Caret;
            ScrollToOffset(m_Caret);
            ResetCaretBlink();
            InvalidateVisual();
            OnStateChanged();
            Edited?.Invoke(this, EventArgs.Empty);
            return true;
        }

        private byte ReadByte(
            ulong offset
        )
        {
            Span<byte> value = stackalloc byte[1];
            return m_Source!.Read(offset, value) == 1 ? value[0] : (byte)0;
        }

        private void OpenGroup()
        {
            if (!m_GroupOpen && m_Source != null)
            {
                m_Source.BeginGroup("Edit bytes");
                m_GroupOpen = true;
            }
        }

        private void CloseGroup()
        {
            if (m_GroupOpen && m_Source != null)
            {
                m_GroupOpen = false;

                try
                {
                    m_Source.EndGroup();
                }
                catch (Exception)
                {
                    // The document was closed under the view.
                }
            }
        }

        private void ResetCaretBlink()
        {
            m_CaretOn = true;
            m_CaretTimer.Stop();

            if (IsKeyboardFocused)
            {
                m_CaretTimer.Start();
            }

            InvalidateVisual();
        }

        private void OnStateChanged()
        {
            InvalidateVisual();
            StateChanged?.Invoke(this, EventArgs.Empty);
        }

        private void OnEdited()
        {
            InvalidateVisual();
            OnStateChanged();
            Edited?.Invoke(this, EventArgs.Empty);
        }

        // Rendering

        private Brush GetBrush(
            string key,
            Brush fallback
        )
        {
            return TryFindResource(key) as Brush ?? fallback;
        }

        private ushort Glyph(
            char character
        )
        {
            if (character < 256)
            {
                ushort cached = m_GlyphCache[character];

                if (cached != 0 || character == ' ')
                {
                    return cached;
                }
            }

            if (m_ExtraGlyphs.TryGetValue(character, out ushort glyph))
            {
                return glyph;
            }

            glyph = m_Typeface!.CharacterToGlyphMap.TryGetValue(character, out ushort found) ? found : m_GlyphCache['.'];
            m_ExtraGlyphs[character] = glyph;
            return glyph;
        }

        private sealed class RunBuilder
        {
            public readonly List<ushort> m_Glyphs = new(128);
            public readonly List<double> m_Positions = new(128);

            public void Add(
                ushort glyph,
                double x
            )
            {
                m_Glyphs.Add(glyph);
                m_Positions.Add(x);
            }

            public void Clear()
            {
                m_Glyphs.Clear();
                m_Positions.Clear();
            }
        }

        private void FlushRun(
            DrawingContext context,
            RunBuilder run,
            Brush brush,
            double baseline
        )
        {
            if (run.m_Glyphs.Count == 0 || m_Typeface == null)
            {
                run.Clear();
                return;
            }

            var advances = new double[run.m_Glyphs.Count];

            for (int index = 0; index < advances.Length; ++index)
            {
                advances[index] = index + 1 < advances.Length ? run.m_Positions[index + 1] - run.m_Positions[index] : m_CharWidth;
            }

            var glyph_run = new GlyphRun(m_Typeface, 0, false, EditorFontSize, (float)m_PixelsPerDip, run.m_Glyphs.ToArray(),
                                          new Point(run.m_Positions[0], baseline), advances, null, null, null, null, null, null);
            context.DrawGlyphRun(brush, glyph_run);
            run.Clear();
        }

        private static char DisplayChar(
            byte value
        )
        {
            return value >= 0x20 && value < 0x7F ? (char)value : value >= 0xA0 ? (char)value : '.';
        }

        protected override void OnRender(
            DrawingContext context
        )
        {
            Brush background = GetBrush("Brush.Editor.Background", Brushes.White);
            context.DrawRectangle(background, null, new Rect(0, 0, ActualWidth, ActualHeight));

            if (m_Typeface == null)
            {
                return;
            }

            Brush foreground = GetBrush("Brush.Editor.Foreground", Brushes.Black);
            Brush offset_brush = GetBrush("Brush.Editor.Offset", Brushes.Gray);
            Brush header_brush = GetBrush("Brush.Editor.Header", Brushes.Gray);
            Brush header_active = GetBrush("Brush.Editor.HeaderActive", Brushes.Blue);
            Brush zero_brush = GetBrush("Brush.Editor.Unreadable", Brushes.LightGray);
            Brush selection = GetBrush(IsKeyboardFocusWithin ? "Brush.Editor.Selection" : "Brush.Editor.InactiveSelection", Brushes.LightBlue);
            Brush line_highlight = GetBrush("Brush.Editor.LineHighlight", Brushes.Transparent);
            Brush caret_brush = GetBrush("Brush.Editor.Caret", Brushes.Black);
            Pen separator = new(GetBrush("Brush.Editor.Separator", Brushes.LightGray), 1.0 / m_PixelsPerDip);
            Pen outline = new(offset_brush, 1.0 / m_PixelsPerDip);

            double width = Math.Max(0, ActualWidth - ScrollBarSize);
            double height = Math.Max(0, ActualHeight - (m_HorizontalBar.Visibility == Visibility.Visible ? ScrollBarSize : 0));
            double header_height = HeaderHeight;

            context.PushClip(new RectangleGeometry(new Rect(0, 0, width, height)));
            context.PushTransform(new TranslateTransform(-m_ScrollX, 0));

            var normal_run = new RunBuilder();
            var dim_run = new RunBuilder();
            var accent_run = new RunBuilder();

            if (m_Source != null)
            {
                ulong begin = RegionBegin;
                ulong end = RegionEnd;
                int rows = m_VisibleRows + 1;
                ulong first_offset = (FirstRow + m_TopRow) * BYTES_PER_ROW;
                int count = (int)Math.Min((ulong)(rows * BYTES_PER_ROW), end > first_offset ? end - first_offset : 0);

                if (m_Bytes.Length < rows * BYTES_PER_ROW)
                {
                    m_Bytes = new byte[rows * BYTES_PER_ROW];
                }

                int read = 0;

                try
                {
                    read = count > 0 ? m_Source.Read(first_offset, m_Bytes.AsSpan(0, count)) : 0;
                }
                catch (Exception)
                {
                    read = 0;
                }

                ulong selection_start = SelectionStart;
                ulong selection_end = selection_start + SelectionLength;
                bool focused = IsKeyboardFocusWithin;

                for (int row = 0; row < rows; ++row)
                {
                    double y = header_height + row * m_LineHeight;

                    if (y > height)
                    {
                        break;
                    }

                    ulong row_offset = first_offset + (ulong)row * BYTES_PER_ROW;

                    if (row_offset >= end && !(row_offset == end && m_Caret == end) && row_offset > begin)
                    {
                        break;
                    }

                    double baseline = y + m_Baseline;
                    bool caret_row = m_Caret >= row_offset && m_Caret < row_offset + BYTES_PER_ROW;

                    if (caret_row)
                    {
                        context.DrawRectangle(line_highlight, null, new Rect(m_ScrollX, y, m_ContentWidth + width, m_LineHeight));
                    }

                    // Selection backgrounds.
                    for (int column = 0; column < BYTES_PER_ROW; ++column)
                    {
                        ulong offset = row_offset + (ulong)column;

                        if (offset < selection_start || offset >= selection_end || offset < begin || offset >= end)
                        {
                            continue;
                        }

                        bool joined = offset + 1 < selection_end && column + 1 < BYTES_PER_ROW;
                        double left = CellX(column, HexColumn.COLUMN_HEX);
                        double right = joined ? CellX(column + 1, HexColumn.COLUMN_HEX) : left + 2 * m_CharWidth;
                        context.DrawRectangle(selection, null, new Rect(left, y, right - left, m_LineHeight));
                        context.DrawRectangle(selection, null, new Rect(CellX(column, HexColumn.COLUMN_TEXT), y, m_CharWidth, m_LineHeight));
                    }

                    string offset_text = row_offset.ToString(m_OffsetDigits == 16 ? "X16" : "X8", CultureInfo.InvariantCulture);
                    RunBuilder offset_run = caret_row ? accent_run : dim_run;

                    for (int index = 0; index < offset_text.Length; ++index)
                    {
                        offset_run.Add(Glyph(offset_text[index]), PADDING + index * m_CharWidth);
                    }

                    FlushRun(context, offset_run, caret_row ? header_active : offset_brush, baseline);

                    for (int column = 0; column < BYTES_PER_ROW; ++column)
                    {
                        ulong offset = row_offset + (ulong)column;
                        int index = row * BYTES_PER_ROW + column;

                        if (offset < begin || offset >= end || index >= read)
                        {
                            continue;
                        }

                        byte value = m_Bytes[index];

                        // Selected bytes use the normal text color, the dim zero color disappears on the selection.
                        bool selected = offset >= selection_start && offset < selection_end;
                        RunBuilder run = value == 0 && !selected ? dim_run : normal_run;
                        double x = CellX(column, HexColumn.COLUMN_HEX);
                        run.Add(Glyph(HEX_DIGITS[value >> 4]), x);
                        run.Add(Glyph(HEX_DIGITS[value & 0x0F]), x + m_CharWidth);
                    }

                    FlushRun(context, normal_run, foreground, baseline);
                    FlushRun(context, dim_run, zero_brush, baseline);

                    // Text column in a second pass so every glyph run advances left to right.
                    for (int column = 0; column < BYTES_PER_ROW; ++column)
                    {
                        ulong offset = row_offset + (ulong)column;
                        int index = row * BYTES_PER_ROW + column;

                        if (offset < begin || offset >= end || index >= read)
                        {
                            continue;
                        }

                        byte value = m_Bytes[index];
                        bool selected = offset >= selection_start && offset < selection_end;
                        RunBuilder run = (value >= 0x20 && value != 0x7F) || selected ? normal_run : dim_run;
                        run.Add(Glyph(DisplayChar(value)), CellX(column, HexColumn.COLUMN_TEXT));
                    }

                    FlushRun(context, normal_run, foreground, baseline);
                    FlushRun(context, dim_run, zero_brush, baseline);

                    // Caret. Underline under the active nibble or character, outline in the other column.
                    if (caret_row)
                    {
                        int column = (int)(m_Caret - row_offset);

                        foreach (HexColumn kind in new[] { HexColumn.COLUMN_HEX, HexColumn.COLUMN_TEXT })
                        {
                            double x = CellX(column, kind);
                            double cell = kind == HexColumn.COLUMN_HEX ? 2 * m_CharWidth : m_CharWidth;

                            if (kind != ActiveColumn || !focused)
                            {
                                context.DrawRectangle(null, outline, new Rect(x + 0.5 / m_PixelsPerDip, y + 0.5 / m_PixelsPerDip, cell - 1 / m_PixelsPerDip, m_LineHeight - 1 / m_PixelsPerDip));
                            }
                            else if (m_CaretOn)
                            {
                                double nibble_x = kind == HexColumn.COLUMN_HEX && m_LowNibble ? x + m_CharWidth : x;
                                double thickness = Math.Max(1.0, Math.Round(2 * m_PixelsPerDip)) / m_PixelsPerDip;
                                context.DrawRectangle(caret_brush, null, new Rect(nibble_x, y + m_LineHeight - thickness - 1, m_CharWidth, thickness));
                            }
                        }
                    }
                }
            }

            // Header row stays on top while scrolling.
            context.DrawRectangle(background, null, new Rect(m_ScrollX, 0, width, header_height));
            double header_baseline = 2 + m_Baseline;

            // The labels must fit their columns, a longer translation keeps the English word.
            string OFFSET_LABEL = Tr("Offset").Length <= m_OffsetDigits ? Tr("Offset") : "Offset";

            for (int index = 0; index < OFFSET_LABEL.Length; ++index)
            {
                accent_run.Add(Glyph(OFFSET_LABEL[index]), PADDING + index * m_CharWidth);
            }

            FlushRun(context, accent_run, header_active, header_baseline);
            int caret_column = (int)(m_Caret % BYTES_PER_ROW);

            for (int column = 0; column < BYTES_PER_ROW; ++column)
            {
                string label = column.ToString("X2", CultureInfo.InvariantCulture);
                RunBuilder run = column == caret_column ? accent_run : normal_run;
                double x = CellX(column, HexColumn.COLUMN_HEX);
                run.Add(Glyph(label[0]), x);
                run.Add(Glyph(label[1]), x + m_CharWidth);
            }

            FlushRun(context, normal_run, header_brush, header_baseline);
            FlushRun(context, accent_run, header_active, header_baseline);
            string TEXT_LABEL = Tr("Text").Length <= BYTES_PER_ROW ? Tr("Text") : "Text";

            for (int index = 0; index < TEXT_LABEL.Length; ++index)
            {
                normal_run.Add(Glyph(TEXT_LABEL[index]), m_TextStart + index * m_CharWidth);
            }

            FlushRun(context, normal_run, header_brush, header_baseline);

            double line_y = Math.Round(header_height * m_PixelsPerDip) / m_PixelsPerDip - 0.5 / m_PixelsPerDip;
            context.DrawLine(separator, new Point(m_ScrollX, line_y), new Point(m_ScrollX + width, line_y));
            double offset_line = Math.Round((m_HexStart - m_CharWidth) * m_PixelsPerDip) / m_PixelsPerDip + 0.5 / m_PixelsPerDip;
            context.DrawLine(separator, new Point(offset_line, 0), new Point(offset_line, height));
            double text_line = Math.Round((m_TextStart - m_CharWidth) * m_PixelsPerDip) / m_PixelsPerDip + 0.5 / m_PixelsPerDip;
            context.DrawLine(separator, new Point(text_line, 0), new Point(text_line, height));

            context.Pop();
            context.Pop();
        }
    }
}