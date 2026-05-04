param(
    [string]$Owner = "",
    [string]$RepoName = "VST-BASE",
    [ValidateSet("public", "private", "internal")]
    [string]$Visibility = "private",
    [switch]$CreateRemote,
    [switch]$SkipInitialCommit
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

function Require-Command($Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command not found: $Name"
    }
}

Require-Command git

if (-not (Test-Path ".git")) {
    git init
}

git branch -M main

if (-not (Test-Path "external\gv2-firmware\.git")) {
    git submodule add -b yolo11-vespa https://github.com/marcory-hub/Seeed_Grove_Vision_AI_Module_V2.git external/gv2-firmware
}

if (-not (Test-Path "external\t-sim-motor-shield\.git")) {
    git submodule add -b master https://github.com/aggerritsen/T-SIMMotorShield.git external/t-sim-motor-shield
}

git submodule update --init --recursive

if (-not $SkipInitialCommit) {
    git add .
    $hasChanges = git status --porcelain
    if ($hasChanges) {
        git commit -m "Initial VST-BASE integration structure"
    }
}

if ($CreateRemote) {
    if ([string]::IsNullOrWhiteSpace($Owner)) {
        throw "Pass -Owner when using -CreateRemote."
    }

    Require-Command gh

    $remoteUrl = "https://github.com/$Owner/$RepoName.git"
    $hasOrigin = git remote | Where-Object { $_ -eq "origin" }

    if (-not $hasOrigin) {
        gh repo create "$Owner/$RepoName" --source . --remote origin "--$Visibility" --push
    } else {
        git push -u origin main
    }

    Write-Host "GitHub repository ready: $remoteUrl"
} else {
    Write-Host "Local Git repository ready."
    Write-Host "To create GitHub remote later:"
    Write-Host "  .\create_github_repo.ps1 -Owner <github-owner> -CreateRemote -Visibility private"
}
