# Full frontier compilation: Zstd levels 1 and 12

Benchmark of the current generator, without dictionaries. This measures actual
frontier reading, page assembly, encoding, checksumming, compression and output,
not compression of preassembled sample pages. It does **not** measure the
proposed dictionary-enabled pipeline, which is not implemented yet.

Host: Ryzen 9 5950X, 16 physical cores, revision 3.402, production optimized
build. Unsliced generation; 16 workers; 2048-byte DTM pages; 1 GiB total
compilation buffer; 16 GiB resident verification limit; 1 GiB fallback
verification cache. Dependency residency 4 GiB total/1024 MiB per database;
shared caches fixed at 64 MiB, no adaptive growth/redistribution. Dependency
DTMs are identical read-only symlinks for every run. Results live in a separate
temporary directory; production databases are untouched.

Five-piece material: `2wX-1wO-1bX-1bO` (102,997,680 positions).
Three pairs in orders 1/12, 12/1, 1/12. Median seconds:

| Level | Frontier compilation | Verify + statistics/fallback | Complete run | File bytes |
|---:|---:|---:|---:|---:|
| 1 | 0.286 | 5.182 | 13.066 | 42,791,245 |
| 12 | 1.079 | 5.096 | 14.125 | 36,454,068 |

Compilation is 3.77x slower (+0.793 s), total runtime is 8.1% longer, and the
file is 14.8% smaller. These runs fit their assembled position ranges into
the configured buffer. Each completed exhaustive verification and all
histograms and example positions matched across all six runs.

Six-piece material: `2wX-1wO-2bX-1bO` (2,317,447,800 positions, raw two-sided
DTM 9,269,791,200 bytes). One pair, level 1 then level 12, as a larger-scale
check rather than a repeated statistical measurement. This exceeds the
compilation buffer and exercises multiple assembly windows. Initialization
and backtracking are rerun normally, but are not controlled by the DTM
compression level; variation in those phases should not be attributed to it.

| Level | Frontier compilation | Verify + statistics/fallback | Complete run | File bytes |
|---:|---:|---:|---:|---:|
| 1 | 3.932 | 157.435 | 327.784 | 769,941,310 |
| 12 | 18.192 | 157.972 | 340.170 | 648,080,765 |

Compilation is 4.63x slower (+14.260 s), total runtime is 3.8% longer, and
the file is 15.8% smaller. Both runs passed exhaustive verification with
`passes=1 updates=0/0`; every WTM/BTM histogram bin and example position
matched. Verification uses a resident database here, so this comparison does
not measure repeated cache-miss decompression savings of the resulting file.

The driver was paused and terminated after its first six-piece level-1 run
completed to avoid four unnecessary repetitions of expensive initialization
and verification; level 12 was run separately with identical settings. The
driver's termination is not a generator failure: all eight generator runs
completed successfully. The reproduction script now specifies three
five-piece pairs and one six-piece pair.

Conclusion: plain level 12 makes full compilation about 3.8–4.6x slower in
these workloads, not the 8–10x suggested by isolated compression timing.
The increase in complete generation time is smaller because other phases
dominate. The dictionary-backed level-12 pipeline remains to be implemented
and benchmarked; these results must not be presented as its runtime.

Raw logs: `/tmp/gwdegtb-frontier-levels-WXNQam/{five,six}`.
Reproduction script: `/tmp/benchmark_frontier_levels.sh`.
No production compression defaults changed.
