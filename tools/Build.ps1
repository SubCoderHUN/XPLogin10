<#
.SYNOPSIS
    Builds XPLogin10 and runs the automated test suite.

.PARAMETER Configuration
    Debug or Release. Defaults to Release.

.PARAMETER Architecture
    x64 or Win32. Must match the Windows you will install on - a 32-bit
    provider cannot be loaded by a 64-bit LogonUI.

.PARAMETER SkipTests
    Build only.

.EXAMPLE
    .\tools\Build.ps1
    .\tools\Build.ps1 -Configuration Debug -Architecture x64
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Architecture = 'x64',

    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build\$Architecture"

Write-Host "XPLogin10 build: $Configuration / $Architecture" -ForegroundColor Cyan

cmake -S $root -B $buildDir -A $Architecture -DXPLOGIN_BUILD_WIN32=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

if (-not $SkipTests) {
    Push-Location $buildDir
    try {
        ctest --build-config $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    } finally {
        Pop-Location
    }
}

$outputDir = Join-Path $buildDir "bin\$Configuration"
if (-not (Test-Path $outputDir)) { $outputDir = Join-Path $buildDir 'bin' }

# The .ini files have to sit next to the binaries for the installer to find them.
foreach ($file in 'XPLogin.ini', 'XPLogin.theme.ini') {
    $source = Join-Path $root "resources\$file"
    $destination = Join-Path $outputDir $file
    if ((Test-Path $source) -and -not (Test-Path $destination)) {
        Copy-Item $source $destination
    }
}

Write-Host "`nBuild output: $outputDir" -ForegroundColor Green
Get-ChildItem $outputDir -Include *.dll, *.exe -Recurse |
    Select-Object Name, Length | Format-Table

# The artwork pack is produced by the build and embedded in the setup, so it is
# only worth reporting - there is nothing to copy separately.
$assets = Join-Path $buildDir 'XPLogin.assets'
if (Test-Path $assets) {
    $size = [math]::Round((Get-Item $assets).Length / 1KB)
    Write-Host "XP artwork baked: $assets ($size KB, embedded in the setup)" -ForegroundColor Green
} else {
    Write-Warning ('No XPLogin.assets was produced. The logon screen will be ' +
                   'drawn from primitives instead of XP''s own bitmaps. ' +
                   'Expected assets\system32\logonui.exe in the repo.')
}

$setup = Join-Path $outputDir 'XPLogin10-Setup.exe'
if (Test-Path $setup) {
    Write-Host 'Next: copy this one file to the test VM and run it.'
    Write-Host "  $setup" -ForegroundColor Yellow
    Write-Host 'For a first install, keep a way in:'
    Write-Host '  XPLogin10-Setup.exe /nofilter' -ForegroundColor Yellow
} else {
    Write-Warning 'XPLogin10-Setup.exe was not produced; check the build output above.'
}
