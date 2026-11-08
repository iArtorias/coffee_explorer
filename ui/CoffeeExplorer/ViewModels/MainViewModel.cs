using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Mvvm;
using CoffeeExplorer.Services;
using CoffeeExplorer.Views.Dialogs;
using static CoffeeExplorer.Services.LanguageService;

namespace CoffeeExplorer.ViewModels
{
    public sealed class MainViewModel : ObservableObject
    {
        private static string PE_FILTER => Tr("Portable executables") + " (*.exe;*.dll;*.sys;*.ocx;*.cpl;*.scr;*.drv;*.efi;*.mui;*.winmd)|*.exe;*.dll;*.sys;*.ocx;*.cpl;*.scr;*.drv;*.efi;*.mui;*.winmd|" + Tr("All files") + " (*.*)|*.*";

        private readonly DispatcherTimer m_StatusTimer;
        // Files being opened right now, a second request for one of them is ignored.
        private readonly HashSet<string> m_Opening = new(StringComparer.OrdinalIgnoreCase);
        private DocumentViewModel? m_ActiveDocument;
        private string m_StatusText = Tr("Ready");
        private bool m_IsShellRegistered;

        public MainViewModel()
        {
            m_StatusTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(8) };
            m_StatusTimer.Tick += (_, _) => { m_StatusTimer.Stop(); StatusText = Tr("Ready"); };

            OpenCommand = new RelayCommand(Open);
            OpenReadOnlyCommand = new RelayCommand(() => Open(true));
            OpenRecentCommand = new RelayCommand(parameter => OpenFile((string)parameter!));
            ClearRecentCommand = new RelayCommand(SettingsStore.ClearRecentFiles, () => RecentFiles.Count > 0);
            SaveCommand = new RelayCommand(() => Save(m_ActiveDocument!), () => m_ActiveDocument != null && !m_ActiveDocument.IsReadOnly);
            SaveAsCommand = new RelayCommand(() => SaveAs(m_ActiveDocument!), () => m_ActiveDocument != null);
            ReloadCommand = new RelayCommand(Reload, () => m_ActiveDocument != null && File.Exists(m_ActiveDocument.Path));
            CloseCommand = new RelayCommand(parameter => Close(parameter as DocumentViewModel ?? m_ActiveDocument!), _ => m_ActiveDocument != null);
            CloseAllCommand = new RelayCommand(() => CloseAll(), () => Documents.Count > 0);
            CloseOthersCommand = new RelayCommand(parameter => CloseOthers(parameter as DocumentViewModel ?? m_ActiveDocument!), _ => Documents.Count > 1);
            ExitCommand = new RelayCommand(() => Window?.Close());
            UndoCommand = new RelayCommand(() => m_ActiveDocument!.Undo(), () => m_ActiveDocument?.CanUndo == true);
            RedoCommand = new RelayCommand(() => m_ActiveDocument!.Redo(), () => m_ActiveDocument?.CanRedo == true);
            RefreshCommand = new RelayCommand(() => m_ActiveDocument!.Refresh(true), () => m_ActiveDocument != null);
            ShowNodeCommand = new RelayCommand(parameter => m_ActiveDocument?.NavigateTo((string)parameter!), _ => m_ActiveDocument != null);
            GoToOffsetCommand = new RelayCommand(GoToOffset, () => m_ActiveDocument != null);
            SetThemeCommand = new RelayCommand(parameter => SetTheme((ThemeChoice)parameter!));
            ToggleShellCommand = new RelayCommand(ToggleShellIntegration);
            OptionsCommand = new RelayCommand(ShowOptions);
            AboutCommand = new RelayCommand(() => new AboutDialog().ShowFor(Window));
            ToggleToolbarCommand = new RelayCommand(() => { Placement.ToolbarVisible = !Placement.ToolbarVisible; OnPropertyChanged(nameof(IsToolbarVisible)); });
            ToggleStatusBarCommand = new RelayCommand(() => { Placement.StatusBarVisible = !Placement.StatusBarVisible; OnPropertyChanged(nameof(IsStatusBarVisible)); });

            SettingsStore.RecentFilesChanged += (_, _) => RefreshRecentFiles();
            ThemeService.ThemeChanged += (_, _) => { OnPropertyChanged(nameof(ThemeText)); RefreshThemeChoices(); };

