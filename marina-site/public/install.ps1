# Install Marina on Windows by building it inside WSL.
# Native Windows is not supported: the compiler and runtime use POSIX.
#   irm https://marina-lang.org/install.ps1 | iex
$ErrorActionPreference = "Stop"

Write-Host "Marina builds on macOS and Linux. On Windows this installer uses WSL."

if (-not (Get-Command wsl -ErrorAction SilentlyContinue)) {
    Write-Error "WSL is not installed. Run 'wsl --install' in an elevated PowerShell, reboot, then re-run this script."
}

wsl -e sh -c "curl -fsSL https://marina-lang.org/install.sh | sh"
if ($LASTEXITCODE -ne 0) {
    Write-Error "The WSL install failed. Inside Ubuntu, install the build tools and re-run: sudo apt install build-essential cmake clang libsqlite3-dev git curl"
}

Write-Host ""
Write-Host "wave is installed inside your WSL distro (~/.local/bin)."
Write-Host "Open a new terminal and run:  wsl wave"
