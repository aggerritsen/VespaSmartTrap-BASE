$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$toolRoot = $PSScriptRoot
python (Join-Path $toolRoot 'random_viewer.py') --base (Join-Path $toolRoot 'images') --seconds 10