            // The custom themes are parsed off the UI thread, the menu fills in after.
            RefreshThemeChoices(false);
            Dispatcher dispatcher = Dispatcher.CurrentDispatcher;
            Task.Run(() => CustomThemes.Available()).ContinueWith(_ => dispatcher.InvokeAsync(() => RefreshThemeChoices(), DispatcherPriority.Background), TaskScheduler.Default);
            LanguageService.LanguageChanged += (_, _) => OnLanguageChanged();
            RefreshRecentFiles();
            m_IsShellRegistered = ShellIntegration.IsRegistered;
        }

        public Window? Window { get; set; }

        public ObservableCollection<DocumentViewModel> Documents { get; } = new();

        public ObservableCollection<string> RecentFiles { get; } = new();

        public DocumentViewModel? ActiveDocument
        {
            get => m_ActiveDocument;
            set
            {
                if (SetProperty(ref m_ActiveDocument, value))
                {
                    OnPropertyChanged(nameof(HasDocument));
                    OnPropertyChanged(nameof(WindowTitle));
                    CommandManager.InvalidateRequerySuggested();
                }
            }
        }

        public bool HasDocument => m_ActiveDocument != null;

        public string WindowTitle => m_ActiveDocument == null ? "Coffee Explorer" : $"{m_ActiveDocument.FileName}{(m_ActiveDocument.IsModified ? " *" : string.Empty)} - Coffee Explorer";

        public string StatusText
        {
            get => m_StatusText;
            private set => SetProperty(ref m_StatusText, value);
        }

        public ThemeMode Theme => SettingsStore.Current.Theme;

        public string ThemeText => ThemeService.DisplayName;

        // 'View > Theme'. System, Light, Dark and the themes of the themes folder.
        public ObservableCollection<ThemeChoice> ThemeChoices { get; } = new();

        private void RefreshThemeChoices(
            bool custom = true
        )
        {
            ThemeChoices.Clear();

            foreach (ThemeChoice choice in ThemeService.Choices(custom))
            {
                ThemeChoices.Add(choice);
            }
        }

        public WindowPlacement Placement => SettingsStore.Current.Window;

        // Width of the structure tree, shared by every document tab and kept in the settings.
        public GridLength TreeWidth
        {
            get => new(Math.Clamp(Placement.TreeWidth, 160, 900));
            set
            {
                if (value.IsAbsolute && Math.Abs(value.Value - Placement.TreeWidth) > 0.5)
                {
                    Placement.TreeWidth = value.Value;
                    OnPropertyChanged();
                }
            }
        }

        public bool IsToolbarVisible => Placement.ToolbarVisible;

        public bool IsStatusBarVisible => Placement.StatusBarVisible;

        public bool IsShellRegistered
        {
            get => m_IsShellRegistered;
            private set => SetProperty(ref m_IsShellRegistered, value);
        }

        public ICommand OpenCommand { get; }

        public ICommand OpenReadOnlyCommand { get; }

        public ICommand OpenRecentCommand { get; }

        public ICommand ClearRecentCommand { get; }

        public ICommand SaveCommand { get; }

        public ICommand SaveAsCommand { get; }

        public ICommand ReloadCommand { get; }

        public ICommand CloseCommand { get; }

        public ICommand CloseAllCommand { get; }

        public ICommand CloseOthersCommand { get; }

        public ICommand ExitCommand { get; }

        public ICommand UndoCommand { get; }

        public ICommand RedoCommand { get; }

        public ICommand RefreshCommand { get; }

        public ICommand ShowNodeCommand { get; }

        public ICommand GoToOffsetCommand { get; }

        public ICommand SetThemeCommand { get; }

        public ICommand ToggleShellCommand { get; }

        public ICommand OptionsCommand { get; }

        public ICommand AboutCommand { get; }

        public ICommand ToggleToolbarCommand { get; }

        public ICommand ToggleStatusBarCommand { get; }

        // Status and errors

        public void SetStatus(
            string text
        )
        {
            StatusText = text;
            m_StatusTimer.Stop();
            m_StatusTimer.Start();
        }

        public void ShowError(
            string title,
            string message
        )
        {
            MessageService.ShowError(Window, title, message);
        }

        public void OnDocumentStateChanged(
            DocumentViewModel document
        )
        {
            if (document == m_ActiveDocument)
            {
                OnPropertyChanged(nameof(WindowTitle));
                CommandManager.InvalidateRequerySuggested();
            }
        }

        private void RefreshRecentFiles()
        {
            RecentFiles.Clear();

            foreach (string path in SettingsStore.Current.RecentFiles)
            {
                RecentFiles.Add(path);
            }
        }

        // Files

        private void Open()
        {
            Open(false);
        }

