# Exact-page Zstd dictionary experiment

Measured 2026-09-16 on Ryzen 9 5950X, one thread, Zstd 1.5.5, Clang -O3 -DNDEBUG -march=native.

## Method

Two actual five-piece DTMs and WDLs derived from them: `2wX-1wO-2bX-0bO` and `1wX-2wO-2bX-0bO`. The benchmark reads the actual compressed payloads and decompresses them into the exact byte streams originally passed to Zstd; it does not reconstruct a guessed encoding. Source-page CRCs are checked.

Per database, deterministically shuffle the stored-page IDs, use 4,000 pages for training and the next 10,000 for testing, with no overlap. Implicit all-draw pages are excluded because they require neither compression nor decompression. Training is separate for each material and each format. DTM samples contain 1,024 codec bytes representing one 2 KiB canonical 16-bit single-side page. WDL samples contain 1,024 bytes representing 2,048 two-sided positions.

Train independent 16, 64 and 110 KiB dictionaries with ZDICT_trainFromBuffer. Prepare CDict/DDict once, use persistent contexts and normal Zstd frames (dictionary IDs retained). Compression column measures one full 10,000-page pass, excludes dictionary training/preparation. Decode columns are medians of five passes; pure decode excludes codec expansion/CRC, while checked decode includes the format-4 byte expansion to canonical 16-bit codes and CRC, or WDL CRC. Checked DTM decoding mirrors the relevant codec rules but is a standalone helper, not a timed EgtbView call. Neither includes disk I/O, ranking, cache lookup, nor context setup. Correctness is checked byte-for-byte against original encoded streams before timing; all source-page CRCs pass again during checked decoding. This is an offline microbenchmark, not an engine throughput prediction.

Ratios below divide **actual codec-stream bytes by compressed payload bytes**, not canonical DTM bytes. They exclude dictionary bytes and file directories/headers. All test sets have 10,240,000 codec bytes. Dictionary storage is a one-time per-file cost and should not be charged as if every 10,000-page sample were an independent file. Full-file size gains remain to be measured.

## 1wX-2wO-2bX-0bO-dtm

| Level | Dictionary KiB | Literals off | Payload ratio | Compress µs/page | Pure decode µs/page | Codec + CRC µs/page |
|---:|---:|:---:|---:|---:|---:|---:|
| 1 | 0 | no | 5.56:1 | 5.339 | 1.719 | 2.859 |
| 3 | 0 | no | 5.59:1 | 5.538 | 1.597 | 2.766 |
| 6 | 0 | no | 6.26:1 | 10.489 | 1.379 | 2.532 |
| 8 | 0 | no | 6.36:1 | 23.893 | 1.309 | 2.465 |
| 9 | 0 | no | 6.29:1 | 22.152 | 1.343 | 2.505 |
| 12 | 0 | no | 6.59:1 | 42.731 | 1.823 | 2.965 |
| 19 | 0 | no | 6.92:1 | 308.211 | 1.811 | 2.960 |
| 6 | 0 | yes | 5.19:1 | 9.691 | 0.856 | 2.012 |
| 3 | 16 | no | 6.10:1 | 2.891 | 0.889 | 2.053 |
| 6 | 16 | no | 6.41:1 | 12.140 | 0.929 | 2.088 |
| 9 | 16 | no | 6.70:1 | 22.094 | 0.851 | 2.018 |
| 12 | 16 | no | 6.82:1 | 39.863 | 0.796 | 1.961 |
| 3 | 64 | no | 6.08:1 | 2.811 | 0.884 | 2.038 |
| 6 | 64 | no | 6.41:1 | 12.317 | 0.929 | 2.091 |
| 9 | 64 | no | 6.75:1 | 23.250 | 0.796 | 1.956 |
| 12 | 64 | no | 6.95:1 | 42.718 | 0.742 | 1.905 |
| 3 | 110 | no | 6.06:1 | 2.829 | 0.951 | 2.095 |
| 6 | 110 | no | 6.40:1 | 12.296 | 0.971 | 2.175 |
| 9 | 110 | no | 6.76:1 | 24.109 | 0.836 | 1.991 |
| 12 | 110 | no | 6.97:1 | 41.706 | 0.765 | 1.923 |

