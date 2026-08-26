CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
BENCH_CFLAGS ?= -O3 -DNDEBUG -march=native -std=c11 -Wall -Wextra -Wpedantic
LDLIBS ?= -lzstd

.PHONY: all test check-stats benchmark benchmark-egtb benchmark-movegen clean

all: test_index test_egtb test_movegen test_generator test_material test_bitmap generate_egtb check_stats benchmark_index benchmark_egtb \
	benchmark_movegen

generate_egtb: generate_egtb.o generator.o bitmap.o material.o movegen.o endgame_index.o egtb.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_index: test_index.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

check_stats: check_stats.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

test_egtb: test_egtb.o egtb.o wdl.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_movegen: test_movegen.o movegen.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

test_generator: test_generator.o generator.o bitmap.o material.o movegen.o endgame_index.o egtb.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_material: test_material.o material.o
	$(CC) $(CFLAGS) -o $@ $^

test_bitmap: test_bitmap.o bitmap.o
	$(CC) $(CFLAGS) -o $@ $^

benchmark_index: benchmark_index.c endgame_index.c endgame_index.h
	$(CC) $(BENCH_CFLAGS) -o $@ benchmark_index.c endgame_index.c

benchmark_egtb: benchmark_egtb.c egtb.c egtb.h
	$(CC) $(BENCH_CFLAGS) -o $@ benchmark_egtb.c egtb.c $(LDLIBS)

benchmark_movegen: benchmark_movegen.c movegen.c movegen.h endgame_index.c \
		endgame_index.h
	$(CC) $(BENCH_CFLAGS) -o $@ benchmark_movegen.c movegen.c endgame_index.c

test: test_index test_egtb test_movegen test_generator test_material test_bitmap
	./test_index
	./test_index 0 0 1 1
	./test_index 1 1 1 0
	./test_egtb
	./test_movegen
	./test_generator
	./test_material
	./test_bitmap

check-stats: check_stats
	./check_stats 7piece-stats.txt

benchmark: benchmark_index
	./benchmark_index

benchmark-egtb: benchmark_egtb
	./benchmark_egtb

benchmark-movegen: benchmark_movegen
	./benchmark_movegen

clean:
	$(RM) test_index test_egtb test_movegen test_generator test_material test_bitmap check_stats benchmark_index \
		benchmark_egtb benchmark_movegen test_index.o test_egtb.o \
		test_movegen.o test_generator.o test_material.o test_bitmap.o check_stats.o endgame_index.o egtb.o wdl.o movegen.o \
		generator.o bitmap.o material.o generate_egtb generate_egtb.o
