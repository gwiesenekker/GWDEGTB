# Dictionary-backed WDL integration (revision 3.403)

GWD's missing-WDL generation now defaults to Zstd level 12 with a per-file
trained dictionary. DTM and frontier compression are unchanged. Dictionary
training samples up to 4,000 stratified pages and limits dictionary size to
110 KiB (also capped by the available training data). Existing WDL files are
reused, not automatically regenerated.

## Actual-file benchmark

Ryzen 9 5950X, optimized native build, Zstd 1.5.5. New files compiled with
16 workers. Probe measurements use compressed images already in RAM, 200,000
uniformly sampled positions, five alternating-order rounds, and median wall
time. Board generation is outside timing; the probe includes compact-board
indexing, cache lookup, decompression on misses and CRC32C verification.
Each measured result is checked against a separately decompressed reference.
The entire new bitmap was also compared byte-for-byte with the plain reference.

### Largest five-piece material: 2wX-1wO-1bX-1bO

| Encoding | File bytes | Page load + CRC (µs) | Probe, 256 KiB cache (µs) | Probe, 64 MiB (µs) | Probe, 256 MiB (µs) |
|---|---:|---:|---:|---:|---:|
| Plain level 3 | 18,537,701 | 3.144 | 3.186 | 1.289 | 0.122 |
| Plain level 8 | 17,608,666 | 2.917 | 2.985 | 1.184 | 0.136 |
| Dictionary level 12 | 15,556,790 | 1.416 | 1.478 | 0.669 | 0.125 |

Dictionary generation took 0.865 s including training (one run). Page-load
time is 55% lower than plain level 3; the complete file is 16% smaller.
The 256 MiB trace is all hits: its small timing differences are noise, not a
dictionary speedup. Cache hit/miss counts are identical across encodings.

### Largest six-piece material: 2wX-1wO-2bX-1bO

| Encoding | File bytes | Page load + CRC (µs) | Probe, 256 KiB cache (µs) | Probe, 64 MiB (µs) | Probe, 256 MiB (µs) |
|---|---:|---:|---:|---:|---:|
| Plain level 3 | 350,563,643 | 2.970 | 2.993 | 3.017 | 2.247 |
| Plain level 8 | 326,508,576 | 2.588 | 2.628 | 2.654 | 1.990 |
| Dictionary level 12 | 301,235,406 | 1.464 | 1.533 | 1.576 | 1.237 |

Dictionary generation took 9.839 s including training (one run). Page-load
time is 51% lower than plain level 3; the file is 14% smaller. This does not
imply a 2× whole-search improvement: only misses decompress pages. Uncompressed
page-cache capacity and hit rate do not improve when compressed files shrink.

The policy is a measured default, not a claim that level 12 and 110 KiB are
optimal for every material or processor.

## Compatibility and validation

- Dictionary-backed output uses WDL format 2, with CRC32C-protected dictionary.
- Updated readers accept both versions, including mixed-dictionary probes.
- Small/draw-heavy datasets and disabled training retain format 1.
- Rebuild/relink GWD before using format 2. No GWD call-site changes required.
- `EGTB_WDL_DICTIONARY_KIB=0` disables training; 1..256 selects its size cap.
- Full `make test`, dictionary corruption and mixed four-thread probe tests pass.
- Dictionary test passes ASan+UBSan; leak detection was disabled for the sandbox.
- Both real databases compare exactly after full decompression.

Local raw benchmark traces: `/tmp/wdl-dict-production-probes.csv` and
`/tmp/wdl-dict-six-production-probes.csv`. Probe harness:
`/tmp/bench_wdl_dictionary_probes.c`. Compile runner:
`/tmp/bench-wdl-dictionary-compile`.
