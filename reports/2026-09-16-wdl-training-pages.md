# WDL dictionary training-page count

Decision: use **4,096** sampled pages instead of 4,000 (revision 3.405).
The power of two is convenient, not an algorithmic speed requirement.

## Method

Ryzen 9 5950X, clang O3 native, Zstd 1.5.5, level 12, 16 compilation workers.
Tested the largest five-piece (`2wX-1wO-1bX-1bO`) and six-piece
(`2wX-1wO-2bX-1bO`) databases. Three complete compilations per configuration,
with reversed count order on round two; times below are medians including
source sampling, dictionary training and file generation.

Production stratified sampling, draw-page exclusion and dictionary-size cap
were retained. Thus 1,024 samples produce an approximately 32 KiB dictionary;
4,096 and 16,384 produce 110 KiB dictionaries. This is a policy comparison,
not a fixed-dictionary-size experiment (see controlled experiment below).

Page-load timings include directory lookup, decompression and CRC32C with the
compressed files in RAM. Five alternating-order rounds, same 200,000 random
position trace. GWD compact probes were also measured and checked individually.
All three outputs per material compare byte-for-byte after full decompression.

## Complete files

| Material | Sample limit | File bytes | Generation seconds | Page load + CRC µs |
|---|---:|---:|---:|---:|
| Five-piece | 1,024 | 16,128,186 | 0.503 | 1.497 |
| Five-piece | 4,096 | 15,568,003 | 0.802 | 1.391 |
| Five-piece | 16,384 | 15,566,272 | 1.801 | 1.384 |
| Six-piece | 1,024 | 303,687,821 | 8.354 | 1.514 |
| Six-piece | 4,096 | 301,315,966 | 9.031 | 1.486 |
| Six-piece | 16,384 | 300,994,047 | 9.832 | 1.478 |

For five-piece material, moving from 4,096 to 16,384 saves 1,731 bytes
(0.011%). For six-piece material it saves 321,919 bytes (0.107%). The small
decode differences do not establish a worthwhile or universal speed advantage.
For five-piece GWD probes with a 64 MiB cache, medians were 0.698, 0.658 and
0.655 µs respectively. All-hit probes were approximately 0.11 µs in all cases.

## Controlled held-out experiment

`benchmark_wdl_training.c` trains nested sets of 1,024 / 4,096 / 16,384
non-draw pages with **fixed 110 KiB dictionary size**, then compresses the
same 10,000 disjoint held-out pages for each. This separates sample count from
the production dictionary-size cap. Training times are single observations;
decode medians cover five passes. Exact bytes and source CRCs are checked.

| Material | Training pages | Training seconds | Held-out payload bytes | Decode + CRC µs/page |
|---|---:|---:|---:|---:|
| Five-piece | 1,024 | 0.068 | 2,958,642 | 1.263 |
| Five-piece | 4,096 | 0.210 | 2,930,175 | 1.277 |
| Five-piece | 16,384 | 0.833 | 2,923,202 | 1.237 |
| Six-piece | 1,024 | 0.063 | 2,539,342 | 1.160 |
| Six-piece | 4,096 | 0.190 | 2,530,192 | 1.134 |
| Six-piece | 16,384 | 0.736 | 2,528,095 | 1.164 |

Here the larger training set saves only 0.24% / 0.08% of held-out payload
relative to 4,096, and does not consistently improve decode speed.

## Reproduction and limits

The complete-file comparison builds `wdl.c` with
`-DWDL_DICTIONARY_SAMPLES=1024`, `4096`, or `16384`, leaving all other settings
unchanged. Local driver, binaries, generated files and raw logs are under
`/tmp/wdl-sample-bench/`; `run.sh` documents compilation invocation and probe
order. Numeric probe labels 1/2/3 mean 1,024/4,096/16,384 samples.

This is two materials on one CPU, not proof of the optimal count for all EGTBs.
Existing WDLs remain valid and are not automatically regenerated. The new default
can slightly change file size/hash but not WDL values. Build and targeted WDL
compilation/dictionary regression tests pass with the 4,096 default.
