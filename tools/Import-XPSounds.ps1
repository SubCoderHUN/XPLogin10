<#
.SYNOPSIS
    Copies Windows XP sound files into an XPLogin10 installation.

.DESCRIPTION
    XPLogin10 does not ship Microsoft's sound files. This script copies them
    from a source you already have a licence for - a mounted Windows XP ISO, an
    old installation, a backup - into the install directory and writes the
    paths into XPLogin.ini.

    Nothing is downloaded. If the source does not contain the files, the script
    says so and changes nothing.

.PARAMETER Source
    Folder to copy from. Typically <XP drive>\WINDOWS\Media.

.PARAMETER InstallPath
    XPLogin10 install directory. Defaults to the value recorded at install time
    in HKLM\SOFTWARE\XPLogin10\InstallPath.

.EXAMPLE
    .\Import-XPSounds.ps1 -Source D:\WINDOWS\Media
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Source,

    [string]$InstallPath
)

$ErrorActionPreference = 'Stop'

# event name -> the file XP used
$soundMap = [ordered]@{
    'logon'    = 'Windows XP Logon Sound.wav'
    'logoff'   = 'Windows XP Logoff Sound.wav'
    'error'    = 'Windows XP Error.wav'
    'click'    = 'Windows XP Menu Command.wav'
    'shutdown' = 'Windows XP Shutdown.wav'
}

if (-not (Test-Path -LiteralPath $Source)) {
    throw "Source folder not found: $Source"
}

if (-not $InstallPath) {
    $key = 'HKLM:\SOFTWARE\XPLogin10'
    if (Test-Path $key) {
        $dll = (Get-ItemProperty -Path $key -Name 'InstallPath' -ErrorAction SilentlyContinue).InstallPath
        if ($dll) { $InstallPath = Split-Path -Parent $dll }
    }
}
if (-not $InstallPath) {
    $InstallPath = $PSScriptRoot
    Write-Warning "XPLogin10 is not installed; using $InstallPath"
}

$mediaPath = Join-Path $InstallPath 'media'
New-Item -ItemType Directory -Force -Path $mediaPath | Out-Null

$copied = @{}
foreach ($event in $soundMap.Keys) {
    $sourceFile = Join-Path $Source $soundMap[$event]
    if (-not (Test-Path -LiteralPath $sourceFile)) {
        Write-Warning "not found, skipping: $($soundMap[$event])"
        continue
    }
    $destination = Join-Path $mediaPath $soundMap[$event]
    Copy-Item -LiteralPath $sourceFile -Destination $destination -Force
    $copied[$event] = $destination
    Write-Host "copied $($soundMap[$event])"
}

if ($copied.Count -eq 0) {
    Write-Warning 'No sound files were found. XPLogin.ini was not modified.'
    return
}

$iniPath = Join-Path $InstallPath 'XPLogin.ini'
if (-not (Test-Path -LiteralPath $iniPath)) {
    Write-Warning "XPLogin.ini not found at $iniPath; paths were not written."
    Write-Host 'Add these lines to the [sounds] section yourself:'
    foreach ($event in $copied.Keys) { Write-Host ("  {0} = {1}" -f $event, $copied[$event]) }
    return
}

# Rewrite only the keys we own inside [sounds]; everything else is untouched.
$lines = Get-Content -LiteralPath $iniPath
$inSounds = $false
$output = New-Object System.Collections.Generic.List[string]

foreach ($line in $lines) {
    if ($line -match '^\s*\[(.+)\]\s*$') {
        $inSounds = ($Matches[1].Trim().ToLowerInvariant() -eq 'sounds')
        $output.Add($line)
        continue
    }
    if ($inSounds -and $line -match '^\s*([A-Za-z]+)\s*=') {
        $key = $Matches[1].ToLowerInvariant()
        if ($copied.ContainsKey($key)) {
            $output.Add(("{0} = {1}" -f $key, $copied[$key]))
            continue
        }
    }
    $output.Add($line)
}

Set-Content -LiteralPath $iniPath -Value $output -Encoding UTF8
Write-Host "`nUpdated $iniPath"
Write-Host 'Sign out and back in to hear the sounds on the welcome screen.'
