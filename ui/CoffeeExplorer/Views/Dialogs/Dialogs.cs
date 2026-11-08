using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using CoffeeExplorer.Services;
using Microsoft.Win32;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.Views.Dialogs
{
    public static class FileDialogs
    {
        public static string? Open(
            string title,
            string filter
        )
        {
            var dialog = new OpenFileDialog { Title = title, Filter = filter, CheckFileExists = true };
            return dialog.ShowDialog(Application.Current?.MainWindow) == true ? dialog.FileName : null;
        }

        public static string[] OpenMany(
            string title,
            string filter
        )
        {
            var dialog = new OpenFileDialog { Title = title, Filter = filter, CheckFileExists = true, Multiselect = true };
            return dialog.ShowDialog(Application.Current?.MainWindow) == true ? dialog.FileNames : Array.Empty<string>();
        }

        public static string? Save(
            string title,
            string file_name,
            string filter,
            string? directory = null
        )
        {
            var dialog = new SaveFileDialog { Title = title, FileName = file_name, Filter = filter, OverwritePrompt = true };

            if (directory != null && Directory.Exists(directory))
            {
                dialog.InitialDirectory = directory;
            }

            return dialog.ShowDialog(Application.Current?.MainWindow) == true ? dialog.FileName : null;
        }
    }

    // Add section. Name, size, characteristics as checkboxes, a file for 'file data'.
    public sealed class AddSectionDialog : FormDialog
    {
        private readonly TextBox m_Name;
        private readonly TextBox? m_Size;
        private readonly TextBox? m_File;
        private readonly List<(CheckBox Box, uint Flag)> m_Flags = new();

        public AddSectionDialog(
            bool with_file,
            bool header_only
        )
            : base(with_file ? Tr("Add section (file data)") : header_only ? Tr("Add section (header only)") : Tr("Add section (empty space)"), Tr("Add"), 480)
        {
            AddText(with_file
                ? Tr("Appends a section with the contents of a file after the last section.")
                : header_only
                    ? Tr("Appends a section header without raw data. The loader reserves the virtual size as zeroed memory.")
                    : Tr("Appends a section filled with zeros after the last section."));

            m_Name = AddTextBox(Tr("Name (up to 8 characters)"), ".new");
            m_Name.MaxLength = 8;

            if (with_file)
            {
                m_File = new TextBox();
                var browse = MakeBrowseButton((_, _) =>
                {
                    string? path = FileDialogs.Open(Tr("Section data"), Tr("All files") + " (*.*)|*.*");

                    if (path != null)
                    {
                        m_File.Text = path;
                    }
                });
                AddField(Tr("File"), MakeRow((m_File, new GridLength(1, GridUnitType.Star)), (browse, GridLength.Auto)));
            }
            else
            {
                m_Size = AddTextBox(header_only ? Tr("Virtual size (hex)") : Tr("Size (hex)"), "1000", true);
            }

            AddSection(Tr("Characteristics"));
            var grid = new CheckGrid(2);
            uint defaults = with_file ? 0x40000040u : 0xC0000040u;

            foreach ((string label, uint flag) in new[]
            {
                ("Code (CNT_CODE)", 0x00000020u),
                ("Initialized data", 0x00000040u),
                ("Uninitialized data", 0x00000080u),
                ("Discardable", 0x02000000u),
                ("Shared", 0x10000000u),
                ("Execute", 0x20000000u),
                ("Read", 0x40000000u),
                ("Write", 0x80000000u)
            })
            {
                var box = new CheckBox { Content = Tr(label), IsChecked = (defaults & flag) != 0, Margin = new Thickness(0, 0, 12, 6) };
                m_Flags.Add((box, flag));
                grid.Add(box);
            }

            Add(grid.Panel);
        }

        public string SectionName => m_Name.Text.Trim();

        public ulong SectionSize { get; private set; }

        public uint Characteristics => m_Flags.Where(item => item.Box.IsChecked == true).Aggregate(0u, (value, item) => value | item.Flag);

        public string? FilePath => m_File?.Text.Trim();

        protected override string? Validate()
        {
            if (SectionName.Length == 0 || SectionName.Length > 8 || SectionName.Any(character => character > 0x7E || character < 0x20))
            {
                return Tr("The name must be 1 to 8 printable ASCII characters");
            }

            if (m_File != null)
            {
                return File.Exists(m_File.Text.Trim()) ? null : Tr("Choose an existing file");
            }

            if (!TryParseNumber(m_Size!.Text, out ulong size) || size == 0 || size > 0x7FFFFFFF)
            {
                return Tr("The size must be a hex number between 1 and 7FFFFFFF");
            }

            SectionSize = size;
            return null;
        }
    }

    // Add resource (raw). Type, name, language and the data file.
    public sealed class AddResourceDialog : FormDialog
    {
        private readonly ComboBox m_Type;
        private readonly TextBox m_Name;
        private readonly TextBox m_Language;
        private readonly TextBox m_File;

        private static readonly (string Name, int Id)[] TYPES =
        {
            ("Cursor", 1), ("Bitmap", 2), ("Icon", 3), ("Menu", 4), ("Dialog", 5), ("String table", 6), ("Font directory", 7), ("Font", 8),
            ("Accelerators", 9), ("RCData", 10), ("Message table", 11), ("Cursor group", 12), ("Icon group", 14), ("Version info", 16),
            ("Dialog include", 17), ("Plug and Play", 19), ("VxD", 20), ("Animated cursor", 21), ("Animated icon", 22), ("HTML", 23), ("Manifest", 24)
        };

        public AddResourceDialog()
            : base(Tr("Add resource (raw)"), Tr("Add"), 480)
        {
            AddText(Tr("Adds the bytes of a file as a resource. A resource with the same type, name and language is replaced. Pick a standard type or type a name for a custom type."));

            m_Type = new ComboBox { IsEditable = true };

            foreach ((string name, int id) in TYPES)
            {
                m_Type.Items.Add($"{id} - {Tr(name)}");
            }

            m_Type.Text = $"10 - {Tr("RCData")}";
            AddField(Tr("Type"), m_Type);
            m_Name = AddTextBox(Tr("Name (decimal id or a string)"), "1");
            m_Language = AddTextBox(Tr("Language (decimal, 1033 = English (United States), 0 = neutral)"), "1033");

            m_File = new TextBox();
            var browse = MakeBrowseButton((_, _) =>
            {
                string? path = FileDialogs.Open(Tr("Resource data"), Tr("All files") + " (*.*)|*.*");

                if (path != null)
                {
                    m_File.Text = path;
                }
            });

            AddField(Tr("File"), MakeRow((m_File, new GridLength(1, GridUnitType.Star)), (browse, GridLength.Auto)));
        }

        // Decimal id as a number, anything else as a string name.
        public object ResourceType
        {
            get
            {
                string text = m_Type.Text.Trim();
                int dash = text.IndexOf(" - ", StringComparison.Ordinal);
                string head = dash > 0 ? text.Substring(0, dash) : text;
                return uint.TryParse(head, out uint id) ? id : text;
            }
        }

        public object ResourceName => uint.TryParse(m_Name.Text.Trim(), out uint id) ? id : m_Name.Text.Trim();

        public uint LanguageId => uint.TryParse(m_Language.Text.Trim(), out uint language) ? language : 1033;

        public string FilePath => m_File.Text.Trim();

        protected override string? Validate()
        {
            if (m_Type.Text.Trim().Length == 0)
            {
                return Tr("Choose or type a resource type");
            }

            if (m_Name.Text.Trim().Length == 0)
            {
                return Tr("Type a name or an id");
            }

            if (ResourceName is uint id && id > 0xFFFF)
            {
                return Tr("Resource ids are 16 bit (0 to 65535)");
            }

            if (!uint.TryParse(m_Language.Text.Trim(), out uint language) || language > 0xFFFF)
            {
                return Tr("The language is a decimal number up to 65535");
            }

            return File.Exists(FilePath) ? null : Tr("Choose an existing file");
        }
    }

    // Go to a file offset in the hex editor.
    public sealed class GoToDialog : FormDialog
    {
        private readonly TextBox m_Offset;

        public GoToDialog()
            : base(Tr("Go to offset"), Tr("Go to"), 360)
        {
            m_Offset = AddTextBox(Tr("File offset (hex, add \" d\" for decimal)"), string.Empty, true);
        }

        public ulong Offset { get; private set; }

        protected override string? Validate()
        {
            if (!TryParseNumber(m_Offset.Text, out ulong offset))
            {
                return Tr("Type a hex offset such as 400 or 0x1000, or a decimal one such as 1024 d");
            }

            Offset = offset;
            return null;
        }
    }

    // Fill a range of the hex view with a byte pattern.
    public sealed class FillDialog : FormDialog
    {
        private readonly TextBox m_Pattern;
        private readonly TextBox m_Start;
        private readonly TextBox m_Length;

        public FillDialog(
            ulong start,
            ulong length
        )
            : base(Tr("Fill"), Tr("Fill"), 380)
        {
            m_Start = AddTextBox(Tr("Start offset (hex)"), Format.Offset(start), true);
            m_Length = AddTextBox(Tr("Length (hex)"), length.ToString("X"), true);
            m_Pattern = AddTextBox(Tr("Pattern (hex bytes, repeated)"), "00", true);
        }

        public byte[] Pattern { get; private set; } = Array.Empty<byte>();

        public ulong Start { get; private set; }

        public ulong Length { get; private set; }

        protected override string? Validate()
        {
            if (!TryParseNumber(m_Start.Text, out ulong start))
            {
                return Tr("The start offset is not a hex number");
            }

            if (!TryParseNumber(m_Length.Text, out ulong length) || length == 0 || length > 0x10000000)
            {
                return Tr("The length must be a hex number between 1 and 10000000");
            }

            if (!Format.TryParseHexBytes(m_Pattern.Text, out byte[] pattern) || pattern.Length == 0)
            {
                return Tr("The pattern must be hex bytes such as 00 or 90 CC");
            }

            Start = start;
            Length = length;
            Pattern = pattern;
            return null;
        }
    }

    public sealed class AboutDialog : DialogWindow
    {
        public AboutDialog()
        {
            Title = Tr("About Coffee Explorer");
            Width = 460;
            SizeToContent = SizeToContent.Height;

            string version = Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "1.0.0";
            var panel = new StackPanel { Margin = new Thickness(24, 22, 24, 12) };

            var header = new DockPanel { Margin = new Thickness(0, 0, 0, 14) };
            var image = new Image { Width = 40, Height = 40, Margin = new Thickness(0, 0, 14, 0) };
            image.SetResourceReference(Image.SourceProperty, "Image.AppLogo");
            header.Children.Add(image);
            var title = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            title.Children.Add(new TextBlock { Text = "Coffee Explorer", FontSize = 20, FontWeight = FontWeights.SemiBold });
            var subtitle = new TextBlock { Text = Tr("Version {0}, PE editor", version) };
            subtitle.SetResourceReference(TextBlock.ForegroundProperty, "Brush.Text.Muted");
            title.Children.Add(subtitle);
            header.Children.Add(title);
            panel.Children.Add(header);

            AddLine(panel, Tr("A modern re-creation of CFF Explorer by Daniel Pistelli (NTCore)."));
            AddLine(panel, Tr("Core: pecore {0}, PE parsing by PeLib (RetDec fork, MIT).", PeCoreVersion()));
            AddLine(panel, DieService.IsAvailable ? Tr("Signatures: Detect It Easy by hors (MIT), installed.") : Tr("Signatures: Detect It Easy by hors (MIT), not installed."));
            AddLine(panel, Tr("Disassembly: Zydis (MIT). Icons: Visual Studio 2026 Image Library."));

            var link = new Hyperlink(new Run(SettingsStore.FilePath));
            link.Click += (_, _) =>
            {
                string? folder = Path.GetDirectoryName(SettingsStore.FilePath);

                if (folder != null && Directory.Exists(folder))
                {
                    Process.Start(new ProcessStartInfo(folder) { UseShellExecute = true });
                }
            };

            var settings = new TextBlock { Margin = new Thickness(0, 8, 0, 0), TextWrapping = TextWrapping.Wrap };
            settings.Inlines.Add(new Run(Tr("Settings:") + " "));
            settings.Inlines.Add(link);
            panel.Children.Add(settings);

            var ok = new Button { Content = Tr("OK"), IsDefault = true, IsCancel = true, HorizontalAlignment = HorizontalAlignment.Right };
            ok.Click += (_, _) => DialogResult = true;
            var footer = new Border { Padding = new Thickness(20, 12, 20, 12), Child = ok, BorderThickness = new Thickness(0, 1, 0, 0) };
            footer.SetResourceReference(Border.BackgroundProperty, "Brush.Panel.Background");
            footer.SetResourceReference(Border.BorderBrushProperty, "Brush.Panel.Border");

            var root = new DockPanel();
            DockPanel.SetDock(footer, Dock.Bottom);
            root.Children.Add(footer);
            root.Children.Add(panel);
            Content = root;
        }

        private static string PeCoreVersion()
        {
            try
            {
                uint version = Interop.PeDocument.CoreVersion;
                return $"{version >> 16}.{(version >> 8) & 0xFF}.{version & 0xFF}";
            }
            catch (Exception)
            {
                return "unknown";
            }
        }

        private static void AddLine(
            Panel panel,
            string text
        )
        {
            var block = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 6) };
            panel.Children.Add(block);
        }
    }

    // Grid of check boxes filled row by row.
    internal sealed class CheckGrid
    {
        private readonly int m_Columns;
        private int m_Count;

        public CheckGrid(
            int columns
        )
        {
            m_Columns = columns;

            for (int index = 0; index < columns; ++index)
            {
                Panel.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            }
        }

        public Grid Panel { get; } = new() { Margin = new Thickness(0, 0, 0, 8) };

        public void Add(
            UIElement element
        )
        {
            if (m_Count % m_Columns == 0)
            {
                Panel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            }

            Grid.SetRow(element, m_Count / m_Columns);
            Grid.SetColumn(element, m_Count % m_Columns);
            Panel.Children.Add(element);
            ++m_Count;
        }
    }
}