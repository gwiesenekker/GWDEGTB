ifneq ($(filter default undefined,$(origin CC)),)
CC := clang
endif
AR ?= ar
CFLAGS ?= -O3 -DNDEBUG -march=native -std=c11 -Wall -Wextra -Wpedantic -pthread
BENCH_CFLAGS ?= -O3 -DNDEBUG -march=native -std=c11 -Wall -Wextra -Wpedantic -pthread
LDLIBS ?= -lzstd
COMPAT_DIR ?= ../compat
CPPFLAGS += -I$(COMPAT_DIR)
.DEFAULT_GOAL := all

# Shared platform layer (also linked by tools using the library internals).
compat.o: $(COMPAT_DIR)/compat.c $(COMPAT_DIR)/compat.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $(COMPAT_DIR)/compat.c

generate_egtb libgwdegtb.a: compat.o
generate_egtb_padded generate_egtb_table verify_dtm dtm_examples \
test_dependency_resident test_shared_cache test_active_transfer test_sliced \
test_egtb test_dtm16 test_generator test_generator_padded test_progress \
test_crc32c test_wdl_compile benchmark_egtb test_frontier: compat.o

.PHONY: test-compat
test-compat: test_compat_include test_compat_runtime
	./test_compat_include
	./test_compat_runtime
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror=implicit-function-declaration -fsyntax-only test_platform_guards.c
	@if $(CC) $(CPPFLAGS) $(CFLAGS) -Werror=implicit-function-declaration -DTEST_FORBIDDEN_RAW_CALL -fsyntax-only test_platform_guards.c >/dev/null 2>&1; then echo 'raw-call guard failed'; exit 1; fi

test_compat_runtime: test_compat_runtime.c compat.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

test_compat_include: test_compat_include.c compat.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(COMPAT_DIR) -o $@ $^

test: test-compat
BUILD_REVISION := $(strip $(shell cat REVISION))

.PHONY: all test check-stats benchmark benchmark-egtb benchmark-movegen benchmark-combinatorial-index benchmark-wdl-probe benchmark-tunstall benchmark-wdl3 clean

all: libgwdegtb.a test_index test_slice_index test_sliced test_combinatorial_index test_egtb test_gwdegtb test_movegen test_generator test_generator_padded test_material test_bitmap generate_egtb dtm_examples verify_dtm check_stats benchmark_index benchmark_combinatorial_index benchmark_egtb \
	benchmark_movegen
all: benchmark_wdl_probe
all: benchmark_tunstall
all: benchmark_wdl3
all: test_dtm16 test_progress test_8piece
all: test_storage_safety
all: test_wdl_compile test_wdl_dictionary

test_wdl_dictionary: test_wdl_dictionary.o libgwdegtb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_wdl_compile: test_wdl_compile.o wdl.o egtb.o progress.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: test_wdl_compile test_wdl_dictionary
all: test_compact_pipeline

test: generate_egtb

generate_egtb generate_egtb_padded generate_egtb_table verify_dtm: dependency_resident.o

test_dependency_resident: test_dependency_resident.o dependency_resident.o egtb.o progress.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

all test: test_dependency_resident
all test: test_shared_cache
all test: test_active_transfer

# The test includes the coordinator implementation for controlled policy fixtures.
test_active_transfer: test_active_transfer.o egtb.o progress.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_shared_cache: test_shared_cache.o dependency_resident.o egtb.o progress.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

.PHONY: test-restart
.PHONY: test-verification-modes
test-verification-modes: generate_egtb verify_dtm
	sh ./test_verification_modes.sh "$(CURDIR)/generate_egtb" "$(CURDIR)/verify_dtm"
.PHONY: test-tmux
test-tmux:
	./test_tmux.sh
.PHONY: test-adaptive
test-adaptive: generate_egtb verify_dtm test_dependency_resident
	sh ./test_adaptive.sh "$(CURDIR)/generate_egtb" "$(CURDIR)/verify_dtm" "$(CURDIR)/test_dependency_resident"