        private void Open(
            bool read_only
        )
        {
            foreach (string path in FileDialogs.OpenMany(read_only ? Tr("Open read only") : Tr("Open"), PE_FILTER))
            {
                OpenFile(path, read_only);
            }
        }

        // Opens the file on a worker thread (reading and parsing a large image takes a moment), the window stays live.
        public async void OpenFile(
            string path,
            bool read_only = false
        )
        {
            path = Path.GetFullPath(path);
            DocumentViewModel? existing = Documents.FirstOrDefault(document => string.Equals(document.Path, path, StringComparison.OrdinalIgnoreCase));

            if (existing != null)
            {
                ActiveDocument = existing;
                return;
            }

            if (!m_Opening.Add(path))
            {
                return;
            }

            try
            {
                await OpenFileCore(path, read_only);
            }
            finally
            {
                m_Opening.Remove(path);
            }
        }

        private async Task OpenFileCore(
            string path,
            bool read_only
        )
        {
            if (!File.Exists(path))
            {
                SettingsStore.RemoveRecentFile(path);
                ShowError(Tr("File not found"), path);
                return;
            }

            long size = new FileInfo(path).Length;
            int limit = SettingsStore.Current.LargeFileMegabytes;

            if (limit > 0 && size > (long)limit * 1024 * 1024 &&
                 !MessageService.Confirm(Window, Tr("Large file"), Tr("{0} is {1}. Coffee Explorer loads the whole file into memory. Open it anyway?", Path.GetFileName(path), Format.Bytes((ulong)size)),
                                         Tr("Open"), Tr("Cancel"), MessageKind.MESSAGE_WARNING))
            {
                return;
            }

            PeDocument core;
            SetStatus(Tr("Opening {0}...", Path.GetFileName(path)));

            try
            {
                // 'GetInfo' parses the image, the document view asks for it first. The local PDB lookup runs here too.
                // The overview and the disassembler name functions with it and a large PDB must not stall the window.
                core = await Task.Run(() =>
                {
                    PeDocument opened = PeDocument.Open(path, 0, read_only);

                    if (opened.GetInfo().IsPe)
                    {
                        DocumentViewModel.LoadLocalSymbols(opened);
                    }

                    return opened;
                });
            }
            catch (CoreException error)
            {
                ShowError(Tr("The file could not be opened"), $"{path}\n\n{error.Message}");
                return;
            }

            DocumentViewModel document;

            try
            {
                document = new DocumentViewModel(this, core, path, read_only);
            }
            catch (Exception error)
            {
                core.Dispose();
                ShowError(Tr("The file could not be opened"), $"{path}\n\n{error.Message}");
                return;
            }

            Documents.Add(document);
            ActiveDocument = document;
            SettingsStore.AddRecentFile(path);

            if (SettingsStore.Current.Die.ScanOnOpen)
            {
                document.StartDieScan();
            }

            SetStatus(document.IsPe ? Tr("Opened {0} ({1})", document.FileName, document.FileTypeText)
                                  : Tr("Opened {0}, not a PE file: only the overview and the hex editor are available", document.FileName));
        }

        public bool Save(
            DocumentViewModel document
        )
        {
            if (document.IsReadOnly)
            {
                return SaveAs(document);
            }

            return document.Save(null);
        }

        public bool SaveAs(
            DocumentViewModel document
        )
        {
            string? path = FileDialogs.Save(Tr("Save as"), document.FileName, PE_FILTER, Path.GetDirectoryName(document.Path));
            return path != null && document.Save(path);
        }

        private void Reload()
        {
            DocumentViewModel document = m_ActiveDocument!;

            if (document.IsModified && !MessageService.Confirm(Window, Tr("Reload"), Tr("Discard the changes to {0} and load it again from disk?", document.FileName), Tr("Reload"), Tr("Cancel"), MessageKind.MESSAGE_WARNING))
            {
                return;
            }

            document.Reload();
        }

        public bool Close(
            DocumentViewModel document
        )
        {
            if (document.IsModified)
            {
                ActiveDocument = document;

                switch (MessageService.AskSave(Window, document.FileName))
                {
                    case SaveChoice.SAVE_CHOICE_SAVE:
                        if (!Save(document))
                        {
                            return false;
                        }

                        break;
                    case SaveChoice.SAVE_CHOICE_CANCEL:
                        return false;
                }
            }

            int index = Documents.IndexOf(document);
            Documents.Remove(document);
            document.Dispose();

            if (m_ActiveDocument == document || m_ActiveDocument == null)
            {
                ActiveDocument = Documents.Count == 0 ? null : Documents[Math.Min(index, Documents.Count - 1)];
            }

            return true;
        }

