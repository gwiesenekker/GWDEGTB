CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
BENCH_CFLAGS ?= -O3 -DNDEBUG -march=native -std=c11 -Wall -Wextra -Wpedantic
LDLIBS ?= -lzstd

.PHONY: all test check-stats benchmark benchmark-egtb benchmark-movegen \
	generate-wk-bk clean

all: test_index test_egtb test_movegen test_generator check_stats benchmark_index benchmark_egtb \
	benchmark_movegen

generate_wk_bk: generate_wk_bk.o generator.o movegen.o endgame_index.o egtb.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_index: test_index.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

check_stats: check_stats.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

test_egtb: test_egtb.o egtb.o wdl.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_movegen: test_movegen.o movegen.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

test_generator: test_generator.o generator.o movegen.o endgame_index.o egtb.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

benchmark_index: benchmark_index.c endgame_index.c endgame_index.h
	$(CC) $(BENCH_CFLAGS) -o $@ benchmark_index.c endgame_index.c

benchmark_egtb: benchmark_egtb.c egtb.c egtb.h
	$(CC) $(BENCH_CFLAGS) -o $@ benchmark_egtb.c egtb.c $(LDLIBS)

benchmark_movegen: benchmark_movegen.c movegen.c movegen.h endgame_index.c \
		endgame_index.h
	$(CC) $(BENCH_CFLAGS) -o $@ benchmark_movegen.c movegen.c endgame_index.c

test: test_index test_egtb test_movegen test_generator
	./test_index
	./test_index 0 0 1 1
	./test_index 1 1 1 0
	./test_egtb
	./test_movegen
	./test_generator

check-stats: check_stats
	./check_stats 7piece-stats.txt

benchmark: benchmark_index
	./benchmark_index

benchmark-egtb: benchmark_egtb
	./benchmark_egtb

benchmark-movegen: benchmark_movegen
	./benchmark_movegen

generate-wk-bk: generate_wk_bk
	./generate_wk_bk

clean:
	$(RM) test_index test_egtb test_movegen test_generator check_stats benchmark_index \
		benchmark_egtb benchmark_movegen test_index.o test_egtb.o \
		test_movegen.o test_generator.o check_stats.o endgame_index.o egtb.o wdl.o movegen.o \
		generator.o generate_wk_bk generate_wk_bk.o
