ifneq ($(filter default undefined,$(origin CC)),)
CC := clang
endif
AR ?= ar
CFLAGS ?= -O3 -DNDEBUG -march=native -std=c11 -Wall -Wextra -Wpedantic -pthread
BENCH_CFLAGS ?= -O3 -DNDEBUG -march=native -std=c11 -Wall -Wextra -Wpedantic -pthread
LDLIBS ?= -lzstd
BUILD_REVISION := $(strip $(shell cat REVISION))

.PHONY: all test check-stats benchmark benchmark-egtb benchmark-movegen benchmark-combinatorial-index clean

all: libgwdegtb.a test_index test_slice_index test_sliced test_combinatorial_index test_egtb test_gwdegtb test_movegen test_generator test_generator_padded test_material test_bitmap generate_egtb check_stats benchmark_index benchmark_combinatorial_index benchmark_egtb \
	benchmark_movegen

%.o: %.c Makefile
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

generate_egtb: generate_egtb.o sliced.o generator_padded.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o revision.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

libgwdegtb.a: gwdegtb.o wdl.o egtb.o material.o endgame_index.o
	$(AR) rcs $@ $^

generate_egtb.o: revision.h

generate_egtb_padded: generate_egtb.o sliced.o generator_padded.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o revision.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

generate_egtb_table: generate_egtb.o sliced.o generator.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o revision.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

revision.o: revision.c revision.h REVISION Makefile
	$(CC) $(CFLAGS) -MMD -MP -DGWDEGTB_REVISION='"$(BUILD_REVISION)"' -c -o $@ revision.c

generator_padded.o: generator.c generator.h frontier.h bitmap.h material.h movegen.h endgame_index.h egtb.h Makefile
	$(CC) $(CFLAGS) -MMD -MP -DEGTB_PADDED_MOVEGEN -c -o $@ generator.c

test_index: test_index.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

test_slice_index: test_slice_index.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

test_sliced: test_sliced.o sliced.o generator_padded.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o revision.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_combinatorial_index: test_combinatorial_index.o combinatorial_index.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

check_stats: check_stats.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

test_egtb: test_egtb.o egtb.o wdl.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_gwdegtb: test_gwdegtb.o libgwdegtb.a
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_movegen: test_movegen.o movegen.o endgame_index.o
	$(CC) $(CFLAGS) -o $@ $^

test_generator: test_generator.o generator.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_generator_padded: test_generator.o generator_padded.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_material: test_material.o material.o
	$(CC) $(CFLAGS) -o $@ $^

test_bitmap: test_bitmap.o bitmap.o
	$(CC) $(CFLAGS) -o $@ $^

benchmark_index: benchmark_index.c endgame_index.c endgame_index.h
	$(CC) $(BENCH_CFLAGS) -o $@ benchmark_index.c endgame_index.c

benchmark_combinatorial_index: benchmark_combinatorial_index.c combinatorial_index.c combinatorial_index.h endgame_index.c endgame_index.h
	$(CC) $(BENCH_CFLAGS) -o $@ benchmark_combinatorial_index.c combinatorial_index.c endgame_index.c

benchmark_egtb: benchmark_egtb.c egtb.c egtb.h
	$(CC) $(BENCH_CFLAGS) -o $@ benchmark_egtb.c egtb.c $(LDLIBS)

benchmark_movegen: benchmark_movegen.c movegen.c movegen.h endgame_index.c \
		endgame_index.h
	$(CC) $(BENCH_CFLAGS) -o $@ benchmark_movegen.c movegen.c endgame_index.c

test: test_index test_slice_index test_sliced test_combinatorial_index test_egtb test_gwdegtb test_movegen test_generator test_generator_padded test_material test_bitmap
	./test_index
	./test_slice_index
	./test_sliced
	./test_index 0 0 1 1
	./test_index 1 1 1 0
	./test_combinatorial_index
	./test_egtb
	./test_gwdegtb
	./test_movegen
	./test_generator
	./test_generator_padded
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

benchmark-combinatorial-index: benchmark_combinatorial_index
	./benchmark_combinatorial_index

clean:
	$(RM) libgwdegtb.a test_index test_slice_index test_sliced test_combinatorial_index test_egtb test_gwdegtb test_movegen test_generator test_generator_padded test_material test_bitmap check_stats benchmark_index benchmark_combinatorial_index \
		benchmark_egtb benchmark_movegen test_index.o test_slice_index.o test_sliced.o test_egtb.o \
		test_gwdegtb.o test_movegen.o test_generator.o test_material.o test_bitmap.o test_combinatorial_index.o check_stats.o combinatorial_index.o endgame_index.o egtb.o wdl.o gwdegtb.o movegen.o \
	generator.o generator_padded.o frontier.o bitmap.o material.o sliced.o generate_egtb \
		generate_egtb_padded generate_egtb_table generate_egtb.o revision.o
	$(RM) *.d

-include $(wildcard *.d)
