# Changelog

All notable changes to ecm-tools-reloaded are documented in this file.

## [3.0.1.6] — 2026-06-18

### GUI

- **Batch Split/Combine CUE tab** — New "Batch Split/Combine CUE" tab
  (Tab 5) recursively processes all `.cue` files in a directory, calling
  `cue_cmd_split` or `cue_cmd_combine` on each. Uses the same atomic
  work-stealing thread pool as Batch Encode/Decode with configurable
  parallel jobs (0=auto). Output directory is required to preserve input
  files. Includes Force overwrite checkbox.

- **Copy unmodified files on batch CUE** — New "Copy unmodified files to
  output" checkbox on the Batch Split/Combine CUE tab. When checked,
  single-track CUEs (split mode) and already-combined CUEs (combine mode)
  are copied as-is to the output directory instead of being skipped.
  Respects the Force overwrite setting.

## [3.0.1.5] — 2026-06-15

### Bug Fixes

- **Batch decode crash under parallel execution** — `signal(SIGSEGV)` does
  not catch SEH exceptions (access violations, stack overflows) on MinGW
  Windows, so worker threads crashed silently. Replaced with
  `AddVectoredExceptionHandler` (Windows) / `signal(SIGSEGV)` fallback
  (other platforms). Handler calls `longjmp` to a per-thread `jmp_buf` to
  recover and continue with the next file.

- **FLAC library thread-safety** — Removed the global `flaczlib_stream`
  from `flaczlib.cpp`. Each `flaczlib` instance owns its own embedded
  `flaczlib_stream strm` member; all five FLAC C callbacks now receive
  the instance via `client_data` instead of referencing the global.

### GUI

- **Per-file batch progress** — The progress gauge during batch encode
  and batch decode now shows `completed_files / total_files * 100`
  instead of per-file decode percentage, which is meaningless when
  multiple files are processed in parallel.

## [3.0.1.4] — 2026-06-14

### Features

- **Parallel batch processing** — New `--batch-jobs <n>` flag controls how
  many batch files (CUE/ECM3) are processed concurrently. In batch mode,
  `-j` (per-file stream parallelism) is forced to `1` to avoid thread
  oversubscription; only `--batch-jobs` controls parallelism. Default `0`
  auto-detects CPU count via `std::thread::hardware_concurrency()`. When
  `--batch-jobs` is `1`, files are processed sequentially (previous behavior).

### GUI

- **Batch jobs controls** — New "Parallel batch jobs (0=auto)" spin controls
  on both the Batch Encode and Batch Decode tabs (range 0–64, default 0).
  Both tabs now use a thread pool with atomic work-stealing when the value
  is > 1, with mutex-protected log output to prevent interleaving.

## [3.0.1.3] — 2026-06-14

### Bug Fixes

- **Parallel encode CRC mismatch on CUE/BIN images** — When encoding
  multi-track CUE sheets with `-j 2+`, the CRC (EDC) stored at the end
  of the ECM data block was incorrect. The `encode_single_stream()`
  function accumulated `byte_count` as 2352 per sector regardless of
  the actual EDC size (`edc_size`), which can be 2076 (0x81C) when
  `OO_REMOVE_ECC` is enabled. This caused `edc_combine()` to shift the
  partial CRC by the wrong amount, producing a wrong combined CRC that
  failed verification on decode. Fixed by accumulating the actual
  `edc_size` per sector instead of hardcoding 2352.

## [3.0.1.2] — 2026-06-13

### Bug Fixes

- **FLAGS not written to CUE on decode** — `write_cue_from_metadata()`
  omitted `FLAGS DCP/4CH/PRE/SCMS` lines in the reconstructed `.cue`,
  breaking bit-identical round-trip for CUEs with FLAGS (e.g., Afraid
  Gear (Japan)). Fixed by adding FLAGS output to both split and combined
  output paths, matching the existing `write_cue_file()`.

- **Windows icons not showing** — `resources.rc`/`resource.h` existed
  but were never compiled into the executable in the CLI build
  (`build.sh`); the GUI build (`build_gui.sh`) referenced the stale
  `ecm3.rc` with numeric IDs (1, 2) that were never linked. Fixed by
  compiling `resources.rc` via `windres` in both build scripts and
  setting icons via `WM_SETICON` with correct resource IDs.

- **File association icon using wrong resource ID** — The registry
  `DefaultIcon` value used `L",2"` (old `ecm3.rc` numeric ID), now
  changed to `L",-%d", IDI_ECM3FILE` (200) from `resources.rc`.

## [3.0.1.1] — 2026-06-10

### Bug Fixes

