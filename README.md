# International draughts endgame database generator

### Optional consistency verification

`EGTB_VERIFICATION=full|slices|none` selects generation verification; the CLI
`--verification full|slices|none` overrides the environment. Default: `full`.

| Mode | Unsliced database | Man-row sliced database |
| --- | --- | --- |
| `full` | Exhaustive verification, repair if needed | Verify each slice and the merged database |
| `slices` | Same as `full` | Verify each slice; skip merged consistency verification |
| `none` | Skip consistency verification and repair | Skip slice and merged consistency verification and repair |

For example, `export EGTB_VERIFICATION=none` applies to existing family scripts,
including `4x4.sh`. In `slices` mode, all-kings material still receives full
verification because it has no man-row slices.

Skipped final verification prints **NOT VERIFIED** in startup and summary output.
Statistics and examples are still collected in a parallel, checksum-checked
page scan using `-j` workers (capped by page count); each worker has a private
sequential reader and a 1 MiB histogram. No forward moves or successor probes
are performed by that scan. Histogram counts and lowest-index example choices
are independent of the worker count.
Structural checks and durable publication remain enabled. The DTM file format
is unchanged: retain the generation log to record its verification status.
Matching statistics alone do not prove correctness. Unchecked dependencies can
propagate incorrect values into later databases.

Unchecked slice checkpoints persist `consistency_passes=0` in the checksummed
manifest. They can resume with `none`, but `full`/`slices` refuse to trust them
(including orphan slices without verification evidence). Use a fresh workspace
for a verified regeneration. Completed slices from verified runs remain reusable
with any mode. Later, run `verify_dtm` on the final databases in dependency order;
this checks rather than repairs them. Keep unverified results labelled as such
until these checks have succeeded.

This C library can calculate Distance-To-Mate (DTM) EGTBs for International Polish Draughts.

GWDEGTB supports exact two-through-eight-piece endgame databases on the 50
playable squares of a 10x10 board. It includes a dense reversible position
index, international-rules move generation, multithreaded retrograde analysis,
compressed DTM and WDL storage, consistency repair, final verification, and
regression and performance tests.

Current published version: **3.6**; executable revision **3.601**.
See [Version history](CHANGELOG.md) for changes in each tagged version.

### Keep remote generation running with tmux

Run the launcher **on the generation server**, after connecting with SSH:

```sh
ssh 8p
cd /tmp2/gwies/endgame7
./run_tmux.sh numactl --interleave=all ./all.sh
# Or, with EGTB_* settings already exported:
# ./run_tmux.sh numactl --interleave=all ./4x3.sh
```

`all.sh` is your own settings/job-selection script, not supplied by the project.
The launcher starts session `egtb` and attaches when invoked from a terminal.
Detach with **Ctrl+B, then D**; reconnect with `tmux attach -t egtb`.
SSH disconnection does not stop the tmux-hosted job. The command's exit status
is printed and a shell remains open after completion or failure. An existing
session causes the launcher to refuse a second run; inspect/close that session
before starting another. Inside tmux, the launcher simply runs its command.
It does not detect generators running outside that session or on another socket.

Exported `EGTB_*` settings and `PATH` are passed from the launching shell, even
when the tmux server has an older environment; stale server `EGTB_*` variables
are cleared for the new command. NUMA policy belongs **inside** the launcher,
as above, rather than `numactl ... ./run_tmux.sh`. Family scripts remain unchanged
and do not automatically create nested sessions. Use `-s NAME` for another
session name. `make test-tmux` tests the launcher on a private tmux socket.

The summary includes per-material dependency cache statistics, summed across
workers, separately for generation and final verification. `Full MiB` is the
size of one shared, uncompressed paired 16-bit array; `Mode` says whether that
dependency is actually `resident` or `cached`. Resident reads count as hits,
not page decompressions; initial resident loading is not included in these
cache counters. The shared-pool summary reports loaded/cached database counts
and resident array bytes. Verification counters are phase deltas; dependencies
remain warm. Slice-internal cache traffic is not part of these material tables.

## Highlights

- Exact WTM and BTM distance-to-mate values in plies.
- Dense indexing with no holes for illegal man placements.
- International draughts captures: compulsory capture, maximum capture count,
  flying kings, and delayed removal of captured pieces.
- Page-partitioned POSIX threading for initialization, retrograde propagation,
  compilation, consistency checking, and final verification.
- Checksummed Zstd-compressed DTM and WDL pages.
- Canonical material orientation with automatic color/board mirroring.
- Compressed on-disk frontier streams and persistent outcome bitmaps, avoiding
  a full database scan for every DTM layer.
- A defensive exact-DTM consistency check/repair pass, followed by fatal
  read-only verification; unreachable setup positions remain part of the graph.
- Tested position counts for all 120 seven-piece material distributions.
- Independent counts and sampled index/slice round trips for all 165
  eight-piece material distributions; eight-piece move-generation tests.

## Requirements and build

The generator targets Linux/POSIX and requires a C11 compiler, POSIX threads, and
the Zstandard development library. The GWD-facing library also has a Windows
Clang/MSVC-ABI port using the shared `../compat` directory; see
[Windows library build and integration](WINDOWS.md). Native Windows validation
is performed separately from the Linux regression suite. On Debian or Ubuntu the required package is
`libzstd-dev`. The Makefile defaults to Clang when `CC` has not been selected;
use `make CC=gcc ...` to override it.

The default production build is optimized for the local processor:

```sh
make generate_egtb
```

The Makefile uses:

```text
-O3 -DNDEBUG -march=native -std=c11 -Wall -Wextra -Wpedantic -pthread
```

`-DNDEBUG` removes validation from hot indexing and cache paths. The caller
must then honor their documented preconditions. Because `-march=native` may
emit CPU-specific instructions, rebuild after copying the source to a machine
with a different processor.

For a portable build, override `CFLAGS`, for example:

```sh
make clean
make CFLAGS='-O3 -DNDEBUG -std=c11 -Wall -Wextra -Wpedantic -pthread' generate_egtb
```

Run the complete regression suite and verify all known seven-piece counts:

```sh
make test
make check-stats
```

`make libgwdegtb.a` builds the static library used by GWD. Link it with Zstd
and POSIX threads, for example `-L/path/to/GWDEGTB -lgwdegtb -lzstd -pthread`.

### Retry an interrupted generation

```sh
./generate_egtb --restart -j 16 4 1 0 1
./generate_egtb --restart --sliced -j 16 4 1 0 1
```

`--restart` deletes the target `.dtm` and leftover `.dtm.incomplete` before
regeneration, without creating backups or acquiring locks. Run generation jobs
serially; do not restart a database while another process is generating it.
Slice checkpoints in `.dtm.work` are preserved for normal sliced-resume
validation. Without `--restart`, existing outputs are not replaced. Family
scripts pass `--restart` automatically. Old backups and lock files from earlier
versions are left untouched and are no longer used.

### Verify a completed DTM

`verify_dtm` opens a database strictly read-only and exhaustively checks both
side-to-move values of every indexed position against all legal successors.
Opening validates the header and page directory; the complete current DTM is
then decompressed and CRC32C-checked page by page. Captures, promotions and
other material-changing moves are resolved through the required lower-material
DTMs in the same directory. Missing dependencies, truncated files, checksum
errors and DTM inconsistencies all produce a nonzero exit status.

```sh
make verify_dtm

./verify_dtm -d /path/to/dtms -j 16 3 1 1 2
./verify_dtm -d /path/to/dtms -j 16 3 1 0 3
./verify_dtm -d /path/to/dtms -j 16 2 2 3 0
./verify_dtm -d /path/to/dtms -j 16 2 2 2 1
```

The argument order is WK, WM, BK, BM. `-j` defaults to four and can also be
set through `EGTB_THREADS`. The verifier uses the same memory controls as final
generation verification:

```sh
# Decompress the current DTM completely when it needs at most 32 GiB.
EGTB_RESIDENT_LIMIT_GIB=32 ./verify_dtm -d /path/to/dtms -j 16 3 1 1 2

# Force disk-cache verification, with a 16 GiB total current-DTM cache.
EGTB_RESIDENT_LIMIT_GIB=0 EGTB_VERIFICATION_CACHE_GIB=16 \
    ./verify_dtm -d /path/to/dtms -j 16 3 1 1 2
```

Dependency caches remain private per worker and database. The command never
repairs, compacts or otherwise writes the DTM. A successful run ends with a
prominent `VERIFIED ...` line and reports the number of values checked.

The manually maintained `REVISION` file supplies the revision printed at
startup. It is independent of Git tags and commit identifiers:

```sh
./generate_egtb --revision
```

## Timestamped progress and ETA

Generation logs timestamp phase starts/completions and print progress once
per minute while work is running. Progress includes completed/total work,
percentage, elapsed wall time, throughput and estimated seconds remaining.
The ETA uses `remaining work / average wall-clock throughput`; it is an
estimate and may change as work becomes more or less expensive.

Initialization, each backtracking source/candidate phase, frontier compilation,
each consistency check/correction pass, compaction, resident loading, final
verification, slice merging and statistics collection report separately.
Verification counts exclude positions already verified. Backtracking cannot
predict the final mate distance, and repair cannot predict how many passes
remain, so ETAs refer to the **current phase**, not the whole database.
Stages without a measurable work count report elapsed time and `ETA=unknown`.

```sh
# Default: periodic progress every 60 seconds, plus phase boundaries.
./generate_egtb -j 16 1 1 1 1

# Optional interval in seconds (1..3600); 0 disables progress reporting.
EGTB_PROGRESS_SECONDS=30 ./generate_egtb -j 16 1 1 1 1

# Job scripts redirect output: follow their log while generation is running.
# Use the exact log path printed by the family script:
tail -f logs/2x2/1wX-1wO-1bX-1bO-rev3.309-20260912T120000Z-12345.log
```

Timestamps use local time with a UTC offset. Durations and ETA use the
monotonic clock, so system-clock adjustments do not distort them. Updates
are newline-delimited and flushed immediately; the final statistics tables
retain their existing format. The library is silent unless the optional,
process-wide progress reporter is explicitly enabled before launching workers.

## Board and material representation

Squares are numbered 0 through 49. A position contains four `uint64_t`
bitboards; only their low 50 bits are used:

- white men, legal on squares 5..49;
- black men, legal on squares 0..44;
- white kings, legal on squares 0..49;
- black kings, legal on squares 0..49.

Material names and generator arguments follow GWD order: white kings, white
men, black kings, black men. A database name has the form:

```text
<WK>wX-<WM>wO-<BK>bX-<BM>bO.dtm
```

