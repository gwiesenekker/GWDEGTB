# Parallel shared-cache initialization

Local benchmark on the 5950X host (16 physical cores, 32 logical CPUs),
2026-09-25. No changes or benchmarks were run on the active 8p generator.

Command: `./test_shared_init 4096`, optimized Clang Makefile build.
Three rounds, with the middle round reversing thread-count order. Each case
allocates and initializes a fresh 4 GiB shared cache, then discards it and
replaces it with 3 GiB. All page words are initialized with C11 `atomic_init`;
no lazy-zero assumption or concurrent access to uninitialized atomic objects.
The backing file is temporary and all-draw, so this measures allocation,
first-touch and initialization, not decompression or hit-path performance.

| Requested threads | 4 GiB initialization median | Full 4→3 GiB discard-resize median |
|---:|---:|---:|
| 1 | 1.972413 s | 1.879490 s |
| 8 | 0.446528 s | 0.727173 s |
| 16 | 0.423553 s | 0.726789 s |
| 32 | 0.440727 s | 0.733618 s |
| 64 | 0.438948 s | 0.740997 s |

At 16 threads: initialization is 4.66× faster and full discard-resize is
2.59× faster. Allocation itself takes only tens of microseconds; old-storage
freeing takes about 0.4 seconds and remains serial. Sixty-four threads
oversubscribe this local machine; these results do not predict scaling on the
dual-socket 64-core server or directly predict the earlier 40.95-second pause.

The generator passes its worker count at quiescent maintenance checkpoints.
The initializer uses disjoint slot/page ranges, with at least 4 MiB per worker,
and keeps all probes stopped until joins complete. Migration still runs serially.
Regression coverage includes uneven partitions, retained probes, migration,
discard shrink/grow, dense transitions, minimal fallback-sized caches and a
linker-wrapped test in which alternate thread creations fail.