- **Fixed crash on successive GUI operations** — `OnRun` reassigned
  `m_worker = std::thread(...)` without joining the previous thread,
  causing `std::terminate()` ("terminate called without an active
  exception") on every second operation. Fixed by calling
  `m_worker.join()` before creating the new thread.

## [3.0.1.0] — 2026-06-10

### CUE Split/Combine Utility

- **New `--split-cue` flag** — Splits a combined `.cue`/`.bin` into
  per-track `.bin` files + multi-FILE `.cue`. Determines track boundaries
  from INDEX sector offsets; supports any track count (1–99+). Single-track
  discs are a no-op (consistent with decode `-S` behavior).

- **New `--combine-cue` flag** — Combines per-track `.bin` files (referenced
  by a multi-FILE `.cue`) into a single `.bin` + single-FILE `.cue`. Strips
  `(Track NN)` suffixes from filenames. Adjusts INDEX positions from
  relative (per-track file) to absolute (combined image).

- **`-o` required for split/combine** — Both `--split-cue` and `--combine-cue`
  require `-o <directory>` for output, protecting the original CUE from
  accidental overwrite. `-f` controls overwrite in the output directory.

- **Track naming matches decode split** — `(Track N)` for 2–9 tracks,
  `(Track NN)` for 10+, single-track no split. Consistent with `-S` decode
  output via shared `write_cue_file()`.

- **Output CRLF on all platforms** — `write_cue_file()` in `cue_parser.cpp`
  uses binary mode + explicit `"\r\n"`, ensuring bit-identical round-trips
  between Windows and Linux.

- **New source files**: `cuesplit.h`, `cuesplit.cpp`.

### GUI — CUE Split/Combine Tab (Tab 6)

- **New tab** in the wxWidgets GUI for CUE split and combine operations.
  Mode selector (`wxChoice`), input CUE picker, output dir picker, and
  force-overwrite checkbox.

- **Output dir picker** — Uses `wxDirPickerCtrl`; output directory must be
  specified (same as CLI `-o` requirement).

- **CLI/GUI shared implementation** — Both call `cue_cmd_split()` and
  `cue_cmd_combine()` from `cuesplit.cpp`.

### GUI Thread Safety

- **`TextCtrlStream`** — `std::mutex` + `wxQueueEvent` for thread-safe
  output to the log pane from background worker threads.

- **Progress gauge** — Backend progress callback posts `wxThreadEvent` via
  `wxQueueEvent` for live progress bar updates.

### WavPack Codec

- **Added WavPack compression** — `C_WAVPACK` (value 7) for audio streams.
  Uses `WavpackOpenFileOutput`/`WavpackPackSamples` for encode and
  `WavpackUnpackSamples` for decode. Wraps raw WavPack blocks in ECM3's
  zlib framing via `wavpackzlib.cpp`.

- **Hybrid mode disabled** — Always uses lossless mode (decoder rejects
  `MODE_HYBRID`). Frames per block: 512 (configurable via
  `CONFIG_FLOAT_OVERRIDE`).

- **Compression level remap** — `-c N` (0–9): 0–1 Fast, 2–4 Normal, 5–7 High,
  8–9 Very High; `-e` adds `CONFIG_EXTRA_MODE` with `xmode` scaled by level.

- **Empty-stream handling** — Zero-data streams produce minimal valid `wvpk`
  block via `Z_FINISH` with no input; ECM3 layer overrides 0‑byte streams
  to `C_NONE`.

- **Byte-identical round-trip verified** — Tested with CDDA 1:15 raw data,
  gap-only image, all-zero data, and full Shellshock (USA) 633 MB image.

### Bug Fixes

- **CUE parser `::tolower` filename corruption** — Parser was lowercasing
  entire CUE lines including quoted filenames, destroying case of filenames
  and `(Track NN)` suffixes. Fixed by saving `orig_line` before `::tolower`
  and using `orig_line` for all quoted-value extractions (`FILE`, `ISRC`,
  `CATALOG`, `CDTEXTFILE`).

- **CUE combine case-insensitive strip** — `base_name.find(" (Track ")`
  → `lower_name.find(" (track ")` after lowering a copy, so `(Track 1)`
  matches regardless of case.

- **`flaczlib.cpp` warning** — `%d` → `%lu` for `total_samples` format
  specifier.

- **GUI CUE filename bug** — Output derivation runs after auto-CUE detection;
  both branches use `stem()` correctly.

- **GUI batch output directory** — Optional dir pickers added to Batch
  Encode and Batch Decode tabs.

### Build System

- **All-in-one build scripts** — `build.sh`, `build_gui.sh`, `build.ps1`,
  `build_gui.ps1` detect platform, check `.a` format (ELF vs PE), rebuild
  wrong-platform deps from source, produce executable.

- **Cross-platform build isolation** — Platform-specific CMake build
  directories (`wavpack/build-windows`, `flac/build-linux`, etc.) to avoid
  cache collisions.

- **Static binary verified** — Standalone PE32+ x86-64 (`ecm3.exe`,
  2,685,440 B) works without MinGW DLLs.

## [3.0.0.9] — 2026-06-07

### GUI (wxWidgets)

- **Added wxWidgets GUI** — `guimain.cpp` implements a 6-tab interface:
  Encode, Decode, Batch Encode, Batch Decode, Settings, About.
  Each tab collects parameters and calls `ecm3_encode()`/`ecm3_decode()`
  in a background `std::thread`, with output redirected to a log pane
  via `TextCtrlStream`.

- **Drag-and-drop** — File picker controls accept drag-and-drop via
  `wxFileDropTarget`.

- **File association** — Settings tab registers/unregisters `.ecm3` file
  association for the current user on Windows (writes to
  `HKCU\Software\Classes`).

- **CLI/GUI shared core** — `#ifndef ECM3_GUI` guard on `main()` in
  `ecm3.cpp`; both builds share all source files except the entry point.

- **Build scripts**:
  - `build_gui.ps1` — PowerShell wrapper that invokes MSYS2 bash for the
    GUI build; supports `-Debug` and `-Static` flags.
  - `build_gui.sh` — Shell script using `wx-config` (dynamic) or
    `wx-config-static` (when `STATIC=1`) for compiler flags.
  - Dynamic build (~1.2 MB + wx DLLs) and static build (~9.4 MB standalone)
    both supported.

- **MSYS2 MinGW64 requirement** — GUI must be built with MSYS2 g++ 16.1.0
  because system g++ 14.2.0 has a C++ ABI mismatch with MSYS2 wxWidgets
  DLLs (`std::codecvt`/`std::fpos` undefined references).

### Library Carve-Out

- **`ecm3_core.h`/`ecm3_core.cpp`** — Extracted `ecm3_encode()` and
  `ecm3_decode()` into a separate compilation unit. These functions own
  all file I/O, TOC reading/writing, metadata, and the core encode/decode
  logic. Both CLI and GUI call these entry points.

- **`metadata.h`** — Made 8 previously-static helper functions non-static
  and declared them in a shared header: `build_metadata_from_cue`,
  `write_metadata_block`, `read_metadata_block`,
  `write_cue_from_metadata`, `split_output_bin`, `get_cue_dir`,
  `get_basename`, `fmt_track_number`.

- **`util.h`** — Extracted `temp_file` and `scope_guard` classes from
  `ecm3.cpp` into a standalone header.

- **`g_interrupted`** — Changed from `static` to `extern` so the core
  library can reference it from `ecm3.cpp`.

- **`ECM_FILE_VERSION`** — Moved from `ecm3.cpp` to `ecm3.h`.

### Bug Fixes

- **Fixed `register` keyword warning** — `lzlib4/lzlib4.cpp` used the
  C++17-removed `register` keyword; replaced with plain declarations.

- **Rebuilt `zlib/libz.a` for MinGW** — Previous build was ELF format
  (incompatible with MinGW PE/COFF linking). Rebuilt with
  `make -f win32/Makefile.gcc libz.a` to produce a proper PE/COFF static
  library.

### Build System

- **`build.ps1`** — Added `ecm3_core.cpp` to source list.

- **`build.sh`** — Added `ecm3_core.cpp` to source list.

## [3.0.0.8] — 2026-06-07

### Zstd Compression

- **Added zstd as a new compression module** — `C_ZSTD` (value 6) alongside
  existing `C_ZLIB`, `C_LZMA`, `C_LZMA2`, `C_LZ4`, and `C_FLAC`. Uses zstd's
  streaming API (`ZSTD_compressStream2`/`ZSTD_decompressStream`) for per-sector
  compression via the existing `compressor` class abstraction.

- **zstd v1.5.7 submodule** — Added `git@github.com:facebook/zstd.git` at tag
  v1.5.7 following the same submodule pattern as zlib, xz, lz4, and flac.

- **CLI support** — `zstd` accepted as a compression name for both `-a`/`--acompression`
  (audio) and `-d`/`--dcompression` (data), e.g. `ecm3 -i input.bin -a zstd -d zstd`.

- **Extreme compression** — `-e`/`--extreme-compression` sets zstd level to
  `ZSTD_maxCLevel()` (max useful level) for maximum ratio.

- **Buffer sizing** — zstd streams use 4 MB compression buffers.

- **Build system** — `build.ps1` and `Makefile` updated with all required zstd
  source files from `zstd/lib/common/`, `zstd/lib/compress/`, `zstd/lib/decompress/`,
  and `zstd/lib/dictBuilder/`. `-DZSTD_DISABLE_ASM` set for MinGW compatibility.

### Batch Processing

- **`--batch-cue <dir>`** — Recursively finds all `.cue` files in a directory
  tree and encodes each one with current compression settings. Shows `[N/M]`
  progress per file.

- **`--batch-decode <dir>`** — Recursively finds all `.ecm3` files in a directory
  tree and decodes each one. Shows `[N/M]` progress per file.

### Bug Fixes

- **Fixed CUE round-trip track count** — `generate_cue_from_sectors()` at
  `cue_gen.cpp:78` refused to merge same-type runs when a gap run separated them
  (`&& !runs[i + 1].gap`). This caused synthetic CUE generation to produce
  inflated track counts (e.g., 195 tracks instead of 1 for a single-data-track
  image). Fix: removed the gap-blocking condition so same-mode runs separated by
  gaps merge correctly, producing the expected 1 track. Round-trip verified
  byte-identical: encode without CUE, decode with synthetic CUE produces
  identical CUE and SHA256-identical BIN vs. originals.

- **Fixed auto-detect output naming** — When CUE auto-detection fires on a
  `.bin` input, `options.cue_filename` was never set, causing `output_name_base`
  to be derived from an empty string (stem = `""`) and produce output named
  `.ecm3`. Fix: added `options.cue_filename = cue_path.string()` in the
  auto-detect block so the output name matches the CUE filename.

- **Fixed force flag (`-f`)** — Existing output file was overwritten without
  warning regardless of `-f`. Added `std::filesystem::exists()` check before
  opening output file in both encode and decode paths; errors if file exists
  unless `-f` is given.

- **Suppressed redundant error message** — Generic "ERROR: there was an error
  processing the input file." no longer printed after specific errors (e.g.,
  "output file already exists"). Added `specific_error_printed` flag to gate
  the generic message.

