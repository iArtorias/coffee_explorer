using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Windows.Media;

namespace CoffeeExplorer.Services
{
    // A VS Code color theme from the themes folder next to the executable.
    public sealed class CustomTheme
    {
        public CustomTheme(
            string id,
            string name,
            bool is_dark,
            IReadOnlyDictionary<string, Color> colors
        )
        {
            Id = id;
            Name = name;
            IsDark = is_dark;
            Colors = colors;
        }

        // File name without extension, kept in the settings.
        public string Id { get; }

        public string Name { get; }

        // Light or dark base. Icons, the native dialogs and every color the theme does not define come from it.
        public bool IsDark { get; }

        // Resource keys without the Color. or Brush.
        public IReadOnlyDictionary<string, Color> Colors { get; }
    }

    // Entry of the theme lists in the View menu and the options.
    public sealed class ThemeChoice
    {
        public ThemeChoice(
            ThemeMode mode,
            string? custom,
            string label,
            bool is_checked
        )
        {
            Mode = mode;
            Custom = custom;
            Label = label;
            IsChecked = is_checked;
        }

        public ThemeMode Mode { get; }

        public string? Custom { get; }

        public string Label { get; }

        public bool IsChecked { get; }

        public override string ToString()
        {
            return Label;
        }
    }

    // Loads the themes folder.
    public static class CustomThemes
    {
        public const string FOLDER_NAME = "themes";
        private const string TRANSPARENT = "transparent";

