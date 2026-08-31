# International draughts endgame database generator

This C library can calculate Distance-To-Mate (DTM) EGTBs for International Polish Draughts.

GWDEGTB generates exact two-through-seven-piece endgame databases on the 50
playable squares of a 10x10 board. It includes a dense reversible position
index, international-rules move generation, multithreaded retrograde analysis,
compressed DTM and WDL storage, consistency repair, final verification, and
regression and performance tests.

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
- A repair pass for exact-DTM transpositions and otherwise unreachable setup
  positions, followed by fatal read-only verification.
- Tested position counts for all 120 seven-piece material distributions.

## Requirements and build

The code targets a POSIX system and requires a C11 compiler, POSIX threads, and
the Zstandard development library. On Debian or Ubuntu the required package is
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

The manually maintained `REVISION` file supplies the revision printed at
startup. It is independent of Git tags and commit identifiers:

```sh
./generate_egtb --revision
```

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
databases are repeated to exceed 100 million operations; the six- and
seven-piece cases sample 110 million indices in 1,024 evenly spaced blocks.
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
cases and 100,000 random seven-piece positions for both sides:

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
WDL files and the GWD WDL API are unchanged. This change widens DTM values;
the indexer/material limit remains seven pieces.

Every v4 block carries CRC32C of the expanded 16-bit codes serialized in
little-endian order; legacy blocks retain their original byte CRC. An implicit
directory entry `(offset=0,length=0)` represents an all-draw page and consumes
no block storage.

The file begins with a versioned header containing the page size and maximum
index, followed by an in-memory directory with a 64-bit block offset and
16-bit compressed length per page. A newly written block reserves 20% of the
uncompressed page size by default. A dirty page that outgrows its slot is
appended and its directory entry is updated. `egtb_compact()` rewrites only
live blocks, removes reserved slack and abandoned blocks, and atomically
installs the compacted file.

DTM caches are direct-mapped by page number. Read-only handles share the file
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

The accepted thread count is 1..256. The target file must not already exist.
Captures and promotions enter smaller or earlier material databases, so all
dependencies must be present in the working directory. The supplied family
scripts generate material in GWD order and remove each target immediately
before regenerating it:

```sh
EGTB_THREADS=16 ./1x1.sh
EGTB_THREADS=16 ./2x1.sh
EGTB_THREADS=16 ./2x2.sh
EGTB_THREADS=16 ./3x1.sh
# Continue with 3x2.sh, 3x3.sh, 4x1.sh, ... through the seven-piece jobs.
```

Logs are written below `logs/<family>/`. Generation order sorts by total piece
count, larger White side first, then White kings descending and Black kings
descending. This matches the historical GWD order and ensures promotion
targets are available.

### Man-row sliced generation

`--sliced` reduces the peak working set for material containing men while
producing the same ordinary full-index `.dtm` file:

```sh
./generate_egtb --sliced -j 16 0 4 0 3
```

The most-forward man identifies a slice. One colour with men gives nine
slices; men of both colours give 81. White rows are generated from 2 through
10 and, within each White row, Black rows from 9 through 1. A forward man move
therefore either remains in the current slice or enters a completed read-only
slice. Captures and promotions continue to use normal material dependencies.
Each slice retains the full multithreaded frontier, consistency-repair, and
read-only verification pipeline.

Slice indices are independently dense. Their position counts sum exactly to
the normal full-index count. After every slice is verified, a 9- or 81-way
monotonic merge reranks its positions with the unchanged full index and writes
the standard DTM. Missing, duplicate, or out-of-order full indices are fatal.
The completed full database then undergoes the normal compaction and exhaustive
read-only consistency verification, so GWD and WDL compilation require no
special handling.

Temporary state is stored under `<database>.work`. A completed slice is first
verified under an `.incomplete` name, atomically renamed, and recorded in an
atomically replaced manifest. Restarting the same command validates every
completed slice header and page checksum, restores its generation statistics,
and resumes at the first missing slice. The workspace is deleted only after
the final full database passes verification. Set `EGTB_KEEP_SLICES=1` to retain
it deliberately, for example when testing restart behavior.

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

3. **Initialize exact known values.** Workers enumerate their slices for both
   WTM and BTM. No-move positions become lost-in-zero; immediate mates become
   won-in-one. Captures and promotions query previously generated databases,
   so initialization can also discover DTM values larger than one. Every known
   result is appended to the worker's exact-DTM frontier stream.

4. **Maintain persistent outcomes.** Shared WTM/BTM won and lost bitmaps hold
   accumulated outcomes. The compressed streams identify the exact current
   frontier, while a shared atomic candidate bitmap routes its predecessors to
   their owning workers. These structures replace random writes to a working
   DTM during propagation.

