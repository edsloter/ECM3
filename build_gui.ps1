param(
    [switch]$Static,
    [switch]$Debug
)

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot
Set-Location $ProjectRoot

$MsysBash = $null
foreach ($candidate in @("C:\msys64\usr\bin\bash.exe", "C:\msys32\usr\bin\bash.exe")) {
    if (Test-Path $candidate) { $MsysBash = $candidate; break }
}
if (-not $MsysBash) {
    Write-Host "ERROR: MSYS2 bash not found. Install MSYS2 with mingw-w64-x86_64-wxwidgets3.2-msw." -ForegroundColor Red
    Write-Host "  pacman -S mingw-w64-x86_64-wxwidgets3.2-msw" -ForegroundColor Yellow
    exit 1
}

# Convert Windows path for MSYS2 (C:\Users\... → /c/Users/...)
$ScriptPath = $PSScriptRoot -replace '^([A-Za-z]):', '/$1'
$ScriptPath = $ScriptPath.ToLower() -replace '\\', '/'

Write-Host "Building ecm3-gui..." -ForegroundColor Cyan
if ($Static) { Write-Host "  Static linking enabled" -ForegroundColor Yellow }
if ($Debug)  { Write-Host "  Debug mode enabled" -ForegroundColor Yellow }

$env:STATIC = if ($Static) { "1" } else { "" }
$env:DEBUG = if ($Debug) { "1" } else { "" }

$result = & $MsysBash "-lc" "source $ScriptPath/build_gui.sh" 2>&1
Write-Host $result

$ReleaseDir = Join-Path $ProjectRoot "release\win64_gui"
if (Test-Path (Join-Path $ReleaseDir "ecm3-gui.exe")) {
    $Size = (Get-Item (Join-Path $ReleaseDir "ecm3-gui.exe")).Length
    Write-Host "Build succeeded: $([math]::Round($Size / 1024, 1)) KB" -ForegroundColor Green
} else {
    Write-Host "Build failed." -ForegroundColor Red
    exit 1
}