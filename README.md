# International draughts endgame index

This C library gives a dense index to positions on the 50 playable squares.
It stores positions as four `uint64_t` bitboards (only bits 0 through 49 are
used). White men are restricted to squares 5..49 and black men to 0..44.

The index order scans squares 0..49. At each square the order is empty, white
man, black man, white king, black king. A dynamic-programming table counts the
valid suffixes, so illegal man placements create neither holes nor duplicate
indices.

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

`egtb.c` stores two signed 16-bit DTM values per position in independently
Zstd-compressed pages. The versioned header records the page size and maximum
index. Databases keep their 10-byte-per-page directory in RAM and use a
configurable direct-mapped page cache with dirty write-back and uncompressed
page checksums. Every stored block also carries a CRC32C of its uncompressed
page, which is verified after decompression. Read-only backings are shared by
path; additional views share their immutable directory while owning private
page caches and ZSTD state. View misses use `pread()` and do not share a seek
position.

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

Captured pieces remain occupied during recursive generation and are tracked in
a separate captured mask. This both prevents capturing a piece twice and makes
an already captured piece continue to block a king ray until the move ends.
Capture paths with identical final effects are intentionally not deduplicated.

`draughts_do_move()` can save the original four bitboards in a
`DraughtsUndo`; `draughts_undo_move()` restores that snapshot. Quiet inverse
generation reverses regular man and king moves, rejecting every predecessor
in which the previous mover had a compulsory capture. Neither direction may
promote, so every predecessor retains the EGTB's exact material signature.

The regression suite includes focused tests for forced maximum capture,
backward man captures, flying-king landing choices, delayed captured-piece
removal, forward promotion, inverse material preservation, inverse capture filtering, and
do/undo. It also checks 100,000 random seven-piece positions for both sides,
plus inverse predecessors for the first 10,000:

```sh
make test
```

Benchmark capture detection, full legal generation, generation plus do/undo,
and inverse quiet predecessors with:

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
than page-cache lookups. At 256 EGTB entries per 1024-byte page, each page maps
to exactly four bitmap words, which provides the page/word boundary needed by
the planned threaded slices.

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
saved. It then runs `egtb_make_consistent()`, which recomputes both side-to-move
values of every position from all legal successors and repeats until a complete
pass makes no corrections. Corrections report the index, side, four bitboards,
and old/new DTM. Transitions to another material signature can be resolved by
an external EGTB probe callback.

The generic generator uses the same argument order as GWD:

```sh
./generate_egtb NWHITE_KINGS NWHITE_MEN NBLACK_KINGS NBLACK_MEN
./generate_egtb -j THREADS NWHITE_KINGS NWHITE_MEN NBLACK_KINGS NBLACK_MEN
```

`-j` enables page-partitioned POSIX-thread backtracking. The accepted range is
1 through 256; one thread retains the original implementation. Initialization
and the final self-consistency sweep are currently serial, while the repeated
bitmap/frontier retrograde passes run concurrently.

Filenames use the same order, for example `1wX-0wO-0bX-1bO.dtm` for WK-BM.
The computed 4D material catalog generates only the GWD-canonical orientation:
the side with more pieces is White, or, when piece counts are equal, the side
with more kings is White. A request for the other orientation is redirected to
the canonical database. Mirrored lookups rotate squares with `s -> 49-s`, swap
colors and swap WTM/BTM.

Within each piece count, generation order places the larger material side on
White and enumerates White kings descending, then Black kings descending. This
matches the GWD job order and ensures promotions target earlier databases.

With the generator's 1024-byte page size, the writable database uses a 256 MiB
uncompressed page cache. Every previously generated database opened for
read-only dependency lookups uses a 16 MiB cache. These capacities exclude the
page dictionaries and cache metadata.

During threaded backtracking the 256 MiB writable-cache budget is divided
across the workers. The original shared cache is temporarily reduced to one
page, so the threaded views do not add another 256 MiB to it, and is restored
before the consistency sweep. Each worker owns a contiguous range of complete
EGTB pages and the corresponding whole bitmap words. It has a private writable
view, ZSTD contexts, statistics, error state, and external-DTM catalog/cache.
Consequently the 16 MiB dependency-cache setting is per worker and per opened
dependency.

Workers compress and update pages that still fit their owned reserved blocks
with positional `pwrite()` calls, without locking one another. Fixed-offset
reads likewise use `pread()`, so cache eviction/reload never depends on a
shared or buffered stream position. A single mutex in the shared writable
backing protects end-of-file allocation/appending and directory/header writes.
In particular, choosing the end offset and appending a block that outgrows its
old reservation is one atomic operation.
Frontier construction completes before predecessor processing starts, making
the shared exact/cumulative bitmaps immutable during proof generation.

The `1x1.sh`, `2x1.sh`, `2x2.sh`, `3x1.sh`, and `3x2.sh` jobs accept
`EGTB_THREADS`; for example, `EGTB_THREADS=16 ./3x2.sh`. They default to one
thread. Each script removes the target DTM immediately before regenerating it
and writes separate logs below `logs/<job-name>/`.

DTM page caches are direct-mapped by page number. An `Egtb` backing owns the
file header and one in-memory immutable page dictionary; additional
`EgtbView` objects share that backing while keeping their decompressed pages,
ZSTD contexts, compressed buffers, and cache statistics private. Read-only
view misses use `pread()` and therefore do not share a seek position. Writable
views use the same direct mapping and flush a dirty slot before replacing it.

Only after the final zero-update pass does the generator flush the working
pages, close the working file, compact the live blocks into a temporary file,
and atomically install the compacted database. It reopens the finished file
read-only and prints WTM and BTM counts for every stored DTM value together
with final compression statistics.