5. **Propagate won to lost.** For a won-in-N frontier, legal quiet inverse moves
   generate candidate opponent predecessors. A predecessor is lost in N+1 only
   when every legal forward move is a known win no longer than N and at least
   one is exactly N. This implements the losing side's longest defense.

6. **Propagate lost to won.** For a lost-in-N frontier, every legal inverse move
   directly proves a candidate win in N+1: the inverse generator already
   supplies the legal forward move reaching the loss. Forward moves are not
   regenerated for this existential step. An existing shorter win is retained.

7. **Store frontier streams.** Each worker writes 512-index blocks to one
   checksummed Zstd-compressed append-only temporary file. Files are created as
   `.gwdegtb-frontier-*` in the working directory and immediately unlinked, so
   the filesystem reclaims them automatically when their descriptors close or
   the process exits. No global stream lock or collection of per-distance files
   is required.

8. **Compile the final DTM.** Each worker assembles a page-aligned batch of its
   index range in a bounded, uncompressed paired-entry buffer initialized to
   draws. It replays its existing compressed frontier streams from longest to
   shortest distance, retaining shorter replacements for stale transpositions.
   Completed WTM and BTM pages are compressed and written once, in index order
   within each worker, without reading old pages. All-draw pages need no payload.
   If a worker's range exceeds its buffer, it rereads its streams for each batch.
   No extra partition files are created. This avoids random dirty-page evictions
   and abandoned blocks during compilation; the normal reserved block slack
   remains available for subsequent consistency repairs and is removed by final
   compaction. Frontier files and the output DTM coexist until compilation ends.

9. **Repair exact-DTM consistency.** The first snapshot pass recomputes both
   side-to-move values for every legal position. Workers use a paired
   sequential cursor over corresponding WTM and BTM pages for the scan and a
   separate random-access successor view.
   Corrections are merged and applied by one thread. Corrected positions and
   their quiet predecessors/successors form the next sparse worklist; passes
   continue until no correction remains. This handles initialization
   transpositions and setup positions that have no legal predecessor in the
   current material database.

10. **Recompute the maximum DTM.** After repair, one final linear DTM scan
    determines the actual maximum distance, including any values changed by
    consistency repair.

11. **Record verified positions.** The initial consistency pass also builds a
    bitmap of positions already proved correct. Any correction and its quiet
    predecessor closure are cleared from that bitmap. Final verification can
    safely skip positions that remained verified.

12. **Finalize and compact.** Dirty pages are flushed, the writable database is
    closed, and live compressed blocks are rewritten without holes. The compact
    file atomically replaces the working file and is reopened read-only.

13. **Verify the immutable result.** If the uncompressed database fits the
    resident-memory limit, workers decompress disjoint pages into a shared flat
    array using private Zstd contexts, `pread()`, and CRC32C validation. They
    simultaneously build private DTM histograms, which are merged without a
    separate statistics scan. Larger databases use a large read-only cache.
    The final parallel consistency check is read-only and any mismatch is
    fatal.

## Memory, caches, and configuration

The default 2,048-byte v4 DTM page gives 1,024 positions for one side and exactly
sixteen 64-bit bitmap words. Current production defaults are:

| Purpose | Default |
|---|---:|
| DTM page size (`EGTB_PAGE_SIZE`) | 2,048 bytes |
| Target writable cache | 1 GiB total |
| Frontier compilation assembly buffer | 1 GiB total, divided among workers |
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

The dependency-cache figure is potentially multiplied by both the worker
count and the number of dependency databases actually opened. Cache metadata
and page directories are additional. Configure final handling with:

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

Setting `EGTB_RESIDENT_LIMIT_GIB=0` disables the resident path. Resident
loading is parallel. It is normally fastest when the complete four-byte-per-
position database fits comfortably in physical RAM. The generator reports
cache lookups, hits, misses, decompressions, dirty evictions, compressed
writes, storage ratios, and wall-clock time for each major phase.

## Example: 1 king + 1 man against 1 king + 1 man

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

## Packed WDL databases

A WDL entry uses four bits: two bits for WTM followed by two for BTM. In each
pair, `00` is draw/unknown, `01` is won, and `10` is lost. Sixteen positions fit
in one `uint64_t`. WDL pages are 1,024 bytes and cover 2,048 positions.

`wdl_open()` opens the requested `.wdl`; if it does not exist, it derives the
corresponding `.dtm`, compiles the complete WDL bitmap in memory, collects WTM
and BTM statistics, compresses and checksums the pages, atomically installs the
file, and opens it read-only. Its deliberately simple cache maps page N to
`N % cache_pages` and stores only the page number and uncompressed bytes.

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
