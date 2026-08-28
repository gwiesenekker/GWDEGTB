# International draughts endgame index

This C library gives a dense index to positions on the 50 playable squares.
It stores positions as four `uint64_t` bitboards (only bits 0 through 49 are
used). White men are restricted to squares 5..49 and black men to 0..44.

The index order scans squares 0..49. At each square the order is empty, white
man, black man, white king, black king. A dynamic-programming table counts the
valid suffixes, so illegal man placements create neither holes nor duplicate
indices. Each fixed-material indexer also precomputes the complete rank addition
for every state and actual piece type. Ranking therefore needs one transition
lookup per occupied square while preserving the original index order.

Build and run the exhaustive round-trip test:

```sh
make test
./test_index 1 1 1 1
```

The four arguments are the numbers of white men, black men, white kings and
black kings. The program prints the legal-position count and maximum index,
then independently enumerates every legal placement and verifies
`position -> index -> position`. Large material configurations may require a
very long exhaustive test. Counts that do not fit in `uint64_t` are rejected.

To compare count-only results against the uploaded seven-piece reference data
(without attempting to enumerate billions of positions), run:

```sh
make check-stats
```

Benchmark the largest database for every size from two through seven pieces:

```sh
make benchmark
```

By default, small databases are repeated for stable timing, databases through
five pieces are traversed completely, and six- and seven-piece databases are
sampled in evenly spaced blocks across their full index ranges. Use
`./benchmark_index --full` for exhaustive traversal of all six databases (this
takes a long time), or `./benchmark_index --samples COUNT` to change the sample
limit.

The benchmark uses `-O3 -DNDEBUG -march=native`. Defining `NDEBUG` removes
argument, bitboard, material-count, and index-range checks from the two hot
conversion functions; callers must then satisfy their documented preconditions.

## Compressed EGTB storage

`egtb.c` stores two signed bytes per position in independently Zstd-compressed
pages while exposing exact `int16_t` ply values through its API. Positive odd
wins `1,3,...,253` are stored as `1,2,...,127`; nonpositive even losses
`0,-2,...,-254` are stored as `0,-1,...,-127`; stored `-128` represents
draw/unknown and is returned as `EGTB_DRAW` (`-1`). The version-2 header records
the page size and maximum index. Version-1 four-byte DTM databases are rejected
and must be regenerated. Databases keep their 10-byte-per-page directory in RAM and use a
configurable direct-mapped page cache with dirty write-back and uncompressed
page checksums. Every stored block also carries a CRC32C of its uncompressed
page, which is verified after decompression. Read-only backings are shared by
path; additional views share their immutable directory while owning private
page caches and ZSTD state. View misses use `pread()` and do not share a seek
position. Fixed-offset reads and writes use `pread()` and `pwrite()`.

An offset and length of zero represents an implicit all-draw page. Growing
compressed pages are appended; `egtb_compact()` rewrites all live pages without
holes and atomically replaces the original file.

The default reserved payload capacity is 20% of the uncompressed page size and
is configurable at creation. `egtb_storage_statistics()` reports logical raw
bytes, live Zstd payload bytes, live block bytes including CRC32C headers, total
file bytes, and live-page count so callers can print payload and overall
compression ratios before and after compaction.

Run the deterministic randomized storage regression with `make test`. It
updates 10% as many entries as the largest four-piece database contains, checks
every immediate cached read, scans all entries, compacts, reopens read-only,
and verifies identical WTM/BTM win, loss, and draw statistics.

Benchmark compressed storage at 10%, 50%, and 90% non-draw value density:

```sh
make benchmark-egtb
```

The benchmark fills WTM and BTM values independently, compacts each database,
then reports write/flush throughput, sequential and uniformly random read-only
throughput, compaction time, and final compression ratios. It uses 4,194,304
positions, 4,194,304 random lookups, 1,024-byte pages, and a 1 MiB read cache by
default. Use `--positions COUNT`, `--lookups COUNT`, `--page-size BYTES`, and
`--cache-mib COUNT` to change them. Random DTM values intentionally give a
pessimistic compression workload compared with the locally correlated values
expected in generated databases.

