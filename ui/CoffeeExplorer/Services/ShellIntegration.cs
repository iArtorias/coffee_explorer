using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using Microsoft.Win32;

namespace CoffeeExplorer.Services
{
    // Explorer integration, per user (HKCU) so no elevation is needed.
    public static class ShellIntegration
    {
        private const string VERB_KEY = @"Software\Classes\*\shell\CoffeeExplorer";
        private const string APPLICATION_KEY = @"Software\Classes\Applications\CoffeeExplorer.exe";
        private const string PACKAGES_KEY = @"Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository\Packages";
        private const string LEGACY_DISABLE = "LegacyDisable";
        private const int SHCNE_ASSOCCHANGED = 0x08000000;
        private const int SHCNF_IDLIST = 0;

        // Name of the sparse package, the identity of 'Package\AppxManifest.xml'.
        public const string PACKAGE_NAME = "CoffeeExplorer.ContextMenu";
        private const string PACKAGE_FILE = "CoffeeExplorer.ContextMenu.msix";
        private const string CERTIFICATE_FILE = "CoffeeExplorer.ContextMenu.cer";

        public static readonly string[] EXTENSIONS =
        {
            ".exe", ".dll", ".sys", ".ocx", ".cpl", ".scr", ".drv", ".efi", ".mui", ".winmd", ".ax", ".acm", ".tlb", ".node", ".pyd"
        };

        [DllImport("shell32.dll")]
        private static extern void SHChangeNotify(
            int event_id,
            int flags,
            IntPtr item1,
            IntPtr item2
        );

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetCurrentPackageFullName(
            ref int length,
            StringBuilder? name
        );

        private const int ERROR_INSUFFICIENT_BUFFER = 122;

        [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet = CharSet.Unicode)]
        private delegate int LaunchOutsidePackageProcedure(
            string file,
            string arguments,
            string folder
        );

        public static string ExecutablePath => Environment.ProcessPath ?? Process.GetCurrentProcess().MainModule?.FileName ?? "CoffeeExplorer.exe";

        private static string ProgramFolder => Path.GetDirectoryName(ExecutablePath) ?? AppContext.BaseDirectory;

        private static string Command => $"\"{ExecutablePath}\" \"%1\"";

        // The compact context menu exists since Windows 11 (build 22000).
        public static bool IsWindows11 => Environment.OSVersion.Version.Build >= 22000;

        // The package and its shell extension were built next to the executable.
        public static bool IsPackageAvailable =>
            File.Exists(Path.Combine(ProgramFolder, PACKAGE_FILE)) && File.Exists(Path.Combine(ProgramFolder, CERTIFICATE_FILE)) &&
            File.Exists(Path.Combine(ProgramFolder, "CoffeeExplorer.Shell.dll"));

        // Registered and pointing at this executable.
        public static bool IsRegistered
        {
            get
            {
                try
                {
                    using RegistryKey? key = Registry.CurrentUser.OpenSubKey(VERB_KEY + @"\command");
                    return key?.GetValue(null) is string command && string.Equals(command, Command, StringComparison.OrdinalIgnoreCase);
                }
                catch (Exception)
                {
                    return false;
                }
            }
        }

        // Registered by another copy of Coffee Explorer (moved install, second build folder).
        public static bool IsRegisteredElsewhere
        {
            get
            {
                try
                {
                    using RegistryKey? key = Registry.CurrentUser.OpenSubKey(VERB_KEY + @"\command");
                    return key?.GetValue(null) is string command && !string.Equals(command, Command, StringComparison.OrdinalIgnoreCase);
                }
                catch (Exception)
                {
                    return false;
                }
            }
        }

        // The context menu package is installed for the current user (any version), read from the AppModel repository
        // because Get-AppxPackage takes seconds.
        public static bool IsPackageInstalled
        {
            get
            {
                try
                {
                    using RegistryKey? packages = Registry.CurrentUser.OpenSubKey(PACKAGES_KEY);
                    return packages != null && packages.GetSubKeyNames().Any(name => name.StartsWith(PACKAGE_NAME + "_", StringComparison.OrdinalIgnoreCase));
                }
                catch (Exception)
                {
                    return false;
                }
            }
        }

        // This process has the identity of the context menu package. Older builds of the shell extension started the editor from
        // the COM surrogate of the package, the editor and everything it starts then belong to the package and Windows
        // terminates them while the package is removed or installed again.
        public static bool IsRunningInPackage
        {
            get
            {
                try
                {
                    int length = 0;

                    if (GetCurrentPackageFullName(ref length, null) != ERROR_INSUFFICIENT_BUFFER)
                    {
                        // APPMODEL_ERROR_NO_PACKAGE.
                        return false;
                    }

                    var name = new StringBuilder(length);
                    return GetCurrentPackageFullName(ref length, name) == 0 && name.ToString().StartsWith(PACKAGE_NAME + "_", StringComparison.OrdinalIgnoreCase);
                }
                catch (Exception)
                {
                    return false;
                }
            }
        }

