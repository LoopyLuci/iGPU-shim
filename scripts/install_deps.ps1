<#
Install prerequisites for building Project Synapse on Windows.

Behavior:
- Idempotent: skips already-installed items.
- Prefers `winget` where available, falls back to Chocolatey if present.
- Installs: Visual Studio Build Tools (C++), Git, Python3, CMake (optional).

Usage: Run from an elevated PowerShell prompt (Administrator) when possible.
  .\scripts\install_deps.ps1 [-AcceptAll]

Notes:
- After installing Visual Studio Build Tools, open a "x64 Native Tools Command Prompt"
  or run the `vcvarsall.bat` script to populate environment variables before building.
#>

param(
    [switch]$AcceptAll
)

function Write-Info($m){ Write-Host "[INFO] $m" -ForegroundColor Cyan }
function Write-Warn($m){ Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Write-Err($m){ Write-Host "[ERR] $m" -ForegroundColor Red }

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")) {
    Write-Warn "It's recommended to run this script as Administrator. Some steps may prompt for consent."
}

# Helper: run a command and return success
function Try-Run($cmd, [int]$timeoutSec = 300) {
    try {
        & powershell -NoProfile -Command $cmd
        return $true
    } catch {
        return $false
    }
}

Write-Info "Checking for winget..."
$hasWinget = (Get-Command winget -ErrorAction SilentlyContinue) -ne $null
$hasChoco  = (Get-Command choco -ErrorAction SilentlyContinue) -ne $null

if ($hasWinget) { Write-Info "Found winget - using it for package installs." }
elseif ($hasChoco) { Write-Info "Found Chocolatey - using it for package installs." }
else { Write-Warn "No winget or chocolatey found. The script can still produce instructions but cannot auto-install packages." }

# Visual Studio Build Tools
if ($hasWinget) {
    Write-Info "Installing Visual Studio Build Tools (C++) via winget..."
    if ($AcceptAll -or (Read-Host "Proceed with winget install VisualStudio.BuildTools? (Y/n)") -ne 'n') {
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e --silent
    }
} elseif ($hasChoco) {
    Write-Info "Installing Visual Studio Build Tools via Chocolatey..."
    if ($AcceptAll -or (Read-Host "Proceed with choco install visualstudio2022buildtools? (Y/n)") -ne 'n') {
        choco install visualstudio2022buildtools --ignore-checksums -y
    }
} else {
    Write-Warn "Please install Visual Studio Build Tools manually: https://aka.ms/vs/17/release/vs_BuildTools.exe"
}

# Git
if ((Get-Command git -ErrorAction SilentlyContinue) -eq $null) {
    if ($hasWinget) { Write-Info "Installing Git..."; winget install --id Git.Git -e }
    elseif ($hasChoco) { Write-Info "Installing Git via choco..."; choco install git -y }
    else { Write-Warn "Please install Git: https://git-scm.com/download/win" }
} else { Write-Info "Git already installed." }

# Python3
if ((Get-Command python -ErrorAction SilentlyContinue) -eq $null) {
    if ($hasWinget) { Write-Info "Installing Python3..."; winget install --id Python.Python.3 -e }
    elseif ($hasChoco) { Write-Info "Installing Python3 via choco..."; choco install python -y }
    else { Write-Warn "Please install Python 3.10+: https://www.python.org/downloads/" }
} else { Write-Info "Python already installed." }

# CMake (optional but useful)
if ((Get-Command cmake -ErrorAction SilentlyContinue) -eq $null) {
    if ($hasWinget) { Write-Info "Installing CMake..."; winget install --id Kitware.CMake -e }
    elseif ($hasChoco) { Write-Info "Installing CMake via choco..."; choco install cmake -y }
}

Write-Info "Dependency installation complete. Next steps: open 'x64 Native Tools Command Prompt' and run `msbuild` or `cl` as needed."
Write-Info "See scripts/README_INSTALL.md for more build options."
