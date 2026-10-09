# Segmented storage with lazy previous-mapping recovery

## Status

Implemented as the isolated `benchmark_hybrid_cache` executable. Production
cache code, public APIs and the revision were not changed by this experiment.
Nothing was committed or published. The design now actually avoids replacing
the whole payload allocation, unlike the earlier two-generation lazy prototype.

## Mechanism

Each side has its own directory of equally sized segments. A lookup computes
exact modulo over the current logical per-side slot count, then uses a shift
and mask to locate the segment and offset. Side directories keep existing BTM
addresses stable when WTM capacity changes. There are no split buckets.

Growth allocates and initializes only added segments. Shrink frees trailing
segments. A cross-database transfer detaches trailing segments, invalidates
their metadata in parallel and attaches them to the receiver. It neither copies
nor clears their payload. All payload words already have atomic lifetimes and
the next publisher overwrites the entire page. Resize/transfer occurs only
after every probe thread has joined.

One previous per-side capacity and modulo descriptor is remembered. A failed
current lookup can try that mapping, provided the old logical slot is still
allocated. Shrinking must check this bound before looking up a segment pointer.
An old address equal to the current address is not retried. Before another
resize, the older mapping is discarded: there is never a mapping chain.

Unlike the earlier two-allocation experiment, old slots can now be overwritten
by current-mapping insertions. Old recovery therefore reads the sequence and
tag, copies all 256 atomic words into private scratch, executes the acquire
fence and checks the sequence again. A failed snapshot falls back to normal
decoding. A successful snapshot supplies the value and is opportunistically
published at the current location using the production CAS/release protocol.
The source is never pinned, and no thread holds two slot locks. Failure to
publish does not invalidate the privately captured value.

No retirement bitmap is needed. Tags determine whether an old entry survives.
Duplicate old/new copies are permitted; eviction is a cache miss, not a result
change. The previous mapping is explicitly retired at a quiescent boundary or
replaced by the next resize. A production policy would need to decide how long
to retain fallback, rather than leave it indefinitely.

## Tests and benchmark method

- Local Ryzen 5950X, 16 physical cores, pinned to CPUs 0–15; AOCC Clang with
  `-O3 -DNDEBUG -march=native -std=c11 -pthread`.
- Real production decoder, checksums, miss timing and optimistic slot protocol.
  Read-only database files and an OS-warm file cache. Resident reference loading
  lies outside measurement.
- Every transfer-workload query checked against the resident reference. This
  uses the existing decoder, not an independent DTM implementation.
- Identical indirect lookup dispatch for all candidates in the current driver.
- Three repetitions for transfer tests; five for captured-trace tests. Candidate
  order reverses on alternate rounds. Numbers below are medians.
- Cross-database transfer, transfer back, another forward transfer and explicit
  previous-map retirement; both sides and first/last-position checks throughout.
- Incremental allocation/free test: 8 → 12 → 4 → 14 → 2 → 8 → 8 MiB, with
  1 MiB segments, randomized conflicting probes and assertions that surviving
  first-segment addresses never move.

The trace is the same 9,766,382-query bounded initialization-prefix capture from
generating `1 2 2 0`, targeting `2wX-1wO-2bX-0bO.dtm`, used in the earlier report.
Each worker's sequence is repeated 100 times per measured phase. Relative
inter-worker scheduling is not fixed. Trace checksums agree, but the long trace
timings do not include per-query reference checking. No resize occurs in this
steady-state trace test.

## Transfer and refill

Donor 512 MiB, receiver 256 MiB; transfer 128 MiB, with 4 MiB segments. Each
database then receives four million 90/10 hot/cold queries. Databases:
`2wX-2wO-1bX-1bO.dtm` and `2wX-2wO-0bX-2bO.dtm`.

| Backend | Transfer (s) | Subsequent queries (s) | Combined (s) | Decompressions |
|---|---:|---:|---:|---:|
| Discard/rebuild | 0.1231 | 0.3964 | 0.5257 | 597,195 |
| Eager retained migration | 0.2173 | 0.3274 | 0.5447 | 368,996 |
| Previous split-segment prototype | 0.000685 | 0.2269 | 0.2275 | 256,861 |
| Hybrid exact-modulo + lazy recovery | 0.000666 | 0.2836 | 0.2843 | 342,237 |