Training times (16/64/110 KiB): 0.144 s, 0.137 s, 0.138 s. Validation: PASS.

## 1wX-2wO-2bX-0bO-wdl

| Level | Dictionary KiB | Literals off | Payload ratio | Compress µs/page | Pure decode µs/page | Codec + CRC µs/page |
|---:|---:|:---:|---:|---:|---:|---:|
| 1 | 0 | no | 3.42:1 | 6.995 | 2.626 | 2.693 |
| 3 | 0 | no | 3.41:1 | 7.472 | 2.550 | 2.643 |
| 6 | 0 | no | 3.63:1 | 14.982 | 2.279 | 2.353 |
| 8 | 0 | no | 3.66:1 | 27.861 | 2.200 | 2.274 |
| 9 | 0 | no | 3.65:1 | 31.093 | 2.219 | 2.299 |
| 12 | 0 | no | 3.85:1 | 62.852 | 2.320 | 2.394 |
| 19 | 0 | no | 3.92:1 | 237.285 | 2.313 | 2.393 |
| 6 | 0 | yes | 2.84:1 | 13.574 | 1.216 | 1.276 |
| 3 | 16 | no | 3.78:1 | 4.115 | 1.153 | 1.227 |
| 6 | 16 | no | 3.84:1 | 16.360 | 1.342 | 1.421 |
| 9 | 16 | no | 4.00:1 | 31.750 | 1.232 | 1.292 |
| 12 | 16 | no | 4.09:1 | 47.823 | 1.161 | 1.209 |
| 3 | 64 | no | 3.79:1 | 4.091 | 1.145 | 1.221 |
| 6 | 64 | no | 3.82:1 | 16.445 | 1.365 | 1.450 |
| 9 | 64 | no | 4.05:1 | 32.465 | 1.165 | 1.242 |
| 12 | 64 | no | 4.22:1 | 55.050 | 1.027 | 1.102 |
| 3 | 110 | no | 3.81:1 | 4.034 | 1.134 | 1.208 |
| 6 | 110 | no | 3.86:1 | 16.371 | 1.354 | 1.433 |
| 9 | 110 | no | 4.09:1 | 32.738 | 1.171 | 1.236 |
| 12 | 110 | no | 4.30:1 | 56.119 | 0.998 | 1.071 |

Training times (16/64/110 KiB): 0.190 s, 0.183 s, 0.180 s. Validation: PASS.

## 2wX-1wO-2bX-0bO-dtm

| Level | Dictionary KiB | Literals off | Payload ratio | Compress µs/page | Pure decode µs/page | Codec + CRC µs/page |
|---:|---:|:---:|---:|---:|---:|---:|
| 1 | 0 | no | 6.61:1 | 5.022 | 1.513 | 2.545 |
| 3 | 0 | no | 6.61:1 | 4.770 | 1.309 | 2.345 |
| 6 | 0 | no | 7.79:1 | 9.382 | 0.993 | 2.060 |
| 8 | 0 | no | 7.99:1 | 24.843 | 0.906 | 1.958 |
| 9 | 0 | no | 7.85:1 | 22.440 | 0.940 | 1.991 |
| 12 | 0 | no | 8.46:1 | 41.927 | 1.528 | 2.583 |
| 19 | 0 | no | 9.06:1 | 336.833 | 1.555 | 2.609 |
| 6 | 0 | yes | 7.25:1 | 9.015 | 0.747 | 1.802 |
| 3 | 16 | no | 7.87:1 | 2.400 | 0.817 | 1.871 |
| 6 | 16 | no | 8.47:1 | 10.695 | 0.750 | 1.810 |
| 9 | 16 | no | 8.92:1 | 20.043 | 0.679 | 1.736 |
| 12 | 16 | no | 9.04:1 | 42.388 | 0.627 | 1.679 |
| 3 | 64 | no | 7.83:1 | 2.352 | 0.881 | 1.935 |
| 6 | 64 | no | 8.38:1 | 11.032 | 0.823 | 1.873 |
| 9 | 64 | no | 8.87:1 | 21.380 | 0.730 | 1.777 |
| 12 | 64 | no | 9.28:1 | 42.548 | 0.619 | 1.676 |
| 3 | 110 | no | 7.87:1 | 2.320 | 0.819 | 1.873 |
| 6 | 110 | no | 8.45:1 | 10.773 | 0.755 | 1.810 |
| 9 | 110 | no | 8.97:1 | 20.211 | 0.681 | 1.730 |
| 12 | 110 | no | 9.36:1 | 47.883 | 0.533 | 1.585 |