## Packed WDL databases

Material database filenames use GWD order:
`<wk>wX-<wm>wO-<bk>bX-<bm>bO.dtm`; for example,
`3wX-1wO-1bX-2bO.dtm`. The corresponding packed database uses the `.wdl`
extension. `egtb_material_filename()` constructs either name.

Each WDL position is a four-bit nibble: two bits for WTM followed by two bits
for BTM. In each pair, `00` is draw/unknown, `01` is won, and `10` is lost.
Sixteen positions are packed into each `uint64_t` while compiling. WDL files
use fixed 1,024-byte uncompressed pages, so one page covers 2,048 positions.

`wdl_open()` first opens the requested `.wdl`. If it does not exist, the
function derives the `.dtm` name, allocates and fills the packed bitmap,
collects WTM and BTM statistics, compresses each page with Zstd, installs the
file atomically, logs its compression ratio and statistics, and opens it.
All-draw pages are implicit. Each 14-byte in-memory directory entry contains
the compressed block offset, 16-bit length, and CRC32C of the uncompressed
page.

The read-only WDL cache is deliberately direct-mapped. A page uses cache slot
`page_number % cache_pages`; each slot contains only its page number and the
1,024-byte uncompressed page.

## Move generation

`movegen.c` generates legal international draughts moves directly from the
four bitboards. Men move forward but capture in all four directions; kings
scan across empty diagonal squares and consider every empty landing square
beyond a victim. Captures are compulsory and only globally maximum-length
capture paths are emitted.

The production padded backend maps compact squares 0..49 to GWD fields
6..15, 17..26, 28..37, 39..48, and 50..59. Diagonal steps then become fixed
unsigned shifts by 5 or 6; the unused fields prevent row wrapping. Builds with
BMI2 use PDEP/PEXT to convert whole bitboards at the API boundary and portable
builds use a set-bit fallback. The public position and move representations
remain compact. Precomputed diagonal ray and between-square masks find the
nearest king blocker and all legal landing squares without walking each empty
square. The padded entry points have a `_padded` suffix and are used by the
normal `generate_egtb` executable. `generate_egtb_table` retains the compact
table backend for comparison.

Captured pieces remain occupied during recursive generation and are tracked in
a separate captured mask. Capture recursion keeps only the longest paths found
so far, so it does not repeat the entire capture search merely to emit moves.
This both prevents capturing a piece twice and makes
an already captured piece continue to block a king ray until the move ends.
Capture paths with identical final effects are intentionally not deduplicated.

`draughts_do_move()` can save the original four bitboards in a
`DraughtsUndo`; `draughts_undo_move()` restores that snapshot. Quiet inverse
generation reverses regular man and king moves, rejecting every predecessor
in which the previous mover had a compulsory capture. Neither direction may
promote, so every predecessor retains the EGTB's exact material signature.

The regression suite compares the table and padded backends and includes
focused tests for forced maximum capture,
backward man captures, flying-king landing choices, delayed captured-piece
removal, forward promotion, inverse material preservation, inverse capture filtering, and
do/undo. It also checks 100,000 random seven-piece positions for both sides,
plus inverse predecessors for the first 10,000:

```sh
make test
```

Benchmark both backends for capture detection, full legal generation,
generation plus do/undo, and inverse quiet predecessors with:

```sh
make benchmark-movegen
```

The default benchmark samples one million positions from the largest
seven-piece material configuration. Use
`./benchmark_movegen --samples COUNT` to change the sample count.

## Initial EGTB generation

`egtb_initialize_terminal_positions()` initializes both the white-to-move and
black-to-move values of a newly created all-draw database. A position without a
legal move is stored as lost in zero plies (`0`). If any legal move reaches a
position in which the opponent has no legal move, it is stored as won in one
ply (`1`). All other positions remain draw/unknown (`-1`) for subsequent
retrograde passes. This also handles the special case where the side to move
has no pieces.