For example, `1wX-1wO-1bX-1bO.dtm` contains one king and one man of each
color. The equivalent WDL file ends in `.wdl`.

Only one orientation of a color-swapped pair is generated. The side with more
pieces is stored as White. If both sides have the same number of pieces, the
side with more kings is stored as White. A mirrored lookup rotates every
square by `s -> 49-s`, swaps the colors, and swaps WTM with BTM.

## Dense position index

For a fixed material signature, `endgame_index.c` maps every legal placement
to exactly one index in `0..maximum_index` and provides the inverse mapping.
The index order scans squares 0..49 with the choices empty, white man, black
man, white king, and black king. A dynamic-programming suffix table excludes
illegal promotion-row man placements without creating holes.

The production ranker precomputes the complete rank addition for each state
and actual piece type. Ranking then requires one table transition per occupied
square. The inverse walks the same state space in the other direction. The API
uses the internal argument order white men, black men, white kings, black
kings; the generator command line uses GWD order.

Exhaustively verify a fixed material index with:

```sh
./test_index 1 1 1 1
```

This enumerates every legal position and checks
`position -> index -> position`. Large signatures can take a long time; use
`make check-stats` for count-only verification against `7piece-stats.txt`.

### Index benchmark

Run:

```sh
make benchmark
```

The benchmark separately times index inversion and inversion followed by
ranking. Ranking time is the delta between those measurements. Small
databases are repeated to exceed 100 million operations; the six-, seven- and
eight-piece cases sample 110 million indices in 1,024 evenly spaced blocks.
Use `./benchmark_index --full` for a complete traversal or
`./benchmark_index --samples COUNT` to change the sample count.

The following results were measured with GCC 13.3.0, `-O3 -DNDEBUG
-march=native`, on an AMD Ryzen 9 5950X. Throughput varies with CPU frequency,
compiler, and memory placement.

| Pieces | Material (WM/BM/WK/BK) | Positions | Inversions/s | Round trips/s | Rankings/s by delta |
|---:|:---:|---:|---:|---:|---:|
| 2 | 0/0/1/1 | 2,450 | 42.28 M | 36.64 M | 274.35 M |
| 3 | 1/0/1/1 | 105,840 | 36.46 M | 31.65 M | 240.23 M |
| 4 | 1/1/1/1 | 4,478,160 | 35.74 M | 30.65 M | 215.16 M |
| 5 | 1/1/2/1 | 102,997,680 | 34.84 M | 28.86 M | 168.21 M |
| 6 | 1/1/2/2 | 2,317,447,800 | 33.14 M | 26.77 M | 139.12 M |
| 7 | 1/2/2/2 | 45,793,430,100 | 31.99 M | 25.45 M | 124.36 M |

An experimental combinatorial indexer is retained for comparison:

```sh
make benchmark-combinatorial-index
```

On the same system, the transition ranker was faster for every material size.
The combinatorial inverse became faster from four pieces onward, reaching
17.43 M inversions/s versus 13.40 M/s for the transition inverse at seven
pieces. The production generator retains the transition index because random
position-to-index calculation is its dominant indexing workload.

Unsliced inversion uses a dedicated loop without slice-boundary checks or
frontier-requirement arithmetic. Sliced inversion retains its existing path;
both use the same index order and on-disk format as before.

## Move generation

`movegen.c` implements international draughts rules:

- men move one diagonal square forward and capture in all four directions;
- kings move and capture over arbitrary diagonal distances;
- captures are compulsory;
- only moves with the global maximum number of captured pieces are emitted;
- every legal landing square beyond a king's final victim is retained;
- captured pieces remain blocking until the complete capture move ends;
- the same piece cannot be captured twice.

Capture recursion carries a bitboard of captured pieces, and every emitted move
contains its captured-piece mask. `draughts_do_move()` removes that mask in one
operation and may save the original four bitboards in `DraughtsUndo`;
`draughts_undo_move()` restores the snapshot. Duplicate final positions from
different capture paths are intentionally not removed.

The production padded backend maps the compact squares onto GWD fields
6..15, 17..26, 28..37, 39..48, and 50..59. Diagonal steps become shifts by 5
or 6, while unused fields prevent row wrapping. BMI2 builds use PDEP/PEXT for
whole-bitboard conversion; portable builds use a set-bit fallback. Precomputed
diagonal rays and between-square masks accelerate king blockers, captures, and
landing-square enumeration.

Quiet inverse generation is used only within the same material database. It
does not reverse captures or promotions and rejects a predecessor if the
previous mover would have had a compulsory capture. Thus every inverse move is
a legal quiet forward move that preserves the material signature.

The tests compare the compact table and padded implementations on focused rule
cases and 300,000 random seven/eight-piece positions for both sides:

```sh
make test
make benchmark-movegen
```

## DTM values and compressed storage

Every position has a white-to-move and black-to-move DTM value. Public values
are exact signed ply counts:

- positive odd values `1, 3, 5, ...` mean won in that many plies;
- zero and negative even values `0, -2, -4, ...` mean lost in that many plies;
- `-1` means draw or not yet known during generation;
- a side with no pieces or no legal move is lost in zero.

DTM caches, compilation buffers, and resident arrays use signed 16-bit codes
for each side. Wins `1,3,...,32765` are stored as `(dtm+1)/2`; losses
`0,-2,...,-32766` as `dtm/2`; `INT16_MIN` represents draw/unknown. Public
retrieval still returns exact `int16_t` ply counts. `EgtbEntry` is now four bytes
per position, so applications must rebuild against the updated library/header.

New version-4 files keep separate WTM and BTM planes. The default 2,048-byte
uncompressed page contains **1,024 values for one side**. Before Zstd compression, the
16-bit codes are encoded into a variable-length stream:

- `0x80` represents draw;
- codes from -127 through 126 use their signed one-byte representation;
- `0x7f` introduces a two-byte little-endian signed code for all other values
  (including code 127, i.e. won in 253).

Thus a default page's intermediate stream is `1024 + 2 * extended_value_count`
bytes: 1,024 bytes in the common case, at most 3,072 bytes. Decompression validates
the stream and reconstructs the fixed 2,048-byte page. A 1,024-byte page can still
be selected; it holds 512 values with a 512–1,536-byte intermediate stream.
This improves compact
representation of common values, but smaller position counts per page can
reduce Zstd's compression ratio compared with the old 8-bit format.

Version-2 (paired byte) and version-3 (planar byte) databases remain readable
and writable within their original -254..253-ply limits. Compaction preserves
their format. Their pages expand to twice the header page size in cache;
`egtb_cache_page_size()` reports this allocation size, and the generator adjusts
dependency cache page counts to retain its configured byte budget. Existing
WDL files and the GWD WDL API are unchanged. Material selection and the GWD
registry support up to eight pieces; existing two-through-seven-piece indices
and database formats are unchanged.

Every v4 block carries CRC32C of the expanded 16-bit codes serialized in
little-endian order; legacy blocks retain their original byte CRC. An implicit
directory entry `(offset=0,length=0)` represents an all-draw page and consumes
no block storage.

DTM checksums use eight-byte hardware CRC32C operations on x86-64 CPUs with
SSE4.2, selected at runtime, with a portable fallback on other CPUs. The
checksum values and file format are identical on both paths. `make test`
compares both implementations against an independent reference, including
unaligned buffers and partial final words.

The file begins with a versioned header containing the page size and maximum
index, followed by an in-memory directory with a 64-bit block offset and
16-bit compressed length per page. General writable storage supports a
configurable minimum block reservation (20% of the uncompressed page in the
library defaults). The compact compiler uses zero reserve and exact-sized
blocks instead. A dirty page that outgrows its slot is appended and its
directory entry is updated. `egtb_compact_copy()` removes holes while checking
and preserving compressed blocks; `egtb_compact()` additionally recompresses.
The normal successful generator path needs neither operation.

DTM caches are direct-mapped by page number. Planar cache views retain the
logical page and side throughout addressing; power-of-two slot counts per
side use a mask instead of division. This preserves the existing mapping and
eviction policy, including non-power-of-two cache sizes.
Read-only handles share the file
header and immutable directory by path, while each `EgtbView` owns its cache,
Zstd contexts, buffers, and statistics. Cache misses use `pread()`, so worker
threads do not share a seek pointer. Writable views use `pwrite()` and flush a
dirty slot before replacement.

## Generation command and dependency order

Generate a canonical database with:

```sh
./generate_egtb [--sliced] [-j THREADS] NWHITE_KINGS NWHITE_MEN NBLACK_KINGS NBLACK_MEN
```

For example:

```sh
./generate_egtb -j 16 1 1 1 1
```

The accepted material range is 2..8 pieces, with at least one piece per side.
The accepted thread count is 1..256. The target file must not already exist
unless `--restart` is specified.
Captures and promotions enter smaller or earlier material databases, so all
dependencies must be present in the working directory. The supplied family
scripts generate material in GWD order and use `--restart` to replace each
target before regenerating it:

```sh
EGTB_THREADS=16 ./1x1.sh
EGTB_THREADS=16 ./2x1.sh
EGTB_THREADS=16 ./2x2.sh
EGTB_THREADS=16 ./3x1.sh
# Continue with 3x2.sh, 3x3.sh, 4x1.sh, ... through the seven-piece jobs.
```

Set `EGTB_SLICED=--sliced` to make every family script use man-row sliced
generation. An unset value, or any value other than the exact string
`--sliced`, selects ordinary generation:

```sh
EGTB_THREADS=16 EGTB_SLICED=--sliced ./4x3.sh
```

Logs are written below `logs/<family>/` (or `EGTB_LOG_DIR`). Names include
`<material>-rev<executable-revision>-<UTC-timestamp>-<PID>.log`; the revision is
queried from the executable before each job, not read from the source tree.
Earlier logs are left untouched. Generation order sorts by total piece
count, larger White side first, then White kings descending and Black kings
descending. This matches the historical GWD order and ensures promotion
targets are available.

### Man-row sliced generation

`--sliced` reduces the peak working set for material containing men while
producing the same ordinary full-index `.dtm` file:

```sh
./generate_egtb --sliced -j 16 0 4 0 3
```

For king-only material, `--sliced` prints a warning and is ignored because
there are no man rows by which to partition the database.

The most-forward man identifies a slice. One colour with men gives up to nine
slices; men of both colours give up to 81. White rows are generated from 2
toward 10 and, within each White row, Black rows from 9 toward 1, omitting
empty frontier rows. A forward man move
therefore either remains in the current slice or enters a completed read-only
slice. Captures and promotions continue to use normal material dependencies.
Each slice uses compact compilation and mandatory full read-only verification,
with automatic consistency repair only as a mismatch fallback.