### Build System

- **Fixed zlib build for MinGW** — `zlib/libz.a` contained Linux-compiled objects
  using `__errno_location` (unavailable on MinGW). Rebuilt zlib locally with
  MinGW to resolve linker errors on encode paths.

## [3.0.0.7] — 2026-06-03

### Deterministic Encoding

- **All three encoding paths produce bit-identical `.ecm3` files** — CUE
  (`--cue`), BIN+auto-CUE (`-i image.bin` with sibling `.cue`), and BIN+synthetic
  (`-i image.bin` without a `.cue`) now all go through `build_metadata_from_cue`.
  When no CUE is available, `generate_cue_from_sectors` synthesises one from
  sector analysis so the same metadata logic applies everywhere.

- **New `cue_gen.h`/`cue_gen.cpp`** — `generate_cue_from_sectors()` extracted to
  its own compilation unit, keeping CUE generation separate from the core encode
  pipeline.

- **Fixed hardcoded temp file name** — `ecm3_encode_clean.tmp` now includes the
  process ID and a counter (`ecm3_encode_clean.<PID>.<N>.tmp`), preventing
  file-lock collisions when multiple encodes run concurrently.

### Build System

- **Added `build.sh`** — Bash equivalent of `build.ps1` for Linux/macOS builds.

- **Added `#pragma once`** to `ecm3.h`, `cue_parser.h`, `sector_tools.h` —
  Required after splitting `cue_gen` into its own translation unit; prevents
  double-inclusion errors.

- **Made `banner()` inline** in `banner.h` — Avoids multiple-definition linker
  errors when `ecm3.h` is included from more than one `.cpp` file.

