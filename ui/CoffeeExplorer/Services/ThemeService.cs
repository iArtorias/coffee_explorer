using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using Microsoft.Win32;

namespace CoffeeExplorer.Services
{
    // Swaps the light or dark color and icon dictionaries and keeps every window title bar in sync.
    // In system mode the theme follows the Windows app mode live. A custom theme is its light or dark base
    // with the colors of the VS Code theme laid over it.
    public static class ThemeService
    {
        private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
        private const int DWMWA_BORDER_COLOR = 34;
        private const int DWMWA_CAPTION_COLOR = 35;
        private const int DWMWA_TEXT_COLOR = 36;
        private static ResourceDictionary? m_Colors;
        private static ResourceDictionary? m_Icons;
        private static ThemeMode m_Mode = ThemeMode.THEME_SYSTEM;

        public static bool IsDark { get; private set; }

        // The custom theme in use, null for system, light and dark.
        public static CustomTheme? Custom { get; private set; }

        public static ThemeMode Mode => m_Mode;

        // Built in modes and the themes of the themes folder, the current one checked. 'custom' false leaves the folder
        // unread (startup, the folder is parsed in the background).
        public static List<ThemeChoice> Choices(
            bool custom = true
        )
        {
            var list = new List<ThemeChoice>
            {
                new( ThemeMode.THEME_SYSTEM, null, LanguageService.Tr( "Use system setting" ), m_Mode == ThemeMode.THEME_SYSTEM ),
                new( ThemeMode.THEME_LIGHT, null, LanguageService.Tr( "Light" ), m_Mode == ThemeMode.THEME_LIGHT ),
                new( ThemeMode.THEME_DARK, null, LanguageService.Tr( "Dark" ), m_Mode == ThemeMode.THEME_DARK )
            };

            // The current custom theme is listed right away, it is loaded already.
            if (!custom)
            {
                if (m_Mode == ThemeMode.THEME_CUSTOM && Custom != null)
                {
                    list.Add(new ThemeChoice(ThemeMode.THEME_CUSTOM, Custom.Id, Custom.Name, true));
                }

                return list;
            }

            foreach (CustomTheme theme in CustomThemes.Available())
            {
                bool current = m_Mode == ThemeMode.THEME_CUSTOM && string.Equals(Custom?.Id, theme.Id, StringComparison.OrdinalIgnoreCase);
                list.Add(new ThemeChoice(ThemeMode.THEME_CUSTOM, theme.Id, theme.Name, current));
            }

            return list;
        }

        // Name for the status bar. The custom theme, light or dark.
        public static string DisplayName => Custom?.Name ?? (IsDark ? LanguageService.Tr("Dark") : LanguageService.Tr("Light"));

        public static event EventHandler? ThemeChanged;

        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(
            IntPtr hwnd,
            int attribute,
            ref int value,
            int size
        );

        // uxtheme exports by ordinal (Windows 10 1903 and later). The common Open and Save dialogs and native menus
        // of the process follow this mode.
        private const int PREFERRED_APP_MODE_FORCE_DARK = 2;
        private const int PREFERRED_APP_MODE_FORCE_LIGHT = 3;

        [DllImport("uxtheme.dll", EntryPoint = "#135")]
        private static extern int SetPreferredAppMode(
            int mode
        );

        [DllImport("uxtheme.dll", EntryPoint = "#136")]
        private static extern void FlushMenuThemes();

        private static void ApplyNativeMode(
            bool dark
        )
        {
            try
            {
                SetPreferredAppMode(dark ? PREFERRED_APP_MODE_FORCE_DARK : PREFERRED_APP_MODE_FORCE_LIGHT);
                FlushMenuThemes();
            }
            catch (Exception error) when (error is EntryPointNotFoundException || error is DllNotFoundException)
            {
                // Older Windows, the system dialogs stay light.
            }
        }

        public static void Initialize(
            ThemeMode mode,
            string? custom = null
        )
        {
            SystemEvents.UserPreferenceChanged += OnUserPreferenceChanged;

            // Every window gets its title bar themed once it has a handle.
            EventManager.RegisterClassHandler(typeof(Window), FrameworkElement.LoadedEvent, new RoutedEventHandler((sender, _) => ApplyTitleBar((Window)sender)));

            if (Apply(mode, custom) is string error)
            {
                Log.Write($"Theme {custom}: {error}");
            }
        }