For traversal experiments, `EGTB_SLICE_ORDER` accepts `row` (the default),
`column`, `tile2`, `tile3`, or `diagonal`. Coordinates run away from promotion;
every order completes both forward-move slice dependencies first. Tiles are
2-by-2 or 3-by-3, processed row-first both between and within tiles. Diagonal
order processes increasing sums of the two coordinates. Checkpoint names and
the merged file format are unchanged, so traversal can change on resume.
This option does not retain private slice caches across slice boundaries.
`benchmark_slice_order.sh` compares all five orders against an unsliced reference
using real dependencies, full verification, histogram checks and repeated runs.

Slice indices are independently dense. Their position counts sum exactly to
the normal full-index count. Empty slices are omitted: six or more men cannot
fit behind a frontier on their final single row. After every nonempty slice
is verified, an up-to-9- or up-to-81-way
monotonic merge reranks its positions with the unchanged full index and writes
the standard DTM. The merge uses the requested thread count, capped by the
number of output pages. Workers own contiguous, page-aligned output ranges;
binary searches locate the corresponding local ranges in every slice.
Input files and immutable indexers are shared, while sequential readers,
caches, compression contexts and batch writers are private. Compressed batches
are written in unordered, hole-free layout, as in frontier compilation.
Boundary postconditions, input/output coverage, and missing, duplicate or
out-of-order full indices are checked in production. Checkpoints and file
formats are unchanged; existing verified slices can be resumed and merged
with a different thread count.
The completed full database then undergoes exhaustive read-only consistency
verification before publication, so GWD and WDL compilation require no
special handling.

Temporary state is stored under `<database>.work`. A completed slice is first
verified under an `.incomplete` name, atomically renamed, and recorded in an
atomically replaced CRC32C-protected manifest. Restarting the same command
validates the manifest checksum and every completed slice header and page
checksum, restores its generation statistics, and resumes at the first missing
slice. Since revision 3.508, each completed slice is scanned using the requested
thread count, capped by its page count, with private page-aligned readers and
per-slice progress/ETA (`resume integrity check`). These checksum/decode checks
also run with `--verification none`; they are not game-theoretic verification.
Legacy version-1 manifests are accepted once and upgraded atomically.
The workspace is deleted only after the final full database passes
verification. Set `EGTB_KEEP_SLICES=1` to retain it deliberately, for example
when testing restart behavior.

### Eight-piece generation

Eight-piece material uses the existing 16-bit DTM format and configurable
2,048-byte pages. Generate all required capture and promotion dependencies
first, in the same kings-first order as smaller databases. For example:

```sh
./generate_egtb -j 16 4 0 4 0
./generate_egtb --sliced -j 16 0 4 0 4
```

The tests check all 165 eight-piece material counts, sampled full/slice index
round trips, table/padded move generation, large frontier indices, GWD lookup
and mirroring, and a small exhaustive slice with synthetic external DTMs.
They do **not** constitute verification of a fully generated eight-piece EGTB.

Memory and disk requirements remain substantial. Four kings against four
kings has 37,581,505,500 positions (150,326,022,000 uncompressed DTM bytes for
both sides). The largest eight-piece material has 884,567,138,400 positions.
Five backtracking bitmaps alone need approximately `5 * positions / 8` bytes;
use the current slice's count when estimating sliced generation memory.
Add the page directory, caches, dependency views, assembly buffers and frontier
storage. Men-row slices are unequal in size. The resident-memory limit selects
cached verification when the full database will not fit; it does not cap all
generator memory. Full eight-piece WDL loading also needs half a byte per
position, so GWD should only attach databases that fit its memory budget.

## Generation pipeline

The generator deliberately separates retrograde propagation from the final
random-access DTM file. This avoids repeatedly scanning and rewriting the
compressed database for every mate distance.

1. **Create the index and empty database.** The legal position count and
   maximum index are calculated for the fixed material. A version-4 DTM is
   created logically as all draws.

2. **Partition work at page boundaries.** Each worker owns a contiguous range
   of complete DTM pages and the corresponding whole 64-bit bitmap words. No
   two workers write the same final page.

3. **Initialize known outcomes and DTM seeds.** Workers enumerate their slices for both
   WTM and BTM. No-move positions become lost-in-zero; immediate mates become
   won-in-one. Captures and promotions query previously generated databases,
   so initialization can also discover DTM values larger than one. Every known
   result is appended to the worker's exact-DTM frontier stream.
   Mixed internal/external potential losses without an external draw or loss
   are also scheduled as deferred candidates at the largest external winning
   distance. An initialized win can still be shortened by an internal move.

4. **Maintain persistent outcomes.** Shared WTM/BTM won and lost bitmaps hold
   accumulated outcomes. The compressed streams identify the exact current
   frontier, while a shared atomic candidate bitmap routes its predecessors to
   their owning workers. These structures replace random writes to a working
   DTM during propagation.
   Lost-in-zero entries are propagated first, so winning quiet moves that
   immobilize the opponent are discovered before the won-in-one layer.

5. **Propagate won to lost.** For a won-in-N frontier, legal quiet inverse moves
   generate candidate opponent predecessors. A predecessor is lost in N+1 only
   when every legal forward move is a known win no longer than N and at least
   one is exactly N. This implements the losing side's longest defense.
   Deferred external candidates join the same layer, even if no internal
   successor reaches distance N. Successor evaluation stops at the first
   disproof; the padded backend still generates the complete legal move list.

6. **Propagate lost to won.** For a lost-in-N frontier, every legal inverse move
   directly proves a candidate win in N+1: the inverse generator already
   supplies the legal forward move reaching the loss. Forward moves are not
   regenerated for this existential step. An existing shorter win is retained.

7. **Store frontier streams.** Each worker writes 512-index blocks to one
   CRC32C-checksummed Zstd-compressed append-only temporary file. Frontier
   checksums use the same hardware-accelerated implementation and portable
   fallback as DTM pages. Files are created as
   `.gwdegtb-frontier-*` in the working directory and immediately unlinked, so
   the filesystem reclaims them automatically when their descriptors close or
   the process exits. No global stream lock or collection of per-distance files
   is required.
   Deferred external candidates use a second compressed store with one file per
   worker; that store is released before final DTM compilation.

8. **Compile a compact temporary DTM.** Each worker assembles page-aligned
   paired-entry buffers from its compressed frontier streams, replayed from
   longest to shortest distance so shorter replacements win. It encodes WTM
   and BTM pages separately and batches their compressed blocks into up to
   4 MiB positional writes. A short mutex-protected offset reservation allocates
   exactly the batch's byte count; compression and writes run outside the lock.
   Blocks can interleave between workers, but directory offsets identify every
   page. There is no reserved per-page padding and no abandoned batch tails.
   Each worker keeps its writer and ZSTD context across assembly buffers; no
   worker waits for another worker's index range to finish. Compilation uses
   the requested number of workers, without a separate compression pool.
   Physical block order (and therefore file SHA-1) can differ between runs.
   Compare decoded values for exact content equality, not whole-file hashes.
   Draw-only pages have no payload. When a worker's range exceeds its assembly
   buffer, it scans the frontier block directory for each buffer window, but
   skips blocks outside that window before reading, decompressing or checking
   their payload. Each block keeps minimum/maximum indices in RAM (16 extra
   bytes per block); overlapping blocks retain record-level filtering and
   checksum verification. This does not require sorted records.
   The five outcome/candidate bitmaps are released before allocating assembly
   buffers. No additional partition files are created.
   Candidate searches stop at each worker's range boundary. Producers count
   newly marked bits atomically, so progress totals need no full-bitmap scan.
   Empty candidate evaluation is skipped; after evaluation workers clear their
   own word-aligned ranges before the next source phase starts.
   All writers finish before the directory is flushed and verification starts.

9. **Verify and collect statistics together.** The completed temporary file is
   reopened read-only. If it fits the resident limit, it is checksum-verified
   and decompressed in parallel into RAM; otherwise workers use separate
   sequential and random-access cache views. One exhaustive parallel forward
   check recomputes both side-to-move values for every position. During this
   check workers collect WTM/BTM histograms (1 MiB per worker), exact maximum DTM,
   and deterministic longest-win/loss and draw examples. These are merged for
   the final output. The success path allocates no correction worklists or
   verified-position bitmap and performs no separate statistics/example scan.

10. **Repair only on a DTM mismatch.** I/O, checksum, codec and dependency-query
    errors are fatal and do not trigger repair. A genuine value mismatch closes
    the resident/read-only views, reopens the unpublished file writable, and
    invokes the existing consistency repair algorithm. Exact-layout files
    support in-place replacements that fit and append larger replacements.
    Only this fallback runs checked block-copy compaction, followed by a fresh
    full read-only verification that rebuilds histograms and examples. A second
    mismatch is fatal. The legacy generation/repair APIs retain their existing
    behavior for callers and regression comparisons.

11. **Publish only a verified result.** Until success, the CLI uses
    `<database>.dtm.incomplete`, never the final `.dtm` filename. After verifying
    and collecting storage metadata it closes the backing, syncs the file,
    renames it to the final name and syncs the parent directory. Successful runs
    require no separate compaction, DTM scan or example scan. A failed complete
    temporary database is retained for inspection; it is not advertised as a
    valid DTM. An existing final database is not intentionally replaced.

12. **Apply the same rule to slices.** Each slice must pass a full forward check
    before it is durably published and recorded in the checksummed, durably
    replaced manifest. Later slices therefore use only verified predecessors.
    The full-index merge uses the compact writer too; its result is independently
    verified before CLI publication. Normal WDL and GWD readers are unchanged.
    fatal.

## Memory, caches, and configuration

The default 2,048-byte v4 DTM page gives 1,024 positions for one side and exactly
sixteen 64-bit bitmap words. Current production defaults are:

| Purpose | Default |
|---|---:|
| DTM page size (`EGTB_PAGE_SIZE`) | 2,048 bytes |
| DTM Zstd compression (`EGTB_COMPRESSION_LEVEL`) | Level 1 |
| Fresh compiled backing cache | One page; output uses batch buffers |
| Frontier compilation assembly buffer | 1 GiB total, divided among workers |
| Compact output batching | Up to 4 MiB plus 1 MiB record metadata per worker |
| Dependency cache | 64 MiB per worker per opened dependency |
| Ordinary read-only handle cache | 16 MiB |
| Resident final-database limit | 32 GiB |
| Nonresident final-verification cache | 32 GiB total |

Override the DTM page size in bytes without recompiling:

```sh
EGTB_PAGE_SIZE=1024 ./generate_egtb -j 16 1 1 1 1
EGTB_PAGE_SIZE=2048 EGTB_THREADS=16 ./4x3.sh
```