        private static IntPtr LaunchOutsidePackageExport()
        {
            string library = Path.Combine(ProgramFolder, "CoffeeExplorer.Shell.dll");

            // An older library has no export, the caller then keeps running inside the package.
            if (NativeLibrary.TryLoad(library, out IntPtr handle) && NativeLibrary.TryGetExport(handle, "LaunchOutsidePackage", out IntPtr export))
            {
                return export;
            }

            return IntPtr.Zero;
        }

        public static bool CanStartOutsidePackage => LaunchOutsidePackageExport() != IntPtr.Zero;

        // Starts Coffee Explorer through Explorer, the new process has no package identity.
        public static bool StartOutsidePackage(
            string arguments
        )
        {
            try
            {
                IntPtr export = LaunchOutsidePackageExport();

                if (export == IntPtr.Zero)
                {
                    return false;
                }

                var launch = Marshal.GetDelegateForFunctionPointer<LaunchOutsidePackageProcedure>(export);
                int result = launch(ExecutablePath, arguments, ProgramFolder);

                if (result < 0)
                {
                    Log.Write($"Start outside the package: 0x{result:X8}");
                }

                return result >= 0;
            }
            catch (Exception error)
            {
                Log.Write("Start outside the package: " + error.Message);
                return false;
            }
        }