The initialization routine does not flush, close, compact, or report final
storage statistics. Its open read/write handle is passed directly to the later
retrograde passes. The database is finalized only after those passes converge.

`egtb_backtrack_wins_to_losses()` performs the universal reverse step for any
positive odd distance `N`. For every WTM won-in-`N` position it generates legal
black quiet predecessors, then replays every legal Black move from each
predecessor. The predecessor's BTM value becomes lost in `N+1` only if every
successor is a known White win no longer than `N`, with at least one successor
exactly won in `N`. This maximum is necessary because the losing side chooses
the longest defense. BTM sources are handled symmetrically. Unknown entries
and longer existing losses are updated; shorter losses are preserved.
`egtb_backtrack_won_in_one()` remains as a convenience wrapper using `N=1`.
Neither function flushes the database.

The win-to-loss pass builds two packed, reusable `uint64_t` bitmaps for one
successor side at a time: positions won in exactly `N`, and positions won in
at most `N`. It iterates only the set bits in the exact frontier. Forward
successors in the current EGTB are proved through the immutable bitmaps rather
than page-cache lookups. At 512 EGTB entries per 1024-byte page, each page maps
to exactly eight bitmap words, which provides the page/word boundary used by
the threaded slices.

`egtb_backtrack_losses_to_wins()` performs the existential reverse step for any
positive even distance `N`. It generates legal predecessors of lost-in-`N`
positions. The inverse generator supplies the corresponding legal forward
move, which itself proves that the predecessor can reach the lost-in-`N`
position; forward moves are therefore not regenerated. Unknown entries and
longer positive wins are updated to won in `N+1`; shorter wins are retained.
`egtb_backtrack_lost_in_two()` is the convenience wrapper for `N=2`. Neither
function flushes the database.

The loss-to-win pass needs only one frontier bitmap: positions lost in exactly
`N`. Iterating that bitmap and generating legal inverse moves provides the
existential proof directly. The current EGTB value is still read before writing
to preserve an already known shorter win; that eligibility state does not
require a second bitmap.

`egtb_generate()` starts with terminal initialization and alternates the two
parameterized passes for distances 1, 2, 3, 4, and so on. It stops when a pass
creates no new or shortened entries. No intermediate pass is explicitly
saved. It then runs `egtb_make_consistent()`, whose first pass recomputes both
side-to-move values of every position from all legal successors. Corrections
seed a deduplicated worklist with their quiet, non-promoting predecessors and
successors. Later passes check only that affected worklist and continue until
a pass makes no corrections. Corrections report the index, side, four
bitboards, and old/new DTM. Transitions to another material signature can be
resolved by an external EGTB probe callback.

The generic generator uses the same argument order as GWD:

```sh
./generate_egtb NWHITE_KINGS NWHITE_MEN NBLACK_KINGS NBLACK_MEN
./generate_egtb -j THREADS NWHITE_KINGS NWHITE_MEN NBLACK_KINGS NBLACK_MEN
./generate_egtb --revision
```

A plain `make generate_egtb` is the native production build and uses
`-O3 -DNDEBUG -march=native`. Override `CFLAGS` explicitly for a portable or
debug build. Because `-march=native` specializes the executable for the build
machine, rebuild after copying the source to a machine with a different CPU.

The manually maintained `REVISION` file supplies the revision printed at
startup. Change its value explicitly in increments of 0.001; Make treats the
file as an input and rebuilds the revision object without modifying it. For
example, development builds following version 1.9 can be numbered `1.901`,
`1.902`, and so on. This revision is intentionally independent of commit
identifiers.

`-j` enables page-partitioned POSIX-thread backtracking. The accepted range is
1 through 256. Initialization, bitmap/frontier retrograde propagation,
frontier compilation, snapshot-based consistency repair, and the final
read-only consistency verification are divided across the requested threads.
After compaction, any mismatch found by final verification is fatal.

