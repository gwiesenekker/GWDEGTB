# Parallel cache migration and phase measurements

Retained-page resize now partitions source slots among the configured cache
workers. Destination sequence CAS claims prevent overlapping writes when fractional
growth or shrink maps multiple old tags to one new slot. Readers remain quiescent
until every worker joins. Failed thread creation uses caller-thread fallback.
Cache collisions may retain a different page; database values and formats are unchanged.

## Local benchmark

Command: `./test_shared_init 256`. Ryzen 5950X, three rounds, reversed middle
order. All source slots populated by probing an all-draw temporary database;
each resize is followed by validation. These measure memory movement rather than
compression. Times below are medians in milliseconds.

| Requested threads | Grow migration | Grow total | Shrink migration | Shrink total |
| --- | ---: | ---: | ---: | ---: |
| 1 | 43.23 | 254.85 | 31.43 | 158.90 |
| 8 | 33.65 | 101.48 | 29.62 | 92.56 |
| 16 | 34.09 | 103.00 | 28.33 | 91.52 |
| 32 | 34.54 | 106.10 | 29.41 | 90.39 |
| 64 | 34.18 | 106.30 | 27.65 | 92.79 |

Grow: 256→384 MiB; shrink: 384→192 MiB. Migration alone gains modestly;
parallel initialization accounts for most of the total improvement. This is not
a prediction for a many-GiB NUMA workload. 64 threads oversubscribe this host.

Regression checks use nonuniform per-page values, both sides, fractional growth,
collision-producing shrink, dense transitions, uneven partitions and retained
probe handles. Thread-creation failure injection exercises caller fallback.

Validation passed: full Linux `make -j16 test`, all seven portable CMake tests,
ASan+UBSan migration test, and injected alternating thread-creation failures.
A four-thread two/three-piece generation confirmed resident-load and directory
timing records in the real generator path. Revision: 3.703.

## Next-job diagnostics

- `shared cache migration timing`: allocation, initialization, migration,
  freeing and total seconds; actual worker counts; copied pages and collision drops.
- `resident dependency load`: start/completion, MiB, one loading thread and elapsed
  seconds, including allocation and decoding. Different dependencies may overlap.
- `DTM page-directory`: load-and-validate, write and storage-statistics operations,
  page count, directory MiB and elapsed seconds.

These lines require progress logging. Timers are per operation, not per lookup.
Nested/overlapping timings must not be summed into application wall time. Resident
dependency loading and directory processing are measured, not parallelized here.