        public bool CloseAll()
        {
            foreach (DocumentViewModel document in Documents.ToList())
            {
                if (!Close(document))
                {
                    return false;
                }
            }

            return true;
        }

        private void CloseOthers(
            DocumentViewModel keep
        )
        {
            foreach (DocumentViewModel document in Documents.Where(document => document != keep).ToList())
            {
                if (!Close(document))
                {
                    return;
                }
            }
        }

        private void GoToOffset()
        {
            var dialog = new GoToDialog();

            if (dialog.ShowFor(Window))
            {
                m_ActiveDocument!.ShowInHexEditor(dialog.Offset, 1);
            }
        }

        // Settings

        private void SetTheme(
            ThemeChoice choice
        )
        {
            // The folder is read again, an edited theme file shows its new colors.
            CustomThemes.Reload();
            string? error = ThemeService.Apply(choice.Mode, choice.Custom);
            SettingsStore.Current.Theme = ThemeService.Mode;
            SettingsStore.Current.CustomTheme = ThemeService.Custom?.Id;
            OnPropertyChanged(nameof(Theme));
            SettingsStore.Save();

            if (error != null)
            {
                ShowError(Tr("The theme could not be loaded"), error);
            }
        }

        // Turns the integration on or off, used by the copy that restarted outside the package.
        public void SetShellIntegration(
            bool register
        )
        {
            if (register != ShellIntegration.IsRegistered)
            {
                ToggleShellIntegration();
            }
        }

        private async void ToggleShellIntegration()
        {
            try
            {
                await ToggleShellIntegrationAsync();
            }
            catch (Exception error)
            {
                Log.Error(error);
                ShowError(Tr("Shell integration failed"), error.Message);
                IsShellRegistered = ShellIntegration.IsRegistered;
            }
        }

        private async Task ToggleShellIntegrationAsync()
        {
            bool register = !ShellIntegration.IsRegistered;

            // Removing or installing the package terminates this process when it runs inside the package [IsRunningInPackage].
            if (ShellIntegration.IsWindows11 && ShellIntegration.IsPackageAvailable && ShellIntegration.IsRunningInPackage)
            {
                RestartOutsidePackage(register);
                return;
            }

            try
            {
                if (register)
                {
                    ShellIntegration.Register();
                }
                else
                {
                    ShellIntegration.Unregister();
                }
            }
            catch (Exception error)
            {
                ShowError(Tr("Shell integration failed"), error.Message);
                IsShellRegistered = ShellIntegration.IsRegistered;
                return;
            }

            IsShellRegistered = ShellIntegration.IsRegistered;

            // Windows 11. The compact menu entry comes from the sparse package, PowerShell takes a few seconds.
            if (!ShellIntegration.IsWindows11 || !ShellIntegration.IsPackageAvailable)
            {
                SetStatus(register ? Tr("Added \"Open with Coffee Explorer\" to the Explorer context menu") : Tr("Removed \"Open with Coffee Explorer\" from the Explorer context menu"));
                return;
            }

            SetStatus(register ? Tr("Adding \"Open with Coffee Explorer\" to the Windows 11 context menu...") : Tr("Removing \"Open with Coffee Explorer\" from the Windows 11 context menu..."));

            if (!register)
            {
                string? remove_error = await ShellIntegration.UnregisterPackageAsync();
                ShellIntegration.SyncClassicVerb();

                if (remove_error != null)
                {
                    ShowError(Tr("The Windows 11 context menu entry could not be removed"), remove_error);
                }
                else
                {
                    SetStatus(Tr("Removed \"Open with Coffee Explorer\" from the Explorer context menu"));
                }

                return;
            }

            string? package_error = await ShellIntegration.RegisterPackageAsync();

            // The package is signed with the certificate of this build, Windows installs it once the machine trusts it.
            if (package_error != null && ShellIntegration.IsTrustError(package_error) &&
                 MessageService.Confirm(Window, Tr("Trust the context menu package"),
                                         Tr("The Windows 11 context menu entry comes from a small package signed with the self-signed certificate of this Coffee Explorer build. " +
                                             "Windows installs it once the certificate is in Trusted People of this computer, which needs administrator rights once.") + "\n\n" +
                                         Tr("Trust the certificate now?"), Tr("Trust"), Tr("Cancel"), MessageKind.MESSAGE_INFORMATION))
            {
                if (await ShellIntegration.TrustCertificateAsync())
                {
                    package_error = await ShellIntegration.RegisterPackageAsync();
                }
            }

            // Developer mode registers the loose manifest without any certificate.
            if (package_error != null && await ShellIntegration.RegisterPackageManifestAsync() == null)
            {
                package_error = null;
            }

            // The package entry also shows in show more options, the classic verb steps aside.
            ShellIntegration.SyncClassicVerb();

            if (package_error == null)
            {
                SetStatus(Tr("Added \"Open with Coffee Explorer\" to the Explorer context menu (compact menu and Show more options)"));
            }
            else
            {
                ShowError(Tr("The Windows 11 context menu entry could not be added"),
                           Tr("\"Open with Coffee Explorer\" is available under Show more options. Windows refused the context menu package:") + "\n\n" + package_error);
            }
        }

