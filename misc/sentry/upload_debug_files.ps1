# Upload Windows PDBs / executables to Sentry as debug information files.
# Prerequisites: sentry-cli installed, authenticated (sentry-cli login or SENTRY_AUTH_TOKEN).
param(
    [Parameter(Mandatory = $true)]
    [string]$PdbDirectory,
    [Parameter(Mandatory = $true)]
    [string]$Org,
    [Parameter(Mandatory = $true)]
    [string]$Project
)

$ErrorActionPreference = "Stop"
if (-not (Get-Command sentry-cli -ErrorAction SilentlyContinue)) {
    Write-Error "sentry-cli not found. Install from https://docs.sentry.io/product/cli/"
}

& sentry-cli debug-files upload --org $Org --project $Project $PdbDirectory