Accepted sizes are powers of two from 128 through 32,768 bytes. The generator
prints the selected size at startup. The setting applies to normal and sliced
generation; existing databases retain the page size recorded in their headers.
A sliced workspace must be resumed with its original page size. Cache budgets
remain byte-based, so larger pages reduce the number of cached pages, not the
configured cache memory. WDL pages remain fixed at 1,024 bytes.

DTM compression defaults to Zstd level 1. Override it without rebuilding:

```sh
EGTB_COMPRESSION_LEVEL=6 ./generate_egtb -j 16 1 1 1 1
EGTB_COMPRESSION_LEVEL=6 EGTB_THREADS=16 ./3x2.sh
```

The setting accepts positive levels from 1 through the linked Zstd library's
maximum (currently 22), is printed at startup, and applies to new normal and
sliced DTM files. Existing files retain their stored compression level, including
completed slices when resuming a workspace. Fallback block-copy compaction preserves
the compressed blocks. Temporary frontier streams remain at level 1; WDL
compression is independent and unchanged.

The dependency-cache figure is potentially multiplied by both the worker
count and the number of dependency databases actually opened. Cache metadata
and page directories are additional.

### Shared resident dependencies

`generate_egtb` and `verify_dtm` share immutable dependency arrays across all
workers. Defaults are **2 GiB total** and **256 MiB per dependency**. A canonical
dependency is loaded and checksum-verified once on first use. Other workers
wait for that load, then retain a pointer and probe without locks. Resident
dependencies do not allocate per-worker page caches. Arrays remain available
through final verification and are freed after all workers finish.

```sh
# Defaults, stated explicitly:
EGTB_DEPENDENCY_RESIDENT_GIB=2 EGTB_DEPENDENCY_RESIDENT_MAX_MIB=256 \
  ./generate_egtb -j 16 3 0 2 1
# Permit larger dependencies within an 8 GiB total budget:
EGTB_DEPENDENCY_RESIDENT_GIB=8 EGTB_DEPENDENCY_RESIDENT_MAX_MIB=2048 \
  ./generate_egtb -j 16 3 0 2 1
# Cache-only comparison:
EGTB_DEPENDENCY_RESIDENT_GIB=0 ./generate_egtb -j 16 3 0 2 1
```

Both settings accept nonnegative whole numbers; zero disables admission.
Admission is first-use among eligible databases, not an adaptive ranking of
hotness. The per-database cap prevents a single large, potentially cold
dependency from occupying the whole budget. Dependencies exceeding either
limit use page caches (private by default); there is no residency eviction or
promotion during a run. Corruption, I/O or allocation failures while loading an admitted
database are fatal, not silently treated as draws or cache misses.

Budgets count paired 16-bit array payloads (four bytes per position), not all
process memory. Allow additional space for directories, approximately 1 MiB
of histogram metadata per resident database, loader workspaces, fallback
caches, current-database residency and generation bitmaps. Bitmaps are already
allocated during initialization. A load runs on the requesting worker without
spawning extra threads or disturbing phase progress; distinct dependencies
can load concurrently. The pool also serves external material dependencies
during sliced generation, but completed-slice caches remain unchanged.
GWD's public lookup APIs are unchanged.

### Experimental optimistic shared dependency cache

Set `EGTB_DEPENDENCY_SHARED_CACHE_MIB` to enable a shared read-only page cache
for each dependency which is not admitted to residency. Default **0** retains
the existing private caches. This setting is a **per-database payload budget
shared by all workers**, not a total memory limit.

```sh
# Compare with 16 workers each using the default 64 MiB private cache:
EGTB_DEPENDENCY_RESIDENT_GIB=0 EGTB_DEPENDENCY_SHARED_CACHE_MIB=1024 \
  ./generate_egtb -j 16 3 0 2 1
```

Resident dependencies still take priority. A cache that holds all pages uses
collision-free addressing; otherwise each side uses as many whole page
slots as fit the budget. Actual allocation is reported, with
another 64 bytes of metadata per physical slot and private codec/scratch
workspaces per worker. Multiply by the number of nonresident dependencies
opened when planning RAM.

Hits optimistically read a sequence counter, atomic page tag and atomic
64-bit payload word, then validate the sequence. They do not acquire locks
or write shared state. On a miss or concurrent replacement, the worker
decompresses and checksum-validates into private scratch memory. It then
tries once to publish with a compare-and-exchange and atomic word stores;
if another worker is publishing, it returns its private result without waiting.
Duplicate decompressions are possible. Sequence counters saturate rather than
wrap. The shared data is never passed directly to Zstd.

Both `generate_egtb` and `verify_dtm` support this option. Writable databases,
current-database verification caches, completed-slice caches and GWD's API
are unchanged. This remains opt-in pending representative performance tests.
`test_shared_cache` exercises concurrent replacement, dense-cache hits,
shared admission, wide DTM values, partial/draw pages and corruption rejection.

#### Adaptive fractional growth and lazy dense mode

Revision 3.502 adds named cache policies. Select one for the whole process:

```sh
export EGTB_DEPENDENCY_CACHE_POLICY=cost-gated-v1  # Default
# Alternative experiment:
# export EGTB_DEPENDENCY_CACHE_POLICY=spare-budget-v1
# export EGTB_DEPENDENCY_CACHE_POLICY=idle-reclaim-v1
# Experimental slice traversal; default remains row-first.
# export EGTB_SLICE_ORDER=tile2  # row | column | tile2 | tile3 | diagonal
```

`cost-gated-v1` retains the existing load-cost admission and fractional growth.
`spare-budget-v1` uses a 0.1% admission floor when a larger allocation fits
within unused shared budget, using discard growth if needed (or the configured floor if lower).
Growth needing redistribution still requires the original configured floor,
normally 0.5%. An eligible database goes directly to dense-lazy mode if its full
new allocation, including metadata, consumes at most half the currently free pool.
Otherwise it uses fractional growth. Both retain confidence, warm-up, cooldown,
recovery reservation and allocate-before-free safety checks.

Revision 3.507 adds experimental `idle-reclaim-v1`, using the same growth admission
as `spare-budget-v1`. When a pressure-qualified receiver is capacity-blocked, it
selects the largest eligible idle cache and discards it down to 1 MiB (or the
configured initial size if smaller). Eligibility requires no lookups for 60
seconds, renewed grace at each phase boundary, no pending growth assessment,
and no shrink within the last 300 seconds. Even a cache hit resets idle time.
It retains a minimal allocation-failure fallback, counts metadata against the
budget, and respects `EGTB_DEPENDENCY_SHARED_CACHE_REBALANCE=0`.
This is not a reversible transfer: there is no rollback reservation or promised
benefit. Returning donors refill and may grow immediately; receiver growth is
reevaluated at the next checkpoint. It does not reclaim resident arrays or shrink
active caches. The default policy remains unchanged.

Revision 3.602 also monitors dependencies that fell back to private caches because
the shared budget was full. Private misses sample page-load time once per 256
misses; cache hits do no timing. At quiescent checkpoints, two measured pressure
windows can request shared admission under `idle-reclaim-v1`, reclaiming a
genuinely idle donor if necessary. All worker probes switch together after
successful allocation. Allocation failure leaves private lookup intact and retries
after a cooldown. Phase changes restart measurement and donor idle grace.
Logs report private lookup/decompression pressure and admission outcomes.
Old private views remain allocated until their catalogs close, preserving their
cumulative statistics; their memory, like other private fallback caches, is not
part of the shared-cache budget. Other policies report private pressure without
performing these admissions.

Revision 3.603 remembers the capacity discarded by idle reclamation. If that
dependency becomes busy again, fresh pressure measurements that clear the normal
load-share floor allow `idle-reclaim-v1` to request its previous capacity directly,
instead of repeating the growth ladder from 1 MiB. Revision 3.605 starts returning-cache
measurement at the phase boundary and requires one substantial pressure window
(100,000 lookups, 1,000 decompressions and 16 fresh timed samples), rather than
warm-up followed by two windows. Ordinary growth still uses two windows.
Budget/metadata/fallback accounting and post-growth assessment still apply.
A budget-limited partial recovery retains the
target for a later pressure-qualified attempt; full recovery clears it. The hint
survives phase changes but reserves no memory and cannot evict an active cache.
`cache recovery` log lines report the remembered capacity and actual result.

Under `idle-reclaim-v1`, growth leaves 5% of the shared allocation budget free
for new admissions (metadata included). Admissions may consume this headroom;
it is not extra RAM or a guarantee that every dependency will fit. From 3.606,
pressure-qualified reclaimed caches may also use it to bootstrap recovery. The
allocation ceiling is the larger of the initial allocation, one quarter of the
headroom, and the minimum two-slot-per-side allocation, bounded by the remembered
target and actual free budget (including metadata and failure fallback). Logs
identify this as `returning-cache-admission-headroom`. This prevents a returning
1 MiB cache being blocked solely because it is already admitted; it does not
promise full recovery or solve competition between active caches. Existing
active caches are not arbitrarily shrunk to restore the margin.
From 3.607, `idle-reclaim-v1` with rebalancing enabled can lend unused headroom
to one cache once ordinary growth is capacity-blocked and fresh measured cost
clears the normal admission floor. The cache's pre-loan payload is recorded.
Qualified private admissions and returning caches below their bootstrap ceiling
can recall that loan at a quiescent checkpoint, even while the borrower is active.
Only loaned capacity is recalled, not the protected base. Migration preserves
surviving entries when overlap fits; otherwise discard/refill avoids exceeding
the budget but can make the entire borrower cold. Allocation failure retains a
usable cache and records any missing base capacity for recovery. Borrowing pauses
for 60 seconds after repayment and while a qualified private admission is pending.
Logs report `cache headroom loan` and `cache headroom repayment`, including the
repayment method and elapsed time. This is experimental: refill and resize costs
may outweigh the benefit; lower miss counts alone do not establish a speedup.
From 3.608, recovery and loan recall use the same page-realizable bootstrap
allocation. A sub-page remainder in the budget cannot trigger repeated loan
repayments after a returning cache has already reached that target.
From 3.609, a borrower already eligible for idle reclamation is reduced directly
to the idle floor when its loan is recalled, avoiding an intermediate allocation
of its protected base. The protected capacity remains a recovery hint; loaned
capacity is not added to that hint. Logs identify `cache headroom idle repayment`.
Active borrowers and borrowers still inside the idle/resize/assessment guards
retain the normal loan-only repayment path; no waiting or grace reduction is added.
Idle grace remains 60 seconds: faster recovery does not
permit reclaiming a dependency merely because it has not been used yet in a slice.