test-restart: generate_egtb
	sh ./test_restart.sh "$(CURDIR)/generate_egtb"

test_compact_pipeline: test_compact_pipeline.o generator_padded.o frontier.o bitmap.o movegen.o libgwdegtb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: test_compact_pipeline

test_storage_safety: test_storage_safety.o libgwdegtb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: test_storage_safety

test_8piece: test_8piece.o generator_padded.o frontier.o bitmap.o movegen.o libgwdegtb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_progress: test_progress.o progress.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

%.o: %.c Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

generate_egtb: generate_egtb.o dtm_fen.o sliced.o generator_padded.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o progress.o revision.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

dtm_examples: dtm_examples.o dtm_fen.o material.o endgame_index.o movegen.o egtb.o progress.o revision.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

verify_dtm: verify_dtm.o generator_padded.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o progress.o revision.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

libgwdegtb.a: gwdegtb.o wdl.o egtb.o progress.o material.o endgame_index.o dtm_fen.o
	$(AR) rcs $@ $^

generate_egtb.o: revision.h

generate_egtb_padded: generate_egtb.o dtm_fen.o sliced.o generator_padded.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o progress.o revision.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

generate_egtb_table: generate_egtb.o dtm_fen.o sliced.o generator.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o progress.o revision.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

revision.o: revision.c revision.h REVISION Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -DGWDEGTB_REVISION='"$(BUILD_REVISION)"' -c -o $@ revision.c

generator_padded.o: generator.c generator.h frontier.h bitmap.h material.h movegen.h endgame_index.h egtb.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -DEGTB_PADDED_MOVEGEN -c -o $@ generator.c

test_index: test_index.o endgame_index.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

test_slice_index: test_slice_index.o endgame_index.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

test_sliced: test_sliced.o sliced.o generator_padded.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o progress.o revision.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_combinatorial_index: test_combinatorial_index.o combinatorial_index.o endgame_index.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

check_stats: check_stats.o endgame_index.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

test_egtb: test_egtb.o egtb.o progress.o wdl.o endgame_index.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_crc32c: test_crc32c.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

test: test_crc32c

test_dtm16: test_dtm16.o egtb.o progress.o wdl.o endgame_index.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_gwdegtb: test_gwdegtb.o libgwdegtb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_movegen: test_movegen.o movegen.o endgame_index.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

test_generator: test_generator.o generator.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o progress.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_generator_padded: test_generator.o generator_padded.o frontier.o bitmap.o material.o movegen.o endgame_index.o egtb.o progress.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_material: test_material.o material.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

test_slice_order: test_slice_order.o

test: test_slice_order

test-slice-order: test_slice_order
	./test_slice_order

test_bitmap: test_bitmap.o bitmap.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

benchmark_index: benchmark_index.c endgame_index.c endgame_index.h
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) -o $@ benchmark_index.c endgame_index.c

benchmark_combinatorial_index: benchmark_combinatorial_index.c combinatorial_index.c combinatorial_index.h endgame_index.c endgame_index.h
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) -o $@ benchmark_combinatorial_index.c combinatorial_index.c endgame_index.c

benchmark_egtb: benchmark_egtb.c egtb.c egtb.h progress.c progress.h
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) -o $@ benchmark_egtb.c egtb.c progress.c compat.o $(LDLIBS)

benchmark_movegen: benchmark_movegen.c movegen.c movegen.h endgame_index.c \
		endgame_index.h
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) -o $@ benchmark_movegen.c movegen.c endgame_index.c

benchmark_wdl_probe: benchmark_wdl_probe.o libgwdegtb.a
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) -o $@ $^ $(LDLIBS)

benchmark_tunstall: benchmark_tunstall.o libgwdegtb.a
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) -o $@ $^ $(LDLIBS) -lm