        // Closes this window (unsaved changes are asked about as on exit) and starts a copy through Explorer that opens the
        // same files and finishes the shell integration change.
        private void RestartOutsidePackage(
            bool register
        )
        {
            IsShellRegistered = ShellIntegration.IsRegistered;

            if (Window == null)
            {
                return;
            }

            if (!ShellIntegration.CanStartOutsidePackage)
            {
                ShowError(Tr("Shell integration failed"),
                           Tr("Coffee Explorer was started from the Windows 11 context menu and runs inside its package, Windows would close it while the package changes. " +
                              "Start Coffee Explorer from its folder or the Start menu and try again."));
                return;
            }

            if (!MessageService.Confirm(Window, Tr("Restart Coffee Explorer"),
                                         Tr("Coffee Explorer was started from the Windows 11 context menu and runs inside its package. " +
                                            "Windows closes every program of the package while the package changes, so Coffee Explorer restarts outside of it to finish.") + "\n\n" +
                                         Tr("Restart now?"), Tr("Restart"), Tr("Cancel"), MessageKind.MESSAGE_INFORMATION))
            {
                return;
            }

            // Read before closing, closing the window closes the documents.
            string[] files = Documents.Select(document => document.Path).Where(File.Exists).ToArray();
            bool closed = false;

            EventHandler on_closed = (_, _) => closed = true;
            Window.Closed += on_closed;
            Window.Close();
            Window.Closed -= on_closed;

            if (!closed)
            {
                // Unsaved changes, the user kept the window.
                return;
            }

            string arguments = $"--wait-pid={Environment.ProcessId} --shell-integration={(register ? "on" : "off")}";

            foreach (string file in files)
            {
                arguments += " \"" + file + "\"";
            }

            SettingsStore.Save();

            if (!ShellIntegration.StartOutsidePackage(arguments))
            {
                MessageService.ShowError(null, Tr("Shell integration failed"), Tr("Coffee Explorer could not be started again. Start it from its folder or the Start menu and try again."));
            }
        }

        private void ShowOptions()
        {
            var dialog = new OptionsDialog(SettingsStore.Current, ShellIntegration.IsRegistered);

            if (!dialog.ShowFor(Window))
            {
                return;
            }

            AppSettings settings = dialog.Result;
            SettingsStore.Current = settings;
            CustomThemes.Reload();

            if (ThemeService.Apply(settings.Theme, settings.CustomTheme) is string theme_error)
            {
                settings.Theme = ThemeService.Mode;
                settings.CustomTheme = null;
                ShowError(Tr("The theme could not be loaded"), theme_error);
            }

            ThemeService.ApplyFont(settings.UiFontFamily, settings.UiFontSize);

            if (!string.Equals(settings.Language, LanguageService.Current, StringComparison.OrdinalIgnoreCase))
            {
                LanguageService.Apply(settings.Language);
            }

            OnPropertyChanged(nameof(Theme));
            OnPropertyChanged(nameof(Placement));

            if (dialog.ShellIntegration != ShellIntegration.IsRegistered)
            {
                ToggleShellIntegration();
            }

            SettingsStore.Save();

            if (SettingsStore.SetStoreInAppData(dialog.StoreInAppData) is string error)
            {
                ShowError(Tr("The settings could not be moved"), error);
            }

            SettingsChanged?.Invoke(this, EventArgs.Empty);
        }

        public event EventHandler? SettingsChanged;

        // Text built in code does not follow the XAML bindings, refresh it here.
        private void OnLanguageChanged()
        {
            OnPropertyChanged(nameof(ThemeText));
            RefreshThemeChoices();
            m_StatusTimer.Stop();
            StatusText = Tr("Ready");

            foreach (DocumentViewModel document in Documents)
            {
                document.OnLanguageChanged();
            }
        }

        // Called by the window, false keeps it open.
        public bool ConfirmExit()
        {
            return CloseAll();
        }
    }
}