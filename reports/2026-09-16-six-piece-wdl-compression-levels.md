# Largest six-piece WDL: compression and match-play probes

Material: `2wX-1wO-2bX-1bO`, the largest available six-piece DTM by
position count. 2,317,447,800 positions; maximum index 2,317,447,799.
Packed WDL: 1,158,723,900 bytes. Uncompressed WDL page: 1024 bytes.

Measured on Ryzen 9 5950X (16 physical cores), Zstd 1.5.5, current
revision-3.402 library, Clang `-O3 -DNDEBUG -march=native`.
The original DTM and existing production WDL files were not modified.

Compression times are medians of three 16-worker compilations, alternating
level order. They include DTM reading/conversion, compression, directory,
and durable publication. These are warm-filesystem-cache measurements.
Every level's complete decompressed bitmap was compared against level 1.

Match-play timings use one thread, compressed images entirely in RAM,
200,000 fixed random compact-bitboard positions/sides, five rounds with
alternating level order. Each fresh probe is warmed with the same query
sequence before the timed replay. Every returned value is compared with
the independently decompressed bitmap, outside timing. Probe time includes
ranking, cache access, and miss processing, not index inversion or allocation.
This is a synthetic random trace, not an engine search trace.

Separate forced page-load timing includes directory lookup, reusable-context
Zstd decompression, and CRC validation without ranking/cache lookup. Of the
200,000 selected pages, 199,731 require decompression; the rest are implicit
all-draw pages. It is a miss-work benchmark rather than per-miss instrumentation.

Requested probe cache budgets include metadata and round down to power-of-two
entry counts. The 64 MiB request has 32,768 slots; 256 MiB has 131,072 slots.
Neither holds this entire database. Replaying the warm-up sequence creates
some hits even though it only visits a subset of the database.

Artifacts: `/tmp/wdl-six-levels/compile.csv`, `/tmp/wdl-six-levels/probes.csv`.
Harnesses: `/tmp/bench_wdl_levels.c` compiled with `-DVERIFY_ONLY`, and
`/tmp/bench_wdl_probe_levels.c` with directory and material arguments.
No production compression default was changed.

## Completed levels 1–19 comparison

Levels 10–19 were subsequently compiled three times each. All nineteen complete
bitmaps were compared byte-for-byte. Probe measurements below come from a fresh
five-round sweep of **all nineteen levels together**, not separate low/high
timing series. Additional raw results: `compile-high.csv` and `probes-all.csv`
in `/tmp/wdl-six-levels`. Every measured probe returned the reference value.

| Level | File MiB | Compile seconds (16 threads) | Page load + CRC µs | Probe 64 MiB µs | Probe 256 MiB µs |
|---:|---:|---:|---:|---:|---:|
| 1 | 333.27 | 3.97 | 3.029 | 3.101 | 2.299 |
| 2 | 340.78 | 4.02 | 3.083 | 3.128 | 2.354 |
| 3 | 334.32 | 4.04 | 2.969 | 3.009 | 2.252 |
| 4 | 329.29 | 4.32 | 2.913 | 2.974 | 2.220 |
| 5 | 319.89 | 4.54 | 2.714 | 2.784 | 2.082 |
| 6 | 314.70 | 4.83 | 2.667 | 2.734 | 2.048 |
| 7 | 312.69 | 5.21 | 2.614 | 2.686 | 2.019 |
| 8 | 311.38 | 6.15 | 2.583 | 2.658 | 2.001 |
| 9 | 312.55 | 6.34 | 2.614 | 2.678 | 2.009 |
| 10 | 312.09 | 6.90 | 2.608 | 2.682 | 2.017 |
| 11 | 300.17 | 7.72 | 2.684 | 2.759 | 2.063 |
| 12 | 293.64 | 9.51 | 2.702 | 2.786 | 2.082 |
| 13 | 291.08 | 12.13 | 2.694 | 2.751 | 2.068 |
| 14 | 289.46 | 12.92 | 2.700 | 2.772 | 2.069 |
| 15 | 288.76 | 15.61 | 2.700 | 2.765 | 2.075 |
| 16 | 289.44 | 20.03 | 2.705 | 2.778 | 2.083 |
| 17 | 288.45 | 23.54 | 2.722 | 2.783 | 2.082 |
| 18 | 288.32 | 27.34 | 2.714 | 2.780 | 2.080 |
| 19 | 288.32 | 26.94 | 2.708 | 2.775 | 2.077 |

Hit rates were identical at every level: 0.013% for 256 KiB, 3.086% for
64 MiB, and 30.877% for 256 MiB. In the 256 MiB case there were 61,754 hits,
138,246 misses, and 138,059 decompressions per 200,000 measured probes.
Requested budgets are not compressed-file caches: they hold decoded pages.
Thus smaller compressed size does not improve these hit rates.

Level 8 is the best measured match-play choice here. Against current level 3,
it reduces page-load time by 13.0%, complete 256 MiB probe time by 11.1%, and
file size by 6.9%. Levels 9/10 are close; small differences should not be
overinterpreted. Levels 11–19 trade a little decoding speed for smaller files.
Level 18 is smallest (302,327,474 bytes versus 302,328,228 for level 19), 7.4%
smaller than level 8, but page loading takes 5.1% longer and complete probes
take 3.9% longer. Generation is about 4.4 times as long as level 8.

This supports level 8 when probe speed is the priority, consistent with the
five-piece experiment. It does not prove a universal best level or predict
the whole engine's nodes/second gain. No setting was changed.
