param(
    [string]$Zig = "",
    [switch]$RunSelfTest
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $root "scripts\source_manifest.ps1")
. (Join-Path $root "scripts\toolchain.ps1")
Assert-FcsSourceManifest -Root $root
$Zig = Resolve-FcsZig -Root $root -Requested $Zig

$build = Join-Path $root "build"
$stageRoot = Join-Path $build "staging"
$stage = Join-Path $stageRoot ([Guid]::NewGuid().ToString("N"))
$minHook = Join-Path $root "third_party\minhook"
New-Item -ItemType Directory -Force -Path $build | Out-Null
New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$cppFlags = @(
    "-target", "x86_64-windows-gnu", "-O2", "-std=c++17",
    "-fms-extensions", "-fno-exceptions", "-fno-rtti", "-nostdlib++",
    "-Wall", "-Wextra", "-Werror", "-Wno-nullability-completeness",
    "-DUNICODE", "-D_UNICODE"
)
$minHookIncludes = @(
    "-I$(Join-Path $minHook 'include')",
    "-I$(Join-Path $minHook 'src')"
)

# MinHook is C. Compile it as C rather than letting the C++ link command infer
# a language from neighboring first-party sources.
$minHookObjects = @()
foreach ($relativeSource in $FcsMinHook64Sources) {
    $source = Join-Path $root $relativeSource
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($source)
    $object = Join-Path $stage "minhook_$stem.o"
    & $Zig cc -target x86_64-windows-gnu -std=c17 -O2 `
        @minHookIncludes -c $source -o $object
    if ($LASTEXITCODE -ne 0) {
        throw "MinHook build failed for $relativeSource ($LASTEXITCODE)."
    }
    $minHookObjects += $object
}

$hookSources = Resolve-FcsSourcePaths -Root $root `
    -RelativePaths @($FcsCommonSources + $FcsHookSources)
& $Zig c++ @cppFlags @minHookIncludes @hookSources @minHookObjects `
    -o (Join-Path $stage "FfxivCleanStreamHook64.dll") `
    "-Wl,--out-implib,$(Join-Path $stage 'FfxivCleanStreamHook64.lib')" `
    -shared -ld3d11 -ldxgi -luser32 -lkernel32 -lole32
if ($LASTEXITCODE -ne 0) { throw "Hook build failed ($LASTEXITCODE)." }

$hostSources = Resolve-FcsSourcePaths -Root $root `
    -RelativePaths @($FcsCommonSources + $FcsHostSources)
& $Zig c++ @cppFlags @hostSources `
    -o (Join-Path $stage "FfxivCleanStream.exe") `
    "-Wl,--subsystem,windows" `
    -ld3d11 -ldxgi -luser32 -lgdi32 -lkernel32 -lole32
if ($LASTEXITCODE -ne 0) { throw "Host build failed ($LASTEXITCODE)." }

& $Zig c++ @cppFlags (Join-Path $root $FcsGpuSelfTestSource) `
    -o (Join-Path $stage "GpuShareSelfTest.exe") `
    -ld3d11 -ldxgi -lole32
if ($LASTEXITCODE -ne 0) { throw "Self-test build failed ($LASTEXITCODE)." }

if ($RunSelfTest) {
    & (Join-Path $stage "GpuShareSelfTest.exe")
    if ($LASTEXITCODE -ne 0) {
        throw "GPU sharing self-test failed ($LASTEXITCODE)."
    }
}

# Promote only after every target has built (and, when requested, the staged
# self-test has passed). BUILD-SHA256SUMS.txt is written last, so consumers can
# detect an interrupted promotion instead of trusting a mixed binary set.
$requiredArtifacts = @(
    "FfxivCleanStream.exe",
    "FfxivCleanStreamHook64.dll",
    "FfxivCleanStreamHook64.lib",
    "GpuShareSelfTest.exe"
)
foreach ($artifact in $requiredArtifacts) {
    $stagedArtifact = Join-Path $stage $artifact
    if (-not (Test-Path -LiteralPath $stagedArtifact -PathType Leaf)) {
        throw "Staged build artifact is missing: $stagedArtifact"
    }
}

$promotionArtifacts = @($requiredArtifacts)
foreach ($symbolFile in @(
    "FfxivCleanStream.pdb",
    "FfxivCleanStreamHook64.pdb",
    "GpuShareSelfTest.pdb"
)) {
    if (Test-Path -LiteralPath (Join-Path $stage $symbolFile) -PathType Leaf) {
        $promotionArtifacts += $symbolFile
    }
}
foreach ($artifact in $promotionArtifacts) {
    Copy-Item -LiteralPath (Join-Path $stage $artifact) `
        -Destination (Join-Path $build $artifact) -Force
}

$runtimeArtifacts = @(
    "FfxivCleanStream.exe",
    "FfxivCleanStreamHook64.dll",
    "FfxivCleanStreamHook64.lib",
    "GpuShareSelfTest.exe"
)
$buildHashes = foreach ($artifact in $runtimeArtifacts) {
    $hash = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath (Join-Path $build $artifact)).Hash.ToLowerInvariant()
    "$hash  $artifact"
}
$buildHashes | Set-Content -LiteralPath (Join-Path $build "BUILD-SHA256SUMS.txt") `
    -Encoding ascii

# A failed build keeps its uniquely named stage for diagnosis, but no later
# invocation reuses it. A successful build removes only its own stage.
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "Built standalone clean-stream binaries in $build"
