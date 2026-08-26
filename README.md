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
