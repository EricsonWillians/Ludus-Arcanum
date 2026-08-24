# Performance baseline

Baseline captured on 2026-08-22 with a release (`-O3`) build on Ubuntu 24.04, GCC 13.3,
and an Intel Core i7-12700. Google Benchmark reported CPU frequency scaling enabled, so
these values are local observations rather than CI thresholds.

| Benchmark | Median CPU time | Notes |
| --- | ---: | --- |
| Named PCG32 raw draw | 10.4 ns | Existing stream |
| Build 8x8 rectangular topology | 4.32 us | 14.8 million spaces/s |
| Build 32x32 rectangular topology | 124 us | 8.26 million spaces/s |
| Build 128x128 rectangular topology | 3.17 ms | 5.17 million spaces/s |
| Transaction commit plus undo | 19.1 us | Includes canonical post-commit hash |
| Replay 128 movement transactions | 45.3 us | 2.83 million transactions/s |
| Canonical session hash | 17.7 us | 8x8 topology, one entity, populated history |
| Native ray evaluation | 388 ns | 8x8 topology, seven reachable spaces, one entity |
| Native no-op action submit + undo | 525 ns | Reference for callback-boundary comparison |
| Python no-op action submit + undo | 1.12 us | Embedded CPython callback with the same native session path |
| Publish 96-element render snapshot | 124 ns | Copy plus atomic immutable publication |
| Acquire latest render snapshot | 13.2 ns | Atomic shared ownership, no simulation lock |
| Publish 10,000-piece render snapshot | 16.5 us | Stress-scene copy plus immutable publication |
| Prepare 10,000-quad sprite batch | 159 us | Warm reusable buffers, deterministic layer order |
| Pick one space on an 8x8 board | 36.7 ns | CPU bounds test |
| Build 128x128 Studio topology preview | 4.69 ms | 16,384 spaces plus 32,512 visible undirected links |
| Tactical legal-action enumeration | 6.57 us | 23 spaces, 9 entities, movement plus range/LOS attacks |
| Tactical viewer snapshot | 9.47 us | Fog/card filtering, 23 spaces, links, pieces, and action hints |

The no-op comparison puts the incremental Python callback boundary near 0.59 us in this
microbenchmark. It is not a promise for arbitrary callbacks, and it reinforces the
design choice to lower common traversals into native batches rather than call Python
once per topology link or entity.

Run the same suite with the `benchmarks` CMake preset. Large-entity-query baselines
belong to the milestone that introduces that path.

The render stress fixture contains 10,000 simultaneously visible piece quads. Its
159 us CPU batch-preparation median is about 1% of a 60 Hz frame budget, before driver
submission and GPU work. Mesa/Xvfb smoke runs validate desktop GL, GLES, software,
shader, atlas, fallback, and event-loop lifecycle, but are not physical timing evidence.

The Studio preview benchmark measures validation, directed topology generation, and
construction of complete space/link presentation arrays for the maximum rectangular
extent. Its 4.69 ms median is an authoring-time operation, not steady-state rendering;
ordinary 8x8 edits are much smaller. Package file I/O is intentionally excluded so
filesystem and storage-cache behavior do not obscure the native construction cost.

The tactical measurements were added on 2026-08-23 and use the same release toolchain
and host. They are medians of five repetitions with CPU frequency scaling enabled.
Both paths are native after package load: action enumeration performs no Python calls,
and the viewer snapshot filters authoritative entities without copying `GameState`.

## Physical GPU capture

Captured on 2026-08-23 with `ludus-player --stress-sprites 10000` at 1920×1080. The
capture discarded 120 warm-up frames, then measured 600 frames with asynchronous
`GL_TIME_ELAPSED` queries. Hardware was an NVIDIA GeForce RTX 3060 Ti using the NVIDIA
595.84 driver and the negotiated OpenGL ES 3.2 backend.

| Metric | Median | p95 | p99 | Milestone target |
| --- | ---: | ---: | ---: | ---: |
| Renderer CPU | 1.467 ms | 2.820 ms | 2.965 ms | p95 < 4 ms |
| GPU elapsed | 0.293 ms | 0.355 ms | 0.366 ms | p95 < 8 ms |
| Presented frame | 16.652 ms | 18.478 ms | 18.954 ms | p95 < 18 ms |

The renderer CPU and GPU targets pass with substantial margin. Presented-frame timing
is close but does not pass: X11/GTK delivered a 16.652 ms median, while compositor and
frame-clock jitter put p95 0.478 ms above the target. Of 600 measured intervals, 63
exceeded 18 ms. This does not indicate GPU saturation—the GPU p95 was 0.355 ms—but it
remains an open physical integration finding. CI deliberately asserts lifecycle and
correctness, not host timing.