        // Returns null, or why the custom theme could not be used (system is applied instead).
        public static string? Apply(
            ThemeMode mode,
            string? custom = null
        )
        {
            CustomTheme? theme = null;
            string? error = null;

            if (mode == ThemeMode.THEME_CUSTOM && (string.IsNullOrWhiteSpace(custom) || !CustomThemes.TryLoad(custom, out theme, out error)))
            {
                error ??= LanguageService.Tr("No custom theme is selected");
                mode = ThemeMode.THEME_SYSTEM;
                theme = null;
            }

            m_Mode = mode;
            bool dark = theme?.IsDark ?? (mode == ThemeMode.THEME_DARK || (mode == ThemeMode.THEME_SYSTEM && IsSystemDark()));

            // Same base and the same loaded theme (a reloaded file is a new object). Nothing to do.
            if (m_Colors != null && dark == IsDark && ReferenceEquals(theme, Custom))
            {
                return error;
            }

            Custom = theme;
            IsDark = dark;
            ApplyNativeMode(dark);

            var dictionaries = Application.Current.Resources.MergedDictionaries;
            var colors = new ResourceDictionary { Source = new Uri(dark ? "Themes/Dark.xaml" : "Themes/Light.xaml", UriKind.Relative) };

            // Disassembly token colors of the x64dbg themes, a custom theme keeps its base.
            colors.MergedDictionaries.Add(new ResourceDictionary { Source = new Uri(dark ? "Themes/DisassemblyDark.xaml" : "Themes/DisassemblyLight.xaml", UriKind.Relative) });

            if (theme != null)
            {
                foreach (KeyValuePair<string, Color> pair in theme.Colors)
                {
                    var brush = new SolidColorBrush(pair.Value);
                    brush.Freeze();
                    colors["Color." + pair.Key] = pair.Value;
                    colors["Brush." + pair.Key] = brush;
                }
            }

            var icons = new ResourceDictionary { Source = new Uri(dark ? "Themes/IconsDark.xaml" : "Themes/IconsLight.xaml", UriKind.Relative) };

            // Colors and icons sit in front of 'Controls.xaml', the styles reference them dynamically.
            if (m_Colors != null)
            {
                dictionaries.Remove(m_Colors);
            }

            if (m_Icons != null)
            {
                dictionaries.Remove(m_Icons);
            }

            dictionaries.Insert(0, colors);
            dictionaries.Insert(1, icons);
            m_Colors = colors;
            m_Icons = icons;

            foreach (Window window in Application.Current.Windows)
            {
                ApplyTitleBar(window);
            }

            ThemeChanged?.Invoke(null, EventArgs.Empty);
            return error;
        }

        // Interface font from the options. Every style and window reads Font.Ui and Font.Size as dynamic resources.
        public static void ApplyFont(
            string family,
            double size
        )
        {
            ResourceDictionary resources = Application.Current.Resources;

            try
            {
                resources["Font.Ui"] = new FontFamily(string.IsNullOrWhiteSpace(family) ? AppSettings.DEFAULT_UI_FONT : family);
            }
            catch (Exception)
            {
                resources["Font.Ui"] = new FontFamily(AppSettings.DEFAULT_UI_FONT);
            }

            resources["Font.Size"] = Math.Clamp(size, 8.0, 24.0);
        }

        public static bool IsSystemDark()
        {
            try
            {
                using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
                return key?.GetValue("AppsUseLightTheme") is int value && value == 0;
            }
            catch (Exception)
            {
                return false;
            }
        }

        public static void ApplyTitleBar(
            Window window
        )
        {
            IntPtr handle = new WindowInteropHelper(window).Handle;

            if (handle == IntPtr.Zero)
            {
                return;
            }

            int dark = IsDark ? 1 : 0;
            DwmSetWindowAttribute(handle, DWMWA_USE_IMMERSIVE_DARK_MODE, ref dark, sizeof(int));

            // Windows 11 lets the caption match the window background like Visual Studio, older builds ignore it.
            if (Application.Current.TryFindResource("Color.Window.Background") is Color caption &&
                 Application.Current.TryFindResource("Color.Window.Foreground") is Color text &&
                 Application.Current.TryFindResource("Color.Window.Border") is Color border)
            {
                int caption_value = ToColorRef(caption);
                int text_value = ToColorRef(text);
                int border_value = ToColorRef(border);
                DwmSetWindowAttribute(handle, DWMWA_CAPTION_COLOR, ref caption_value, sizeof(int));
                DwmSetWindowAttribute(handle, DWMWA_TEXT_COLOR, ref text_value, sizeof(int));
                DwmSetWindowAttribute(handle, DWMWA_BORDER_COLOR, ref border_value, sizeof(int));
            }
        }

        private static int ToColorRef(
            Color color
        )
        {
            return color.R | (color.G << 8) | (color.B << 16);
        }

        private static void OnUserPreferenceChanged(
            object sender,
            UserPreferenceChangedEventArgs arguments
        )
        {
            if (m_Mode != ThemeMode.THEME_SYSTEM)
            {
                return;
            }

            // The event arrives on a system thread.
            Application.Current?.Dispatcher.BeginInvoke(() => Apply(ThemeMode.THEME_SYSTEM));
        }
    }
}