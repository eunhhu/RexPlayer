[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$KernelPath,
    [string]$ExpectedSha256 = "",
    [switch]$Shutdown
)

$ErrorActionPreference = "Stop"
$resolvedKernel = (Resolve-Path -LiteralPath $KernelPath).Path
$header = [System.IO.File]::ReadAllBytes($resolvedKernel)[0..1]
if ($header[0] -ne 0x4d -or $header[1] -ne 0x5a) {
    throw "The selected file does not have the expected bzImage PE header: $resolvedKernel"
}

$actualSha256 = (Get-FileHash -LiteralPath $resolvedKernel -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ExpectedSha256 -and $actualSha256 -ne $ExpectedSha256.ToLowerInvariant()) {
    throw "Kernel SHA-256 mismatch: expected $ExpectedSha256, got $actualSha256"
}

$configPath = Join-Path $env:USERPROFILE ".wslconfig"
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backupPath = "$configPath.rexplayer-backup-$timestamp"
if (Test-Path -LiteralPath $configPath) {
    Copy-Item -LiteralPath $configPath -Destination $backupPath -Force
    $lines = [System.Collections.Generic.List[string]]::new()
    (Get-Content -LiteralPath $configPath) | ForEach-Object { [void]$lines.Add($_) }
} else {
    $backupPath = "NONE"
    $lines = [System.Collections.Generic.List[string]]::new()
}

$escapedPath = $resolvedKernel.Replace("\", "\\")
$setting = "kernel=$escapedPath"
$sectionStart = -1
$sectionEnd = $lines.Count
for ($index = 0; $index -lt $lines.Count; $index++) {
    if ($lines[$index].Trim().ToLowerInvariant() -eq "[wsl2]") {
        $sectionStart = $index
        for ($next = $index + 1; $next -lt $lines.Count; $next++) {
            if ($lines[$next].Trim() -match '^\[.+\]$') {
                $sectionEnd = $next
                break
            }
        }
        break
    }
}

if ($sectionStart -lt 0) {
    $lines.Insert(0, "[wsl2]")
    $lines.Insert(1, $setting)
    $lines.Insert(2, "")
} else {
    $kernelIndex = -1
    for ($index = $sectionStart + 1; $index -lt $sectionEnd; $index++) {
        if ($lines[$index].Trim() -match '^kernel\s*=') {
            $kernelIndex = $index
            break
        }
    }
    if ($kernelIndex -ge 0) {
        $lines[$kernelIndex] = $setting
    } else {
        $lines.Insert($sectionStart + 1, $setting)
    }
}

$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($configPath, (($lines -join "`r`n").TrimEnd() + "`r`n"), $utf8NoBom)
$verified = Get-Content -LiteralPath $configPath | Where-Object { $_ -eq $setting }
if (-not $verified) {
    throw "Failed to persist the custom WSL kernel setting"
}

Write-Output "WSL_CONFIG=$configPath"
Write-Output "WSL_CONFIG_BACKUP=$backupPath"
Write-Output "KERNEL=$resolvedKernel"
Write-Output "KERNEL_SHA256=$actualSha256"
Write-Output "KERNEL_SETTING=PASS"
if ($Shutdown) {
    & wsl.exe --shutdown
    if ($LASTEXITCODE -ne 0) {
        throw "wsl.exe --shutdown failed with exit $LASTEXITCODE"
    }
    Write-Output "WSL_SHUTDOWN=PASS"
}
