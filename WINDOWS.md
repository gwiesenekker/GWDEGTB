# GWD library on Windows (Clang / MSVC ABI)

Only the GWD-facing library is ported. The generator, slice/checkpoint tooling,
and family shell scripts remain Linux programs. Creating a missing WDL **from
an existing DTM** is part of the library and remains supported, including its
parallel compression and loading paths.

## Sources and dependencies

Compile these C files as C11, for 64-bit Windows:

* `gwdegtb.c`, `wdl.c`, `egtb.c`, `progress.c`
* `material.c`, `endgame_index.c`, `dtm_fen.c`
* `../compat/compat.c` (once only if GWD already compiles this file)

Add the endgame7, shared compat, and Zstd include directories. Include the
associated headers, including `egtb_platform.h`, `crc32c.h`, and `cache_modulo.h`.
Link a matching Windows Zstd library, `bcrypt.lib`, and `ws2_32.lib`. Keep the
same CRT selection as GWD and Zstd (for example `/MD`). Do not add `pthread.h`,
`-pthread`, a pthread emulation library, or generator sources on Windows.

GWD continues to include `gwdegtb.h`; public lookup signatures and file formats
are unchanged. C11 atomics/thread-local storage and Clang bit-operation
intrinsics are retained. The 128-bit multiply-high operation is isolated in
`compat_mulhi_u64`, with intrinsic/portable fallback implementations.

## Optional standalone CMake build

From a Visual Studio developer terminal with Clang, Ninja and CMake available:

```powershell
cmake -S . -B build-windows -G Ninja -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_BUILD_TYPE=Release `
  -DCOMPAT_DIR=C:/path/to/compat `
  -DZSTD_INCLUDE_DIR=C:/path/to/zstd/include `
  -DZSTD_LIBRARY=C:/path/to/zstd/lib/zstd.lib
cmake --build build-windows
ctest --test-dir build-windows --output-on-failure
```

This builds only the library and portable tests. The `gwdegtb` test checks both
sides, compact/padded bitboards, mirroring, missing-WDL generation, parallel
WDL loading/decompression, compressed probes, DTM values and unavailable files.
Its DTM fixture is synthetic: this tests storage/API behavior, not game solving.
The compatibility test performs concurrent I/O above 4 GiB; allow temporary
disk space for a file of that logical size (Linux normally stores it sparsely).

## Compatibility contracts

* Thread/mutex/condition/once functions return zero or an error code. Thread
  creation and joining no longer call `exit`; **GWD must check their results**.
  Windows uses a correctly typed `_beginthreadex` trampoline and closes joined
  thread handles. Try-lock returns `EBUSY` when it cannot acquire the lock.
* File/time wrappers return `-1` and set `errno`; allocation returns `NULL`.
  No compatibility function aborts. Legacy void-returning functions
  `compat_unlock_file`, `compat_fdprintf`, `return_cpu_flags`, `compat_sleep`,
  and `compat_getrandom_u64` now return status (fdprintf returns byte count).
  Existing callers can compile while ignoring those values, but should be
  updated to handle failures. Integer APIs use `int64_t`/`uint64_t`.
* `compat_lseek` now returns a 64-bit offset. Positioned I/O explicitly supplies
  an offset on each operation; no seek/read pair is used. Windows handles are
  synchronous, so same-handle I/O can serialize internally. Do not mix these
  operations with concurrent seek/stdio access on that same handle.
* Windows descriptor opens are binary; temporary files are created exclusively.
  File sizes and offsets are 64-bit. Aligned allocations must be released with
  `compat_aligned_free`, not plain `free` on Windows.
* Publication flushes and closes the file before replacement. Linux retains
  rename plus directory-fsync. Windows uses `MoveFileExA` with replacement and
  write-through; there is no claimed POSIX-equivalent directory-fsync guarantee.
  Paths currently use the Windows system ANSI encoding, not a new UTF-8 API.
* `COMPAT_FORBID_RAW_CALLS` is enabled in migrated implementation files via
  `egtb_platform.h`. It rejects accidental platform calls; it does not silently
  redirect them. Do not include this private header in GWD's public headers.

## Read-only full scan of existing databases

After building, run this against a directory containing matching DTM/WDL pairs:

```bat
C:\Tmp2\gwdegtb\build-windows\test_database_files.exe Z:/ 1wX-0wO-1bX-0bO 1wX-0wO-0bX-1bO 0wX-1wO-0bX-1bO
```

Use canonical basenames without extensions. Each named database must have both
files already present. This tool never generates or modifies databases, including
when a WDL is missing. It loads the complete compressed WDL into RAM using four
threads by default, then scans every position, both sides to move. Use `-j N`
before the directory to select 1..256 threads for both loading and scanning.
Scanning workers are capped at the number of WDL pages. It checks
decompression and page checksums, validates DTM value parity, compares each WDL
outcome with its DTM outcome, and prints WTM/BTM outcome totals and full DTM
histograms. Each worker has a private DTM reader, Zstd context and histogram;
the compressed WDL image is shared read-only. Histograms are merged after joining
all workers (approximately 1 MiB histogram memory per worker).
An additional sample of up to 10,000 positions (including endpoints) checks the
public compact DTM and compact/padded compressed-WDL lookup APIs. Databases are
processed one at a time; allow enough RAM for the largest compressed WDL named.
Start with the small two-piece files above, then try larger files.

For the largest five-piece database by position count:

```bat
C:\Tmp2\gwdegtb\build-windows\test_database_files.exe -j 8 Z:/ 2wX-1wO-1bX-1bO > C:\Tmp2\gwdegtb\five-piece-scan.txt 2>&1
```

This is a complete logical-data scan, not a game-theoretic consistency
verification: matching files could still share incorrect game results. It does
not inspect unused gaps or trailing bytes. It exits nonzero on any failure.
It is manual rather than part of CTest, because CTest needs no external databases.
The additional sampled public-API checks remain single-threaded: the public DTM
lookup API is deliberately single-threaded for GWD's root search.

The shared compatibility directory is outside the endgame7 Git repository:
copy/version its two files together with this port. Native Windows validation
is still required; Linux tests do not execute the Win32 branches.

Microsoft documents explicit `OVERLAPPED` offsets for synchronous file handles
in [ReadFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile)
and [WriteFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile).
