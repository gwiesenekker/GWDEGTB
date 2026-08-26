CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
BENCH_CFLAGS ?= -O3 -DNDEBUG -march=native -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all test check-stats benchmark clean

all: test_index check_stats benchmark_index

test_index: test_index.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

check_stats: check_stats.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

benchmark_index: benchmark_index.c endgame_index.c endgame_index.h
	$(CC) $(BENCH_CFLAGS) -o $@ benchmark_index.c endgame_index.c

test: test_index
	./test_index
	./test_index 0 0 1 1
	./test_index 1 1 1 0

check-stats: check_stats
	./check_stats 7piece-stats.txt

benchmark: benchmark_index
	./benchmark_index

clean:
	$(RM) test_index check_stats benchmark_index test_index.o check_stats.o \
		endgame_index.o