Revision 3.610 adds bounded **active-cache transfer trials** to `idle-reclaim-v1`
when rebalancing is enabled. Normal growth and idle reclamation remain preferred.
When a pressure-qualified shared receiver cannot grow, the coordinator may take
the smallest of 5% of an active donor's payload, 1% of the pool budget, or 50% of
the receiver's payload (rounded down to whole page pairs). The donor must have
fresh substantial observations and measured load pressure below a quarter of the
receiver's; low pressure selects an experiment, not proof that shrinking is safe.
The donor is never reduced below the configured initial cache size by this path.

Only one trial runs at a time, at quiescent checkpoints, with memory reserved for
both forward and reverse discard-resizes including fallback allocations and
metadata. After one warm-up window, two windows of at least 100,000 lookups per
cache and at least five seconds each compare sampled load cost per lookup,
normalized to pre-trial traffic. Receiver savings must exceed added donor cost
by 10% and amortize the all-worker resize stall within a projected 300 seconds.
This is observational evidence, not a guaranteed runtime improvement. Acceptance
keeps the new capacities; weak benefit, inconclusive timing, a 180-second deadline,
counter resets or a phase boundary restores the old capacities. Allocation
failures retain the recovery reservation and retry. Cached contents may need to
refill after resizing or rollback. Donors are protected for 300 seconds afterward.
Logs identify `cache active transfer` and `cache active measurement` and explain
acceptance or rollback. Set `EGTB_DEPENDENCY_SHARED_CACHE_REBALANCE=0` to disable
rebalancing, including these trials. Free system RAM does not enlarge the configured
shared-cache budget automatically.

Revision 3.611 preflights active transfers against estimated **round-trip** resize
cost (forward plus rollback). A cold estimate of 4 GiB/s over old+new allocation
footprints is replaced by a larger measured coordinated-resize cost when observed.
Projected savings use the receiver's measured load cost times its fractional
capacity increase, capped at 25%; this is a conservative admission heuristic, not
a measured miss curve. A trial must plausibly repay that all-worker stall within
300 seconds before allocating anything. `cache active admission` logs explain
`resize-cost-payback` and `failed-pair-unchanged` rejections, throttled per receiver.
Failed donor/receiver pairs are remembered across slice and phase changes. A retry
requires at least 30 minutes **and** a 25% capacity change in either cache or a
doubling of receiver load pressure; elapsed time or a new phase alone is not enough.
The history lasts for the current generator process. Existing idle reclamation,
normal growth, rollback memory reservations and verification behaviour are unchanged.

Policy logs include `lookups-window`, `window-seconds`, `lookups/s`, and
`idle-seconds`. Lookup windows are intervals between maintenance checkpoints;
`decompressions/s` is the separately smoothed pressure rate, not the same window.
Routine logging remains throttled to once per minute. All policies report the
idle donor eligibility in active/shadow mode; shadow reports do not simulate
the access pattern after reclamation.

Every other registered policy is automatically evaluated in shadow mode; no
shadow-policy variable is needed. Policies are pure functions of the same active
cache measurements and common executor guards: they do not mutate counters,
cooldowns, allocations or redistribution trials. Per-policy logging state is
separate. These are **one-checkpoint counterfactual decisions**, not simulated
alternative cache histories or predictions of runtime. A/B runs remain necessary.

`cache policy evaluation` groups all policies for one database, with phase,
smoothed decompression rate, sampled load cost, load share, confidence and free
budget. Its following `cache policy decision` lines give active/shadow mode,
reason, target and threshold. Routine evaluations are limited to once per minute
per database, even if rejection reasons fluctuate. A new grow-versus-keep
disagreement is logged immediately; the same disagreement at the same cache
capacity remains suppressed across intervening warm-up/keep decisions. Counts
of suppressed evaluations are included. Phase changes restart the logging window.
`cache policy selection` identifies each policy's highest-scoring growth candidate;
only the active choice can be executed. Selection summaries are also limited to
once per minute. An outstanding redistribution trial can defer it. Actual growth,
shrink, rollback and phase changes are always logged immediately.
Phases are explicitly labelled initialization, backpropagation and verification
(including slice runs); boundaries restart measurements and settle pending trials.
Policy selection remains fixed for the run; automatic phase-specific switching
is not implemented. Unknown policy names fail configuration.

To enable growth, also set a **total** shared-cache budget:

```sh
export EGTB_DEPENDENCY_SHARED_CACHE_MIB=64  # Initial payload per cached dependency
export EGTB_DEPENDENCY_SHARED_CACHE_GIB=8   # Combined shared-cache allocation budget
export EGTB_DEPENDENCY_SHARED_CACHE_GROWTH=1.5 # Default factor, copied into each cache
export EGTB_DEPENDENCY_SHARED_CACHE_MIN_LOAD_PERCENT=0.5 # Estimated worker-time threshold
```

The total defaults to **0**, retaining fixed-size behavior. The initial MIB
setting must also be nonzero to enable shared caches. Existing resident
dependency admission still has priority and its budget is separate.

At a quiescent checkpoint, the coordinator samples private probe counters.
It discards the first nonempty interval (and the first interval after growth)
as warm-up. Complete samples require at least 100,000 lookups. Growth requires
two observations with at least 1,000 actual decompressions (not necessarily
consecutive), at least 16 successful timed page loads, and an estimated load
share of at least **0.5%** of worker elapsed time. The share is
`smoothed_decompressions_per_second * sampled_load_ns / 1e9 / workers`.
The coordinator supplies the actual (page-count-capped) worker count for each
phase. `EGTB_DEPENDENCY_SHARED_CACHE_MIN_LOAD_PERCENT` accepts 0..100 (default
0.5); zero disables only this cost threshold, not confidence or hysteresis.
Pressure uses a time-aware weighted average with a 5-second time constant:
`weight = elapsed / (5 + elapsed)`. Quiet windows and idle checkpoints decay
the pressure instead of immediately forgetting bursts. There is no hit-rate cutoff: a busy dependency
with 99.9% hits can still benefit. The eligible dependency with the highest
**estimated load-worker-seconds/second per additional MiB** is grown (additional allocation
includes slot metadata). Rates use monotonic elapsed time between checkpoints;
these measure workload pressure, not decompression CPU time or guaranteed savings.
Implicit-draw misses do not count. An idle checkpoint resets the sample clock.
Each cache keeps its own scaling factor, initially 1.5.
Under high measured load share, revision 3.313 takes up to three factor steps
at once: the smallest power covering load share / admission floor, capped at
3.375x (or an explicitly larger base factor). Zero admission floor disables
acceleration. Existing sample confidence, measurement and memory guards apply.
The environment setting
`EGTB_DEPENDENCY_SHARED_CACHE_GROWTH` accepts 1.1..4. Sizes round down to complete
pages with equal capacity per side; growth requests advance by at least one
page per side. Ordinary growth is capped by steady-state capacity, reserving a
minimal fallback cache. If old and new allocations can coexist within budget,
pages are migrated. Otherwise, at the quiescent checkpoint, the old storage is
discarded before allocating the larger cache, which refills lazily. Logs identify
`strategy=discard-and-grow` versus `strategy=migrate`. Reversible coordinator
transfers retain their existing migration and recovery reservations.

Arbitrary slot counts use exact remainder addressing (reciprocal multiplication
for 32-bit page numbers, a mask for powers of two, and a safe wider fallback).
Dense caches retain direct addressing. Migration can create collisions after
fractional scaling; the first entry is retained and others load again on demand.
Sequence numbers are not used as a recency estimate. The explicit quiescent
`egtb_shared_cache_resize` primitive also supports shrinking, but the automatic
policy does not yet choose factors below one or reclaim caches.

If the requested size
would hold every page, allocation is capped to the exact page count and
addressing becomes dense. Loaded pages are migrated without disk reads;
remaining pages load on demand. Dense caches never evict and do not grow again,
but retain the optimistic sequence checks for concurrent first loads.

Initialization and final verification use rounds of up to 1,048,576 positions
per worker when adaptive mode is enabled, keeping ownership ranges unchanged.
All workers join before maintenance and the next round starts afterwards.
Backtracking also checks between worker batches; repair checks between passes.
Progress remains one continuous phase. This adds occasional thread-launch/join
overhead but no maintenance checks, shared counter updates or locks per probe.
Growth messages report size, mode, decompressions/second, smoothed pressure and
elapsed allocation/initialization/migration time. The summary reports total
growth stalls. One in 256 shared-cache misses is timed with a monotonic clock;
only successful actual decompressions contribute samples. Timed page loading
includes I/O, CRC and decode, not only Zstd CPU time. Growth logs report a
smoothed sampled load cost and estimated summed worker elapsed load-seconds
per wall second (not CPU utilization), normalized share, threshold and worker
count. Admission uses this estimate, but it is not a promise of savings:
I/O waits and scheduling affect the measurement and workload may change.
The cost estimate is retained across growth, refreshed by subsequent timed
loads, and reset if counters regress. Only one cache grows per checkpoint.
Hits perform no timing.
After growth, one active interval is discarded as warm-up, then two complete
samples are collected before another resize can qualify. Their combined counts
and elapsed time report before/after decompressions/second and decompressions
per million lookups; the baseline likewise aggregates the last two complete
pre-growth windows. If the latter improves by less than 10%, the cache waits
two additional complete samples before reconsidering smoothed pressure.
This bounded cooldown does not permanently disable growth: further growth
may still help, especially near dense addressing. Workloads can change between samples, so
these are observational comparisons, not measured causal speedups. This first
policy only grows caches; shrinking, reclamation and bidirectional hysteresis
remain future work.

The total budget includes shared page payload, 64-byte slot metadata, and
old/new allocation overlap when migrating (or minimal fallback storage when
discarding). New dependencies that
cannot obtain their initial allocation fall back to private caches; those
caches, codec workspaces, resident arrays, bitmaps and directories are outside
this budget. Migration failure retains the old cache. Discard-growth failure
retains a preallocated minimal working cache; accounting is updated and further
growth of that cache is disabled. This cannot protect against OS OOM termination.
This is not a whole-process RAM limit.

Cache-policy benchmark, 2026-09-12: Ryzen 9 5950X, two workers, ext4 `/tmp`,
`1 2 2 0` (51,369,120 positions), Zstd level 1, 2,048-byte pages,
dependency residency disabled, 1 GiB adaptive shared-cache budget and 1 GiB
compilation/current-residency/verification budgets. One unmeasured warm-up,
then three repetitions per configuration with alternating old/new order.
Median total seconds (including exhaustive verification):

