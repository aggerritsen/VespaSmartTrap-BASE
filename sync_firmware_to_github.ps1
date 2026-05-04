param(
    [string]$CommitMessage = "",
    [string]$Remote = "origin",
    [switch]$SkipPull,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Invoke-RepoGit {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Args
    )

    Write-Host ("git " + ($Args -join " "))
    if (-not $DryRun) {
        & git @Args
        if ($LASTEXITCODE -ne 0) {
            throw "git command failed: git $($Args -join ' ')"
        }
    }
}

function Get-GitOutput {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Args
    )

    & git @Args
    if ($LASTEXITCODE -ne 0) {
        throw "git command failed: git $($Args -join ' ')"
    }
}

function Get-ExternalRepoPaths {
    $paths = @()

    if (Test-Path -LiteralPath "external") {
        Get-ChildItem -LiteralPath "external" -Directory | ForEach-Object {
            $gitPath = Join-Path $_.FullName ".git"
            if (Test-Path -LiteralPath $gitPath) {
                $paths += $_.FullName
            }
        }
    }

    return $paths
}

function Stash-ExternalRepo {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoPath
    )

    Push-Location -LiteralPath $RepoPath
    try {
        $status = (& git status --porcelain) -join "`n"
        if ($LASTEXITCODE -ne 0) {
            throw "Could not read status in external repo: $RepoPath"
        }

        if ([string]::IsNullOrWhiteSpace($status)) {
            Write-Host "External clean: $RepoPath"
            return
        }

        $repoName = Split-Path -Leaf $RepoPath
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $message = "auto-stash before firmware sync $stamp"
        Write-Host "Stashing external repo: $repoName"
        Write-Host $status

        if (-not $DryRun) {
            & git stash push -u -m $message
            if ($LASTEXITCODE -ne 0) {
                throw "Could not stash external repo: $RepoPath"
            }
        }
    }
    finally {
        Pop-Location
    }
}

$repoRoot = Get-GitOutput @("rev-parse", "--show-toplevel") | Select-Object -First 1
$repoRoot = $repoRoot.Trim()
Set-Location -LiteralPath $repoRoot

if ([string]::IsNullOrWhiteSpace($CommitMessage)) {
    $CommitMessage = Read-Host "Commit message for firmware/base changes"
}

if ([string]::IsNullOrWhiteSpace($CommitMessage)) {
    throw "Commit message is required."
}

Write-Host "Repo: $repoRoot"
Write-Host "Remote: $Remote"

Get-ExternalRepoPaths | ForEach-Object {
    Stash-ExternalRepo -RepoPath $_
}

$parentStatus = (& git status --porcelain -- . ":(exclude)external/*") -join "`n"
if ($LASTEXITCODE -ne 0) {
    throw "Could not read parent repo status."
}

if (-not [string]::IsNullOrWhiteSpace($parentStatus)) {
    Write-Host "Staging parent repo changes except external/*"
    Invoke-RepoGit @("add", "--all", "--", ".", ":(exclude)external/*")

    if ($DryRun) {
        Write-Host "Dry run: skipping staged diff and commit."
    }
    else {
    $staged = (& git diff --cached --name-only) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw "Could not inspect staged changes."
    }

    if (-not [string]::IsNullOrWhiteSpace($staged)) {
        Write-Host "Staged files:"
        Write-Host $staged
        Invoke-RepoGit @("commit", "-m", $CommitMessage)
    }
    else {
        Write-Host "No staged parent changes after excluding external/*."
    }
    }
}
else {
    Write-Host "No parent repo changes to commit."
}

$branch = Get-GitOutput @("branch", "--show-current") | Select-Object -First 1
$branch = $branch.Trim()
if ([string]::IsNullOrWhiteSpace($branch)) {
    throw "Cannot sync while detached from a branch."
}

if (-not $SkipPull) {
    Invoke-RepoGit @("pull", "--rebase", $Remote, $branch)
}

Invoke-RepoGit @("push", $Remote, $branch)

Write-Host "Firmware/base sync complete."