## [3.0.0.6] — 2026-06-03

### CLI Improvements

- **Independent ECM encode progress** — `Encode(ECM)(%)` now reaches 100%
  independently before Audio/Data compression begins. The encoder uses a
   two-pass approach: pass 1 cleans all sectors (updating ECM progress),
  pass 2 compresses from a temporary file (updating Audio/Data progress).
  When only `-i` is passed, progress shows `Analyze(100%) Encode(ECM)(00%→100%)`
  with no Audio/Data sections.

## [3.0.0.5] — 2026-06-02

### Bug Fixes

- **Fixed encode progress > 100% in parallel mode** — `encode_single_stream`
  tracked progress using `start_byte_offset + result.byte_count` (file offset),
  not bytes processed by this stream. When the display thread summed progress
  across all parallel streams, the aggregate could exceed the total input size,
  producing percentages above 100 (e.g., "Encode(124%)"). Fix: store
  `result.byte_count` instead, so each counter reflects only its own work and
  the sum can never exceed 100%.

### CLI Improvements

- **Added `-h`/`--help` option** — Properly registered as a short/long option
  in `getopt_long` (was previously missing; `--help` fell through to the `?`
  handler and printed a spurious "ERROR: input file cannot be opened").

- **Bare `ecm3` now shows help** — Running `ecm3` with no arguments displays
  the usage text and exits with code 0, instead of printing "ERROR: input file
  cannot be opened".

- **Unknown options show usage** — `ecm3 --unknown` or `ecm3 -x` now prints
  getopt's error followed by the full help text and exits with code 1, without
  the misleading "ERROR: there was an error processing the input file."

### CUE Workflow

- **`--cue` can be used standalone** — `ecm3 --cue myfile.cue` is now sufficient
  without `-i`; the CUE sheet itself references the BIN files. The old `-i`
  argument is redundant when `--cue` is given.

- **Mutual exclusion warning** — If both `--cue` and `-i` are provided, a
  WARNING is printed on stderr and `-i` is silently ignored. The CUE sheet is
  the authoritative source for which BIN files to encode.

- **Help text updated** — Usage examples now include `ecm3 --cue <cue>` and
  notes explain standalone `--cue` behavior and precedence rules.

- **Format detection guarded** — The ECM3 format probe no longer attempts to
  open an empty filename when `--cue` is used without `-i`.

## [3.0.0.4] — 2026-06-02

### Bug Fixes

- **Fixed heap buffer overflow in TOC allocation** — `task_to_streams_header`
  and `task_to_sectors_header` allocated memory using the serialized struct
  size (`ECM_STREAM_SIZE` = 9, `ECM_SECTOR_SIZE` = 5) via
  `calloc(uncompressed_size, 1)`, but accessed elements via pointer arithmetic
  using the in-memory struct size (`sizeof(stream)` = 12, `sizeof(sector)` = 8
  with alignment padding). This caused a heap buffer overflow on `free()`,
  producing `STATUS_HEAP_CORRUPTION` (exit code `-1073740940`). Fix: use
  `calloc(count, sizeof(stream))` / `calloc(count, sizeof(sector))`.

- **Fixed decode path not opening input/output files** — The reconstructed
  encode block from v3.0.0.3 omitted the code that opens `in_file` and
  `out_file` in the decode (`else`) branch. Decode attempted `seekg` on an
  unopened stream, producing "ERROR: Failed to read TOC position". Fix: added
  file open and error check at the start of the decode block.

## [3.0.0.3] — 2026-06-02

### Bug Fixes

- **Fixed `edc_combine` producing wrong CRC in parallel encode** — The
  `edc_combine_table[0]` was initialized as `odd^2` (shift by 2 bytes) instead
  of `odd` (shift by 1 byte). This caused the GF(2) polynomial shift to be off
  by one power at each binary decomposition step, producing incorrect combined
  EDC values for any stream whose byte count was not a power of 2. Parallel
  encode (`-j > 1` with multiple streams) produced `.ecm3` files that failed
  CRC verification on decode. Sequential encode (`-j 1`) was unaffected.
  Fix: `memcpy(edc_combine_table[0], odd, sizeof(odd))` instead of
  `gf2_matrix_square(edc_combine_table[0], odd)`.

- **Fixed `Z_FINISH` never sent in parallel compress** — In
  `encode_single_stream`, `current_sector_abs` (0-based per-stream counter) was
  compared against `script.stream_data.end_sector` (an absolute sector index
  in the image). Since `current_sector_abs` never reached `end_sector`,
  `Z_FINISH` was never called and the compressed stream was never properly
  terminated. Fix: compute `total_sectors_in_stream` from the script and use it
  for the `Z_FINISH` condition.

- **Fixed multi-file CUE output filename** — When `--cue <cuefile>` was passed
  with a BIN input, the output filename was derived from the BIN path instead
  of the CUE path, producing e.g. `track01.bin.ecm3` instead of
  `discname.ecm3`. Now `output_name_base` is set from `options.cue_filename`
  in the `--cue` case, so the output name matches the CUE filename with `.cue`
  replaced by `.ecm3`.

- **Fixed `goto exit` crossing variable initialization** — Declared
  `std::string output_name_base` before `get_options` to prevent `goto exit`
  from jumping past its initialization, which was undefined behavior.

- **Added error checks for TOC position write** — Added `out_file.good()`
  checks after `seekp(4)` and `write()` when writing the TOC position back to
  the ECM3 header, preventing silent write failures on the final header update.

## [3.0.0.2] — 2026-06-02

### Bug Fixes

- **Fixed divide-by-zero crash** in progress display code: `setcounter_encode`,
  `setcounter_decode`, `setcounter_analyze`, and parallel encode/decode display
  threads divided by `mycounter_total` / `total` / `total_decode_bytes` which
  could be zero for corrupt or empty input files. All division sites now guard
  with `if (denominator > 0)`. Also fixed divide-by-zero in `summary()` for
  `ecm_size / total_size`, `compressed_size / ecm_size`, and
  `compressed_size / total_size`.

