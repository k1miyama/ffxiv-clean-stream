param(
    [string]$Zig = "",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "toolchain.ps1")
$Zig = Resolve-FcsZig -Root $root -Requested $Zig

# Packaging always starts from a fresh production build and complete black-box
# regression run. This prevents a valid checksum file from blessing stale code.
& (Join-Path $root "tests\build_tests.ps1") -Zig $Zig -Run
if ($LASTEXITCODE) { throw "Release verification failed ($LASTEXITCODE)." }

$build = Join-Path $root "build"
$release = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    Join-Path $root "release"
} elseif ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
    [System.IO.Path]::GetFullPath($OutputDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
}
$stageRoot = Join-Path $build "package-staging"
$stage = Join-Path $stageRoot ([Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path $release | Out-Null

$packageFiles = @(
    [pscustomobject]@{ Source = "build\FfxivCleanStream.exe"; Destination = "FfxivCleanStream.exe" },
    [pscustomobject]@{ Source = "build\FfxivCleanStreamHook64.dll"; Destination = "FfxivCleanStreamHook64.dll" },
    [pscustomobject]@{ Source = "build\GpuShareSelfTest.exe"; Destination = "GpuShareSelfTest.exe" },
    [pscustomobject]@{ Source = "LICENSE.txt"; Destination = "LICENSE.txt" },
    [pscustomobject]@{ Source = "third_party\minhook\LICENSE.txt"; Destination = "MinHook-LICENSE.txt" },
    [pscustomobject]@{ Source = "README.md"; Destination = "README.md" },
    [pscustomobject]@{ Source = "RUN-FIRST.txt"; Destination = "RUN-FIRST.txt" },
    [pscustomobject]@{ Source = "THIRD-PARTY-NOTICES.txt"; Destination = "THIRD-PARTY-NOTICES.txt" },
    [pscustomobject]@{ Source = "VERSION.txt"; Destination = "VERSION.txt" }
)

foreach ($file in $packageFiles) {
    $source = Join-Path $root $file.Source
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Release input is missing: $source"
    }
    Copy-Item -LiteralPath $source `
        -Destination (Join-Path $stage $file.Destination) -Force
}

$checksumLines = foreach ($file in $packageFiles) {
    $hash = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath (Join-Path $stage $file.Destination)).Hash.ToLowerInvariant()
    "$hash  $($file.Destination)"
}
$stagedManifest = Join-Path $stage "SHA256SUMS.txt"
$checksumLines | Set-Content -LiteralPath $stagedManifest -Encoding ascii

$expectedReleaseNames = @($packageFiles | ForEach-Object { $_.Destination }) +
    @("SHA256SUMS.txt")
$unexpectedReleaseFiles = @(Get-ChildItem -LiteralPath $release -File |
    Where-Object { $expectedReleaseNames -notcontains $_.Name })
if ($unexpectedReleaseFiles.Count) {
    throw "Release contains unclassified files: $($unexpectedReleaseFiles.Name -join ', ')."
}

# Copy the manifest last. If promotion is interrupted, the previous manifest
# will not match the mixed directory and verification will fail visibly.
foreach ($file in $packageFiles) {
    Copy-Item -LiteralPath (Join-Path $stage $file.Destination) `
        -Destination (Join-Path $release $file.Destination) -Force
}
Copy-Item -LiteralPath $stagedManifest `
    -Destination (Join-Path $release "SHA256SUMS.txt") -Force

$seen = @()
foreach ($line in Get-Content -LiteralPath (Join-Path $release "SHA256SUMS.txt")) {
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') {
        throw "Malformed release checksum entry: $line"
    }
    $name = $Matches[2]
    if (@($packageFiles | ForEach-Object { $_.Destination }) -notcontains $name) {
        throw "Unexpected release checksum entry: $name"
    }
    $actual = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath (Join-Path $release $name)).Hash.ToLowerInvariant()
    if ($actual -ne $Matches[1]) {
        throw "Release checksum mismatch: $name"
    }
    $seen += $name
}
$missing = @($packageFiles | Where-Object { $seen -notcontains $_.Destination })
if ($missing.Count) {
    throw "Release checksum manifest is incomplete: $($missing.Destination -join ', ')."
}

# A successful package removes only its own unique staging directory. Failed
# stages remain available for diagnosis and are never reused by later runs.
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "Packaged and verified release in $release"
