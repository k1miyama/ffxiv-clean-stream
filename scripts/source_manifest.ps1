# Tracked source ownership for every binary in this repository.
# Keep these lists explicit: Assert-FcsSourceManifest fails when a new source
# file is not assigned to a target or when a listed file disappears.

# First-party implementation files use .cpp. First-party headers use .h or
# .hpp and are checked independently by tests/build_tests.ps1.
$FcsFirstPartyImplementationExtension = ".cpp"
$FcsFirstPartyHeaderExtensions = @(".h", ".hpp")
$FcsUnsupportedFirstPartyImplementationExtensions = @(".c", ".cc", ".cxx")
$FcsUnsupportedFirstPartyHeaderExtensions = @(".hh", ".hxx")

# Implementation files listed here are linked into both production binaries.
# Shared declarations that need no object code stay as normal headers instead.
$FcsCommonSources = @()

$FcsHostSources = @(
    "host\fcs_host.cpp",
    "host\fcs_ui.cpp",
    "host\fcs_localization.cpp",
    "host\fcs_clipboard.cpp",
    "host\fcs_preview_window.cpp",
    "host\fcs_controller.cpp",
    "host\fcs_target.cpp",
    "host\fcs_session.cpp",
    "host\fcs_preview_resources.cpp",
    "host\fcs_preview_frames.cpp"
)

$FcsHookSources = @(
    "hook\fcs_hook.cpp",
    "hook\fcs_state.cpp",
    "hook\fcs_gpu_transport.cpp",
    "hook\fcs_capture_control.cpp",
    "hook\fcs_resource_negotiation.cpp",
    "hook\fcs_capture_resources.cpp",
    "hook\fcs_frame_capture.cpp",
    "hook\fcs_swapchain_hooks.cpp",
    "hook\fcs_worker.cpp"
)

$FcsMinHookSources = @(
    "third_party\minhook\src\buffer.c",
    "third_party\minhook\src\hook.c",
    "third_party\minhook\src\trampoline.c",
    "third_party\minhook\src\hde\hde32.c",
    "third_party\minhook\src\hde\hde64.c"
)

$FcsMinHook64Sources = @(
    "third_party\minhook\src\buffer.c",
    "third_party\minhook\src\hook.c",
    "third_party\minhook\src\trampoline.c",
    "third_party\minhook\src\hde\hde64.c"
)

$FcsCaptureSupportSources = @(
    "tests\capture_process_support.cpp",
    "tests\capture_ipc_support.cpp",
    "tests\capture_gpu_support.cpp"
)
$FcsSyntheticGameSource = "tests\synthetic_d3d11_game.cpp"
$FcsProtocolTestSource = "tests\plain_legacy_ring_protocol_test.cpp"
$FcsGpuSelfTestSource = "tests\gpu_share_self_test.cpp"
$FcsStatusDumpSource = "tests\fcs_status_dump.cpp"

$FcsCaptureTestTargets = @(
    [pscustomobject]@{
        Source = "tests\capture_e2e_test.cpp"
        Output = "CaptureE2ETest.exe"
    },
    [pscustomobject]@{
        Source = "tests\capture_late_rehook_test.cpp"
        Output = "CaptureLateRehookTest.exe"
    },
    [pscustomobject]@{
        Source = "tests\capture_perf_test.cpp"
        Output = "CapturePerfTest.exe"
    },
    [pscustomobject]@{
        Source = "tests\capture_plain_legacy_test.cpp"
        Output = "CapturePlainLegacyTest.exe"
    },
    [pscustomobject]@{
        Source = "tests\host_preview_lifecycle_test.cpp"
        Output = "HostPreviewLifecycleTest.exe"
        LinkCaptureSupport = $false
        AdditionalSources = @(
            "host\fcs_controller.cpp",
            "host\fcs_ui.cpp",
            "host\fcs_localization.cpp",
            "host\fcs_clipboard.cpp",
            "host\fcs_preview_window.cpp",
            "host\fcs_session.cpp",
            "host\fcs_preview_resources.cpp",
            "host\fcs_preview_frames.cpp"
        )
        AdditionalLibraries = @("-lgdi32", "-lole32")
    }
)

$FcsTestRuns = @(
    [pscustomobject]@{ Output = "PlainLegacyRingProtocolTest.exe"; Arguments = @() },
    [pscustomobject]@{ Output = "GpuShareSelfTest.exe"; Arguments = @() },
    [pscustomobject]@{ Output = "HostPreviewLifecycleTest.exe"; Arguments = @() },
    [pscustomobject]@{ Output = "CaptureE2ETest.exe"; Arguments = @() },
    [pscustomobject]@{ Output = "CaptureE2ETest.exe"; Arguments = @("--host-nt-handle") },
    [pscustomobject]@{ Output = "CaptureE2ETest.exe"; Arguments = @("--host-legacy") },
    [pscustomobject]@{ Output = "CapturePlainLegacyTest.exe"; Arguments = @() },
    [pscustomobject]@{ Output = "CaptureLateRehookTest.exe"; Arguments = @() },
    [pscustomobject]@{ Output = "CapturePerfTest.exe"; Arguments = @() }
)

$FcsTestSources = @($FcsCaptureSupportSources) + @(
    $FcsSyntheticGameSource,
    $FcsProtocolTestSource,
    $FcsGpuSelfTestSource,
    $FcsStatusDumpSource
) + @($FcsCaptureTestTargets | ForEach-Object { $_.Source })