- **Fixed partial output file not removed on interrupt/error**: The `exit:` label
  in `main()` tried to remove the output file before closing `in_file`/`out_file`,
  which failed on Windows because the file handle was still open. Files are now
  closed before deletion. Also replaced fragile `ifstream::read(&dummy, 0)` check
  with `std::filesystem::exists()`/`std::filesystem::remove()`.

- **Fixed SIGINT not terminating worker threads**: `encode_single_stream`,
  `decode_single_stream`, and `disk_decode_sequential` inner loops now check
  `g_interrupted` via `CHECK_INTERRUPT()` macro, allowing prompt thread exit on
  Ctrl+C. Parallel encode/decode display threads also check `g_interrupted`
  and exit their loop immediately.

- **Fixed crash when encoding multi-file CUE sheets** (split BIN images):
  `options.in_filename` was not updated after split-BIN concatenation, causing
  `disk_analyzer` and parallel workers to open the wrong file (the first BIN
  instead of the concatenated temp file) via mmap. The mmap path followed the
  original filename while the ifstream path used the temp file, leading to an
  access violation. Now `options.in_filename` is updated to point to the temp
  file after successful concatenation.

### Fuzz Testing

- Tested with truncated files, header bit flips, compressed data bit flips,
  zeroed regions, and extremely short files. Zero crashes across 87 test cases
  after dividing-by-zero fixes.

### Code Dedup

- Added `parse_compression_name()` helper consolidating `-a`/`-d` compression
  name parsing from `get_options()` into a single `static` function with forward
  declaration.

- Added `find_track_index01()` helper extracting the INDEX 01 sector-offset
  search loop from `build_metadata_from_cue()`, replacing two identical loops.

- Moved `decompress_header` from duplicate definitions in `ecm3.cpp` and
  `ecm3_extract_streams.cpp` to `decompress_header.h` as `inline`, included
  by both files.

- Consolidated `ecc_checkpq`/`ecc_writepq` via shared `ecc_computepq` helper,
  eliminating 80+ lines of duplicated inner loop.

- Consolidated `-a`/`-d` compression name parsing via `parse_compression_name()`,
  a parsing counterpart to the existing `compression_name()` display function.

### SIGINT Handling

- Worker threads (`encode_single_stream`, `decode_single_stream`) now check
  `g_interrupted` on each sector iteration for prompt exit on Ctrl+C.
- Sequential decode (`disk_decode_sequential`) also checks interrupt flag.
- Parallel encode/decode display threads exit immediately when `g_interrupted`
  is set, no longer sleeping until `done`/`decode_done`.

### Other

- PCLMULQDQ EDC: evaluated and deferred — slicing-by-4 is already efficient
  for 2352-byte sectors; SIMD CRC adds complexity with marginal benefit.

- RAII for goto-exit cleanup: evaluated and deferred — the existing `goto exit`
  pattern is functional and cleanup is correct (files closed before deletion).

## [3.0.0.1] — 2026-05-31

### Bit-Identical Output (Determinism)

Ensured ecm3 produces identical output across different machines and runs
when given the same input data and settings.

- **Zero-initialized bitfield structs**: Changed `malloc` to `calloc` for
  `stream` and `sector` structs. These contain 1-bit and 3-bit bitfields
  packed into `uint8_t`, so unused bits from `malloc` could be random, causing
  different compressed output on each run.

- **Fixed double `deflateInit` bug** in `compress_header`: The zlib stream was
  being initialized twice, which could produce non-deterministic header data.

- **Integer-only LZ4 HC depth calculation**: Replaced floating-point expression
  `(1.34 * comp_level)` with `((134 * comp_level) / 100)` to eliminate
  platform-dependent floating-point rounding differences.

### LZMA2 Compression Support

- Added `C_LZMA2` (value 5) as a new compression mode alongside existing
  `C_ZLIB`, `C_LZMA`, `C_LZ4`, and `C_FLAC`. Uses `LZMA_FILTER_LZMA2` alone,
  without the X86 BCJ branch/call/jump filter. The BCJ filter is
  non-deterministic for non-x86 CD-ROM data and produces different output across
  runs; LZMA2 omits it for deterministic compression while offering better
  ratios than LZMA for most CD-ROM data.

- Updated all compression switch statements in `compressor.cpp`,
  `ecm3.cpp`, and `ecm3_extract_streams.cpp` to handle the new mode.

### CUE Sheet Support

Added full CUE sheet support for preserving and reconstructing disc track
metadata through the ECM3 encode/decode cycle.

#### Encode (`--cue <cuefile>`)

- New `--cue` long-only option (no short form) accepts a CUE sheet file.
- Parses the CUE sheet (FILE, TRACK, INDEX, FLAGS, ISRC, CATALOG, PREGAP
  directives) via new `cue_parser.cpp` / `cue_parser.h`.
- Converts track metadata into compact binary `track_metadata_entry` /
  `track_metadata_header` structs and writes them as an
  `ECMFILE_BLOCK_TYPE_METADATA` block in the ECM3 file after the ECM data
  block.
- Metadata is stored structurally in the ECM3 format — not as an embedded
  .cue file — enabling reconstruction even if the original .cue is lost.

#### Auto-Detection of .cue Input

- If the input file has a `.cue` extension and `--cue` is not explicitly
  provided, the program automatically:
  1. Parses the CUE sheet
  2. Resolves the referenced BIN file relative to the CUE directory
  3. Encodes the BIN with track metadata embedded
- Explicitly passing `--cue` with a `.cue` input file is rejected as an
  error.

#### Decode (Automatic)