        // Resource key, VS Code color keys, the first one present wins, "transparent" ends the chains high contrast themes
        // leave empty.
        private static readonly (string Key, string[] Chain)[] MAPPING =
        {
            ( "Window.Background", new[] { "titleBar.activeBackground", "sideBar.background" } ),
            ( "Window.Foreground", new[] { "foreground" } ),
            ( "Window.Border", new[] { "titleBar.border", "sideBar.border", "contrastBorder" } ),
            ( "Panel.Background", new[] { "sideBar.background", "panel.background" } ),
            ( "Panel.Border", new[] { "panel.border", "sideBar.border", "contrastBorder" } ),
            ( "Panel.HeaderForeground", new[] { "sideBarTitle.foreground", "descriptionForeground" } ),
            ( "Editor.Background", new[] { "editor.background" } ),
            ( "Editor.Foreground", new[] { "editor.foreground" } ),
            ( "Editor.LineHighlight", new[] { "editor.lineHighlightBackground", "transparent" } ),
            ( "Editor.Selection", new[] { "editor.selectionBackground" } ),
            ( "Editor.SelectionForeground", new[] { "editor.selectionForeground", "list.activeSelectionForeground", "foreground" } ),
            ( "Editor.InactiveSelection", new[] { "editor.inactiveSelectionBackground" } ),
            ( "Editor.Offset", new[] { "editorLineNumber.foreground" } ),
            ( "Editor.OffsetActive", new[] { "editorLineNumber.activeForeground" } ),
            ( "Editor.Header", new[] { "editorLineNumber.foreground" } ),
            ( "Editor.HeaderActive", new[] { "textLink.foreground", "focusBorder" } ),
            ( "Editor.Modified", new[] { "errorForeground", "editorError.foreground" } ),
            ( "Editor.OddColumn", new[] { "editor.foreground" } ),
            ( "Editor.Caret", new[] { "editorCursor.foreground", "editor.foreground" } ),
            ( "Editor.Unreadable", new[] { "disabledForeground" } ),
            ( "Editor.Separator", new[] { "editorIndentGuide.background1", "editorRuler.foreground" } ),
            ( "Editor.FindMatch", new[] { "editor.findMatchHighlightBackground", "editor.findMatchBackground" } ),
            ( "Editor.Difference", new[] { "diffEditor.removedTextBackground", "editor.findMatchBackground" } ),
            ( "Text.Foreground", new[] { "foreground" } ),
            ( "Text.Muted", new[] { "descriptionForeground" } ),
            ( "Text.Disabled", new[] { "disabledForeground" } ),
            ( "Text.Link", new[] { "textLink.foreground" } ),
            ( "Text.Error", new[] { "errorForeground" } ),
            ( "Accent.Background", new[] { "button.background" } ),
            ( "Accent.Foreground", new[] { "button.foreground" } ),
            ( "Accent.Hover", new[] { "button.hoverBackground", "button.background" } ),
            ( "Focus.Border", new[] { "focusBorder" } ),
            ( "Control.Background", new[] { "input.background" } ),
            ( "Control.Foreground", new[] { "input.foreground", "foreground" } ),
            ( "Control.Border", new[] { "dropdown.border", "input.border" } ),
            ( "Control.Placeholder", new[] { "input.placeholderForeground" } ),
            ( "Control.Hover", new[] { "toolbar.hoverBackground", "list.hoverBackground" } ),
            ( "Control.Pressed", new[] { "toolbar.activeBackground", "list.activeSelectionBackground", "transparent" } ),
            ( "Control.Checked", new[] { "inputOption.activeBackground", "list.activeSelectionBackground", "transparent" } ),
            ( "Button.Background", new[] { "button.secondaryBackground", "transparent" } ),
            ( "Button.Foreground", new[] { "button.secondaryForeground", "foreground" } ),
            ( "Button.Hover", new[] { "button.secondaryHoverBackground", "list.hoverBackground" } ),
            ( "Button.Border", new[] { "button.secondaryBorder", "dropdown.border" } ),
            ( "Menu.Background", new[] { "menu.background" } ),
            ( "Menu.Foreground", new[] { "menu.foreground", "foreground" } ),
            ( "Menu.Selection", new[] { "menu.selectionBackground", "transparent" } ),
            ( "Menu.SelectionForeground", new[] { "menu.selectionForeground", "menu.foreground", "foreground" } ),
            ( "Menu.Separator", new[] { "menu.separatorBackground" } ),
            ( "Menu.Border", new[] { "menu.border", "widget.border", "contrastBorder" } ),
            ( "MenuBar.Selection", new[] { "menubar.selectionBackground", "transparent" } ),
            ( "Tab.ActiveBackground", new[] { "tab.activeBackground" } ),
            ( "Tab.InactiveBackground", new[] { "tab.inactiveBackground", "transparent" } ),
            ( "Tab.ActiveForeground", new[] { "tab.activeForeground", "foreground" } ),
            ( "Tab.InactiveForeground", new[] { "tab.inactiveForeground", "foreground" } ),
            ( "Tab.Border", new[] { "tab.border" } ),
            ( "Tab.ActiveBorder", new[] { "tab.activeBorderTop", "focusBorder" } ),
            ( "Tab.Hover", new[] { "tab.hoverBackground", "list.hoverBackground" } ),
            ( "Tab.StripBackground", new[] { "editorGroupHeader.tabsBackground", "tab.inactiveBackground", "transparent" } ),
            ( "List.Hover", new[] { "list.hoverBackground" } ),
            ( "List.Selection", new[] { "list.activeSelectionBackground", "transparent" } ),
            ( "List.SelectionForeground", new[] { "list.activeSelectionForeground", "foreground" } ),
            ( "List.InactiveSelection", new[] { "list.inactiveSelectionBackground", "transparent" } ),
            ( "ScrollBar.Thumb", new[] { "scrollbarSlider.background" } ),
            ( "ScrollBar.ThumbHover", new[] { "scrollbarSlider.hoverBackground" } ),
            ( "ScrollBar.ThumbActive", new[] { "scrollbarSlider.activeBackground" } ),
            ( "StatusBar.Background", new[] { "statusBar.background", "transparent" } ),
            ( "StatusBar.Foreground", new[] { "statusBar.foreground" } ),
            ( "StatusBar.Border", new[] { "statusBar.border", "contrastBorder", "transparent" } ),
            ( "StatusBar.Hover", new[] { "statusBarItem.hoverBackground" } ),
            ( "Widget.Background", new[] { "editorWidget.background" } ),
            ( "Widget.Border", new[] { "editorWidget.border", "widget.border" } ),
            ( "ToolTip.Background", new[] { "editorHoverWidget.background", "editorWidget.background" } ),
            ( "ToolTip.Border", new[] { "editorHoverWidget.border", "widget.border" } ),
            ( "CheckBox.Background", new[] { "checkbox.background" } ),
            ( "CheckBox.Border", new[] { "checkbox.border" } ),
            ( "Badge.Background", new[] { "badge.background" } ),
            ( "Badge.Foreground", new[] { "badge.foreground" } ),
            ( "Chart.Primary", new[] { "charts.blue" } ),
            ( "Chart.Secondary", new[] { "charts.orange" } ),
            ( "Chart.Grid", new[] { "charts.lines" } ),
            ( "Warning.Foreground", new[] { "editorWarning.foreground", "charts.orange" } ),
            ( "Grid.Line", new[] { "sideBarSectionHeader.border", "panel.border" } ),
            ( "Grid.HeaderBackground", new[] { "sideBarSectionHeader.background", "sideBar.background" } ),
            ( "Tree.IndentGuide", new[] { "tree.indentGuidesStroke", "editorLineNumber.foreground" } ),
            ( "Text.Number", new[] { "debugTokenExpression.number", "charts.green" } ),
            ( "List.FocusBorder", new[] { "list.focusOutline", "focusBorder" } ),
            ( "Highlight.Background", new[] { "diffEditor.insertedTextBackground", "editor.findMatchHighlightBackground" } ),
            ( "Highlight.Marker", new[] { "charts.green", "editorGutter.addedBackground" } ),
            ( "Outline.Active", new[] { "contrastActiveBorder", "transparent" } ),
            ( "Outline.Border", new[] { "contrastBorder", "transparent" } ),
        };

