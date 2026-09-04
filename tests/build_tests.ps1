param(
    [string]$Zig = "",
    [switch]$Run
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $repo "scripts\source_manifest.ps1")
. (Join-Path $repo "scripts\toolchain.ps1")
Assert-FcsSourceManifest -Root $repo
$Zig = Resolve-FcsZig -Root $repo -Requested $Zig

$build = Join-Path $repo "build"
$minHook = Join-Path $repo "third_party\minhook"
New-Item -ItemType Directory -Force -Path $build | Out-Null

$cppFlags = @(
    "-target", "x86_64-windows-gnu", "-std=c++17", "-O2",
    "-Wall", "-Wextra", "-Werror", "-Wno-nullability-completeness",
    "-DUNICODE", "-D_UNICODE"
)
$graphicsLibraries = @("-ld3d11", "-ldxgi", "-luser32", "-lkernel32")
$minHookIncludes = @(
    "-I$(Join-Path $minHook 'include')",
    "-I$(Join-Path $minHook 'src')"
)

$firstPartySourceFiles = @(
    foreach ($directory in @('common', 'host', 'hook', 'tests')) {
        Get-ChildItem -LiteralPath (Join-Path $repo $directory) -Recurse -File |
            Where-Object {
                $_.Extension -eq $FcsFirstPartyImplementationExtension -or
                $_.Extension -in $FcsFirstPartyHeaderExtensions
            }
    }
)

$implementationIncludes = @($firstPartySourceFiles |
    Select-String -Pattern '#\s*include\s*["<][^">]+\.(?:c|cc|cpp|cxx)[">]')
if ($implementationIncludes.Count) {
    $locations = $implementationIncludes | ForEach-Object {
        "$($_.Path):$($_.LineNumber)"
    }
    throw "Implementation files must be linked, not included: $($locations -join ', ')."
}

# The producer and consumer hot paths submit through Present and completion
# queries. An explicit Flush would restore the severe frame-time regression.
$hotPathSources = @($firstPartySourceFiles | Where-Object {
    -not $_.FullName.StartsWith((Join-Path $repo 'tests'),
        [System.StringComparison]::OrdinalIgnoreCase)
})
foreach ($source in $hotPathSources) {
    $sourceText = Get-Content -Raw -LiteralPath $source.FullName
    if ($sourceText -match '(?:->|\.)\s*Flush\s*\(') {
        throw "$($source.FullName) contains an explicit Direct3D Flush call."
    }
}

# A split module should not depend on another source file having included its
# prerequisites first. Compile every first-party header as its own C++ unit.
$headerCheckObject = Join-Path $build "header_check.o"
$headerSources = @($firstPartySourceFiles | Where-Object {
    $_.Extension -in $FcsFirstPartyHeaderExtensions
})
foreach ($header in $headerSources) {
    & $Zig c++ @cppFlags -Wno-pragma-once-outside-header `
        -Wno-unused-const-variable -Wno-unused-function `
        -fms-extensions -fno-exceptions -fno-rtti -nostdlib++ `
        -x c++ "-I$repo" @minHookIncludes `
        -c $header.FullName -o $headerCheckObject
    if ($LASTEXITCODE) {
        throw "Standalone header check failed for $($header.FullName) ($LASTEXITCODE)."
    }
}

# Always rebuild production first so the black-box tests cannot accidentally
# exercise a stale DLL left by a different source layout.
& (Join-Path $repo "build.ps1") -Zig $Zig
if ($LASTEXITCODE) { throw "Production build failed ($LASTEXITCODE)." }

