using System;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Runtime;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;
using CoffeeExplorer.Interop;
using CoffeeExplorer.Services;

namespace CoffeeExplorer
{
    public partial class App : Application
    {
        private static readonly string INSTANCE_NAME = "CoffeeExplorer-" + (WindowsIdentity.GetCurrent().User?.Value ?? Environment.UserName);

        private Mutex? m_InstanceMutex;
        private CancellationTokenSource? m_PipeCancel;

        protected override void OnStartup(
            StartupEventArgs arguments
        )
        {
            base.OnStartup(arguments);
            AppDomain.CurrentDomain.UnhandledException += (_, error) => Log.Error(error.ExceptionObject as Exception);
            SettingsStore.Load();
            LanguageService.Apply(SettingsStore.Current.Language);

            // Shell integration from the command line (installers, scripts), no window.
            if (arguments.Args.Contains("--register-shell") || arguments.Args.Contains("--unregister-shell"))
            {
                try
                {
                    if (arguments.Args.Contains("--register-shell"))
                    {
                        ShellIntegration.Register();
                    }
                    else
                    {
                        ShellIntegration.Unregister();
                    }

                    Shutdown(0);
                }
                catch (Exception)
                {
                    Shutdown(1);
                }

                return;
            }

            ShellIntegration.Migrate();
            WaitForPreviousInstance(arguments.Args);
            string[] files = arguments.Args.Where(argument => !argument.StartsWith("--", StringComparison.Ordinal)).Select(Path.GetFullPath).ToArray();

            // Explorer starts one process per file, the first one keeps them all.
            if (SettingsStore.Current.SingleInstance && !arguments.Args.Contains("--new-window"))
            {
                m_InstanceMutex = new Mutex(true, INSTANCE_NAME, out bool created);

                if (!created)
                {
                    if (SendToRunningInstance(files))
                    {
                        Shutdown(0);
                        return;
                    }

                    // The first instance does not answer, this one runs on its own without a pipe server.
                    m_InstanceMutex.Dispose();
                    m_InstanceMutex = null;
                }
            }

            StartJitProfile();
            DispatcherUnhandledException += OnUnhandledException;
            ThemeService.Initialize(SettingsStore.Current.Theme, SettingsStore.Current.CustomTheme);
            ThemeService.ApplyFont(SettingsStore.Current.UiFontFamily, SettingsStore.Current.UiFontSize);

            if (!CheckCore())
            {
                Shutdown(1);
                return;
            }

            var window = new MainWindow();
            MainWindow = window;
            window.Show();

            foreach (string path in files)
            {
                if (File.Exists(path))
                {
                    window.ViewModel.OpenFile(path);
                }
            }

            if (m_InstanceMutex != null)
            {
                StartPipeServer();
            }

            // Restarted outside the context menu package to change the shell integration [MainViewModel.RestartOutsidePackage].
            string? shell = arguments.Args.FirstOrDefault(argument => argument.StartsWith("--shell-integration=", StringComparison.Ordinal));

            if (shell != null)
            {
                bool register = shell.EndsWith("=on", StringComparison.Ordinal);
                Dispatcher.InvokeAsync(() => window.ViewModel.SetShellIntegration(register), DispatcherPriority.ApplicationIdle);
            }

            // Detect It Easy (die.dll and its Qt runtime) loads in the background once the window is up.
            Dispatcher.InvokeAsync(DieService.WarmUp, DispatcherPriority.ApplicationIdle);

            // Previous versions of saved files that were in use at the time are free now.
            Dispatcher.InvokeAsync(() => Task.Run(DeletePendingReplacedFiles), DispatcherPriority.ApplicationIdle);
        }

        private static void DeletePendingReplacedFiles()
        {
            try
            {
                using var answer = PeDocument.QueryStatic(new System.Collections.Generic.Dictionary<string, object> { ["query"] = "cleanup_replaced" });
            }
            catch (Exception error)
            {
                Log.Error(error);
            }
        }

        protected override void OnExit(
            ExitEventArgs arguments
        )
        {
            m_PipeCancel?.Cancel();
            SettingsStore.Save();
            m_InstanceMutex?.Dispose();
            
            base.OnExit(arguments);
        }

