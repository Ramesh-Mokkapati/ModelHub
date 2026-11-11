<#
.SYNOPSIS
    Works out the next release version from the git tags and the commit messages since the last one.

.DESCRIPTION
    The last tag of the form vMAJOR.MINOR.PATCH is the base. The commits after it decide the step
    (Conventional Commits style):
      - "type!: ..." or "BREAKING CHANGE:" in a message  -> major (2.3.1 -> 3.0.0)
      - "feat: ..." or "feat(scope): ..."                 -> minor (2.3.1 -> 2.4.0)
      - anything else (fix, docs, chore, ...)            -> patch (2.3.1 -> 2.3.2)
    With no tag yet, the first release is the version in CMakeLists.txt (MODELHUB_VERSION).
    Raising MODELHUB_VERSION in CMakeLists.txt above the last tag makes the next release that version.
    No new commits since the last tag, or "[skip release]" in the latest commit message, means no release.

    Prints the result; in GitHub Actions it also writes version, tag and release to $GITHUB_OUTPUT.
#>
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
Set-Location $Root

function Invoke-Git {
    $output = & git.exe @args
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed with exit code $LASTEXITCODE" }
    $output
}

$cmake = Get-Content (Join-Path $Root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'set\(MODELHUB_VERSION "(\d+\.\d+\.\d+)"\)') { throw "MODELHUB_VERSION not found in CMakeLists.txt" }
$declared = [version]$Matches[1]

$tagPattern = '^v(\d+)\.(\d+)\.(\d+)$'
$lastTag = Invoke-Git tag --list "v*" --merged HEAD --sort=-v:refname |
    Where-Object { $_ -match $tagPattern } | Select-Object -First 1

$commits = @()
if (-not $lastTag) {
    $version = $declared.ToString()
    $bump = "first release"
    $release = $true
} else {
    $null = $lastTag -match $tagPattern
    $major, $minor, $patch = [int]$Matches[1], [int]$Matches[2], [int]$Matches[3]

    # One record per commit: subject and body, separated by a byte no message contains.
    $log = (Invoke-Git log "$lastTag..HEAD" --format="%s%n%b%x1e") -join "`n"
    $commits = @($log -split "\x1e" | ForEach-Object { $_.Trim() } | Where-Object { $_ })

    $release = $commits.Count -gt 0
    if ($commits | Where-Object { $_ -match '^\w+(\([^)]*\))?!:' -or $_ -match '(?m)^BREAKING[ -]CHANGE:' }) {
        $major++; $minor = 0; $patch = 0; $bump = "major"
    } elseif ($commits | Where-Object { $_ -match '^feat(\([^)]*\))?:' }) {
        $minor++; $patch = 0; $bump = "minor"
    } else {
        $patch++; $bump = "patch"
    }
    $version = "$major.$minor.$patch"
    if ($declared -gt [version]$version) {
        $version = $declared.ToString()
        $bump = "set in CMakeLists.txt"
    }
}

$latest = (Invoke-Git log -1 --format="%B") -join "`n"
if ($latest -match '\[skip release\]') { $release = $false }

$tag = "v$version"
$what = if ($lastTag) { "$($commits.Count) commit(s) since $lastTag" } else { "no tag yet" }
Write-Host "Next version: $version ($bump; $what); release: $($release.ToString().ToLower())"

if ($env:GITHUB_OUTPUT) {
    # UTF-8 without a BOM (Windows PowerShell 5.1's -Encoding utf8 would add one).
    $lines = "version=$version`ntag=$tag`nrelease=$($release.ToString().ToLower())`n"
    [System.IO.File]::AppendAllText($env:GITHUB_OUTPUT, $lines, (New-Object System.Text.UTF8Encoding $false))
}