Training times (16/64/110 KiB): 0.119 s, 0.116 s, 0.119 s. Validation: PASS.

## 2wX-1wO-2bX-0bO-wdl

| Level | Dictionary KiB | Literals off | Payload ratio | Compress µs/page | Pure decode µs/page | Codec + CRC µs/page |
|---:|---:|:---:|---:|---:|---:|---:|
| 1 | 0 | no | 3.58:1 | 6.883 | 2.706 | 2.868 |
| 3 | 0 | no | 3.57:1 | 7.412 | 2.538 | 2.609 |
| 6 | 0 | no | 3.83:1 | 15.086 | 2.228 | 2.310 |
| 8 | 0 | no | 3.87:1 | 31.135 | 2.152 | 2.223 |
| 9 | 0 | no | 3.86:1 | 32.969 | 2.188 | 2.257 |
| 12 | 0 | no | 4.09:1 | 65.972 | 2.306 | 2.381 |
| 19 | 0 | no | 4.17:1 | 246.113 | 2.315 | 2.405 |
| 6 | 0 | yes | 3.03:1 | 13.563 | 1.180 | 1.250 |
| 3 | 16 | no | 3.96:1 | 4.004 | 1.172 | 1.240 |
| 6 | 16 | no | 4.05:1 | 15.884 | 1.260 | 1.342 |
| 9 | 16 | no | 4.22:1 | 31.134 | 1.115 | 1.193 |
| 12 | 16 | no | 4.31:1 | 51.612 | 1.035 | 1.114 |
| 3 | 64 | no | 4.00:1 | 4.006 | 1.164 | 1.231 |
| 6 | 64 | no | 4.08:1 | 16.154 | 1.276 | 1.355 |
| 9 | 64 | no | 4.31:1 | 32.679 | 1.091 | 1.170 |
| 12 | 64 | no | 4.45:1 | 53.704 | 1.005 | 1.082 |
| 3 | 110 | no | 4.02:1 | 3.957 | 1.164 | 1.234 |
| 6 | 110 | no | 4.11:1 | 16.003 | 1.261 | 1.342 |
| 9 | 110 | no | 4.35:1 | 32.740 | 1.083 | 1.161 |
| 12 | 110 | no | 4.53:1 | 54.811 | 0.967 | 1.046 |

Training times (16/64/110 KiB): 0.187 s, 0.177 s, 0.175 s. Validation: PASS.

## Interpretation

The dictionary benefit is reproducible with the real encoding, but its magnitude depends on material and on which costs are included. For DTM, level 12 + 110 KiB dictionary reduces sampled payload by about 20–29% versus level 1. Pure Zstd decoding improves about 2.2–2.8x; with codec expansion and CRC, about 1.5–1.6x. Level 3 + dictionary is an especially attractive balanced candidate: faster compression than plain level 1 and faster decoding, with smaller sampled payload.

For WDL, level 3 + dictionary roughly halves pure decoding time versus current plain level 3 and also compresses faster. Level 12 + dictionary provides the fastest checked decode and smallest sampled payload among the dictionary variants tested, but compression costs around 54–56 µs/page instead of around 4 µs for dictionary level 3. Level 6 + dictionary is not automatically better than level 3. Disabling Huffman literals helps speed but loses size; dictionaries offer a better combined trade-off in these samples.

A 16 KiB dictionary already captures much of the benefit. Larger dictionaries do not improve every level monotonically, so 110 KiB should not be hardcoded without further full-file testing. Dictionaries do not increase the number of uncompressed pages fitting a decoded-page cache. The format would need to persist and validate the dictionary, with immutable prepared dictionaries shared and decompression contexts private. No production format or compression setting changed.

## Reproduction

Source: `benchmark_zstd_dictionary.c` in the repository root. Build/run commands are in its opening comment. Source DTMs are in `/tmp8/gwies/endgame7`. Derived WDLs and full raw results are in `/tmp/wdl-dictionary-benchmark`. Only benchmark outputs were created; original DTMs and production WDLs were not modified.
