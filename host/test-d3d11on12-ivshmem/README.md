# test-d3d11on12-ivshmem

Standalone validation tool for the IVSHMEM-direct buffer design (Phase 1
of `~/.claude/plans/ivshmem-direct-buffers.md`).

## What it tests

In order, with explicit pass/fail per phase:

- **Phase A** — D3D12 device creation; `OpenExistingHeapFromAddress` on
  the IVSHMEM region.
- **Phase B** *(load-bearing)* — `CreatePlacedResource` of a
  `TEXTURE2D` with `ROW_MAJOR` layout in the IVSHMEM heap. If this
  fails, the design is dead in the water on this hardware and we have
  to stick with the cpu-staging fallback we already built.
- **Phase C** — `D3D11On12CreateDevice`; `CreateWrappedResource` to
  wrap the placed resource as an `ID3D11Texture2D` so WGC's D3D11
  context can target it.
- **Phase D** — `Acquire` / `CopyResource` (D3D11 immediate context) /
  `Release`, with a fence wait for GPU completion.
- **Phase E** — Read back through the IVSHMEM BAR mapping and verify
  the GPU wrote the expected per-pixel pattern. Catches cache /
  coherency / tiling problems that would invalidate the design.
- **Phase F** — 100-iteration throughput benchmark. Compares to the
  ~4.7 GB/s needed for sustained 4K @ 144 Hz.

Exit codes:

| Code | Meaning |
|---|---|
| 0 | All phases passed. The design is viable on this hardware. |
| 1 | Environment failure (no IVSHMEM driver, no GPU, no D3D12 device). |
| 2 | Phase B or C failed. The plan needs revisiting. |
| 3 | Phase E failed. Pattern verification mismatch — likely cache or tiling problem. |

## How to run on the Windows guest

1. **Build it.** From a Linux build environment configured for the host
   cross-compile:

       cmake -DBUILD_TEST_D3D11ON12_IVSHMEM=ON .
       make test-d3d11on12-ivshmem

   The binary lands at `test-d3d11on12-ivshmem/test-d3d11on12-ivshmem.exe`
   in the build directory.

2. **Copy to the Windows guest** alongside the existing
   `looking-glass-host.exe` setup. It uses the same Looking Glass
   IVSHMEM driver and reads from device 0 by default.

3. **Stop `looking-glass-host`** before running the test (only one
   process can have IVSHMEM open at a time).

4. **Run** from an Administrator command prompt:

       test-d3d11on12-ivshmem.exe

   Optional flags:

       --width  N   horizontal pixels (default 1920)
       --height N   vertical pixels   (default 1080)
       --iter   N   benchmark iterations (default 100)

   For a 4K test (matches typical capture target):

       test-d3d11on12-ivshmem.exe --width 3840 --height 2160

   Note: a 4K BGRA frame needs ~33 MiB of IVSHMEM. If your IVSHMEM
   region is 32 MiB, the test will refuse to run at 4K — drop to
   1080p first.

5. **Read the output.** Each phase prints `[PASS]` or detailed failure
   info. Phase F prints the achieved bandwidth in GB/s.

## What to do with the results

- **All phases pass** with reasonable Phase F throughput → kick off
  Phase 2+ of the IVSHMEM-direct plan. The design is viable.
- **Phase B fails** → the design is not viable on this hardware. Stick
  with the cpu-staging path. Worth checking with a GPU debug-layer
  validation pass (set `D3D12 debug` flag) to see exactly why.
- **Phase C fails** → D3D11On12 doesn't accept the placed resource.
  Plan needs an alternative interop mechanism.
- **Phase E fails** → bytes landed somewhere but not where we expected.
  Could be cache coherency (host CPU sees stale data), tiling (driver
  silently used a swizzled layout despite ROW_MAJOR request), or BAR
  routing (GPU writes hit a different physical address). Each has
  different implications; the failure output points at first mismatched
  pixels which usually narrows it down.
- **All pass but Phase F is slow** (e.g. < 2 GB/s) → the path works
  but isn't competitive with the existing D3D12 copy-queue path.
  Worth profiling and tuning before adopting; the design probably
  still wins in places (eliminates one copy + simplifies state) even
  at lower bandwidth.

Report back with the full output and we'll plan Phase 2 from there.
