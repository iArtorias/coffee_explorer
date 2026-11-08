# Prepares 'Detect It Easy' for Coffee Explorer in 'out\die'.
#
#   .\tools\build_die.ps1                         Uses the official 'die_library' win64 release.
#   .\tools\build_die.ps1 -Database <db folder>   Replaces the bundled database.
#   .\tools\build_die.ps1 -FromSource -Qt "C:\Qt\5.15.2\msvc2019_64"   Builds die_library from '..\..\die_library-master' instead (Qt 5.15 or 6 for MSVC x64).
#
# 'build.ps1' copies 'out\die' next to 'CoffeeExplorer.exe'. The core loads 'die\die.dll' at runtime, without it 'File Info' and the
# 'Detect It Easy' tool explain how to get it.

param(
    [switch] $FromSource,
    [string] $Qt,
    [string] $Database,
    [string] $Release = "https://github.com/horsicq/die_library/releases/download/Beta/dielib_win64_portable_0.1.0.zip",
    [string] $Source = ( Join-Path ( Split-Path ( Split-Path $PSScriptRoot -Parent ) -Parent ) "die_library-master" )
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$target = Join-Path $root "out\die"

if ( Test-Path $target )
{
    Remove-Item $target -Recurse -Force
}

if ( -not $FromSource )
{
    $download = Join-Path $root "out\download"
    $zip = Join-Path $download ( Split-Path $Release -Leaf )

    if ( -not ( Test-Path $zip ) )
    {
        New-Item -ItemType Directory -Force -Path $download | Out-Null
        Write-Host "==> Download $Release" -ForegroundColor Cyan
        Invoke-WebRequest -Uri $Release -OutFile $zip -UseBasicParsing
    }

    Write-Host "==> Extract $( Split-Path $zip -Leaf )" -ForegroundColor Cyan
    Expand-Archive -Path $zip -DestinationPath $target -Force

    # The import library and the header are for C callers, the core loads the library dynamically.
    Remove-Item ( Join-Path $target "die.lib" ) -ErrorAction SilentlyContinue
    Remove-Item ( Join-Path $target "include" ) -Recurse -ErrorAction SilentlyContinue
}
else
{
    if ( -not $Qt )
    {
        throw "Pass '-Qt' with the Qt for MSVC x64 folder, for example 'C:\Qt\5.15.2\msvc2019_64'"
    }

    if ( -not ( Test-Path ( Join-Path $Source "src\lib\die_lib.cpp" ) ) )
    {
        throw "die_library sources not found in $Source"
    }

    # A zip download of 'die_library' has empty dep folders, the submodules then come from a fresh recursive clone.
    if ( -not ( Test-Path ( Join-Path $Source "dep\die_script\die_script.cmake" ) ) )
    {
        $clone = Join-Path $root "out\die-src"

        if ( -not ( Test-Path ( Join-Path $clone "dep\die_script\die_script.cmake" ) ) )
        {
            Write-Host "The die_library submodules are missing in $Source, cloning a complete copy" -ForegroundColor Yellow

            if ( Test-Path $clone )
            {
                Remove-Item $clone -Recurse -Force
            }

            git clone --recursive --depth 1 --shallow-submodules https://github.com/horsicq/die_library.git $clone
            if ( $LASTEXITCODE -ne 0 ) { throw "git clone failed, install git or fill the dep folders of $Source" }
        }

        $Source = $clone
    }

    $build = Join-Path $root "out\die-build"
    $generator = if ( Get-Command ninja -ErrorAction SilentlyContinue ) { @( "-G", "Ninja" ) } else { @( "-A", "x64" ) }

    cmake -S $Source -B $build @generator -DCMAKE_BUILD_TYPE=MinSizeRel "-DCMAKE_PREFIX_PATH=$Qt"
    if ( $LASTEXITCODE -ne 0 ) { throw "Configure failed" }

    cmake --build $build --config MinSizeRel --parallel
    if ( $LASTEXITCODE -ne 0 ) { throw "Build failed" }

    # The install step copies 'die.dll', the Qt runtime and the MSVC runtime.
    cmake --install $build --config MinSizeRel --prefix $target
    if ( $LASTEXITCODE -ne 0 ) { throw "Install failed" }

    if ( -not $Database )
    {
        $Database = Join-Path $Source "db"
    }
}

# The local 'Detect-It-Easy' checkout carries a newer database than the release zip.
if ( -not $Database )
{
    $local_db = Join-Path ( Split-Path ( Split-Path $PSScriptRoot -Parent ) -Parent ) "Detect-It-Easy-master\db"

    if ( Test-Path $local_db )
    {
        $Database = $local_db
    }
}

if ( $Database )
{
    if ( -not ( Test-Path $Database ) )
    {
        throw "Database folder $Database not found"
    }

    $db = Join-Path $target "db"

    if ( Test-Path $db )
    {
        Remove-Item $db -Recurse -Force
    }

    Write-Host "Copy database from $Database" -ForegroundColor Cyan
    Copy-Item $Database $db -Recurse
}

if ( -not ( Test-Path ( Join-Path $target "die.dll" ) ) )
{
    throw "die.dll is missing in $target"
}

Write-Host "Detect It Easy ready in $target ('build.ps1' copies it next to 'CoffeeExplorer.exe')" -ForegroundColor Green