        // Multicore JIT. The methods compiled during this start are recorded, the next start compiles them on other cores
        // ahead of use. Only the instance that opens a window records, a process that hands its files to the running
        // instance would overwrite the profile with a few methods. The profile sits next to the settings.
        private static void StartJitProfile()
        {
            try
            {
                string? folder = Path.GetDirectoryName(SettingsStore.FilePath);

                if (string.IsNullOrEmpty(folder))
                {
                    return;
                }

                Directory.CreateDirectory(folder);
                ProfileOptimization.SetProfileRoot(folder);
                ProfileOptimization.StartProfile("startup.jitprofile");
            }
            catch (Exception)
            {
                // A read only folder only costs the speed up.
            }
        }

        // '--wait-pid=N'. The instance that restarted this one still holds the single instance mutex while it exits.
        private static void WaitForPreviousInstance(
            string[] arguments
        )
        {
            string? argument = arguments.FirstOrDefault(item => item.StartsWith("--wait-pid=", StringComparison.Ordinal));

            if (argument == null || !int.TryParse(argument.AsSpan("--wait-pid=".Length), out int id))
            {
                return;
            }

            try
            {
                using var process = System.Diagnostics.Process.GetProcessById(id);
                process.WaitForExit(15000);
            }
            catch (Exception)
            {
                // Already gone.
            }
        }

        private static bool CheckCore()
        {
            try
            {
                // PcDocumentGetSize, deferred overview hashes, disassembly spans and branch targets.
                uint version = PeDocument.CoreVersion;

                if (version >= 0x00010100)
                {
                    return true;
                }

                MessageService.ShowError(null, LanguageService.Tr("pecore.dll is outdated"),
                                          LanguageService.Tr("pecore.dll next to CoffeeExplorer.exe is version {0}, this Coffee Explorer needs {1}. Build both again (build.ps1).",
                                                              $"{version >> 16}.{(version >> 8) & 0xFF}", "1.1"));
                return false;
            }
            catch (Exception error) when (error is DllNotFoundException || error is EntryPointNotFoundException || error is BadImageFormatException)
            {
                MessageService.ShowError(null, LanguageService.Tr("pecore.dll is missing"), LanguageService.Tr("Coffee Explorer needs pecore.dll next to CoffeeExplorer.exe.") + "\n\n" + error.Message);
                return false;
            }
        }

        private void OnUnhandledException(
            object sender,
            DispatcherUnhandledExceptionEventArgs arguments
        )
        {
            // Keep the editor alive, unsaved work matters more than a perfect state.
            Log.Error(arguments.Exception);
            MessageService.ShowError(MainWindow, LanguageService.Tr("Unexpected error"), arguments.Exception.Message);
            arguments.Handled = true;

            // A failure before the main window exists leaves a process without any window.
            if (MainWindow == null || !MainWindow.IsLoaded)
            {
                Shutdown(1);
            }
        }

        // Single instance.

        private static bool SendToRunningInstance(
            string[] files
        )
        {
            try
            {
                using var client = new NamedPipeClientStream(".", INSTANCE_NAME, PipeDirection.Out);
                client.Connect(1500);
                byte[] message = Encoding.UTF8.GetBytes(string.Join("\n", files));
                client.Write(message, 0, message.Length);
                return true;
            }
            catch (Exception)
            {
                // The other instance is starting or hung, open a window here instead.
                return false;
            }
        }

        private void StartPipeServer()
        {
            m_PipeCancel = new CancellationTokenSource();
            CancellationToken token = m_PipeCancel.Token;

            Task.Run(async () =>
            {
                while (!token.IsCancellationRequested)
                {
                    try
                    {
                        using var server = new NamedPipeServerStream(INSTANCE_NAME, PipeDirection.In, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                        await server.WaitForConnectionAsync(token);
                        using var reader = new StreamReader(server, Encoding.UTF8);
                        string text = await reader.ReadToEndAsync(token);
                        string[] paths = text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
                        _ = Dispatcher.BeginInvoke(() => ReceiveFiles(paths));
                    }
                    catch (OperationCanceledException)
                    {
                        return;
                    }
                    catch (Exception)
                    {
                        // A broken connection must not stop the server.
                        await Task.Delay(200, CancellationToken.None);
                    }
                }
            }, token);
        }

        private void ReceiveFiles(
            string[] paths
        )
        {
            if (MainWindow is not MainWindow window)
            {
                return;
            }

            foreach (string path in paths)
            {
                if (File.Exists(path))
                {
                    window.ViewModel.OpenFile(path);
                }
            }

            window.BringToFront();
        }
    }
}