- On decode, the program checks the ECM3 TOC for `ECMFILE_BLOCK_TYPE_METADATA`
  blocks. If found, it automatically reconstructs a `.cue` file alongside the
  output BIN.
- No decode-side flag is needed; CUE reconstruction is automatic.

#### Split-BIN Input

- When encoding a multi-file CUE sheet (split BINs per track), all referenced
  BIN files are concatenated into a streaming temp file — never held entirely
  in RAM — before encoding.
- Each track's `file_byte_offset` is computed from the concatenated position
  so metadata remains accurate.

#### Split-BIN Output (`-S` / `--split`)

- New `-S` / `--split` option on decode writes individual per-track BIN files
  with sector ranges derived from the embedded metadata.
- The reconstructed `.cue` file uses per-track `FILE` references matching the
  split output filenames (e.g., `basename (Track 01).bin`).

### CUE Parser (`cue_parser.cpp` / `cue_parser.h`)

New standalone CUE sheet parser supporting:

- `FILE` directive with filename and type (BINARY, etc.)
- `TRACK` directive with mode detection (AUDIO, MODE1/2048, MODE1/2352,
  MODE2/2336, MODE2/2352)
- `INDEX` directives with MSF-to-sector conversion
- `FLAGS` (DCP, 4CH, PRE, SCMS)
- `ISRC` per-track
- `CATALOG` disc-level
- `PREGAP` / `POSTGAP`
- Multi-file CUE sheets (tracks referencing different BIN files)

### Track Metadata Format (`track_metadata_header` / `track_metadata_entry`)

Binary-packed structs stored in `ECMFILE_BLOCK_TYPE_METADATA` blocks,
serialized field-by-field using `put16lsb`/`put32lsb` on write and
`get16lsb`/`get32lsb` on read, ensuring `.ecm3` files are portable across
architectures. On-disk format is always little-endian regardless of host byte
order. New `get16lsb`/`put16lsb` helpers added to `sector_tools` alongside the
existing `get32lsb`/`put32lsb`.

```
track_metadata_header (20 bytes):
  uint8_t  version              // METADATA_VERSION = 1
  uint8_t  flags               // METADATA_FLAG_SPLIT = 0x01
  uint16_t track_count
  uint32_t total_sectors
  uint8_t  media_catalog_number[13]

track_metadata_entry (12 bytes, per track):
  uint8_t  track_number
  uint8_t  track_mode           // cue_track_mode enum
  uint8_t  track_flags          // cue_track_flags bitmask
  uint32_t start_sector         // absolute sector in image
  uint32_t end_sector           // absolute sector in image
  uint16_t pregap_sectors
```

### Architecture

- **No filenames in the ECM3 format**: Track metadata stores only sector numbers,
  track modes, flags, and pregap counts — never filenames. This guarantees that
  two users with identical disc content but different BIN filenames produce
  identical `.ecm3` files. The reconstructed `.cue` references the output BIN by
  name, but that name is derived at decode time, not stored in the compressed data.

### Parallel Encoding and Decoding

- Added `-j`/`--jobs` flag (default 0 = auto-detect all cores). Both encoding
  and decoding support it. Parallelism is applied per-stream — each stream is
  processed in its own thread, then results are assembled in fixed stream order.
  Output is bit-identical regardless of `-j` value. A file encoded with `-j 1`
  on a single-core machine produces the same bytes as `-j 16` on a 16-core
  machine, and similarly for decode.

- **Encoding**: When `jobs > 1`, each stream is compressed into an in-memory
  buffer in parallel, and each stream also computes its own partial EDC inline.
  The per-stream partial EDCs are combined using `edc_combine` (GF(2) polynomial
  shift) in stream order — no sequential pre-pass over the input is needed.
  When `jobs <= 1`, the original sequential code path is used.

- **Decoding**: When `jobs > 1`, each stream is decompressed to a temporary file
  in parallel (never held entirely in RAM). After all threads complete, the main
  thread memory-maps each temp file (`CreateFileMapping`/`MapViewOfFile` on
  Windows, `mmap` on POSIX) for zero-copy concatenation to the output and EDC
  computation, then deletes the temp file. Temp files are also cleaned up on error.
  When `jobs <= 1`, the original sequential code path is used.

### EDC Combine (GF(2) Polynomial Shift)

- Added `edc_combine(edc1, edc2, len2)` to `sector_tools` — combines two partial
  EDC values using GF(2) matrix squaring (same approach as `crc32_combine` in zlib).
  This eliminates the need for a separate EDC pre-pass in parallel encode; each
  stream thread computes its own partial EDC, and the main thread combines them
  using polynomial shift via precomputed 2^k-byte matrices.

### EDC Slicing-By-4

- `edc_compute` now processes 4 bytes per iteration using slicing-by-4 lookup
  tables (`edc_lut1`, `edc_lut2`, `edc_lut3`), falling back to the scalar
  byte-at-a-time loop for the last 0–3 bytes. This gives ~4x throughput for
  bulk EDC computation (encode per-stream, decode concatenation pass).
- The slicing tables are derived from the existing `edc_lut` in `eccedc_init()`
  using the standard CRC slicing-by-N method, guaranteeing bit-identical results
  to the scalar loop on all platforms.
- 4-byte chunks are read via `get32lsb` for portable little-endian behavior,
  ensuring identical output on big-endian architectures.

### Thread-Safe Progress Counters

- Replaced non-thread-safe global static progress counters (`mycounter_analyze`,
  `mycounter_encode`, `mycounter_decode`) with `std::atomic<uint64_t>` values
  and a `std::atomic<bool>` mode flag. Parallel encode/decode now spawns a
  display thread that aggregates per-stream `std::atomic<uint64_t>` progress
  values and updates the progress bar every 100ms. Sequential paths use the
  same atomic infrastructure via `setcounter_encode`/`setcounter_decode`.

