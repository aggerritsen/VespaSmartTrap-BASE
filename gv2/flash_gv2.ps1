[CmdletBinding()]
param(
    [string]$Port = 'COM31',
    [int]$Baudrate = 921600,
    [string]$Protocol = 'xmodem',
    [string]$ImageFile = 'we2_image_gen_local\output_case1_sec_wlcsp\output.img',
    [ValidateSet('gv2-yolo11n-vespa-2026-05top', 'gv2-yolo11n-vespa-2026-02v1')]
    [string]$ModelName = 'gv2-yolo11n-vespa-2026-05top',
    [string]$Model = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$gv2Dir = Join-Path $PSScriptRoot 'firmware'
$sender = Join-Path $gv2Dir 'xmodem\xmodem_send.py'
$imagePath = Join-Path $gv2Dir $ImageFile

if (-not $Model) {
    $Model = "model_zoo/tflm_yolo11_od/$ModelName.tflite 0xB7B000 0x00000"
}

$modelPath = Join-Path $gv2Dir (($Model -split '\s+', 2)[0])

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    throw "Python was not found in PATH."
}

if (-not (Test-Path -LiteralPath $sender)) {
    throw "Missing xmodem sender: $sender"
}

if (-not (Test-Path -LiteralPath $imagePath)) {
    throw "Missing firmware image: $imagePath"
}

if (-not (Test-Path -LiteralPath $modelPath)) {
    throw "Missing model file: $modelPath"
}

$argsList = @(
    'xmodem\xmodem_send.py',
    '--port', $Port,
    '--baudrate', $Baudrate.ToString(),
    '--protocol', $Protocol,
    '--file', $ImageFile,
    '--model', $Model
)

Write-Host 'Flashing GV2:' -ForegroundColor Cyan
Write-Host ("Port: {0}, baudrate: {1}, protocol: {2}" -f $Port, $Baudrate, $Protocol)
Write-Host ("Image: {0}" -f $imagePath)
Write-Host ("Model: {0}" -f $Model)

Push-Location $gv2Dir
try {
    & python @argsList
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
