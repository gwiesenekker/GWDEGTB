# Previous-mapping / retirement-bitmap experiment

## Scope

Isolated prototype in `benchmark_lazy_cache.c`; no production backend changes,
revision bump, commit, or publication. This is a test of lazy migration, **not
yet an implementation of allocation-free in-place resizing**.

At a quiescent resize, the current allocation becomes an immutable previous
generation and a new flat cache is created. Lookup first checks the new mapping
using the production optimistic sequence/tag/atomic-word protocol. Only on a
miss does it check the previous size's mapping and retirement bitmap.

A matching old page supplies the result without decompression. The thread tries
to publish its whole page into the new cache with the usual sequence CAS and
release-fence protocol. Successful publication retires the old slot with an
atomic bitmap OR. Concurrent attempts can duplicate a promotion but cannot
return an incorrect value. A failed publication still returns the immutable
old value and leaves the old slot eligible for another attempt.

The bit means **old slot retired**, not “this page is still in the new cache”.
If the new entry is evicted, a normal miss reloads the database. The old cache
has no writers; old sequence validation/pinning is consequently unnecessary.
Both allocations stay alive until all readers have joined. At the next resize
any older generation is discarded, so chains of previous mappings cannot form.
Discarding it can lose useful pages but cannot change results.

The benchmark uses one global previous-generation context at a time. Production
would need per-database context, accounting, allocation-failure handling and a
coordinator retirement policy. It supports planar 2 KiB pages only.

## Measurements

Ryzen 5950X, 16 physical cores (`taskset -c 0-15`), AOCC Clang, production release
flags. Real read-only databases; existing decoder, CRC and sampled miss timing.
Every queried value is checked against the resident reference. This reference
is not an independent DTM implementation. Files are OS-cache-warm, and resident
reference loading lies outside timed phases.

Compare discard/rebuild, eager retained migration and lazy migration. All use
the same indirect benchmark call boundary. For each candidate, warm the initial
cache, grow, query twice, shrink back, query twice, grow again, query twice,
discard any previous generation, and query again. The workload/seed stays fixed.
Candidate order reverses on alternate rounds. Tests include collisions,
concurrent probes, retirement followed by eviction/refill, repeated resize and
old-generation disposal. Existing probe-open checks cover the database ends;
they use the standard flat lookup, not a targeted lazy partial-page promotion.

### 512 → 768 MiB: 90/10 hot/cold

`2wX-2wO-1bX-1bO.dtm`, four million queries per phase, three rounds. First growth:

| Backend | Resize median (s) | First queries median (s) | Combined median (s) | First-query decompressions |
|---|---:|---:|---:|---:|
| Discard | 0.1267 | 0.2529 | 0.3893 | 378,072 |
| Eager migration | 0.1931 | 0.1646 | 0.3577 | 221,434 |
| Lazy migration | 0.0833 | 0.2378 | 0.3205 | 158,537 |

Combined medians are computed from per-round sums, not by adding separate
medians. Lazy improves this bounded resize-plus-refill phase by 17.7% versus
discard and 10.4% versus eager migration. It performs 58.1% fewer decompressions
than discard, but the first query phase includes full-page copying on promotion.
It is therefore slower than eager migration's already-migrated query phase.

The next warm pass was 0.1320 s for lazy versus 0.1199 s for discard. Almost all
old hits had ended; remaining misses still paid a previous-map/bitmap check.
Retiring the old allocation promptly may matter.

### 64 → 96 MiB: fitting hot set

`2wX-1wO-1bX-1bO.dtm`, 32 million queries per phase, five rounds:

| Backend | First resize + queries median (s) | Next warm pass median (s) |
|---|---:|---:|
| Discard | 0.2704 | 0.2418 |
| Eager migration | 0.2636 | 0.2521 |
| Lazy migration | 0.2697 | 0.2455 |

Essentially tied for this workload; no evidence of the segmented prototype's
large hit-path penalty here. This is **not the same captured trace** as the
earlier segmented test, and does not prove the penalty has disappeared for every
access pattern. Reference checking also contributes to these timings.

A three-round 64 → 96 MiB hot/cold test also passed. Its first-query
decompressions were approximately 194k / 172k / 161k for discard/eager/lazy.
The raw growth, shrink and retirement measurements are retained alongside this
report in `2026-09-25-lazy-cache-data/`.

## Correctness checks

All benchmark values and per-workload checksums agreed. An ASan + UBSan run
using uniform conflicting traffic, 16 threads, repeated 64/96 MiB resizes and
final old-generation disposal passed without diagnostics. No new TSan run or
full production regression suite was performed for this isolated prototype.
The previous experiment's TSan executable was blocked by its runtime mapping
failure. `make benchmark_lazy_cache` and `git diff --check` passed.

## Memory limitation and conclusion

Growing 512 → 768 MiB temporarily holds **1,280 MiB of payload**, plus metadata
and a tiny retirement bitmap. Shrinking also retains the larger old allocation
until retirement. Thus this version cannot provide the immediate RAM transfer
that motivated segmented caches, and still initializes a new allocation.

The bitmap/previous-mapping idea works and reduces eager copying/decompression.
These results support further experimentation, not a production switch or an
end-to-end speedup claim. To avoid replacing the allocation itself, the next
design must combine lazy remapping with stable extensible backing storage
(segments or a reserved virtual-address range). That is a separate allocation
and portability problem, including what happens when a shrink removes an old
slot still holding a useful page. A fair next comparison should use the same
captured trace and a complete sliced workload, with old-generation memory
charged to the same fixed budget.

## Reproduce

```sh
make benchmark_lazy_cache
taskset -c 0-15 ./benchmark_lazy_cache \
  /tmp8/gwies/endgame7/2wX-2wO-1bX-1bO.dtm 512 768 3 250000 2
taskset -c 0-15 ./benchmark_lazy_cache \
  /tmp8/gwies/endgame7/2wX-1wO-1bX-1bO.dtm 64 96 5 2000000 0
```

Arguments: database, initial MiB, alternate MiB, rounds, queries per worker,
workload (0 fitting hot set, 1 uniform, 2 hot/cold; other patterns inherited
from the segmented benchmark driver). Do not link this executable with `egtb.o`.
