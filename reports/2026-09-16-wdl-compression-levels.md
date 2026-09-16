# WDL compression levels 1–9

Measured on 2026-09-16 with the working-tree multithreaded WDL compiler
(revision 3.402), Ryzen 9 5950X, 16 physical cores, Zstd 1.5.5,
Clang `-O3 -DNDEBUG -march=native`.

The largest available five-piece EGTB by position count is
`2wX-1wO-1bX-1bO.dtm`: 102,997,680 positions, maximum index 102,997,679.
Packed WDL size is 51,498,840 bytes, with 1024-byte uncompressed WDL pages.
Source: `/tmp8/gwies/endgame7/2wX-1wO-1bX-1bO.dtm` (read-only).
All benchmark outputs are separate files in `/tmp/wdl-levels-benchmark`.

Compilation uses 16 workers. Reported times are medians of three runs,
including reading/converting DTM, Zstd compression, directory construction,
and durable publication. Compression levels alternate ascending/descending
between rounds. Decompression is the complete public
`wdl_decompress_into_threaded` operation, including warm-cache file reads,
checksum verification, and thread startup; medians of seven runs at each
thread count. Opening the WDL, allocating the destination, and comparison
are outside the timed decompression interval. These are not cold-disk or
isolated Zstd-kernel measurements.

| Zstd level | File bytes | Compile, 16 threads (ms) | Load, 1 thread (ms) | Load, 16 threads (ms) |
|---:|---:|---:|---:|---:|
| 1 | 18,419,623 | 232.150 | 204.259 | 21.578 |
| 2 | 18,877,386 | 244.608 | 206.129 | 23.187 |
| 3 | 18,537,701 | 238.000 | 201.181 | 20.422 |
| 4 | 18,231,309 | 238.111 | 198.456 | 20.718 |
| 5 | 17,871,041 | 263.855 | 192.969 | 19.884 |
| 6 | 17,692,815 | 273.627 | 190.661 | 21.184 |
| 7 | 17,633,297 | 295.095 | 190.378 | 21.743 |
| 8 | 17,608,666 | 312.061 | 190.937 | 19.426 |
| 9 | 17,625,399 | 369.987 | 190.296 | 21.269 |

Every decompression at every level/thread count was compared byte-for-byte
with level 1: all identical. File sizes include headers and the page directory.

Level 8 gave the smallest file, 5.01% below level 3, for 31.1% more compilation
time in this case. Level 6 is a compromise: 4.56% smaller than level 3 for
15.0% more compilation time. Decompression did not get slower at higher
levels; 16-thread differences of a few milliseconds should not be overinterpreted.
Higher Zstd levels do not guarantee monotonically smaller independent small
pages (levels 2 and 9 illustrate this).

No compression default was changed. This single-material experiment is not
evidence for the same savings across all six-/seven-piece material.

Reproduction harness: `/tmp/bench_wdl_levels.c`.
Raw results: `/tmp/wdl-levels-benchmark/results.csv`.

## Match-play probe benchmark

Additional benchmark uses the actual `gwdegtb_wdl_lookup_probe_compact` API,
with the compressed image already in RAM and one calling thread. The same
200,000 pseudorandom positions and sides are precomputed for every level.
Index inversion is outside timing; popcounts, index calculation, cache lookup,
and miss handling are inside. Each fresh probe receives a full warm-up replay
before the measured replay. Every measured result is checked individually
against the independently decompressed level-1 bitmap, outside timing.

Five rounds alternate ascending and descending compression levels. Cache
budgets are 256 KiB, 4 MiB, 64 MiB, and a separate 256 MiB hit-only control.
These are uniformly random queries, not a recorded game-search trace.

A separate forced page-load loop performs directory lookup, Zstd decompression,
and CRC validation on the same random pages, using a reusable context and page
buffer, with no cache or ranking. Almost all selected pages contain compressed
payloads (199,995 out of 200,000). Its time isolates the miss-work cost more
closely than full-file loading, but is not an instrumented per-miss latency
measurement inside the probe itself.

Probe harness: `/tmp/bench_wdl_probe_levels.c`; compiled normally for the three
cache budgets, and with `-DHIT_ONLY` for the 256 MiB control. Raw results:
`/tmp/wdl-levels-benchmark/probe-results.csv` and
`/tmp/wdl-levels-benchmark/probe-hits.csv`.

### Results (median nanoseconds per operation)

| Level | Forced page load + CRC | Probe 256 KiB | Probe 4 MiB | Probe 64 MiB | Probe 256 MiB |
|---:|---:|---:|---:|---:|---:|
| 1 | 3135.9 | 3171.7 | 3075.8 | 1226.6 | 113.1 |
| 2 | 3215.5 | 3263.3 | 3153.3 | 1258.4 | 113.0 |
| 3 | 3114.2 | 3151.9 | 3047.1 | 1213.8 | 109.8 |
| 4 | 3077.3 | 3116.3 | 3006.7 | 1198.3 | 108.5 |
| 5 | 2954.4 | 3008.3 | 2895.1 | 1167.7 | 108.7 |
| 6 | 2914.7 | 2952.4 | 2876.1 | 1153.1 | 104.2 |
| 7 | 2889.3 | 2920.6 | 2837.9 | 1139.6 | 108.6 |
| 8 | 2865.2 | 2904.5 | 2827.2 | 1136.8 | 105.4 |
| 9 | 2884.3 | 2920.1 | 2838.8 | 1135.0 | 118.4 |

All levels had exactly the same measured cache outcomes:

| Requested budget | Hits | Misses | Decompressions | Hit rate |
|---:|---:|---:|---:|---:|
| 256 KiB | 531 | 199,469 | 199,464 | 0.266% |
| 4 MiB | 8,094 | 191,906 | 191,901 | 4.047% |
| 64 MiB | 130,420 | 69,580 | 69,577 | 65.210% |
| 256 MiB | 200,000 | 0 | 0 | 100% |

Requested capacity includes entry metadata and the current probe rounds its
slot count down to a power of two. Consequently a 64 MiB request does not
hold all 49.1 MiB of packed WDL data: it has 32,768 page slots. The 256 MiB
control does hold all pages. All-hit timing variation is noise/layout/cache
effects, not a compression-level advantage: no decompressor runs there.

Level 8 reduced forced page-load time by 8.0% against level 3 (approximately
349,000 versus 321,000 page loads/s), and complete 64 MiB-cache probe time by
6.3%. It also gave the smallest file in the prior experiment. For this
material, level 8 is therefore the best measured match-play choice; level 9
offers no convincing further speed benefit and generates a slightly larger
file more slowly. Results cannot establish a universal winner across all
materials, nor predict the engine's overall nodes/s improvement. No default
was changed.