        public static void Register()
        {
            string icon = $"\"{ExecutablePath}\",0";

            using (RegistryKey verb = Registry.CurrentUser.CreateSubKey(VERB_KEY))
            {
                verb.SetValue(null, "Open with Coffee Explorer");
                verb.SetValue("Icon", icon);
            }

            SyncClassicVerb(false);

            using (RegistryKey command = Registry.CurrentUser.CreateSubKey(VERB_KEY + @"\command"))
            {
                command.SetValue(null, Command);
            }

            using (RegistryKey application = Registry.CurrentUser.CreateSubKey(APPLICATION_KEY))
            {
                application.SetValue("FriendlyAppName", "Coffee Explorer");
            }

            using (RegistryKey application_command = Registry.CurrentUser.CreateSubKey(APPLICATION_KEY + @"\shell\open\command"))
            {
                application_command.SetValue(null, Command);
            }

            using (RegistryKey supported = Registry.CurrentUser.CreateSubKey(APPLICATION_KEY + @"\SupportedTypes"))
            {
                foreach (string extension in EXTENSIONS)
                {
                    supported.SetValue(extension, string.Empty);
                }
            }

            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, IntPtr.Zero, IntPtr.Zero);
        }

        // Hides the classic verb while the package provides the entry, shows it again without the package. Returns true when
        // something changed (explorer is notified when notify is set).
        public static bool SyncClassicVerb(
            bool notify = true
        )
        {
            try
            {
                using RegistryKey? verb = Registry.CurrentUser.OpenSubKey(VERB_KEY, true);

                if (verb == null)
                {
                    return false;
                }

                bool hide = IsWindows11 && IsPackageInstalled;
                bool hidden = verb.GetValue(LEGACY_DISABLE) != null;

                if (hide == hidden)
                {
                    return false;
                }

                if (hide)
                {
                    verb.SetValue(LEGACY_DISABLE, string.Empty);
                }
                else
                {
                    verb.DeleteValue(LEGACY_DISABLE, false);
                }

                if (notify)
                {
                    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, IntPtr.Zero, IntPtr.Zero);
                }

                return true;
            }
            catch (Exception error)
            {
                Log.Write("Classic context menu verb: " + error.Message);
                return false;
            }
        }

        public static void Migrate()
        {
            //
        }

        public static void Unregister()
        {
            Registry.CurrentUser.DeleteSubKeyTree(VERB_KEY, false);
            Registry.CurrentUser.DeleteSubKeyTree(APPLICATION_KEY, false);

            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, IntPtr.Zero, IntPtr.Zero);
        }

        // Windows 11 compact menu.

        // Installs the sparse package for the current user with this folder as its external location. Returns null or the
        // error of PowerShell. 'IsTrustError' tells whether trusting the certificate would help.
        public static Task<string?> RegisterPackageAsync()
        {
            string folder = ProgramFolder;
            string package = Path.Combine(folder, PACKAGE_FILE);
            string script =
                "$ErrorActionPreference = 'Stop'; " +
                $"Get-AppxPackage -Name {PACKAGE_NAME} | Remove-AppxPackage; " +
                $"Add-AppxPackage -Path {Quote(package)} -ExternalLocation {Quote(folder)} -ForceUpdateFromAnyVersion";
            return RunPowerShellAsync(script);
        }

        // Developer mode lets Windows register the loose manifest without any certificate.
        public static Task<string?> RegisterPackageManifestAsync()
        {
            string folder = ProgramFolder;
            string manifest = Path.Combine(folder, "Package", "AppxManifest.xml");
            return RunPowerShellAsync($"$ErrorActionPreference = 'Stop'; Add-AppxPackage -Register {Quote(manifest)} -ExternalLocation {Quote(folder)} -ForceUpdateFromAnyVersion");
        }

        // 0x800B0109, the certificate chain ends in an untrusted root, 0x800B010A means no chain at all.
        public static bool IsTrustError(
            string error
        )
        {
            return error.Contains("0x800B0109", StringComparison.OrdinalIgnoreCase) || error.Contains("0x800B010A", StringComparison.OrdinalIgnoreCase) ||
                   error.Contains("0x800B0100", StringComparison.OrdinalIgnoreCase);
        }

        // Adds the package certificate to 'Trusted People' of the machine, the only step that needs administrator rights.
        // False when the user declined the elevation or the import failed.
        public static Task<bool> TrustCertificateAsync()
        {
            string certificate = Path.Combine(ProgramFolder, CERTIFICATE_FILE);

            return Task.Run(() =>
            {
                try
                {
                    var start = new ProcessStartInfo("powershell.exe")
                    {
                        UseShellExecute = true,
                        Verb = "runas",
                        WindowStyle = ProcessWindowStyle.Hidden,
                        Arguments = "-NoProfile -ExecutionPolicy Bypass -Command \"Import-Certificate -FilePath " + Quote(certificate) +
                                    " -CertStoreLocation Cert:\\LocalMachine\\TrustedPeople | Out-Null\""
                    };

                    using Process process = Process.Start(start)!;
                    process.WaitForExit();
                    return process.ExitCode == 0;
                }
                catch (Exception error)
                {
                    // ERROR_CANCELLED when the UAC prompt is declined.
                    Log.Write("Certificate import: " + error.Message);
                    return false;
                }
            });
        }

        public static Task<string?> UnregisterPackageAsync()
        {
            return RunPowerShellAsync($"$ErrorActionPreference = 'Stop'; Get-AppxPackage -Name {PACKAGE_NAME} | Remove-AppxPackage");
        }

        // The message of a PowerShell error without its position and category lines.
        private static string ShortError(
            string text
        )
        {
            foreach (string marker in new[] { "NOTE:", "At line:" })
            {
                int index = text.IndexOf(marker, StringComparison.Ordinal);

                if (index > 0)
                {
                    text = text.Substring(0, index);
                }
            }

            return text.Trim();
        }

        private static string Quote(
            string text
        )
        {
            return "'" + text.Replace("'", "''") + "'";
        }

        private static Task<string?> RunPowerShellAsync(
            string script
        )
        {
            return Task.Run(() =>
            {
                RunPowerShell(script, out string? error);
                return error;
            });
        }

        // Standard output, or null with the error text when PowerShell failed.
        private static string? RunPowerShell(
            string script,
            out string? error
        )
        {
            error = null;

            try
            {
                var start = new ProcessStartInfo("powershell.exe")
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                };

                start.ArgumentList.Add("-NoProfile");
                start.ArgumentList.Add("-NonInteractive");
                start.ArgumentList.Add("-ExecutionPolicy");
                start.ArgumentList.Add("Bypass");
                start.ArgumentList.Add("-Command");
                start.ArgumentList.Add(script);

                using Process process = Process.Start(start)!;
                Task<string> output = process.StandardOutput.ReadToEndAsync();
                Task<string> errors = process.StandardError.ReadToEndAsync();

                if (!process.WaitForExit(120000))
                {
                    process.Kill(true);
                    error = "PowerShell did not finish within two minutes";
                    return null;
                }

                if (process.ExitCode != 0)
                {
                    string text = errors.Result.Trim();
                    Log.Write("Package registration: " + text);
                    error = text.Length > 0 ? ShortError(text) : $"PowerShell exit code {process.ExitCode}";
                    return null;
                }

                return output.Result;
            }
            catch (Exception failure)
            {
                error = failure.Message;
                return null;
            }
        }
    }
}