### Memory-Mapped Input

### Memory-Mapped I/O

- Both sequential and parallel encode now use memory-mapped file I/O
  (`CreateFileMapping`/`MapViewOfFile` on Windows, `mmap` on POSIX) for the
  input file, falling back to `std::ifstream` if mmap fails. This avoids
  per-sector `read()` syscall overhead.

- Parallel decode uses mmap for both the input `.ecm3` file (in
  `decode_single_stream`) and for reading back temp files during the
  concatenation/EDC pass. The temp file merge is now a zero-copy
  `write()` from mapped memory instead of a `read()`/`write()` loop,
  and EDC is computed directly from the mapped buffer.

- The `mmap_file` RAII class encapsulates the platform-specific mmap
  lifecycle (`open`/`unmap`/`data`/`size`/`is_open`).

### Compressor Buffer Sizing

- `BUFFER_SIZE` is now tuned per compressor type: LZMA/LZMA2 use 8 MB,
  LZ4 uses 6 MB, FLAC/ZLIB use 4 MB, and uncompressed uses the default 5 MB.
  Previous builds used a flat 5 MB for all compressors.

### Library Updates

- **xz/liblzma**: Updated from 5.2.5 to 5.8.3; submodule URL changed to
  `github.com/tukaani-project/xz.git`
- **lz4**: Updated from ~1.9.3 to 1.10.0
- **flac**: Updated from ~1.3.3 to 1.5.0; required new source files
  `fixed_intrin_sse42.c` and `win_utf8_io.c`; build flags
  `-DHAVE_FSEEKO -DFLAC__NO_DLL` added for MinGW
- **zlib**: Submodule URL updated to `madler/zlib.git`

### Build System

- Created `build.ps1` PowerShell build script supporting `-Static`, `-Debug`,
  and `-Jobs` flags
- Static-linked Windows exe built via MinGW g++ with `-static` flag
- Added `cue_parser.cpp` to build sources
- Library build instructions:
  - zlib: `make -C zlib -f win32/Makefile.gcc libz.a`
  - lzma: CMake MinGW Makefiles, `BUILD_SHARED_LIBS=OFF`
  - FLAC: gcc with `-DHAVE_FSEEKO -DFLAC__NO_DLL` + required source files

### Performance

- **mmap readahead**: `mmap_file::open()` now calls `PrefetchVirtualMemory`
  (Windows 8+) or `posix_madvise(MADV_SEQUENTIAL)` (POSIX) after mapping,
  hinting the OS to perform larger readahead for sequential access patterns.

- **Parallel disk analysis**: The analyze phase now runs in parallel when
  `-j > 1` and the file can be memory-mapped. Each thread detects sector types
  for a chunk of the input; the main thread then assembles the stream script
  in order, checks optimization flags, and runs PSX ID detection. When `-j 1`
  or mmap is unavailable, the original sequentialifstream path is used.

- **mmap in disk_analyzer**: The analyze phase now uses mmap when available
  (same as encode/decode), falling back to ifstream. This eliminates
  per-sector `read()` syscalls during analysis.

- **`--verify` mode** (`-V`): Decodes the .ecm3 file and validates EDC
  without writing output. Writes to the null device (`NUL` on Windows,
  `/dev/null` on POSIX). Reports "Verification: PASSED" on success.

- **Throughput display**: Encode and decode progress bars now show real-time
  throughput in MB/s (e.g., `Analyze(100%) Encode(50%) 36.6MB/s`,
  `Decode(50%) 311.9MB/s`).

- **Per-stream compression stats**: When streams are compressed, the encode
  summary now includes a "Per-stream compression" section listing each stream's
  type (audio/data), compression method, sector count, input size, output size,
  and compression ratio. Audio and data totals are shown separately. Per-stream
  data is passed from `image_to_ecm_block` to `summary()` via a new output
  parameter `std::vector<stream_script>*`.

- **Corrupt file handling**: Decode now validates header sizes and read
  operations on `.ecm3` files. Corrupted or truncated files produce clear error
  messages instead of crashes:
  - Title/ID length validated (max 256 bytes) to prevent `std::length_error`
  - TOC block count validated (1–1000 range)
  - Streams/sectors TOC header sizes validated (max 100MB)
  - `ifstream::good()` checked after all header and TOC reads
  - Decompressor return values checked in both sequential and parallel decode
    paths — `Z_DATA_ERROR`, `LZMA_DATA_ERROR`, etc. now produce
    `ECMTOOL_CORRUPTED_STREAM` instead of being silently ignored
  - `try/catch` around `ecm_block_to_image()` catches `std::exception` and
    unknown exceptions from corrupt data
  - `--verify` mode no longer tries to remove `NUL`/`/dev/null` on decode errors

- **RAII temp file cleanup**: Temp files for split-BIN concatenation and
  parallel decode streams are now managed by a `temp_file` RAII class that
  automatically deletes the file on scope exit. This eliminates manual
  `fs::remove()` calls on every error path — the destructor handles cleanup
  whether the function returns normally, returns early on error, or throws.
  The `stream_decode_result` struct uses `temp_file` instead of `std::string`
  for its temp path. The `concat_split_bins` function takes `temp_file&`
  instead of `std::string&`.

- **Decompression error checking**: Both sequential and parallel decode paths
  now check return values from `compressor::decompress()`. The helper function
  `is_decompress_error()` recognizes zlib, LZMA, LZ4, and FLAC error codes,
  allowing benign codes (`Z_BUF_ERROR`, `LZMA_BUF_ERROR`) while treating
  data errors as fatal. Previously, decompression errors were silently ignored,
  which could produce corrupted output or crashes on malformed input.

- **SIGINT handling**: `Ctrl+C` during encode/decode now cleans up gracefully:
  partial output files are removed, temp files are deleted, and the process
  exits with a non-zero return code. Previously, `Ctrl+C` could leave a
  broken output file or orphaned tempfiles.