Combined medians use per-round sums. Hybrid improves this phase by 45.9% versus
discard, but is 24.9% slower than split segments. Exact modulo changes more page
locations than incremental splitting; recovering surviving entries lazily does
not preserve all of them. Some are overwritten or removed before being read.
This is a real tradeoff, not a universally better replacement policy.

## Steady-state real trace

| Payload | Flat lookup phase (s) | Hybrid lookup phase (s) | Extra time |
|---|---:|---:|---:|
| 32 MiB | 0.9811 | 1.0026 | 2.2% |
| 64 MiB | 0.7716 | 0.8858 | 14.8% |
| 96 MiB | 0.7334 | 0.7525 | 2.6% |

The 96 MiB warm passes have no decompressions. The simpler mapping appears more
promising than the earlier bucket-based segmented prototype, whose trace penalty
was 24–34%, but that earlier run used a different benchmark dispatch arrangement.
Treat that historical comparison as directional, not a controlled three-way
measurement. Even the present penalty varies with capacity and access pattern.
It is lookup-phase time, not whole-generator runtime.

## Large-memory transfer

32 GiB donor plus 8 GiB receiver, 64 MiB segments, transferring 4 GiB:

- Median forward transfer: **0.01036 seconds**.
- Median transfer back: **0.01011 seconds**.
- Peak RSS: **43,268,632 KiB = 41.26 GiB**; zero swaps and major page faults.
- Total payload remains exactly 40 GiB before/after each transfer. No old and
  new full-cache allocations coexist. Metadata contributes 3.125% of payload,
  plus small directory/allocation overhead.

This synthetic large test allocates/initializes every payload word, populates
donor tags, and times transfer only, not decoding or refill. The earlier flat
32+8 GiB test took 7.54 seconds for discard/rebuild and 12.95 seconds for retained
migration; these are historical measurements on the same host, not rerun here.

## Correctness and remaining work

Returned values/checksums agreed throughout. Concurrent tests exercised rejected
old-page snapshots (sequence changed during the copy) and continued correctly.
ASan + UBSan passed both the incremental allocation/shrink test and the
cross-database transfer/back/forward test. Build and `git diff --check` passed.
No new TSan or whole-production regression run was performed; the previous
prototype's TSan runtime was blocked by an unexpected-memory-mapping error.

The prototype intentionally still has limitations: planar 2 KiB pages only,
fixed directory bound, coarse segment granularity, no dense/resident fast-mode
integration, no production coordinator integration, and abort-on-allocation-
failure rather than recoverable admission. Existing atomic sequence counters
saturate rather than wrap, as in production. Resetting transferred metadata is
safe only with the enforced global quiescence.

This demonstrates **cheap physical memory transfer with lazy recovery and no
replacement payload allocation**. It does not establish faster complete EGTB
generation. An optional production backend would need failure-safe accounting,
old-mapping retirement policy and an end-to-end sliced A/B benchmark under the
same RAM budget before replacing the existing cache.

## Reproduce

```sh
make benchmark_hybrid_cache
SEG_LINEAR=1 taskset -c 0-15 ./benchmark_hybrid_cache transfer \
  /tmp8/gwies/endgame7/2wX-2wO-1bX-1bO.dtm \
  /tmp8/gwies/endgame7/2wX-2wO-0bX-2bO.dtm 512 4 3 250000 2
taskset -c 0-15 ./benchmark_hybrid_cache trace \
  /tmp8/gwies/endgame7/2wX-1wO-2bX-0bO.dtm \
  /tmp/egtb-segment-trace.bin 64 1 5
taskset -c 0-15 ./benchmark_hybrid_cache large 3
./benchmark_hybrid_cache check /tmp8/gwies/endgame7/2wX-1wO-1bX-1bO.dtm
```

Transfer arguments: two files, donor MiB, segment MiB, rounds, queries/worker,
workload (0 fitting hot set, 1 uniform, 2 hot/cold). `SEG_LINEAR=1` selects the
incremental-split baseline; it does not alter hybrid mapping. Trace arguments:
database, captured trace, payload MiB, segment MiB, rounds.

Raw logs: `2026-09-25-hybrid-cache-data/` beside this report.
