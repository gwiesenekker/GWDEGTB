# Segmented-cache prototype benchmark

This is an isolated experiment, not a production backend change. The running
remote generator and production cache implementation are untouched.

## Implementation

`benchmark_segmented_cache.c` includes `egtb.c` in its own executable, allowing
the benchmark to use the actual production shared cache, page decoder, CRC,
sampled miss timing, and optimistic publication protocol. Do not link it with
`egtb.o`. Build with `make benchmark_segmented_cache`.

The prototype has eight fixed logical buckets per side. Each owns separately
allocated power-of-two-size segments. Two mappings were explored:

1. Exact remainder within each bucket. A count change remaps many pages.
2. Incremental splitting: choose a segment using a mask and fold the unfinished
   part of the next doubling back into its parent. Slot offsets within a segment
   never change. Growth/shrink changes only the split/merged groups.

Transfers happen with all probes stopped. The donor releases segments from its
largest buckets; the receiver attaches them to its smallest buckets. These are
capacity-balancing choices, not a workload-aware replacement policy. Existing
segments and their atomic objects remain allocated. Only transferred sequence
counters and tags are reset; payload is not cleared. The production miss path
fills every page word before publishing it. Cross-database transfer is tested.

The mechanism deliberately excludes concurrent resizing, dense-mode integration,
allocation-failure recovery, and integration with the production coordinator.
It currently requires planar 2 KiB pages. Metadata is 64 bytes per page, as in
the flat cache, plus the segment/bucket directories.

## Method

- Ryzen 5950X, 16 physical cores; `taskset -c 0-15`, 16 threads throughout.
- 128 GB installed RAM (125 GiB visible); AOCC 5.2 / Clang 17.0.6.
- `-O3 -DNDEBUG -march=native -std=c11 -pthread`.
- Three repetitions of the access/transfer/large-memory comparisons, reversing
  implementation order in the middle repetition; five rounds for hit and trace
  comparisons. Reported aggregates are medians, not best runs.
- Real six-piece files: `2wX-2wO-1bX-1bO.dtm` and
  `2wX-2wO-0bX-2bO.dtm`, read-only from `/tmp8/gwies/endgame7`.
- Compressed file contents are OS-cache-warm. "Cold" means an empty application
  cache, not cold disk. Initialization/preloading is outside lookup timings.
- Six access patterns: fitting random hot set, uniform over twice capacity,
  90/10 hot/cold, 16 queries per sequential page, stride-eight bucket skew, and
  a shifted sequential window. The two sequential patterns are separate runs,
  not an ongoing controller adapting to a workload transition.
- 512 and 640 MiB cache payloads test power-of-two and fractional capacities.
- 1, 4 and 16 MiB segments test transfer granularity; larger tests use 16 and
  64 MiB segments.
- Transfer tests include both donor and receiver refill, rollback, both sides,
  page collisions, and partial final pages. Every transfer-workload result is
  checked against a resident reference. All modes must also produce the same
  per-workload checksum. The reference uses the existing decoder, not an
  independent DTM implementation.

An additional instrumented generator (`benchmark_cache_trace.c`, linked instead
of `egtb.o`) captured 9,766,382 real accesses to `2wX-1wO-2bX-0bO.dtm` while
generating `1 2 2 0` with 16 threads. Capture retains up to one million queries
from each of the first 16 callers. It is a bounded initialization-prefix trace,
not a full seven/eight-piece run. The private generation completed exhaustive
verification. Replays preserve each worker's order; inter-worker scheduling is
not fixed. Long trace measurements repeat the trace 100 times per phase to avoid
timing only a few milliseconds. Capture time itself is not benchmark evidence.

## Reproduction

Examples (local paths may need adapting):

```sh
make benchmark_segmented_cache
SEG_LINEAR=1 taskset -c 0-15 ./benchmark_segmented_cache real \
  /tmp8/gwies/endgame7/2wX-2wO-1bX-1bO.dtm 640 4 3 250000 0
SEG_LINEAR=1 taskset -c 0-15 ./benchmark_segmented_cache transfer \
  /tmp8/gwies/endgame7/2wX-2wO-1bX-1bO.dtm \
  /tmp8/gwies/endgame7/2wX-2wO-0bX-2bO.dtm 512 4 3 250000
SEG_LINEAR=1 taskset -c 0-15 ./benchmark_segmented_cache large 32768 64 3
```

`real` arguments: database, cache MiB, segment MiB, rounds, queries per worker,
validate-every-value flag. `transfer`: two databases, donor MiB, segment MiB,
rounds, queries per worker; receiver starts at half donor capacity and transfer
is a quarter of donor capacity. `large`: donor MiB, segment MiB, rounds; receiver
starts at a quarter and transfer is an eighth. The large test uses synthetic
populated tags and allocated/initialized payload, not real compression or refill.

`SEG_LINEAR=0` selects the within-bucket remainder mapping. `SEG_ONLY=0` selects
the fitting-hit workload. `SEG_WORKLOAD=1` selects uniform transfer traffic
(default is 90/10). `SEG_TRACE` names the captured binary trace and `SEG_REPEATS`
controls repetition (1–100). In trace mode use `SEG_ONLY=0`.

Raw outputs are retained under `/tmp/seg-*.log`; intermediate exploratory
implementations also have logs there. Final measured results are summarized below.

## Results

Selected final raw outputs are also saved in `2026-09-25-segmented-cache-data/`.
Final lookup comparisons explicitly prevent inlining of both entry points: an
earlier build inlined only the flat lookup. Both candidates use the same cold
production miss handler. Intermediate timings must not be mixed with these.