function Resolve-FcsSourcePaths {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][object[]]$RelativePaths
    )

    return @($RelativePaths | ForEach-Object { Join-Path $Root ([string]$_) })
}

function Assert-FcsSourceManifest {
    param([Parameter(Mandatory = $true)][string]$Root)

    $resolvedRoot = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    $expectedFirstParty = @(
        $FcsCommonSources + $FcsHostSources + $FcsHookSources + $FcsTestSources
    ) |
        ForEach-Object { ([string]$_).Replace('/', '\') }

    $invalidManifestExtensions = @($expectedFirstParty | Where-Object {
        [System.IO.Path]::GetExtension($_) -ne $FcsFirstPartyImplementationExtension
    })
    if ($invalidManifestExtensions.Count) {
        throw "First-party manifest entries must use $FcsFirstPartyImplementationExtension`: $($invalidManifestExtensions -join ', ')."
    }

    $duplicates = @($expectedFirstParty | Group-Object | Where-Object { $_.Count -ne 1 })
    if ($duplicates.Count) {
        throw "Source manifest contains duplicate entries: $($duplicates.Name -join ', ')."
    }

    $firstPartyFiles = @(
        foreach ($directory in @('common', 'host', 'hook', 'tests')) {
            Get-ChildItem -LiteralPath (Join-Path $resolvedRoot $directory) `
                -Recurse -File
        }
    )

    $unsupportedImplementations = @($firstPartyFiles | Where-Object {
        $_.Extension -in $FcsUnsupportedFirstPartyImplementationExtensions
    })
    $unsupportedHeaders = @($firstPartyFiles | Where-Object {
        $_.Extension -in $FcsUnsupportedFirstPartyHeaderExtensions
    })
    if ($unsupportedImplementations.Count -or $unsupportedHeaders.Count) {
        $unsupportedNames = @($unsupportedImplementations + $unsupportedHeaders |
            ForEach-Object {
                $_.FullName.Substring($resolvedRoot.Length + 1).Replace('/', '\')
            })
        throw "Unsupported first-party source suffix. Use .cpp for implementations and .h or .hpp for headers: $($unsupportedNames -join ', ')."
    }

    $actualFirstParty = @($firstPartyFiles | Where-Object {
        $_.Extension -eq $FcsFirstPartyImplementationExtension
    } | ForEach-Object {
        $_.FullName.Substring($resolvedRoot.Length + 1).Replace('/', '\')
    })

    $unclassified = @($actualFirstParty | Where-Object {
        $expectedFirstParty -notcontains $_
    })
    $missing = @($expectedFirstParty | Where-Object {
        $actualFirstParty -notcontains $_
    })
    if ($unclassified.Count -or $missing.Count) {
        throw "First-party source manifest mismatch. Unclassified: $($unclassified -join ', '); missing: $($missing -join ', ')."
    }

    $expectedMinHook = @($FcsMinHookSources | ForEach-Object {
        ([string]$_).Replace('/', '\')
    })
    $duplicateMinHook = @($expectedMinHook | Group-Object |
        Where-Object { $_.Count -ne 1 })
    if ($duplicateMinHook.Count) {
        throw "MinHook manifest contains duplicate entries: $($duplicateMinHook.Name -join ', ')."
    }
    $actualMinHook = @(Get-ChildItem `
        -LiteralPath (Join-Path $resolvedRoot 'third_party\minhook\src') `
        -Recurse -File -Filter '*.c' | ForEach-Object {
            $_.FullName.Substring($resolvedRoot.Length + 1).Replace('/', '\')
        })
    $unclassifiedMinHook = @($actualMinHook | Where-Object {
        $expectedMinHook -notcontains $_
    })
    $missingMinHook = @($expectedMinHook | Where-Object {
        $actualMinHook -notcontains $_
    })
    if ($unclassifiedMinHook.Count -or $missingMinHook.Count) {
        throw "MinHook source manifest mismatch. Unclassified: $($unclassifiedMinHook -join ', '); missing: $($missingMinHook -join ', ')."
    }

    $productionMinHook = @($FcsMinHook64Sources | ForEach-Object {
        ([string]$_).Replace('/', '\')
    })
    $duplicateProductionMinHook = @($productionMinHook | Group-Object |
        Where-Object { $_.Count -ne 1 })
    $invalidProductionMinHook = @($productionMinHook | Where-Object {
        $expectedMinHook -notcontains $_
    })
    if ($duplicateProductionMinHook.Count -or
        $invalidProductionMinHook.Count) {
        throw "Production MinHook subset is invalid. Duplicates: $($duplicateProductionMinHook.Name -join ', '); unknown: $($invalidProductionMinHook -join ', ')."
    }

    $testOutputs = @(
        'SyntheticD3D11Game.exe',
        'PlainLegacyRingProtocolTest.exe',
        'GpuShareSelfTest.exe',
        'FcsStatusDump.exe'
    ) + @($FcsCaptureTestTargets | ForEach-Object { $_.Output })
    $duplicateOutputs = @($testOutputs | Group-Object |
        Where-Object { $_.Count -ne 1 })
    $unknownRuns = @($FcsTestRuns | Where-Object {
        $testOutputs -notcontains $_.Output
    } | ForEach-Object { $_.Output })
    if ($duplicateOutputs.Count -or $unknownRuns.Count) {
        throw "Test target manifest is invalid. Duplicate outputs: $($duplicateOutputs.Name -join ', '); unknown runs: $($unknownRuns -join ', ')."
    }
}