        // Disassembly colors from the syntax colors (tokenColors) of the theme.
        private static readonly (string Key, string Scope)[] DISASSEMBLY_SCOPES =
        {
            ( "Mnemonic", "keyword.other.instruction" ),
            ( "PushPop", "storage.type.stack" ),
            ( "Call", "entity.name.function.call" ),
            ( "Ret", "keyword.control.return" ),
            ( "ConditionalJump", "keyword.control.conditional" ),
            ( "UnconditionalJump", "keyword.control.flow" ),
            ( "Nop", "comment.nop" ),
            ( "Far", "keyword.control.far" ),
            ( "Int3", "comment.int3" ),
            ( "Unusual", "invalid.illegal" ),
            ( "Prefix", "storage.modifier.prefix" ),
            ( "GeneralRegister", "variable.language.register" ),
            ( "FpuRegister", "variable.language.register.fpu" ),
            ( "MmxRegister", "variable.language.register.mmx" ),
            ( "XmmRegister", "variable.language.register.xmm" ),
            ( "YmmRegister", "variable.language.register.ymm" ),
            ( "ZmmRegister", "variable.language.register.zmm" ),
            ( "MaskRegister", "variable.language.register.mask" ),
            ( "SegmentRegister", "variable.language.register.segment" ),
            ( "MemoryBaseRegister", "variable.language.register.base" ),
            ( "MemoryIndexRegister", "variable.language.register.index" ),
            ( "Value", "constant.numeric.integer" ),
            ( "MemoryScale", "constant.numeric.scale" ),
            ( "Address", "string.unquoted.address" ),
            ( "MemorySize", "storage.type.size" ),
            ( "MemoryBrackets", "punctuation.definition.brackets" ),
            ( "MemoryStackBrackets", "punctuation.definition.brackets.stack" ),
            ( "MemoryOperator", "keyword.operator.arithmetic" ),
            ( "Comma", "punctuation.separator" ),
            ( "Decorator", "storage.modifier.decorator" ),
            ( "Column.Label", "entity.name.function" ),
            ( "Column.Comment", "comment.line" ),
        };

        // { "scope": "a, b" or [ "a", "b" ], "settings": { "foreground": "#..." } }, other members are ignored
        private static readonly Regex TOKEN_RULE = new(@"""scope""\s*:\s*(?<scope>\[[^\]]*\]|""[^""]*"")\s*,\s*""settings""\s*:\s*\{[^{}]*?""foreground""\s*:\s*""(?<color>#[0-9a-fA-F]{3,8})""",
                                                        RegexOptions.CultureInvariant);
        private static readonly Regex QUOTED = new(@"""(?<text>[^""]*)""", RegexOptions.CultureInvariant);

