[CmdletBinding()]
param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot '..\evidence\SHA256SUMS.txt'),
    [string]$RepositoryRoot = (Join-Path $PSScriptRoot '..\..')
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $RepositoryRoot).Path.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
$rootPrefix = $root + [IO.Path]::DirectorySeparatorChar
$pathComparison = if ($env:OS -eq 'Windows_NT') { [StringComparison]::OrdinalIgnoreCase } else { [StringComparison]::Ordinal }
$count = 0

function Assert-RepositoryLeaf {
    param(
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$DisplayName
    )

    $full = [IO.Path]::GetFullPath($Candidate)
    if (-not $full.StartsWith($rootPrefix, $pathComparison)) {
        throw "Path escapes repository root: $DisplayName"
    }
    $relativeFromRoot = $full.Substring($rootPrefix.Length)
    $current = $root
    foreach ($segment in ($relativeFromRoot -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($segment)) {
            throw "Empty path segment is forbidden: $DisplayName"
        }
        $current = Join-Path $current $segment
        if (-not (Test-Path -LiteralPath $current)) {
            throw "Missing repository path: $DisplayName"
        }
        $item = Get-Item -LiteralPath $current -Force
        $isReparsePoint = ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        $hasLinkType = $item.PSObject.Properties.Name -contains 'LinkType'
        if ($isReparsePoint -or ($hasLinkType -and $null -ne $item.LinkType)) {
            throw "Repository path must not contain a symlink/reparse point: $DisplayName"
        }
    }
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "Repository path is not a file: $DisplayName"
    }
    return $full
}

$manifestCandidate = [IO.Path]::GetFullPath($ManifestPath)
$manifest = Assert-RepositoryLeaf -Candidate $manifestCandidate -DisplayName 'manifest'
foreach ($line in Get-Content -LiteralPath $manifest -Encoding UTF8) {
    if ([string]::IsNullOrWhiteSpace($line)) {
        continue
    }
    if ($line -notmatch '^(?<hash>[0-9a-fA-F]{64})  (?<path>.+)$') {
        throw "Malformed SHA256SUMS row: $line"
    }
    $expected = $Matches.hash.ToLowerInvariant()
    $manifestPath = $Matches.path
    $segments = $manifestPath -split '[\\/]'
    if ($segments | Where-Object { $_ -eq '.' -or $_ -eq '..' -or [string]::IsNullOrWhiteSpace($_) }) {
        throw "Relative traversal or empty segment is forbidden: $manifestPath"
    }
    $relative = $manifestPath.Replace('\', [IO.Path]::DirectorySeparatorChar).Replace('/', [IO.Path]::DirectorySeparatorChar)
    if ([IO.Path]::IsPathRooted($relative)) {
        throw "Rooted manifest path is forbidden: $relative"
    }
    if ($env:OS -eq 'Windows_NT' -and $relative.Contains(':')) {
        throw "Windows alternate-data-stream syntax is forbidden: $relative"
    }
    $full = Assert-RepositoryLeaf -Candidate (Join-Path $root $relative) -DisplayName $manifestPath
    $actual = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "SHA-256 mismatch: $manifestPath expected=$expected actual=$actual"
    }
    $count += 1
}

if ($count -eq 0) {
    throw 'Manifest contained no files'
}
Write-Output "POWERSHELL_SHA256=PASS files=$count"