| Shared-cache configuration | Revision 3.308 | Revision 3.309 | Change |
|---|---:|---:|---:|
| Fixed 64 MiB | 35.071 | 35.482 | +1.2% |
| Adaptive, initial 64 MiB | 35.696 | 35.923 | +0.6% |
| Adaptive, initial 1 MiB | 48.646 | 49.438 | +1.6% |

These measurements demonstrate no speedup for this workload. With 64 MiB
initial caches, 3.309 grew the 218 MiB dependency to a 128 MiB cache, whereas
3.308 never grew it. The measured growth pause was 0.071–0.072 seconds.
Starting at 1 MiB caused 13 growths in 3.309 with only 0.036–0.037 seconds of
combined growth pauses: its much slower initialization is not explained by
allocation time alone. All runs passed exhaustive verification and matched
histograms, examples and storage statistics; the final output also matched all
51,369,120 paired values in the existing reference database. This five-piece
benchmark does not establish performance for multi-GiB growth or eight pieces.

Reproduce with `sh benchmark_cache_policy.sh OLD_BINARY NEW_BINARY DEPENDENCY_DIRECTORY 3`
(absolute paths, on an idle host). Raw logs for this run were retained in
`/tmp/gwdegtb-policy-0wrMTx/`; temporary files are not part of the repository.

Measured-cost admission comparison (3.309 → 3.310), same material and budgets,
AOCC 5.2 on the 5950X: one warm-up followed by two repetitions, reversing
old/new order on the second repetition. Mean total seconds:

| Shared-cache configuration | Revision 3.309 | Revision 3.310 | Change |
|---|---:|---:|---:|
| Fixed 64 MiB | 37.884 | 38.104 | +0.6% |
| Adaptive, initial 64 MiB | 38.340 | 38.775 | +1.1% |
| Adaptive, initial 1 MiB | 52.698 | 52.454 | −0.5% |

This does **not** demonstrate a runtime improvement. The 0.5% admission
threshold rejected the previously unhelpful 64→128 MiB growth in both runs:
combined allocation was 99.20 instead of 165.20 MiB. The 1 MiB start performed
12 instead of 13 growths (28.03 versus 32.15 MiB allocated); its cold-start
penalty remains. Growth cadence is unchanged. All 12 measured runs passed
full verification and matched histograms, examples and storage statistics.
Full regression tests, adaptive entry-for-entry comparison on `1 1 1 1`, and
the shared-cache ThreadSanitizer test also passed. Raw timing logs are in
`/tmp/gwdegtb-policy-Ouqko2/`. These measurements do not establish eight-piece
scaling or justify changing the other RAM budgets.

Fractional growth comparison (revision 3.311), same material (`1 2 2 0`),
two threads, AOCC 5.2 on an idle 5950X. Both columns use the same executable,
with growth factors 2 and 1.5 respectively, isolating the policy change.
One warm-up and two repetitions, with reversed order; mean total seconds:

| Shared-cache configuration | Factor 2 | Factor 1.5 |
|---|---:|---:|
| Fixed 64 MiB (control) | 35.444 | 35.381 |
| Adaptive, initial 64 MiB | 35.961 | 36.004 |
| Adaptive, initial 1 MiB | 48.972 | 58.723 |

Neither 64 MiB adaptive run grew on this material. Starting at 1 MiB,
fractional growth used 25.81 instead of 28.03 MiB, but required 17 rather
than 12 growths and took 19.9% longer. Thus 1.5 provides finer memory
increments, not a guaranteed speedup; retain a sensible initial cache size.
All 12 measured runs passed verification and matched histograms, examples
and storage statistics. Full regression and shared-cache ThreadSanitizer
tests passed, including fractional migration, shrinking, collisions and
budget-clamped growth. Logs: `/tmp/gwdegtb-policy-puCiJR/`. This is not a
16-thread or eight-piece scaling benchmark, nor a comparison of old/new
cache-addressing implementations.

#### Conservative memory coordinator (3.312, refined in 3.313)

Adaptive shared pools now reclaim sustained-idle caches when an eligible
growth cannot be funded. Set `EGTB_DEPENDENCY_SHARED_CACHE_REBALANCE=0` to
retain growth-only behavior (default: `1`). Fixed-size pools are unchanged.
Resident arrays remain outside this coordinator and keep their separate budget.

Stage one uses **idle donors only**: no lookups for at least 60 seconds,
including dense caches. It never treats a high hit rate as evidence that an
active cache can shrink. A donor shrinks by its per-cache factor (default
divide by 1.5), rounded to whole slots, down to the smaller of its initial
size and 1 MiB. The coordinator tries a feasible donor for the highest-scored
unfunded growth request. Without a growth request, idle reclamation is attempted
only above 90% budget utilization. This is a conservative heuristic, not an
optimal allocator or a ghost-based miss predictor.

Only one transfer experiment is outstanding. Before starting, the coordinator
checks every allocate-before-free peak for both forward execution and recovery.
It reserves recovery headroom against new admissions and suspends other resizing.
If the donor develops decompression pressure during assessment, the receiver is restored first,
then the donor. A transfer is accepted after receiver warm-up plus two substantial
measurement windows only if decompressions per lookup improve by at least 10%
(or it becomes dense). No evidence within 120 seconds causes rollback; idle-only
reclamation is accepted after that interval if the donor remains below the pressure
floor. An isolated donor hit does not trigger recovery. Recovery uses historical
measured load cost, or 5 microseconds/load until available, with at least 256
decompressions observed and cost normalized by elapsed time and worker count.
Receiver comparisons use the same two-window baseline as ordinary growth.
Explicit initialization and verification boundaries (also for slices) reset idle
grace and pressure windows and roll back outstanding trials. Compilation time
therefore cannot make a dependency immediately eligible for idle reclamation.
Allocation failures during recovery retain the reservation and retry at subsequent
checkpoints; budget headroom does not guarantee allocation success at the OS level.
After assessment the donor is protected for 300 seconds and receiver for 60.
For a reproducible constrained-budget six-piece A/B, run
`sh benchmark_coordinator.sh /absolute/path/generate_egtb /absolute/path/databases 16 2`.
It compares rebalancing off/on in alternating order, with a 1 GiB shared budget
and residency disabled for dependencies. Runs use a fresh `/tmp` directory,
verify exhaustively and compare statistics. Check that transfers actually occur
before interpreting timing differences as coordinator benefits.

The initial 3.312/3.313 two-thread A/B on `1 2 2 0` (one measured pair per
configuration) gave adaptive-64-MiB totals of 37.54/38.15 seconds and
adaptive-1-MiB totals of 60.94/47.98 seconds. Cold-start initialization fell
from 32.71 to 19.40 seconds with 17 versus 7 growths. All statistics matched
and all runs verified. These are preliminary timings, not repeated-trial
confidence intervals or evidence of a redistribution benefit.
The 16-thread constrained `3 0 2 1` trial took 136.25 seconds with rebalancing
disabled and 138.19 seconds enabled; both verified with identical statistics.
Neither triggered a shrink or transfer, so redistribution's production benefit
remains unmeasured. Logs: `/tmp/gwdegtb-coordinator-mlKkde/`.

Later workload changes can still require ordinary growth; rollback protection
is limited to the experiment, not the remainder of generation.

Logs identify experiments, acceptance and rollback, sizes, and peak allocation.
Revision 3.314 adds database filenames to growth, measurement, and coordinator
events (maximum indices are not unique material identifiers). Completion events
also report the acceptance/recovery reason. The dependency memory table shows
current payload, allocation including shared-slot metadata, full decoded size,
capacity coverage, and cached/dense-lazy/resident mode. Shared allocations are
counted once, not multiplied by worker count; private-view memory is not included.
Coverage is capacity, not page occupancy or cache hit rate.
Logs also show recovery reserve. The summary counts shrinks, transfers and rollbacks. The reported
resize stall total includes coordinator resizing as well as growth. No shared
updates or extra checks are added to the lookup hit path. Active-donor trials,
ghost prediction, zero-headroom shrinking and parallel atomic initialization
are deliberately deferred.

`make test-adaptive` generates a private-cache baseline and an adaptive
`1 1 1 1` database, compares every paired value and runs standalone verification
across multiple scan rounds.

Configure current-database final handling separately with:

```sh
EGTB_RESIDENT_LIMIT_GIB=0 ./generate_egtb -j 16 3 0 3 0
EGTB_RESIDENT_LIMIT_GIB=64 ./generate_egtb -j 16 3 0 3 0
EGTB_VERIFICATION_CACHE_GIB=64 ./generate_egtb -j 16 4 0 3 0
EGTB_COMPILATION_BUFFER_GIB=16 ./generate_egtb -j 16 2 2 1 2
```

`EGTB_COMPILATION_BUFFER_GIB` must be a positive integer. Larger buffers reduce
frontier rereads; they replace the large writable cache during compilation,
but do not include outcome bitmaps, dependencies, dictionaries, or metadata.
The library's `EgtbThreadOptions.compilation_buffer_bytes` accepts a byte budget;
zero reuses the writable-cache byte budget. Buffers are rounded down to complete
paired logical pages, with a minimum of one such page per worker, and capped
at each worker's position count. Sliced generation uses the same mechanism.

Setting `EGTB_RESIDENT_LIMIT_GIB=0` disables current-database residency. Resident
loading is parallel. This limit also applies separately to each newly generated
slice: if its decoded size (four bytes per position for both sides) fits, the
slice is loaded once and shared read-only by verification threads. Otherwise
verification uses the cache budget from `EGTB_VERIFICATION_CACHE_GIB`.
The resident slice is released before publication and before generating the
next slice; previous-slice dependency caches remain separate. The log reports
whether each slice used resident or cached verification. The resident limit
is not a process-wide RAM cap, so leave room for dependencies and metadata.
It is normally fastest when the complete four-byte-per-
position database fits comfortably in physical RAM. The generator reports
cache lookups, hits, misses, decompressions, dirty evictions, compressed
writes, storage ratios, and wall-clock time for each major phase.

The timing summary follows execution order: setup, initialization,
backpropagation, frontier compilation, generation subtotal, verification plus
statistics/fallback, storage metadata, durable publication, and total.
The generation subtotal includes the generation phases above it; do not add it
to them. Verification includes close/reopen and resident loading where enabled,
plus any cold repair, compaction and reverification. Small administrative costs
are included in the overall timing but are not all separate phase rows.
For sliced generation, phase times are sums across newly generated slices,
followed by slice verification/fallback and the full-index merge. Reused slice
counts are reported, but their historic timings are excluded. The overall total
is sampled before the optional post-publication slice-workspace cleanup.

## Example: 1 king + 1 man against 1 king + 1 man