        // "key": "#color", commented out ("//"key": ...) for the defaults of a generated theme.
        private static readonly Regex COLOR_LINE = new(@"^\s*(?<comment>//)?\s*""(?<key>[\w.\-]+)""\s*:\s*""(?<value>#[0-9a-fA-F]{3,8})""", RegexOptions.Multiline | RegexOptions.CultureInvariant);
        private static readonly Regex TYPE_LINE = new(@"""type""\s*:\s*""(?<value>[\w]+)""", RegexOptions.CultureInvariant);
        private static readonly Regex NAME_LINE = new(@"""name""\s*:\s*""(?<value>[^""]+)""", RegexOptions.CultureInvariant);

        private static readonly Dictionary<string, CustomTheme> s_Cache = new(StringComparer.OrdinalIgnoreCase);

        public static string FolderPath => Path.Combine(AppContext.BaseDirectory, FOLDER_NAME);

        // Themes of the folder, sorted by name. a broken file is left out and written to the log.
        public static IReadOnlyList<CustomTheme> Available()
        {
            var list = new List<CustomTheme>();

            try
            {
                if (!Directory.Exists(FolderPath))
                {
                    return list;
                }

                foreach (string path in Directory.GetFiles(FolderPath).Where(IsThemeFile))
                {
                    if (TryLoad(Path.GetFileNameWithoutExtension(path), out CustomTheme? theme, out _))
                    {
                        list.Add(theme!);
                    }
                }
            }
            catch (Exception error)
            {
                Log.Error(error);
            }

            return list.OrderBy(theme => theme.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        }

        public static bool TryLoad(
            string id,
            out CustomTheme? theme,
            out string? error
        )
        {
            theme = null;
            error = null;

            string? path = new[] { ".jsonc", ".json" }.Select(extension => Path.Combine(FolderPath, id + extension)).FirstOrDefault(File.Exists);

            if (path == null)
            {
                error = LanguageService.Tr("The theme file {0} was not found in {1}", id, FolderPath);
                return false;
            }

            try
            {
                lock (s_Cache)
                {
                    if (s_Cache.TryGetValue(path, out CustomTheme? cached))
                    {
                        theme = cached;
                        return true;
                    }
                }

                theme = Parse(id, File.ReadAllText(path));
            }
            catch (Exception failure)
            {
                error = failure.Message;
                Log.Error(failure);
                return false;
            }

            if (theme == null)
            {
                error = LanguageService.Tr("{0} has no \"colors\" of a VS Code color theme", Path.GetFileName(path));
                return false;
            }

            lock (s_Cache)
            {
                s_Cache[path] = theme;
            }

            return true;
        }

        // Forgets the parsed files, the next load reads them again (edited themes).
        public static void Reload()
        {
            lock (s_Cache)
            {
                s_Cache.Clear();
            }
        }

        private static bool IsThemeFile(
            string path
        )
        {
            string extension = Path.GetExtension(path);
            return extension.Equals(".jsonc", StringComparison.OrdinalIgnoreCase) || extension.Equals(".json", StringComparison.OrdinalIgnoreCase);
        }

        // Not a JSON parser on purpose. Generated themes have comments, trailing commas and sometimes broken 'tokenColors'
        // entries. only the "colors" object matters here.
        private static CustomTheme? Parse(
            string id,
            string text
        )
        {
            int colors_start = text.IndexOf("\"colors\"", StringComparison.Ordinal);

            if (colors_start < 0)
            {
                return null;
            }

            int colors_end = text.IndexOf("\"tokenColors\"", colors_start, StringComparison.Ordinal);
            string header = text.Substring(0, colors_start);
            string body = colors_end < 0 ? text.Substring(colors_start) : text.Substring(colors_start, colors_end - colors_start);

            // Explicit colors win over the commented defaults.
            var source = new Dictionary<string, string>(StringComparer.Ordinal);

            foreach (Match match in COLOR_LINE.Matches(body))
            {
                string key = match.Groups["key"].Value;
                bool comment = match.Groups["comment"].Success;

                if (!comment || !source.ContainsKey(key))
                {
                    source[key] = match.Groups["value"].Value;
                }
            }

            if (source.Count == 0)
            {
                return null;
            }

            var colors = new Dictionary<string, Color>(StringComparer.Ordinal);

            foreach ((string key, string[] chain) in MAPPING)
            {
                string? value = chain.Where(name => name == TRANSPARENT || source.ContainsKey(name))
                                     .Select(name => name == TRANSPARENT ? "#00000000" : source[name])
                                     .FirstOrDefault();

                if (value == null || !TryParseColor(value, out Color color))
                {
                    // Not defined. The light or dark base keeps its own color.
                    continue;
                }

                // A faint diff tint would not make the highlighted row (entry point) stand out.
                if (key == "Highlight.Background" && color.A < 0x40)
                {
                    color.A = 0x40;
                }

                colors[key] = color;
            }

            if (colors_end >= 0)
            {
                AddDisassemblyColors(text.Substring(colors_end), colors);
            }

            string type = TYPE_LINE.Match(header) is { Success: true } type_match ? type_match.Groups["value"].Value : string.Empty;
            bool dark = type.Length > 0
                ? !type.StartsWith("light", StringComparison.OrdinalIgnoreCase) && !type.Equals("hcLight", StringComparison.OrdinalIgnoreCase)
                : colors.TryGetValue("Editor.Background", out Color background) && Luminance(background) < 0.5;

            string name = NAME_LINE.Match(header) is { Success: true } name_match ? name_match.Groups["value"].Value : NameFromFile(id);
            return new CustomTheme(id, name, dark, colors);
        }

        private static void AddDisassemblyColors(
            string token_colors,
            Dictionary<string, Color> colors
        )
        {
            // Rules in file order, a later rule wins over an earlier one of the same scope like in VS Code.
            var rules = new List<(string Scope, Color Color)>();

            foreach (Match match in TOKEN_RULE.Matches(token_colors))
            {
                if (!TryParseColor(match.Groups["color"].Value, out Color color))
                {
                    continue;
                }

                string scopes = match.Groups["scope"].Value;
                IEnumerable<string> names = scopes.StartsWith("[", StringComparison.Ordinal)
                    ? QUOTED.Matches(scopes).Select(item => item.Groups["text"].Value)
                    : scopes.Trim('"').Split(',');

                foreach (string name in names)
                {
                    string scope = name.Trim();

                    // Descendant selectors ("string meta.image") never describe a disassembly token.
                    if (scope.Length > 0 && !scope.Contains(' '))
                    {
                        rules.Add((scope, color));
                    }
                }
            }

            foreach ((string key, string scope) in DISASSEMBLY_SCOPES)
            {
                int best_length = -1;
                Color best = default;

                foreach ((string rule, Color color) in rules)
                {
                    bool matches = scope == rule || scope.StartsWith(rule + ".", StringComparison.Ordinal);

                    if (matches && rule.Length >= best_length)
                    {
                        best_length = rule.Length;
                        best = color;
                    }
                }

                if (best_length >= 0)
                {
                    colors["Disasm." + key] = best;
                }
            }
        }

        private static string NameFromFile(
            string id
        )
        {
            string name = id.StartsWith("vs_", StringComparison.OrdinalIgnoreCase) ? id.Substring(3) : id;
            string[] words = name.Split(new[] { '_', '-', ' ' }, StringSplitOptions.RemoveEmptyEntries);
            return words.Length == 0 ? id : string.Join(" ", words.Select(word => char.ToUpperInvariant(word[0]) + word.Substring(1)));
        }

        private static double Luminance(
            Color color
        )
        {
            return (0.299 * color.R + 0.587 * color.G + 0.114 * color.B) / 255.0;
        }

        // #RGB, #RGBA, #RRGGBB, #RRGGBBAA (VS Code puts alpha last, WPF first).
        private static bool TryParseColor(
            string text,
            out Color color
        )
        {
            color = default;
            string value = text.TrimStart('#');

            if (value.Length == 3 || value.Length == 4)
            {
                value = string.Concat(value.Select(character => new string(character, 2)));
            }

            if (value.Length == 6)
            {
                value += "FF";
            }

            if (value.Length != 8 || !uint.TryParse(value, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out uint rgba))
            {
                return false;
            }

            color = Color.FromArgb((byte)rgba, (byte)(rgba >> 24), (byte)(rgba >> 16), (byte)(rgba >> 8));
            return true;
        }
    }
}