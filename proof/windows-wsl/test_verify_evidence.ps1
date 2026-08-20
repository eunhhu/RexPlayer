[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$verifier = Join-Path $PSScriptRoot 'verify_evidence.ps1'
$tempBase = Join-Path ([IO.Path]::GetTempPath()) ("rexplayer-verifier-" + [Guid]::NewGuid().ToString('N'))
$root = Join-Path $tempBase 'repository'
$outsideDir = Join-Path $tempBase 'outside'
$manifest = Join-Path $root 'SHA256SUMS.txt'
$payload = Join-Path $root 'payload.txt'
$linkPath = Join-Path $root 'linked'

function Set-TestManifest {
    param([Parameter(Mandatory = $true)][string]$Entry)
    Set-Content -LiteralPath $manifest -Encoding Ascii -Value $Entry
}

function Assert-Rejected {
    param(
        [Parameter(Mandatory = $true)][string]$ExpectedMessage
    )
    try {
        & $verifier -RepositoryRoot $root -ManifestPath $manifest | Out-Null
    }
    catch {
        if ($_.Exception.Message -notmatch $ExpectedMessage) {
            throw
        }
        return
    }
    throw "Verifier accepted a manifest expected to fail: $ExpectedMessage"
}

try {
    New-Item -ItemType Directory -Path $root, $outsideDir -Force | Out-Null
    Set-Content -LiteralPath $payload -Encoding Ascii -Value 'inside'
    $insideHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()

    Set-TestManifest "$insideHash  payload.txt"
    & $verifier -RepositoryRoot $root -ManifestPath $manifest | Out-Null

    Set-TestManifest "$('0' * 64)  ../outside.txt"
    Assert-Rejected 'traversal'

    $outsideFile = Join-Path $outsideDir 'payload.txt'
    Set-Content -LiteralPath $outsideFile -Encoding Ascii -Value 'outside'
    Set-TestManifest "$('0' * 64)  $outsideFile"
    Assert-Rejected 'Rooted manifest path'

    if ($env:OS -eq 'Windows_NT') {
        New-Item -ItemType Junction -Path $linkPath -Target $outsideDir | Out-Null
    }
    else {
        New-Item -ItemType SymbolicLink -Path $linkPath -Target $outsideDir | Out-Null
    }
    $outsideHash = (Get-FileHash -LiteralPath $outsideFile -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-TestManifest "$outsideHash  linked/payload.txt"
    Assert-Rejected 'symlink/reparse point'

    if ($env:OS -eq 'Windows_NT') {
        Set-TestManifest "$('0' * 64)  payload.txt:stream"
        Assert-Rejected 'alternate-data-stream'
    }

    Write-Output 'POWERSHELL_VERIFIER_NEGATIVE_TESTS=PASS'
}
finally {
    if (Test-Path -LiteralPath $linkPath) {
        if ($env:OS -eq 'Windows_NT') {
            [IO.Directory]::Delete($linkPath)
        }
        else {
            Remove-Item -LiteralPath $linkPath -Force
        }
    }
    Remove-Item -LiteralPath $tempBase -Recurse -Force -ErrorAction SilentlyContinue
}