- **Seekable block info in summary**: When `--seekable` is used, the encode
  summary now shows the number of seekable blocks and sector grouping.

- **Improved `--seekable` help text**: The help output now explains that
  seekable mode creates independent compression reset points every N sectors,
  enabling emulators and tools to seek directly to a sector group without
  decompressing the entire stream.

- **FLAC encoder fixes** in `flaczlib.cpp`:
  - Replaced VLA stack allocation (`FLAC__int32 pcm32[samples * channels]`)
    with `std::vector<FLAC__int32>` — the VLA could blow the stack for large
    inputs and is not standard C++
  - Fixed loop iteration count `samples * 2` → `samples * channels` — the
    hardcoded `2` assumed stereo and was incorrect for other channel counts
  - Fixed byte offset calculation `i * channels` → `i * (bits_per_sample / 8)`
    — the original offset assumed 2 channels × 2 bytes equals 2 bytes per
    sample, which only works for 16-bit stereo
  - Fixed 8-bit PCM sign conversion: changed from signed cast to proper
    unsigned-to-signed conversion (`(uint8_t)raw - 128`) for correct
    round-trip with 8-bit audio
  - Fixed `FLAC__stream_encoder_set_blocksize` call order — it was called
    before `set_compression_level`, which overrode the custom blocksize.
    Now called after compression level is set, and only when `block_size > 0`
  - Added explicit `switch` for `FLACZLIB_FULL_FLUSH` and `FLACZLIB_BLOCK`
    flush modes — FLAC does not support mid-stream frame boundaries like
    zlib's `Z_FULL_FLUSH`, so these are documented no-ops; ecm3 handles
    seekability at the block level
- **FLAC blocksize by compression level** in `compressor.cpp`: Instead of
  passing `block_size = 0` (FLAC's defaults: 1152 for levels 0–2, 4608 for
  levels 3–8), now uses 4608 for levels 0–5, 8192 for levels 6–7, and 16384
  for level 8+. These larger blocksizes improve compression ratio on CD audio
  data while remaining within FLAC's streamable subset limits
- **Explicit little-endian serialization** for all ECM block headers and TOC
  structs. Previously, `stream`, `sector`, `block_header`, `blocks_toc`,
  `ecm_header`, and `sec_str_size` were written/read using
  `reinterpret_cast` of packed structs, which relies on implementation-defined
  bitfield layout and native byte order. Now every struct is serialized
  field-by-field using `put16lsb`/`put32lsb`/`put64lsb` on write and
  `get16lsb`/`get32lsb`/`get64lsb` on read. On-disk format sizes are defined
  as explicit constants (`ECM_STREAM_SIZE`, `ECM_SECTOR_SIZE`, etc.)
  independent of `sizeof()`. The `#pragma pack(push, 1)` was removed from
  struct definitions since on-disk layout no longer depends on in-memory
  layout. Added `put64lsb`/`get64lsb` to `sector_tools` for 64-bit fields.
- Fixed `malloc`/`delete[]` mismatch — TOC buffers allocated with
  `malloc`/`calloc` are now freed with `free()` instead of `delete[]`
- Fixed uninitialized memory in `stream` and `sector` bitfield structs
  causing non-deterministic output (`malloc` → `calloc`)
- Fixed double `deflateInit` call in `compress_header` producing
  non-deterministic zlib headers
- Fixed LZ4 HC depth calculation using floating-point arithmetic
  (`1.34 * comp_level` → `(134 * comp_level) / 100`)
- Fixed FLAC `__imp_` link errors by adding `-DFLAC__NO_DLL` to build
- Fixed `--force` and `--keep-output` requiring an argument (changed to boolean
  flags with `no_argument` in getopt)
- Fixed metadata I/O using `reinterpret_cast` of packed structs to byte buffers,
  which produced native byte order on-disk — now uses explicit LE serialization
  for cross-architecture portability
- Fixed `setcounter_encode`/`setcounter_decode` being non-thread-safe global
  statics — progress counters now use `std::atomic` with per-stream tracking
  and a display thread for parallel encode/decode progress bars
- Many typos from original ECM reloaded code, including: 
- Fixed typo `sectors_type_sumary` → `sectors_type_summary` (variable name in
  `ecm3.cpp` and `ecm3.h`)
- Fixed typo `sumpary` → `summary` in encode output header
- Fixed typo `Sumary` → `Summary` in compression output header
- Fixed typo `Mb` → `MB` in the Total line of sector type summary
- Fixed typo `writting` → `writing` in three user-facing error messages
- Fixed typo `extreme-compresison` → `extreme-compression` in source comment
- Fixed typo `bytes_readed` → `bytes_read` (variable name in `ecm3.cpp`,
  `sector_tools.cpp`, and `sector_tools.h` — "readed" is not a word)
- Fixed typo `threated` → `treated` in `sector_tools.h` comment
- Fixed typo `sectors types` → `sector types` in `sector_tools.h` comment
- Fixed typo `wich` → `which` in two `sector_tools.cpp` comments
- Fixed typo `non required ... depending of` → `non-required ... depending on`
  in `sector_tools.cpp` comment
- Fixed typo `splitted` → `split` in `compressor.cpp` comment
- Fixed typo `Writting` → `Writing` in `flaczlib.cpp` comment
- Fixed typo `varible` → `variable` in `flaczlib.cpp` comment
- Fixed grammar `because is causing` → `because it is causing` in
  `flaczlib.cpp` comment
- Fixed typo `temporal variables` → `temporary variables` in `ecm3.cpp` and
  `ecm3_extract_streams.cpp` comments
- Fixed typo `an script` → `a script` in `ecm3.cpp` comment
- Fixed typo `fully readed` → `fully read` and `can be readed` →
  `can be read` in `ecm3_extract_streams.cpp` comments