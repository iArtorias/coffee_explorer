# Coffee Explorer build script.
#
# .\build.ps1                 Release build of core and UI into 'out\bin'.
# .\build.ps1 -Config Debug   Debug build.
# .\build.ps1 -Clean          Removes the out folder first.
# .\build.ps1 -NoZydis        Builds the core without the disassembler (no GitHub download).
# .\build.ps1 -CoreOnly       Builds only the core.
# .\build.ps1 -Die            Prepares 'Detect It Easy' again (it is prepared automatically when 'out\die' is missing).
# .\build.ps1 -NoDie          skips 'Detect It Easy' (no download).
#
# Needs CMake 3.25+, Ninja or Visual Studio 2022/2026 with the C++ workload, and the .NET 8 SDK.
#
# .\build.ps1 -PeLib <dir>    Uses a local 'pelib-updated' checkout instead of fetching it from GitHub.
# Detect It Easy is built separately with 'tools\build_die.ps1'.

param(
    [ValidateSet( "Release", "Debug" )]
    [string] $Config = "Release",
    [switch] $Clean,
    [switch] $NoZydis,
    [switch] $CoreOnly,
    [switch] $Die,
    [switch] $NoDie,
    [string] $PeLib = ""
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$out = Join-Path $root "out"
$core_build = Join-Path $out "core"
$bin = Join-Path $out "bin"

if ( $Clean -and ( Test-Path $out ) )
{
    Remove-Item $out -Recurse -Force
}

function Invoke-Step( [string] $name, [scriptblock] $block )
{
    Write-Host "==> $name" -ForegroundColor Cyan
    & $block

    if ( $LASTEXITCODE -ne 0 )
    {
        throw "$name failed with exit code $LASTEXITCODE"
    }
}

# The core and the shell extension are built with MSVC (MASM kernels of BLAKE3, DbgHelp, WinTrust, the Windows SDK).
# Outside a Visual Studio developer prompt CMake would pick whatever compiler it finds on PATH (MinGW of Strawberry Perl,
# the 'cl.exe' of Visual C++ 6), so the x64 environment of the newest Visual Studio is loaded here.
function Import-VisualStudioEnvironment()
{
    # A developer prompt sets VCToolsInstallDir. A cl.exe on PATH alone proves nothing (Visual C++ 6 installs one).
    if ( $env:VCToolsInstallDir -and ( Test-Path ( Join-Path $env:VCToolsInstallDir "bin\Hostx64\x64\cl.exe" ) ) )
    {
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

    if ( -not ( Test-Path $vswhere ) )
    {
        throw "Visual Studio with the C++ workload was not found (no vswhere.exe). Install it or run build.ps1 from a Developer PowerShell."
    }

    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1

    if ( -not $vs )
    {
        throw "No Visual Studio installation has the C++ x64 tools (Desktop development with C++)."
    }

    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
    Write-Host "==> Visual Studio environment ($vs)" -ForegroundColor Cyan

    # Runs vcvars64 in cmd (a small batch file avoids quoting issues) and copies the resulting environment here.
    $batch = Join-Path ( [IO.Path]::GetTempPath() ) "coffee_vcvars_$PID.cmd"
    Set-Content -Path $batch -Encoding Ascii -Value "@call `"$vcvars`" > nul`r`n@set"

    try
    {
        foreach ( $line in ( & cmd.exe /d /c $batch ) )
        {
            if ( $line -match '^([^=]+)=(.*)$' )
            {
                [Environment]::SetEnvironmentVariable( $Matches[ 1 ], $Matches[ 2 ], "Process" )
            }
        }
    }
    finally
    {
        Remove-Item $batch -ErrorAction SilentlyContinue
    }

    if ( -not $env:VCToolsInstallDir )
    {
        throw "vcvars64.bat did not set up the compiler ($vcvars)"
    }
}

Import-VisualStudioEnvironment

# The compiler by full path, CMake must not pick another 'cl.exe' or 'gcc' from PATH.
$compiler = ( Join-Path $env:VCToolsInstallDir "bin\Hostx64\x64\cl.exe" ) -replace '\\', '/'

# A cache configured with another compiler (an earlier run outside the developer prompt) cannot be switched, start over.
$cache = Join-Path $core_build "CMakeCache.txt"

if ( ( Test-Path $cache ) -and -not ( Select-String -Path $cache -Pattern ( '^CMAKE_CXX_COMPILER:[A-Z]+=' + [regex]::Escape( $compiler ) + '$' ) -Quiet ) )
{
    Write-Host "==> Removing the CMake cache of another compiler" -ForegroundColor Cyan
    Remove-Item $cache -Force
    Remove-Item ( Join-Path $core_build "CMakeFiles" ) -Recurse -Force -ErrorAction SilentlyContinue
}

# Empty. CMake fetches 'iArtorias/pelib-updated' (a cache made with a local folder is switched back too).
$pelib = if ( $PeLib ) { ( Resolve-Path $PeLib ).Path } else { "" }
$zydis = if ( $NoZydis ) { "OFF" } else { "ON" }

# The Visual Studio generator puts binaries in a per-config folder, Ninja does not.
$generator = if ( Get-Command ninja -ErrorAction SilentlyContinue ) { "Ninja" } else { $null }

Invoke-Step "Configure core" {
    if ( $generator )
    {
        cmake -S ( Join-Path $root "core" ) -B $core_build -G $generator "-DCMAKE_BUILD_TYPE=$Config" "-DPECORE_PELIB_DIR=$pelib" "-DPECORE_WITH_ZYDIS=$zydis" `
            "-DCMAKE_C_COMPILER=$compiler" "-DCMAKE_CXX_COMPILER=$compiler"
    }
    else
    {
        cmake -S ( Join-Path $root "core" ) -B $core_build -A x64 "-DPECORE_PELIB_DIR=$pelib" "-DPECORE_WITH_ZYDIS=$zydis"
    }
}

Invoke-Step "Build core" { cmake --build $core_build --config $Config --parallel }

$core_dir = if ( Test-Path ( Join-Path $core_build "$Config\pecore.dll" ) ) { Join-Path $core_build "$Config\" } else { "$core_build\" }

if ( $CoreOnly )
{
    Write-Host "Done: $( Join-Path $core_dir 'pecore.dll' )" -ForegroundColor Green
    return
}

Invoke-Step "Build UI" {
    $project = Join-Path $root "ui\CoffeeExplorer\CoffeeExplorer.csproj"

    # Release is published, ReadyToRun only applies on publish. Debug stays a plain build.
    if ( $Config -eq "Release" )
    {
        dotnet publish $project -c $Config -r win-x64 --self-contained false -p:PeCoreDir=$core_dir -o $bin
    }
    else
    {
        dotnet build $project -c $Config -r win-x64 --self-contained false -p:PeCoreDir=$core_dir -o $bin
    }
}

# Release builds have no debug information.
if ( $Config -eq "Release" )
{
    Get-ChildItem $bin -Filter "*.pdb" -File -ErrorAction SilentlyContinue | Remove-Item -Force
}

# Windows 11 context menu package. The manifest and logos copied by the UI build, packed with makeappx and signed with
# signtool of the Windows SDK. Windows refuses unsigned packages that activate an executable, so the package is signed
# with a self signed certificate made once in 'out\cert' ('CN=Coffee Explorer', the publisher of the manifest).
# Coffee Explorer asks the user to trust its .cer once before it installs the package.
$sdk_bin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\makeappx.exe" -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1

if ( $sdk_bin -and ( Test-Path ( Join-Path $bin "Package\AppxManifest.xml" ) ) )
{
    $makeappx = $sdk_bin.FullName
    $signtool = Join-Path $sdk_bin.DirectoryName "signtool.exe"
    $msix = Join-Path $bin "CoffeeExplorer.ContextMenu.msix"
    $cert_dir = Join-Path $out "cert"
    $pfx = Join-Path $cert_dir "CoffeeExplorer.pfx"
    $cer = Join-Path $cert_dir "CoffeeExplorer.cer"
    $cert_password = "CoffeeExplorer"

    if ( -not ( Test-Path $pfx ) )
    {
        Write-Host "==> Package certificate" -ForegroundColor Cyan
        New-Item -ItemType Directory $cert_dir -Force | Out-Null
        $certificate = New-SelfSignedCertificate -Type Custom -Subject "CN=Coffee Explorer" -KeyUsage DigitalSignature `
            -FriendlyName "Coffee Explorer context menu package" -CertStoreLocation "Cert:\CurrentUser\My" -NotAfter ( Get-Date ).AddYears( 10 ) `
            -TextExtension @( "2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}" )
        $secure = ConvertTo-SecureString $cert_password -Force -AsPlainText
        Export-PfxCertificate -Cert $certificate -FilePath $pfx -Password $secure | Out-Null
        Export-Certificate -Cert $certificate -FilePath $cer | Out-Null
        Remove-Item "Cert:\CurrentUser\My\$( $certificate.Thumbprint )"
    }

    Invoke-Step "Context menu package" {
        & $makeappx pack /o /nv /d ( Join-Path $bin "Package" ) /p $msix | Out-Null

        if ( $LASTEXITCODE -eq 0 )
        {
            & $signtool sign /q /fd SHA256 /f $pfx /p $cert_password $msix
        }
    }

    Copy-Item $cer ( Join-Path $bin "CoffeeExplorer.ContextMenu.cer" ) -Force
}
else
{
    Write-Host "makeappx.exe (Windows SDK) not found, the Windows 11 context menu package is not built" -ForegroundColor Yellow
}

# Detect It Easy. 'tools\build_die.ps1' puts 'die_library' into 'out\die' (downloaded once into 'out\download'). It is prepared
# whenever 'out\die' is missing, '-Die' prepares it again, '-NoDie' skips it. A failure only disables 'File Info'.
$die_dir = Join-Path $out "die"

if ( -not $NoDie -and ( $Die -or -not ( Test-Path ( Join-Path $die_dir "die.dll" ) ) ) )
{
    Write-Host "==> Detect It Easy" -ForegroundColor Cyan

    try
    {
        & ( Join-Path $root "tools\build_die.ps1" )
    }
    catch
    {
        Write-Host "'Detect It Easy' could not be prepared: $( $_.Exception.Message )" -ForegroundColor Yellow
    }

    $global:LASTEXITCODE = 0
}

if ( Test-Path ( Join-Path $die_dir "die.dll" ) )
{
    Write-Host "==> Copy 'Detect It Easy'" -ForegroundColor Cyan
    $bin_die = Join-Path $bin "die"

    if ( Test-Path $bin_die )
    {
        Remove-Item $bin_die -Recurse -Force
    }

    Copy-Item $die_dir $bin_die -Recurse -Force
}
else
{
    Write-Host "'Detect It Easy' is not prepared ('-NoDie' or a failed download), run 'build.ps1 -Die' to prepare it" -ForegroundColor Yellow
}

Write-Host "Done: $( Join-Path $bin 'CoffeeExplorer.exe' )" -ForegroundColor Green