<#
.SYNOPSIS
    Builds a project and pushes its firmware to the board over HTTP OTA.

.DESCRIPTION
    Used by both EnclosureFanControl (C3) and PrinterDisplay (CYD). The board
    listens on port 8080 and this POSTs the image to it, so the PC only makes
    OUTBOUND connections. That is the whole point: ArduinoOTA works the other
    way round (the board dials out to the uploader), which a default Windows
    Firewall blocks. See each project's README for the failure signature.

    The OTA password is read from the project's git-ignored include\secrets.h so
    it never has to live in a tracked file. An empty password disables auth,
    matching the firmware's own rule.

.EXAMPLE
    .\ota-upload.ps1 -Project ..\EnclosureFanControl -Env esp32-c3-devkitm-1 -Device enclosure-fan-control.local

.EXAMPLE
    .\ota-upload.ps1 -Project ..\EnclosureFanControl -Env esp32-c3-devkitm-1 -Device enclosure-fan-control.local -SkipBuild

.EXAMPLE
    .\ota-upload.ps1 -Project ..\PrinterDisplay -Env esp32dev -Device enclosure-display.local
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Project,

    [Parameter(Mandatory = $true)]
    [string] $Env,

    [Parameter(Mandatory = $true)]
    [string] $Device,

    [int]    $Port     = 8080,
    [string] $User     = 'enclosure',
    [string] $Password,
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'

# Resolved from the parameter, not $PSScriptRoot, so the same script serves
# every project. Using the explicit project dir also sidesteps the comment-only
# platformio.ini in the repo root, which makes `pio` fail from there.
$projectDir  = (Resolve-Path -LiteralPath $Project).Path
$secretsFile = Join-Path $projectDir 'include\secrets.h'
$firmware    = Join-Path $projectDir ".pio\build\$Env\firmware.bin"

function Get-PioExe {
    $candidates = @(
        (Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'),
        (Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    $onPath = Get-Command 'pio' -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw "PlatformIO CLI not found. Looked in $env:USERPROFILE\.platformio\penv\Scripts and on PATH."
}

Write-Host "Project: $projectDir" -ForegroundColor DarkGray

# --- build, and gate the upload on it -------------------------------------
# A non-zero build must abort here: otherwise a compile error would silently
# push the PREVIOUS binary and look like a successful update.
if (-not $SkipBuild) {
    Write-Host ''
    Write-Host "==> Building $Env" -ForegroundColor Cyan
    $pio = Get-PioExe
    Push-Location $projectDir
    try {
        & $pio run -e $Env
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed (exit code $LASTEXITCODE). Nothing was uploaded."
        }
    }
    finally {
        Pop-Location
    }
}

if (-not (Test-Path -LiteralPath $firmware)) {
    throw "Firmware not found: $firmware"
}
$bytes = (Get-Item -LiteralPath $firmware).Length
$kb = [math]::Round($bytes / 1KB)

# The app slot is 1.25 MB on the default partition table. PlatformIO's own size
# check should have caught an overflow already, so this is a cheap last guard
# before spending 30s discovering it over the air.
if ($bytes -gt 1310720) {
    throw ("Image is $bytes bytes, over the 1310720-byte app slot. OTA would " +
           "fail mid-write; shrink the firmware or use a custom partition table.")
}

# --- password: single source of truth, never a tracked file ---------------
if (-not $Password) {
    if (Test-Path -LiteralPath $secretsFile) {
        $raw = Get-Content -LiteralPath $secretsFile -Raw
        $m = [regex]::Match($raw, 'getOTAPassword\s*\(\s*\)\s*\{[^}]*?return\s+"([^"]*)"')
        if ($m.Success) { $Password = $m.Groups[1].Value }
    }
}

# --- resolve the board (mDNS, so a DHCP change does not matter) -----------
# Retried because mDNS registration is best-effort: a board that rebooted
# moments ago (or a missed multicast reply) fails the first lookup even though
# the device is perfectly reachable.
$ip = $null
for ($attempt = 1; $attempt -le 4 -and -not $ip; $attempt++) {
    try {
        $ip = ([System.Net.Dns]::GetHostAddresses($Device) |
               Where-Object { $_.AddressFamily -eq 'InterNetwork' } |
               Select-Object -First 1).IPAddressToString
    } catch {
        $ip = $null
    }
    if (-not $ip -and $attempt -lt 4) { Start-Sleep -Seconds 1 }
}
if (-not $ip) {
    throw ("Could not resolve '$Device' after 4 attempts. Check the board is " +
           "powered and on WiFi, and that the serial log shows it listening on " +
           "port $Port. Pass -Device with its IP address to bypass mDNS.")
}

$url = 'http://' + $ip + ':' + $Port + '/update'

Write-Host ''
Write-Host "==> Uploading $kb KB over HTTP OTA" -ForegroundColor Cyan
Write-Host "    target : $Device ($ip)"
if ($Password) {
    Write-Host '    auth   : password from include/secrets.h'
} else {
    Write-Host '    auth   : DISABLED (no password set)' -ForegroundColor Yellow
}

# -sS rather than --progress-bar on purpose: curl's progress bar goes to stderr,
# which PowerShell surfaces as red error records, so a normal upload would look
# like it had failed. A ~1 MB image only takes a few seconds anyway.
$curlArgs = @('-sS', '--max-time', '180')
if ($Password) {
    $curlArgs += @('-u', "$User`:$Password")
}
$curlArgs += @(
    '-F', "firmware=@$firmware",
    '-w', "`nHTTP %{http_code}  %{size_upload} bytes  %{time_total}s`n",
    $url
)

Write-Host '    uploading, the board reboots when it finishes...'
$out = & curl.exe @curlArgs
$curlExit = $LASTEXITCODE
$out | ForEach-Object { Write-Host $_ }

if ($curlExit -ne 0) {
    throw "curl failed (exit $curlExit). The board was not updated."
}
if (($out -join "`n") -notmatch 'HTTP 200') {
    throw 'The board did not accept the upload (no HTTP 200). It was not updated.'
}

Write-Host ''
Write-Host '==> Update accepted; the board is rebooting.' -ForegroundColor Green
Write-Host "    For per-chunk progress watch the serial log; the device reports it"
Write-Host '    about once a second.'
