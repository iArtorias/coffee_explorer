using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Media;
using CoffeeExplorer.Services;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.Views.Dialogs
{
    // Options with 'OK' and 'Cancel'.
    public sealed class OptionsDialog : FormDialog
    {
        private readonly AppSettings m_Settings;
        private readonly ComboBox m_Theme;
        private readonly ComboBox m_Language;
        private readonly ComboBox m_UiFontFamily;
        private readonly TextBox m_UiFontSize;
        private readonly ComboBox m_FontFamily;
        private readonly TextBox m_FontSize;
        private readonly CheckBox m_AppData;
        private readonly CheckBox m_Backups;
        private readonly TextBox m_LargeFile;
        private readonly CheckBox m_SingleInstance;
        private readonly CheckBox m_Shell;
        private readonly CheckBox m_DieOnOpen;
        private readonly CheckBox m_DieDeep;
        private readonly CheckBox m_DieHeuristic;
        private readonly CheckBox m_DieRecursive;
        private readonly CheckBox m_DieAllTypes;
        private readonly CheckBox m_DieVerbose;

        public OptionsDialog(
            AppSettings settings,
            bool shell_registered
        )
            : base(Tr("Options"), Tr("OK"), 540)
        {
            m_Settings = settings.Clone();

            AddSection(Tr("Appearance"));
            m_Theme = AddThemeBox(m_Settings.Theme, m_Settings.CustomTheme);
            m_Language = AddLanguageBox(m_Settings.Language);
            m_UiFontFamily = AddFontBox(Tr("Application font"), m_Settings.UiFontFamily);
            m_UiFontSize = AddTextBox(Tr("Application font size"), m_Settings.UiFontSize.ToString(CultureInfo.InvariantCulture));
            m_FontFamily = AddFontBox(Tr("Hex font"), m_Settings.HexFontFamily);
            m_FontSize = AddTextBox(Tr("Hex font size"), m_Settings.HexFontSize.ToString(CultureInfo.InvariantCulture));

            AddSection(Tr("Files"));
            m_Backups = AddCheckBox(Tr("Keep a .bak copy of the original file when saving"), m_Settings.MakeBackups);
            m_LargeFile = AddTextBox(Tr("Ask before opening files larger than (MB, 0 never asks)"), m_Settings.LargeFileMegabytes.ToString());
            m_SingleInstance = AddCheckBox(Tr("Open files from Explorer in the running Coffee Explorer"), m_Settings.SingleInstance);
            m_AppData = AddCheckBox(Tr("Store settings in AppData (instead of next to the program)"), SettingsStore.UsesAppData);
            AddText(Tr("Settings file: {0}", SettingsStore.FilePath));

            AddSection(Tr("Explorer integration"));
            m_Shell = AddCheckBox(Tr("\"Open with Coffee Explorer\" in the context menu of every file"), shell_registered);
            AddText(Tr("Registered for the current user only, no administrator rights needed. On Windows 11 the entry is added to the compact " +
                         "context menu too: a small package signed with the certificate of this build, Windows asks once to trust it. " +
                         "Coffee Explorer also appears in the Open with list of .exe, .dll, .sys and the other PE extensions."));

            AddSection(Tr("Detect It Easy"));

            if (!DieService.IsAvailable)
            {
                AddText(Tr(DieService.UnavailableText));
            }

            m_DieOnOpen = AddCheckBox(Tr("Scan every opened file and show the result as File info"), m_Settings.Die.ScanOnOpen);
            m_DieDeep = AddCheckBox(Tr("Deep scan"), m_Settings.Die.DeepScan);
            m_DieHeuristic = AddCheckBox(Tr("Heuristic scan"), m_Settings.Die.HeuristicScan);
            m_DieRecursive = AddCheckBox(Tr("Recursive scan (overlay, resources, archives)"), m_Settings.Die.RecursiveScan);
            m_DieAllTypes = AddCheckBox(Tr("Scan all file types"), m_Settings.Die.AllTypes);
            m_DieVerbose = AddCheckBox(Tr("Verbose results"), m_Settings.Die.Verbose);
        }

        public AppSettings Result => m_Settings;

        // System, Light, Dark and the VS Code themes of the themes folder.
        private ComboBox AddThemeBox(
            ThemeMode mode,
            string? custom
        )
        {
            List<ThemeChoice> choices = ThemeService.Choices();
            var box = new ComboBox { ItemsSource = choices, DisplayMemberPath = nameof(ThemeChoice.Label) };
            box.SelectedItem = choices.FirstOrDefault(choice => choice.Mode == mode &&
                                                                 (mode != ThemeMode.THEME_CUSTOM || string.Equals(choice.Custom, custom, System.StringComparison.OrdinalIgnoreCase)))
                               ?? choices[0];
            AddField(Tr("Theme"), box);
            AddText(Tr("Custom themes are VS Code color themes (.jsonc or .json) in {0}", CustomThemes.FolderPath));
            return box;
        }

        // Installed translations (translations\*.lang next to the executable), english is always first.
        private ComboBox AddLanguageBox(
            string value
        )
        {
            IReadOnlyList<LanguageInfo> languages = Available();
            var box = new ComboBox { ItemsSource = languages, DisplayMemberPath = nameof(LanguageInfo.Name) };
            box.SelectedItem = languages.FirstOrDefault(language => string.Equals(language.Id, value, System.StringComparison.OrdinalIgnoreCase)) ?? languages[0];
            AddField(Tr("Language"), box);
            return box;
        }

        public bool StoreInAppData => m_AppData.IsChecked == true;

        // Editable list of the installed fonts, a typed fallback list ('Cascadia Mono, Consolas') is kept as it is.
        private ComboBox AddFontBox(
            string label,
            string value
        )
        {
            var names = Fonts.SystemFontFamilies
                .Select(family => family.FamilyNames.TryGetValue(XmlLanguage.GetLanguage("en-us"), out string? name) ? name : family.Source)
                .Distinct()
                .OrderBy(name => name, System.StringComparer.CurrentCultureIgnoreCase)
                .ToList();
            var box = new ComboBox { IsEditable = true, ItemsSource = names, Text = value, IsTextSearchEnabled = true };
            AddField(label, box);
            return box;
        }

        public bool ShellIntegration => m_Shell.IsChecked == true;

        protected override string? Validate()
        {
            if (!double.TryParse(m_UiFontSize.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out double ui_size) || ui_size < 8 || ui_size > 24)
            {
                return Tr("The application font size must be a number between 8 and 24");
            }

            if (!double.TryParse(m_FontSize.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out double size) || size < 6 || size > 48)
            {
                return Tr("The hex font size must be a number between 6 and 48");
            }

            if (!int.TryParse(m_LargeFile.Text, out int large) || large < 0)
            {
                return Tr("The file size limit must be a whole number of megabytes");
            }

            m_Settings.Language = (m_Language.SelectedItem as LanguageInfo)?.Id ?? ENGLISH;
            var theme = m_Theme.SelectedItem as ThemeChoice;
            m_Settings.Theme = theme?.Mode ?? ThemeMode.THEME_SYSTEM;
            m_Settings.CustomTheme = theme?.Custom;
            m_Settings.UiFontFamily = m_UiFontFamily.Text.Trim().Length > 0 ? m_UiFontFamily.Text.Trim() : AppSettings.DEFAULT_UI_FONT;
            m_Settings.UiFontSize = ui_size;
            m_Settings.HexFontFamily = m_FontFamily.Text.Trim().Length > 0 ? m_FontFamily.Text.Trim() : "Cascadia Mono, Consolas";
            m_Settings.HexFontSize = size;
            m_Settings.MakeBackups = m_Backups.IsChecked == true;
            m_Settings.LargeFileMegabytes = large;
            m_Settings.SingleInstance = m_SingleInstance.IsChecked == true;
            m_Settings.Die.ScanOnOpen = m_DieOnOpen.IsChecked == true;
            m_Settings.Die.DeepScan = m_DieDeep.IsChecked == true;
            m_Settings.Die.HeuristicScan = m_DieHeuristic.IsChecked == true;
            m_Settings.Die.RecursiveScan = m_DieRecursive.IsChecked == true;
            m_Settings.Die.AllTypes = m_DieAllTypes.IsChecked == true;
            m_Settings.Die.Verbose = m_DieVerbose.IsChecked == true;
            return null;
        }
    }
}