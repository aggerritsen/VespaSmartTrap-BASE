[CmdletBinding()]
param(
    [string]$Target = 'all',
    [string]$ExtraMakeArgs = '-j8',
    [switch]$KeepPreviousImage,
    [string]$PreviousImageName = 'output.old'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$gv2Root = Join-Path $repoRoot 'gv2_firmware'
$appRoot = Join-Path $gv2Root 'EPII_CM55M_APP_S'
$buildScript = Join-Path $appRoot 'build.ps1'
$elf = Join-Path $appRoot 'obj_epii_evb_icv30_bdv10\gnu_epii_evb_WLCSP65\EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf'
$imageGenRoot = Join-Path $gv2Root 'we2_image_gen_local'
$imageGenExe = Join-Path $imageGenRoot 'we2_local_image_gen.exe'
$imageGenProject = Join-Path $imageGenRoot 'project_case1_blp_wlcsp.json'
$imageInputElf = Join-Path $imageGenRoot 'input_case1_secboot\EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf'
$outputImage = Join-Path $imageGenRoot 'output_case1_sec_wlcsp\output.img'
$previousImage = Join-Path (Split-Path -Parent $outputImage) $PreviousImageName

if (-not (Test-Path -LiteralPath $buildScript)) {
    throw "Missing build script: $buildScript"
}

if (-not (Test-Path -LiteralPath $imageGenExe)) {
    throw "Missing image generator: $imageGenExe"
}

if (-not (Test-Path -LiteralPath $imageGenProject)) {
    throw "Missing image generator project: $imageGenProject"
}

Write-Host 'Building GV2 ELF...' -ForegroundColor Cyan
& powershell -ExecutionPolicy Bypass -File $buildScript -ExtraMakeArgs $ExtraMakeArgs -Target $Target
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if (-not (Test-Path -LiteralPath $elf)) {
    throw "Build completed, but expected ELF was not found: $elf"
}

if ($KeepPreviousImage -and (Test-Path -LiteralPath $outputImage)) {
    if (Test-Path -LiteralPath $previousImage) {
        Remove-Item -LiteralPath $previousImage -Force
    }
    Rename-Item -LiteralPath $outputImage -NewName $PreviousImageName
}

Write-Host 'Staging ELF for image generator...' -ForegroundColor Cyan
Copy-Item -LiteralPath $elf -Destination $imageInputElf -Force

Write-Host 'Generating output.img...' -ForegroundColor Cyan
Push-Location $imageGenRoot
try {
    & $imageGenExe $imageGenProject
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}
finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $outputImage)) {
    throw "Image generator completed, but output image was not found: $outputImage"
}

$item = Get-Item -LiteralPath $outputImage
Write-Host 'GV2 image ready:' -ForegroundColor Green
Write-Host ("{0} ({1} bytes)" -f $item.FullName, $item.Length)
