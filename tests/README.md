# Synthetic capture tests

These tests do not open or control FFXIV, Discord, or MMOMinion. The GPU end-to-end tests use a
hidden 96×64 Direct3D 11 child process with this render order:

1. Draw a blue game scene.
2. Draw a green native-HUD marker.
3. Call `Present`.
4. Draw a red mock-overlay marker through a downstream hook.
5. Let the production capture DLL copy the clean frame before the overlay runs.

A passing capture contains the blue scene and green HUD marker, contains no red overlay pixels,
and confirms that the downstream overlay still ran exactly once.

## Test structure

- `capture_e2e_support.hpp` exposes the shared fixture API. `capture_process_support.cpp`,
  `capture_ipc_support.cpp`, and `capture_gpu_support.cpp` implement process/injection, IPC, and GPU
  verification responsibilities as separate modules.
- `capture_e2e_test.cpp` owns the scenario for the three keyed-sharing ownership modes.
- `capture_plain_legacy_test.cpp` forces every keyed path to fail, then validates the non-keyed
  query/acknowledgement ring with a slow consumer, resize, stale acknowledgements, and hook loss.
- `capture_late_rehook_test.cpp` installs another overlay after capture starts and verifies that
  capture fails closed without duplicating overlay calls.
- `capture_perf_test.cpp` compares a bounded baseline against 30 FPS capture and checks that every
  submitted frame drains from the ring.
- `host_preview_lifecycle_test.cpp` uses only hidden test-owned windows and safe in-process target
  stubs to exercise the real resolution combo, Start, End, and Copy error buttons, preview X, DPI
  sizing, retained IPC session, no-reinjection resume, and unexpected-window-destruction cleanup.
  Its injected in-memory clipboard writer verifies the complete copied error without reading or
  changing the user's clipboard.
- `plain_legacy_ring_protocol_test.cpp` deterministically exercises protocol states that should not
  depend on GPU timing.
- `gpu_share_self_test.cpp` checks every supported cross-device texture-sharing mode.
- `synthetic_d3d11_game.cpp` is the hidden test target.
- `fcs_status_dump.cpp` prints the current IPC state for diagnostics.

First-party implementations use `.cpp`, and first-party headers use `.h` or `.hpp`. Every `.cpp`
file is compiled as its own translation unit. Shared implementation files are linked; they are never
included from another implementation file.

## Build and run

From the repository root, build production and test binaries with strict warnings for first-party
C++ code. Bundled MinHook C sources use separate upstream-compatible compiler flags:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\build_tests.ps1
```

Build and run the complete suite:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\build_tests.ps1 -Run
```

Pass `-Zig <path-to-zig.exe>` when Zig 0.16.0 is not in `.tools/zig-0.16.0` or on `PATH`.

The script rebuilds the production DLL first so black-box tests cannot use a stale binary. It also:

- verifies that every first-party `.cpp` belongs to the tracked source manifest and rejects
  unsupported first-party implementation suffixes;
- rejects textual includes of `.c`, `.cc`, `.cpp`, or `.cxx` implementation files;
- compiles every first-party `.h` and `.hpp` header independently as C++;
- treats first-party C++ warnings as errors; and
- rejects an explicit Direct3D `Flush` call anywhere in shared, host, or hook sources.

Success exits with code 0 and prints a `PASS` result for every executed test.
