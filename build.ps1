param(
    [switch]$Static,
    [switch]$Debug,
    [int]$Jobs = 16
)

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot
Set-Location $ProjectRoot

# Find MSYS2 bash
$MsysBash = $null
foreach ($candidate in @("C:\msys64\usr\bin\bash.exe", "C:\msys32\usr\bin\bash.exe")) {
    if (Test-Path $candidate) { $MsysBash = $candidate; break }
}
if (-not $MsysBash) {
    Write-Host "ERROR: MSYS2 bash not found." -ForegroundColor Red
    Write-Host "  Install MSYS2 with: pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake" -ForegroundColor Yellow
    exit 1
}

# Convert Windows path for MSYS2 (C:\Users\... → /c/Users/...)
$ScriptPath = $PSScriptRoot -replace '^([A-Za-z]):', '/$1'
$ScriptPath = $ScriptPath.ToLower() -replace '\\', '/'

$env:STATIC = if ($Static) { "1" } else { "" }
$env:DEBUG = if ($Debug) { "1" } else { "" }
$env:JOBS = $Jobs

Write-Host "Building ecm3..." -ForegroundColor Cyan
if ($Static) { Write-Host "  Static linking enabled" -ForegroundColor Yellow }
if ($Debug)  { Write-Host "  Debug mode enabled" -ForegroundColor Yellow }

$result = & $MsysBash "-lc" "cd $ScriptPath && source ./build.sh" 2>&1
Write-Host $result

$Output = Join-Path $ProjectRoot "release\win64\ecm3.exe"
if (Test-Path $Output) {
    $Size = (Get-Item $Output).Length
    Write-Host "`nBuild succeeded: $Output ($([math]::Round($Size / 1024, 1)) KB)" -ForegroundColor Green
} else {
    Write-Host "`nBuild failed." -ForegroundColor Red
    exit 1
}