The detailed output below is a historical measurement, before revision 3.005.
With terminal-zero propagation and deferred external loss candidates, this
database requires one consistency pass and zero corrections. Revision 3.005
replaced recompression with checked block-copy compaction and folded the final
DTM scan into consistency histograms. Revision 3.102 goes further: direct compact
compilation and one verification/statistics pass replace the normal repair,
compaction and example-scan phases described in that historical output.
An indicative single-run comparison on the same
host (not a controlled scaling benchmark) gave:

| Threads | Revision 3.004 total | Revision 3.005 total |
|---|---:|---:|
| 1 | 8.519 s | 6.226 s |
| 16 | 1.939 s | 1.251 s |

The compacted 1/1/1/1 database was byte-identical. All 23 canonical two-to-four
piece databases generated with four threads required zero corrections and
passed standalone `verify_dtm`, which checks all positions without a skip bitmap.
The same held for all 12 five-piece 3x2 databases. Real nine-slice (2/0/1/1) and
81-slice (1/1/1/1) generations also passed full verification and matched their
unsliced databases entry-for-entry.
Regression tests also cover high external DTM seeds, slice resume, shrinking
DTM maxima, failed cache loads, shared-handle cleanup, and legacy-format copying.

The following run used revision 2.101, 16 threads, and the optimized native
build on the Ryzen 9 5950X described above. Individual correction records,
repetitive cache tables, and the BTM frequency table are omitted here; WTM and
BTM distributions were identical. The lifecycle labels below show the current
revision 2.102 output format.

```text
$ ./generate_egtb -j 16 1 1 1 1
GWDEGTB revision 2.102 starting
generating 1wX-1wO-1bX-1bO.dtm with 16 threads, 1024 MiB writable cache total, 64 MiB dependency cache per worker/database
generated 1wX-1wO-1bX-1bO.dtm: material=1 1 1 1 positions=4478160 maximum-index=4478159 passes=19 maximum-dtm=19 threads=16
self-consistency: passes=2 updates=277/277
final read-only consistency verification: threads=16 resident=8 MiB positions-checked=8540 positions-skipped=8947780
WTM: wins=882645 losses=26562 draws=3568953
WTM DTM statistics:
     DTM            Frequency
     -18                   30
     -16                  120
     -14                  194
     -12                  237
     -10                  913
      -8                 5919
      -6                 4109
      -4                 2861
      -2                12088
      -1              3568953
       0                   91
       1               262080
       3               123240
       5               199481
       7               260127
       9                27705
      11                 5969
      13                 1895
      15                 1266
      17                  684
      19                  198
BTM: wins=882645 losses=26562 draws=3568953
storage: raw=8956320 payload=1587465 file=1709863 bytes overall=19.09% (5.24:1)
wall-clock timings:
  setup/create                      0.020 s
  initialization                    0.149 s
  backpropagation                   0.319 s
  frontier compilation              0.340 s
  consistency repair                0.301 s
  final DTM scan                    0.116 s
  generator total                   1.227 s
  finalize/close                    0.003 s
  compact/reopen                    0.369 s
  resident load                     0.008 s
  final verification                0.008 s
  statistics extraction             0.001 s
  total                             1.637 s
GWDEGTB revision 2.102 completed
```

The `positions-checked` and `positions-skipped` totals count WTM and BTM
separately, hence twice the number of indexed placements.

## Representative DTM positions

`generate_egtb` prints a deterministic example of the longest WTM win, WTM
loss, BTM win and BTM loss, plus one draw. Existing databases can be scanned
without regenerating them:

```sh
./dtm_examples 1 1 1 1
./dtm_examples -d /path/to/dtm 3 0 0 3
```

The four material arguments use the standard white-kings, white-men,
black-kings, black-men order. Output is PDN FEN followed by the exact DTM in
braces:

```text
DTM example positions for 1wX-0wO-1bX-0bO.dtm:
WTM longest win      W:WK41:BK46 {3}
WTM longest loss     W:WK5:BK46 {-2}
BTM longest win      B:WK46:BK41 {3}
BTM longest loss     B:WK5:BK46 {-2}
draw                 W:WK49:BK50 {-1}
```

The reusable C scan is `egtb_find_dtm_examples()`; FEN formatting is provided
by `egtb_format_dtm_fen()` in `dtm_fen.h`.

## Packed WDL databases

A WDL entry uses four bits: two bits for WTM followed by two for BTM. In each
pair, `00` is draw/unknown, `01` is won, and `10` is lost. Sixteen positions fit
in one `uint64_t`. WDL pages are 1,024 bytes and cover 2,048 positions.

`wdl_open()` opens the requested `.wdl`; if it does not exist, it derives the
corresponding `.dtm`, streams disjoint page ranges through parallel DTM readers,
collects WTM and BTM statistics, compresses and checksums the pages, atomically installs the
file, and opens it read-only. Its deliberately simple cache maps page N to
`N % cache_pages` and stores only the page number and uncompressed bytes.

GWD's on-demand WDL generation now uses **Zstd level 12 with a trained
dictionary**, optimized for read-only page decompression. This setting is
independent of `EGTB_COMPRESSION_LEVEL` (DTM generation). Low-level compilation
functions still accept an explicit compression level.

Before compilation, at most 4,096 pages are sampled across the entire position
range, including both sides. Only non-draw pages train the dictionary. The
default dictionary limit is 110 KiB, capped at 1/32 of the sampled bytes.
Files below 256 WDL pages, samples with fewer than 128 non-draw pages, or
unsuitable training samples fall back to dictionary-free compression. Sampling
and training are single-threaded and bounded to about 4 MiB of sample storage;
page compilation remains parallel. Allocation or source-read/checksum errors
fail generation instead of silently disabling the dictionary.

`EGTB_WDL_DICTIONARY_KIB=0` disables training; values 1..256 set the maximum
dictionary size in KiB. This is a generation-time setting only: readers use
the dictionary recorded in the file, regardless of their environment.

Dictionary-backed files use WDL format 2. The 64-byte header stores dictionary
length at byte 56 and its CRC32C at byte 60. The dictionary follows the existing
page directory, before page payloads, and is stored only once. Its checksum is
verified before preparing the shared immutable Zstd dictionary. Each reader
thread/probe retains a private decompression context. Page CRCs, packed values,
implicit draw pages, and the GWD/MPI allocation/attach API are unchanged.

New readers accept both format 1 and format 2, including mixed databases in one
probe. Old libraries cannot read format 2: **relink GWD with the new library**
before using new files. Existing WDLs are reused, never automatically replaced;
explicit recompilation is required to gain dictionary compression. For raw
`WdlImage` page consumers, use `wdl_image_dictionary()` with
`ZSTD_decompress_usingDDict()` rather than decoding without the dictionary.

Missing-file generation defaults to four workers. Set `EGTB_WDL_THREADS=16`
to use sixteen for implicit generation (including compressed-WDL information
and loading calls) and the default resident decompression call. Explicit
`gwdegtb_wdl_decompress_threads(..., n)` uses `n` both to generate a missing
WDL and subsequently decompress it. Existing WDL files are never regenerated
just because the thread count changes. Values must be in 1..256; workers are
capped by the page count. These are process-local threads: only the MPI master
should generate/load, as before.

Each compilation worker owns a two-page DTM sequential view, a Zstd context,
and a bounded batch of up to 1,024 compressed WDL pages (approximately 1 MiB).
It reads both sides together, packs the unchanged four-bit representation, and
writes exact-sized batches using `pwrite`. No whole WDL bitmap is allocated
during compilation. The completed temporary file is flushed and atomically
published; source checksum or I/O failures prevent publication. Pages that
contain only draws remain implicit. Parallel output has identical results and
compression size, but physical batch ordering/file hashes may differ.

Low-level callers can use `wdl_compile_threaded(..., thread_count, ...)` and
`wdl_open_threaded(..., thread_count)`. `wdl_compile()` remains a one-worker
wrapper. The legacy `dtm_cache_pages` compilation argument remains accepted,
but the streaming implementation needs only two pages per worker.

### Resident WDL API for GWD

`gwdegtb.h` provides a process-wide registry of fully decompressed WDL
bitmaps. GWD first passes a basename directly from its configuration to obtain
the dense maximum index and exact allocation size:

```c
uint64_t maximum_index;
size_t bytes;

if (!gwdegtb_wdl_info("1wX-0wO-0bX-1bO",
                      &maximum_index, &bytes)) {
    fprintf(stderr, "%s\n", gwdegtb_last_error());
}
```

The number of positions is `maximum_index + 1`; packed WDL storage requires
`(positions + 1) / 2` bytes. GWD allocates those bytes, then asks GWDEGTB to
decompress directly into the allocation:

```c
void *bitmap = malloc(bytes);

if (!gwdegtb_wdl_decompress(egtb_directory,
                            "1wX-0wO-0bX-1bO",
                            bitmap, bytes)) {
    fprintf(stderr, "%s\n", gwdegtb_last_error());
}

if (!gwdegtb_wdl_attach("1wX-0wO-0bX-1bO", bitmap, bytes)) {
    fprintf(stderr, "%s\n", gwdegtb_last_error());
}
```

The optional `.wdl` suffix is accepted. GWD owns the allocation. A mirrored
basename resolves to its canonical database automatically.

Resident WDL decompression uses four worker threads by default. Compressed
pages are read independently with `pread()`, decompressed with one Zstd context
per worker, checksum-verified, and written directly into disjoint ranges of the
caller-owned bitmap. Applications that want another worker count can call
`gwdegtb_wdl_decompress_threads()` explicitly:

```c
if (!gwdegtb_wdl_decompress_threads(egtb_directory,
                                    "1wX-0wO-0bX-1bO",
                                    bitmap, bytes, 8)) {
    fprintf(stderr, "%s\n", gwdegtb_last_error());
}
```

`gwdegtb_wdl_decompress()` is also the GWD open-or-generate boundary. If the
canonical `.wdl` is absent, the process calling this function compiles it from
the corresponding `.dtm`, atomically installs the compressed WDL file, and
then decompresses it into the supplied allocation. With OpenMPI, only the
master calls this function, so the other ranks never open or generate files;
they attach the synchronized shared bitmap as usual.

For OpenMPI, GWD obtains `bytes`, calls its existing
`my_mpi_allocate_shared()`, and the master calls
`gwdegtb_wdl_decompress()` with the shared pointer. After synchronization,
every rank, including the master, calls `gwdegtb_wdl_attach()` with its
rank-local shared pointer and byte count.
Call `gwdegtb_wdl_unload_all()` on every rank before freeing the MPI windows.