benchmark_wdl3: benchmark_wdl3.o libgwdegtb.a
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) -o $@ $^ $(LDLIBS)

test: test_index test_slice_index test_sliced test_combinatorial_index test_egtb test_dtm16 test_gwdegtb test_movegen test_generator test_generator_padded test_material test_bitmap
	./test_dependency_resident
	./test_wdl_compile
	./test_wdl_dictionary
	./test_shared_cache
	./test_active_transfer
	./test_crc32c
	./test_frontier
	./test_compact_pipeline
	sh ./test_restart.sh "$(CURDIR)/generate_egtb"
	./test_progress
	./test_8piece
	./test_index
	./test_index 0 0 0 0
	./test_index 1 1 1 1
	./test_slice_index
	./test_slice_order
	./test_sliced
	./test_sliced 2048
	./test_sliced 1024 resident
	./test_sliced 2048 resident
	./test_sliced 1024 cached nine
	./test_sliced 2048 resident wide-nine
	./test_sliced 1024 cached wide-81
	./test_index 0 0 1 1
	./test_index 1 1 1 0
	./test_combinatorial_index
	./test_egtb
	./test_dtm16
	./test_storage_safety
	./test_gwdegtb
	./test_movegen
	./test_generator
	./test_generator_padded
	./test_material
	./test_bitmap
	sh ./test_family_logs.sh

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

benchmark-wdl-probe: benchmark_wdl_probe
	@echo "usage: ./benchmark_wdl_probe DIRECTORY DATABASE [CACHE_MIB] [LOOKUPS]"

benchmark-tunstall: benchmark_tunstall
	@echo "usage: ./benchmark_tunstall DIRECTORY DATABASE [8|16]"

benchmark-wdl3: benchmark_wdl3
	@echo "usage: ./benchmark_wdl3 DIRECTORY DATABASE"

test_frontier: test_frontier.c frontier.c frontier.h crc32c.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_frontier.c compat.o $(LDLIBS)

test: test_progress test_8piece test_frontier

clean:
	$(RM) compat.o test_compat_include test_compat_runtime test_platform_guards
	$(RM) test_slice_order test_slice_order.o
	$(RM) test_dependency_resident test_dependency_resident.o dependency_resident.o
	$(RM) test_shared_cache test_shared_cache.o
	$(RM) test_active_transfer test_active_transfer.o
	$(RM) test_frontier
	$(RM) test_compact_pipeline test_compact_pipeline.o
	$(RM) test_storage_safety test_storage_safety.o
	$(RM) test_crc32c test_crc32c.o
	$(RM) test_8piece test_8piece.o
	$(RM) test_progress test_progress.o
	$(RM) test_dtm16 test_dtm16.o
	$(RM) test_wdl_compile test_wdl_compile.o
	$(RM) test_wdl_dictionary test_wdl_dictionary.o
	$(RM) libgwdegtb.a test_index test_slice_index test_sliced test_combinatorial_index test_egtb test_gwdegtb test_movegen test_generator test_generator_padded test_material test_bitmap check_stats benchmark_index benchmark_combinatorial_index \
		benchmark_egtb benchmark_movegen benchmark_wdl_probe benchmark_wdl_probe.o benchmark_tunstall benchmark_tunstall.o benchmark_wdl3 benchmark_wdl3.o test_index.o test_slice_index.o test_sliced.o test_egtb.o \
		test_gwdegtb.o test_movegen.o test_generator.o test_material.o test_bitmap.o test_combinatorial_index.o check_stats.o combinatorial_index.o endgame_index.o egtb.o progress.o wdl.o gwdegtb.o movegen.o \
	generator.o generator_padded.o frontier.o bitmap.o material.o sliced.o generate_egtb \
		generate_egtb_padded generate_egtb_table generate_egtb.o revision.o \
		dtm_examples dtm_examples.o dtm_fen.o
	$(RM) verify_dtm verify_dtm.o
	$(RM) *.d

-include $(wildcard *.d)