Filenames use the same order, for example `1wX-0wO-0bX-1bO.dtm` for WK-BM.
The computed 4D material catalog generates only the GWD-canonical orientation:
the side with more pieces is White, or, when piece counts are equal, the side
with more kings is White. A request for the other orientation is redirected to
the canonical database. Mirrored lookups rotate squares with `s -> 49-s`, swap
colors and swap WTM/BTM.

Within each piece count, generation order places the larger material side on
White and enumerates White kings descending, then Black kings descending. This
matches the GWD job order and ensures promotions target earlier databases.

With the generator's 1024-byte page size, the writable database uses a 1 GiB
uncompressed page cache. Every previously generated database opened for
read-only dependency lookups uses a 64 MiB cache per worker. These capacities
exclude the page dictionaries and cache metadata. The final read-only consistency check
uses a separate 1 GiB cache budget divided across its workers, so increasing
verification locality does not multiply the dependency-cache allocation.

During threaded backtracking each worker owns a contiguous range of complete
EGTB pages and the corresponding whole bitmap words. Exact DTM layers are
stored as temporary, append-only streams of 64-bit indices. The streams use
checksummed ZSTD blocks and one unlinked temporary backing file per worker, so
they require neither a global stream lock nor thousands of open files.

Initialization uses the same page-aligned partition. Every worker evaluates
both side-to-move values in its slice with a private external-DTM catalog and
appends every non-draw result to the appropriate private stream. This includes
terminal loss-in-zero, win-in-one, and higher DTMs introduced by captures or
promotions into previously generated databases.

Persistent WTM/BTM won and lost bitmaps replace the writable working EGTB.
Each exact source position is processed by its owner exactly once. Generated
predecessors are routed through a shared candidate bitmap using atomic 64-bit
OR operations. After a barrier, workers inspect only the candidate bits in
their page-aligned slices and append proven results to their private next-DTM
streams. Previously generated dependency EGTBs still use a private read-only
catalog and cache per worker.

After retrograde convergence, workers compile their DTM streams into disjoint
page ranges of the final all-draw database. Streams are applied from the
largest distance down to zero, so a later shorter result supersedes a stale
longer record. Only this compilation phase uses the 256 MiB writable-cache
budget, divided across the workers. The shared cache is temporarily reduced to
one page and restored before the sparse consistency repair.

The family job scripts from `1x1.sh` through the seven-piece `4x3.sh`,
`5x2.sh`, and `6x1.sh` jobs accept `EGTB_THREADS`; for example,
`EGTB_THREADS=16 ./3x2.sh`. They default to one thread. Each script removes the
target DTM immediately before regenerating it and writes separate logs below
`logs/<job-name>/`.

DTM page caches are direct-mapped by page number. An `Egtb` backing owns the
file header and one in-memory immutable page dictionary; additional
`EgtbView` objects share that backing while keeping their decompressed pages,
ZSTD contexts, compressed buffers, and cache statistics private. Read-only
view misses use `pread()` and therefore do not share a seek position. Writable
views use the same direct mapping and flush a dirty slot before replacing it.

Consistency repair uses snapshot iterations. Every worker opens a private
read-only view of the compiled database and writes mismatches from its owned
range to an in-memory correction stream. After the views close, one thread
applies the range-ordered streams through the writable backing and constructs
the affected-position bitmap. Later parallel iterations inspect only affected
successors and predecessors. The generator then flushes the repaired pages,
compacts the live blocks into a temporary file, and atomically installs the
compacted database. It reopens the finished file read-only, runs the parallel
fatal consistency verification, and prints WTM and BTM counts for every stored
DTM value together with final compression statistics. The summary also prints
monotonic wall-clock timings for setup, initialization, backpropagation,
frontier compilation, consistency repair, the final DTM scan, finalization,
compaction/reopen, final verification, the statistics scan, and total runtime.
It also reports aggregate lookups, hits, misses, hit rate, decompressions,
dirty evictions, and compressed writes for the current-database views and the
external dependency caches. Dependency statistics cover the complete generator
phase and, separately, the freshly reopened final-verification catalogs.