$buildManifest = Join-Path $build "BUILD-SHA256SUMS.txt"
$expectedRuntimeArtifacts = @(
    "FfxivCleanStream.exe",
    "FfxivCleanStreamHook64.dll",
    "FfxivCleanStreamHook64.lib",
    "GpuShareSelfTest.exe"
)
$verifiedRuntimeArtifacts = @()
foreach ($line in Get-Content -LiteralPath $buildManifest) {
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') {
        throw "Malformed build checksum entry: $line"
    }
    $artifact = $Matches[2]
    if ($expectedRuntimeArtifacts -notcontains $artifact) {
        throw "Unexpected build checksum entry: $artifact"
    }
    $actual = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath (Join-Path $build $artifact)).Hash.ToLowerInvariant()
    if ($actual -ne $Matches[1]) {
        throw "Build artifact checksum mismatch: $artifact"
    }
    $verifiedRuntimeArtifacts += $artifact
}
$missingRuntimeArtifacts = @($expectedRuntimeArtifacts | Where-Object {
    $verifiedRuntimeArtifacts -notcontains $_
})
if ($missingRuntimeArtifacts.Count) {
    throw "Build checksum manifest is incomplete: $($missingRuntimeArtifacts -join ', ')."
}

$minHookObjects = @()
foreach ($relativeSource in $FcsMinHookSources) {
    $source = Join-Path $repo $relativeSource
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($source)
    $object = Join-Path $build "test_mh_$stem.o"
    & $Zig cc -target x86_64-windows-gnu -std=c17 -O2 `
        @minHookIncludes -c $source -o $object
    if ($LASTEXITCODE) {
        throw "MinHook test object build failed for $relativeSource ($LASTEXITCODE)."
    }
    $minHookObjects += $object
}

$captureSupportObjects = @()
foreach ($relativeSource in $FcsCaptureSupportSources) {
    $source = Join-Path $repo $relativeSource
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($source)
    $object = Join-Path $build "$stem.o"
    & $Zig c++ @cppFlags -c $source -o $object
    if ($LASTEXITCODE) {
        throw "Capture E2E support build failed for $relativeSource ($LASTEXITCODE)."
    }
    $captureSupportObjects += $object
}

& $Zig c++ @cppFlags @minHookIncludes `
    (Join-Path $repo $FcsSyntheticGameSource) `
    @minHookObjects @graphicsLibraries `
    -o (Join-Path $build "SyntheticD3D11Game.exe")
if ($LASTEXITCODE) { throw "Synthetic target build failed ($LASTEXITCODE)." }

foreach ($program in $FcsCaptureTestTargets) {
    $programSources = @($program.Source)
    if ($program.PSObject.Properties.Name -contains 'AdditionalSources') {
        $programSources += @($program.AdditionalSources)
    }
    $resolvedProgramSources = Resolve-FcsSourcePaths `
        -Root $repo -RelativePaths $programSources
    $programSupportObjects = @($captureSupportObjects)
    if ($program.PSObject.Properties.Name -contains 'LinkCaptureSupport' -and
        -not [bool]$program.LinkCaptureSupport) {
        $programSupportObjects = @()
    }
    $programLibraries = @($graphicsLibraries)
    if ($program.PSObject.Properties.Name -contains 'AdditionalLibraries') {
        $programLibraries += @($program.AdditionalLibraries)
    }
    $compileArguments = @('c++') + @($cppFlags) +
        @($resolvedProgramSources) + @($programSupportObjects) +
        @($programLibraries) + @('-o', (Join-Path $build $program.Output))
    & $Zig @compileArguments
    if ($LASTEXITCODE) {
        throw "$($program.Output) build failed ($LASTEXITCODE)."
    }
}

& $Zig c++ @cppFlags (Join-Path $repo $FcsProtocolTestSource) `
    -o (Join-Path $build "PlainLegacyRingProtocolTest.exe")
if ($LASTEXITCODE) { throw "Plain legacy protocol test build failed ($LASTEXITCODE)." }

& $Zig c++ @cppFlags (Join-Path $repo $FcsStatusDumpSource) `
    -luser32 -lkernel32 -o (Join-Path $build "FcsStatusDump.exe")
if ($LASTEXITCODE) { throw "Status dump tool build failed ($LASTEXITCODE)." }

Write-Host "Built modular production binaries and capture tests in $build"

if ($Run) {
    foreach ($test in $FcsTestRuns) {
        $executable = Join-Path $build $test.Output
        & $executable @($test.Arguments)
        if ($LASTEXITCODE) {
            $invocation = @($test.Output) + @($test.Arguments)
            throw "$($invocation -join ' ') failed ($LASTEXITCODE)."
        }
    }
}