`gwdegtb_wdl_lookup()` accepts GWD's four padded bitboards in WK/WM/BK/BM
order plus side-to-move. It pop-counts the material, uses BMI2 PEXT (or a
portable set-bit fallback) to map GWD fields to compact squares, applies board
rotation/color and side-to-move mirroring when required, and performs a direct
lookup in the packed resident bitmap. It returns `1` for win, `0` for loss,
`-1` for draw, and `-32768` if the material has not been loaded or the input is
invalid. Load all configured WDL databases before starting concurrent lookup;
do not unload them until those lookups have stopped.

GWD versions that already use GWDEGTB's compact square-0..49 bitboards can
avoid the padded-board conversion by calling `gwdegtb_wdl_lookup_compact()`:

```c
int16_t result = gwdegtb_wdl_lookup_compact(
    white_kings, white_men, black_kings, black_men,
    GWDEGTB_WHITE_TO_MOVE);
```

It performs the same validation, material lookup and automatic mirroring as
the padded entry point.

### Compressed-resident WDL API for GWD

Use `gwdegtb_wdl_compressed_info_threads(directory, name, &bytes, n)`
before allocation and
`gwdegtb_wdl_compressed_load_threads(directory, name, memory, bytes, n)`
afterwards to choose the worker count explicitly. Both use `n` to generate a
missing WDL; the load call also reads disjoint byte ranges in parallel into
caller-owned memory using `pread`. No decompression is performed while loading
the compressed image. Counts must be 1..256; small files use at most one worker
per MiB, with single-worker reads performed directly. Buffers may be partially
filled on failure and must not be attached until loading succeeds.

The calls without `_threads` use `EGTB_WDL_THREADS` (default 4) for generation
and loading. Set the explicit count on **both** calls: the size-query call
may perform generation before allocation. Parallel reads may benefit SSD/NVMe
storage but are not guaranteed faster on HDDs. MPI synchronization and per-search
thread probes are unchanged. Resident `gwdegtb_wdl_decompress_threads()` likewise
uses its explicit count for both missing-file generation and decompression.

The compressed tier keeps each complete WDL file image in caller-owned
memory and expands only the pages touched by search. This can reduce WDL RAM
substantially, at the cost of one small mutable cache per search thread.
The compressed images, directory entries and indexers are immutable after
attachment. A GwdegtbWdlProbe is not thread-safe and belongs to exactly one
thread; its cache budget is shared across all compressed WDLs attached in that
process.

For an OpenMPI shared allocation, the master obtains the exact compressed-file
size (and generates the WDL from DTM if it is absent), broadcasts that size,
and loads the file image. Every rank attaches its rank-local pointer only after
the shared window has been synchronized:

~~~c
size_t compressed_bytes = 0;
uint64_t shared_bytes = 0;
unsigned wdl_threads = 16;

if (is_master &&
    !gwdegtb_wdl_compressed_info_threads(egtb_directory, database_name,
                                         &compressed_bytes, wdl_threads))
    abort();
if (is_master && compressed_bytes == 0) {
    /* Neither the configured WDL nor its source DTM exists yet: skip it. */
}
if (is_master)
    shared_bytes = compressed_bytes;
MPI_Bcast(&shared_bytes, 1, MPI_UINT64_T, 0, communicator);
compressed_bytes = (size_t)shared_bytes;

if (compressed_bytes == 0)
    continue;

/* Allocate compressed_bytes in the existing MPI shared-window wrapper. */
if (is_master &&
    !gwdegtb_wdl_compressed_load_threads(egtb_directory, database_name,
                                         shared_image, compressed_bytes, wdl_threads))
    abort();

/* Synchronize the shared window here. */
if (!gwdegtb_wdl_compressed_attach(database_name, shared_image,
                                   compressed_bytes))
    abort();
~~~

Create one probe for the main search thread and one independently in every
worker thread:

~~~c
GwdegtbWdlProbe *probe;
size_t cache_bytes = 16 * 1024 * 1024;

if (!gwdegtb_wdl_probe_create(cache_bytes, &probe))
    abort();

int16_t result = gwdegtb_wdl_lookup_probe(
    probe, white_kings, white_men, black_kings, black_men,
    GWDEGTB_WHITE_TO_MOVE);
~~~

For compact square-0..49 bitboards, use the otherwise identical
`gwdegtb_wdl_lookup_probe_compact()` call.

Material selection and mirroring remain automatic. The result convention is
identical to gwdegtb_wdl_lookup(). Probe statistics expose lookups, hits,
misses and actual page decompressions. They also report the requested cache
budget, actual allocated bytes, and power-of-two entry count; the allocated
size can be noticeably smaller than the requested budget because the
direct-mapped cache requires a power-of-two number of entries. At shutdown,
first destroy every
thread's probe, then call gwdegtb_wdl_compressed_unload_all() on every rank,
and only then release the shared windows. The unload call destroys GWDEGTB's
small image handles and indexers; it never frees caller-owned image bytes.

The performance comparison utility runs identical random positions through
the fully resident and compressed tiers:

~~~sh
make benchmark_wdl_probe
./benchmark_wdl_probe /path/to/wdl 1wX-1wO-1bX-1bO 16 1000000
~~~

The final two arguments are the per-thread cache size in MiB and lookup count.
On a local 1wX-1wO-1bX-1bO test, a warm 16 MiB direct-mapped probe cache
reached a 99.78% hit rate and took about 1.22 times the resident lookup time.
Larger databases and smaller caches can be much more expensive because every
miss performs Zstd decompression and CRC32C verification; benchmark the actual
GWD workload before choosing the cache budget.

An experimental codec utility compares the existing page-wise Zstd-3 format
with a basic Tunstall code over the identical, unmodified 4-bit WDL stream:

~~~sh
make benchmark_tunstall
./benchmark_tunstall /path/to/wdl 3wX-0wO-3bX-0bO 16
./benchmark_tunstall /path/to/wdl 3wX-0wO-3bX-0bO 8
~~~

The final argument selects fixed 8- or 16-bit codes. The experiment builds one
global dictionary per EGTB, encodes every 1,024-byte page independently, checks
sampled round trips, and compares estimated file size, compression time and
hot-buffer page expansion. It deliberately retains every GWDEGTB position and
does not reproduce KingsRow's capture-position exclusions, multiple tuned
catalogs, or direct compressed-run lookup.

The companion three-bit experiment reserves eight codes for the common WDL
outcome pairs, records the rare loss/loss pair in an exception list, and
compares fixed and delta-varint exception positions. It Zstd-compresses the
current four-bit form and both three-bit forms independently for every page,
then constructs a hypothetical hybrid from the smallest page candidate:

~~~sh
make benchmark_wdl3
./benchmark_wdl3 /path/to/wdl 3wX-0wO-3bX-0bO
~~~

The existing 14-byte directory can carry the selected codec in unused high
bits of the compressed-length field, so the estimated hybrid does not require
a larger directory. Sampled pages are expanded and compared byte-for-byte
before decode throughput is measured.

### Disk-cached DTM API for GWD

Exact DTM probing is intended for the single-threaded root search and PV
construction. GWD only calls `gwdegtb_dtm_lookup()` on the root process. The
library derives the canonical filename from the position, lazily opens it on
its first lookup, retains its dictionary, and assigns it a small cache of
checksum-verified uncompressed pages. The complete database remains compressed
on disk.

Query with the DTM directory, a per-database cache budget in bytes, the same
padded WK/WM/BK/BM bitboards as WDL, and side-to-move:

```c
size_t cache_bytes = 4 * 1024 * 1024;
int16_t dtm = gwdegtb_dtm_lookup(egtb_directory, cache_bytes,
                                 white_kings, white_men,
                                 black_kings, black_men,
                                 GWDEGTB_WHITE_TO_MOVE);
```

When GWD already stores square-0..49 compact bitboards, the corresponding
call avoids padded-board conversion:

```c
int16_t dtm = gwdegtb_dtm_lookup_compact(
    egtb_directory, cache_bytes,
    white_kings, white_men, black_kings, black_men,
    GWDEGTB_WHITE_TO_MOVE);
```

Its lookup, mirroring, caching and result semantics are otherwise identical.

Subsequent probes of the same canonical material reuse its open handle and
cache. The budget is rounded down to complete expanded pages, with a minimum
of one page for WTM and one for BTM. A missing canonical file is remembered so
later probes return unavailable without repeatedly accessing the filesystem;
`gwdegtb_dtm_close_all()` clears both open handles and remembered misses.

The result is the exact public ply value: `1, 3, 5, ...` for a win; `0, -2,
-4, ...` for a loss; and `-1` for a draw. `GWDEGTB_DTM_UNAVAILABLE`
(`INT16_MIN`) means that the required database is not open, the input is
invalid, or disk lookup failed. A side to move with no pieces is returned as
lost-in-0 without requiring a database. At orderly shutdown,
`gwdegtb_dtm_close_all()` closes every DTM handle and releases its indexer,
dictionary, and page cache.

## Storage and move-generation benchmarks

The storage benchmark writes independent randomized WTM/BTM values at 10%,
50%, and 90% non-draw density, compacts the files, and reports write, flush,
sequential-read, random-read, and compression results:

```sh
make benchmark-egtb
./benchmark_egtb --positions 4194304 --lookups 4194304 \
  --page-size 4096 --cache-mib 16
```

Random DTM values deliberately form a pessimistic compression workload
compared with the locally correlated values of generated databases.

The move-generation benchmark measures capture detection, complete legal move
generation, generation plus do/undo, and quiet inverse generation for both the
compact table and padded backends:

```sh
make benchmark-movegen
./benchmark_movegen --samples 1000000
```

## Source layout

| Files | Purpose |
|---|---|
| `endgame_index.c/.h` | Production dense position index and inverse |
| `combinatorial_index.c/.h` | Experimental alternative indexer |
| `movegen.c/.h` | International-rules move generation and do/undo |
| `bitmap.c/.h` | Persistent and frontier bitmap primitives |
| `frontier.c/.h` | Compressed per-worker exact-DTM streams |
| `egtb.c/.h` | Versioned compressed DTM storage and caches |
| `wdl.c/.h` | Packed compressed WDL compilation and lookup |
| `gwdegtb.c/.h` | GWD padded-board resident WDL registry and lookup |
| `material.c/.h` | Canonical material ordering and mirroring |
| `generator.c/.h` | Initialization, retrograde analysis, repair, verification |
| `sliced.c/.h` | Resumable man-row sliced generation and full-index merge |
| `generate_egtb.c` | Command-line orchestration, dependencies, reporting |
| `libgwdegtb.a` | Static library target for integration with GWD |
| `test_*.c`, `check_stats.c` | Regression and reference-count validation |
| `benchmark_*.c` | Index, cache/storage, and move-generation benchmarks |