### Large redistribution pauses

Median seconds, three rounds. These measure resizing, not subsequent lookups.

| Initial donor + receiver; transfer | Discard/rebuild | Retained migration | Segmented transfer |
|---|---:|---:|---:|
| 8 + 2 GiB; transfer 1 GiB | 2.197 | 3.481 | 0.00298 |
| 32 + 8 GiB; transfer 4 GiB | 7.535 | 12.950 | 0.01118 |

The large test reached 73,662,588 KiB maximum RSS (70.25 GiB), with no swapping.
This is the process peak across all candidates, not a separately measured
segmented peak. Segmentation retains the 40 GiB payload and resets only the
transferred tags/sequence metadata (128 MiB for a 4 GiB transfer). The payload
does not move. This explains the large pause reduction; it is not evidence of
a comparable end-to-end generator speedup.

### Redistribution plus real decoding/refill

512 MiB donor, 256 MiB receiver, transfer 128 MiB; 4 MiB segments. Following
redistribution, each database receives four million 90/10 hot/cold queries.
Every returned value is checked. Medians over three rounds:

| Backend | Resize (s) | Subsequent queries (s) | Resize + queries (s) | Subsequent decompressions |
|---|---:|---:|---:|---:|
| Discard/rebuild | 0.1323 | 0.4143 | 0.5466 | 597,138 |
| Retained migration | 0.2094 | 0.3519 | 0.5879 | 369,386 |
| Segmented incremental split | 0.00070 | 0.2343 | 0.2349 | 257,101 |

Combined medians are calculated from each round's sum, not by adding separate
medians. Here segmentation cuts resize-plus-refill time by 57% versus discard.
It also preserves more useful entries. The opposite transfer (rollback) was
tested and its results validated too. This is a fixed transfer workload, not an
adaptive-coordinator or whole-generator benchmark.

### Steady-state lookup cost: important regression

The captured generator trace was replayed 100 times (976.6 million queries),
five rounds per capacity, with no transfer during measurement:

| Cache payload | Flat time (s) | Segmented time (s) | Extra elapsed time |
|---|---:|---:|---:|
| 32 MiB | 1.0011 | 1.2366 | 23.5% |
| 64 MiB | 0.7843 | 0.9799 | 24.9% |
| 96 MiB | 0.7020 | 0.9396 | 33.8% |

Hit rates were approximately 99.91%, 99.99%, and 100%, respectively, for both
backends. The 96 MiB case has no warm-pass decompressions: the extra addressing
and indirection are visible directly. Absolute timings had some scheduling/code
layout variability. A separate three-round, 64-byte-aligned-probe control still
showed penalties of approximately 23%, 27%, and 20%; alignment did not remove the
regression. These are lookup-phase penalties, not whole-generation slowdowns.

Fractional capacity also has a collision tradeoff. At 640 MiB with 4 MiB segments:

| Workload (4 million queries) | Flat (s) | Segmented (s) |
|---|---:|---:|
| Fitting hot set | 0.0277 | 0.0281 |
| Uniform, twice capacity | 1.2241 | 1.2226 |
| 90/10 hot/cold | 0.1608 | 0.2134 |
| Sequential, 16 queries/page | 0.2702 | 0.2506 |
| Stride-eight bucket skew | 1.2158 | 1.2152 |
| Shifted sequential window | 0.2869 | 0.2819 |

The fractional hot/cold test made 222,213 decompressions versus 201,836 for flat
(10.1% more). Incremental splitting avoids global remapping, but partially split
buckets have uneven contention. It is not equivalent to exact modulo over every
available slot. Short timings should not be overinterpreted.

Exploratory 1/4/16 MiB segment tests all passed value checks. The 16 MiB version
lost more useful entries after transfer (~312k versus ~257k decompressions for
1/4 MiB). Smaller granularity helps retention, but this does not establish one
universal best segment size. Simple per-bucket modulo also transferred cheaply,
but caused more remapping/refill than incremental splitting in the hot/cold test.

## Validation and limits

- Concurrent probes, collisions, both sides, final partial pages, cross-database
  transfers and rollback passed value/checksum comparisons.
- ASan + UBSan passed the small transfer/rollback tests for both mappings,
  including uneven bucket sizes and the aligned-probe variant.
- GCC ThreadSanitizer built, but could not execute: `unexpected memory mapping`
  both inside and outside the sandbox. No claim of a TSan-clean prototype.
- `make benchmark_segmented_cache` and `git diff --check` passed.
- The instrumented trace-capture generation passed full verification. The entire
  production regression suite was not rerun for this isolated benchmark.
- No production cache implementation, revision, or remote running job was changed.

## Recommendation

Segmentation clearly solves the expensive whole-cache resize problem. It does
not yet justify replacing every flat cache: accumulated lookup overhead can
outweigh occasional saved resize pauses. For example, saving 7.5 seconds once
does not compensate for adding 25 seconds to a 100-second lookup-heavy phase.

The next useful experiment would be an optional segmented backend for large,
frequently redistributed caches, retaining flat/resident/dense paths for stable
or small databases. Integrate the coordinator only behind an experimental flag,
then compare a real sliced generator run end-to-end, including initialization,
all lookup work, resize/refill, memory peak, and total time. Before production,
the prototype also needs dense transitions, allocation/recovery handling and
support for tiny/empty caches (currently a minimum of one segment per bucket).

The evidence supports **much cheaper redistribution**, not yet **faster overall
EGTB generation**. No commit or publication was made.
