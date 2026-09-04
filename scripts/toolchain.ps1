function Resolve-FcsZig {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [string]$Requested = ""
    )

    $candidate = $null
    if ($Requested) {
        if (Test-Path -LiteralPath $Requested -PathType Leaf) {
            $candidate = (Resolve-Path -LiteralPath $Requested).Path
        } else {
            $command = Get-Command $Requested -ErrorAction SilentlyContinue
            if ($command) { $candidate = $command.Source }
        }
        if (-not $candidate) {
            throw "Zig compiler was not found at '$Requested'."
        }
    } else {
        $portableCandidates = @(
            (Join-Path $Root ".tools\zig-0.16.0\zig.exe"),
            (Join-Path $Root "..\zig-0.16.0\zig-x86_64-windows-0.16.0\zig.exe")
        )
        $candidate = $portableCandidates | Where-Object {
            Test-Path -LiteralPath $_ -PathType Leaf
        } | Select-Object -First 1
        if (-not $candidate) {
            $command = Get-Command zig -ErrorAction SilentlyContinue
            if ($command) { $candidate = $command.Source }
        }
    }

    if (-not $candidate) {
        throw "Zig 0.16.0 was not found. Install it, add it to PATH, or pass -Zig <path-to-zig.exe>."
    }

    $versionOutput = & $candidate version 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Could not read the Zig version from '$candidate'."
    }
    $version = ([string]$versionOutput).Trim()
    if ($version -ne "0.16.0") {
        throw "Zig 0.16.0 is required; '$candidate' reports version '$version'."
    }

    return $candidate
}
