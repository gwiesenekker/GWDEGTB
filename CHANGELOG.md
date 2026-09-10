# Version history

Published versions are Git tags. The `REVISION` file identifies the executable
revision independently of individual commits; version 3.3 starts at 3.301.
Earlier summaries below were reconstructed from the tagged source and commit
history. Release versions and DTM/WDL file-format versions are separate.

## [3.3](https://github.com/gwiesenekker/GWDEGTB/tree/v3.3) — 2026-09-11

### Frontier compilation

- Release the five outcome/candidate bitmaps before allocating compilation
  buffers, saving 0.625 bytes per position during this phase.
- Track minimum/maximum position indices for each compressed frontier block.
  Skip blocks outside the assembly window before reading, decompressing or
  checksumming their payload. Preserve record filtering and overwrite order
  for overlapping blocks, including unsorted records.
- Replace frontier FNV-1a checksums with CRC32C, using hardware acceleration
  where available and a portable fallback. Keep 4 KiB frontier blocks.
- Retain parallel unordered batch writing: physical DTM block order and file
  hashes may differ between runs; decoded results remain the correctness test.

### Generation workflow

- Add `--restart` and use it in family scripts. It removes existing `.dtm` and
  `.dtm.incomplete` files while retaining `.dtm.work` slice checkpoints.
  Non-regular output files are refused. No backups or concurrent-run locks are
  created; do not run two generators for the same output concurrently.
- Verify slices using a shared resident array when they fit the configured
  resident limit; otherwise use the configured verification-cache budget.
- Put timing summary phases in execution order, remove obsolete scan/repair
  rows, show slice verification and merge times, and exclude historical slice
  timings when resuming completed checkpoints.
- Add a README-linked version history.

### Validation and compatibility

- Add frontier range, known-checksum, corruption, restart, resident-slice and
  timing-summary regression coverage; exercise multi-window compilation.
- Benchmark 4/16/64 KiB frontier blocks on `3 0 0 2`, single-threaded, using
  1/16 MiB assembly buffers and three repetitions. All 18 runs passed full
  verification without repairs and produced identical DTMs. Larger blocks
  were not consistently faster, so the default remains 4 KiB.
- Existing DTM/WDL formats and completed slice checkpoints are unchanged.
  The new checksum applies to transient, process-local frontier streams.

## Earlier tagged versions

These are feature summaries, not exhaustive lists of every internal revision.
Version numbers 2.6–2.8 have no Git release tags and are not invented here.

| Version | Main changes |
|---|---|
| [3.2](https://github.com/gwiesenekker/GWDEGTB/tree/v3.2) | Compile compact temporary DTMs directly; combine exhaustive verification, statistics and examples; automatic repair fallback; durable publication after verification; apply the pipeline to slices. |
| [3.1](https://github.com/gwiesenekker/GWDEGTB/tree/v3.1) | Complete lost-in-zero and deferred external-loss frontier scheduling; early exit for disproved loss candidates; storage-safety and finalization improvements; hardware CRC32C for database pages; unsliced inversion optimization; default Zstd level 1 with a runtime override. |
| [3.0](https://github.com/gwiesenekker/GWDEGTB/tree/v3.0) | Expand GWD integration with disk DTM lookup, compressed WDL probes and compact-board APIs; parallel WDL loading; standalone DTM verification and examples; checksummed slice manifests; move-generation improvements and 4x4 job script. |
| [2.9](https://github.com/gwiesenekker/GWDEGTB/tree/v2.9) | Include the preceding 16-bit DTM storage, configurable pages, timestamp/ETA reporting and eight-piece support; expose slicing through family scripts and tolerate kings-only requests for slicing. |
| [2.5](https://github.com/gwiesenekker/GWDEGTB/tree/v2.5) | Bounded page-aligned compilation from per-worker frontier streams; write completed pages once; configurable assembly-memory budget and multi-batch tests. |
| [2.4](https://github.com/gwiesenekker/GWDEGTB/tree/v2.4) | Separate WTM/BTM storage planes; generate a missing WDL from its DTM through the GWD integration API. |
| [2.3](https://github.com/gwiesenekker/GWDEGTB/tree/v2.3) | Man-row sliced generation, slice indexing, resumable checkpoints and full-index merge. |
| [2.2](https://github.com/gwiesenekker/GWDEGTB/tree/v2.2) | Public GWD integration library, caller-owned WDL buffers, attachment and automatic material mirroring. |
| [2.1](https://github.com/gwiesenekker/GWDEGTB/tree/v2.1) | Separate sequential/random views and paired cursors; resident DTM loading for verification with histogram collection. |
| [2.0](https://github.com/gwiesenekker/GWDEGTB/tree/v2.0) | Track verified positions and invalidate affected predecessors to avoid redundant final consistency checks. |
| [1.9](https://github.com/gwiesenekker/GWDEGTB/tree/v1.9) | Indexing, move-generation and cache optimizations; combinatorial-index experiments; manually maintained executable revision. |
| [1.8](https://github.com/gwiesenekker/GWDEGTB/tree/v1.8) | Compressed frontier streams, threaded generation improvements, padded move generation, timing/cache instrumentation and additional family scripts. |
| [1.7](https://github.com/gwiesenekker/GWDEGTB/tree/v1.7) | Two-column DTM frequency statistics. |
| [1.6](https://github.com/gwiesenekker/GWDEGTB/tree/v1.6) | Compact signed-byte DTM storage. |
| [1.5](https://github.com/gwiesenekker/GWDEGTB/tree/v1.5) | Page-partitioned multithreaded EGTB generation. |
| [1.4](https://github.com/gwiesenekker/GWDEGTB/tree/v1.4) | Bitmap retrograde backtracking and per-view caches. |
| [1.3](https://github.com/gwiesenekker/GWDEGTB/tree/v1.3) | Cross-material moves during retrograde backtracking. |
| [1.2](https://github.com/gwiesenekker/GWDEGTB/tree/v1.2) | Generic dependency-aware EGTB generation. |
| [1.1](https://github.com/gwiesenekker/GWDEGTB/tree/v1.1) | Compressed DTM/WDL storage, move generation, initial retrograde generator and regression tests. |
| [1.0](https://github.com/gwiesenekker/GWDEGTB/tree/v1.0) | Dense position indexing and inversion, validation and benchmarks. |
