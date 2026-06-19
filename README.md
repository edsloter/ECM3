# ecm3

Error Code Modeler 3 — encode and decode CD-ROM disc images into the ECM3 format with sector-level lossless compression, track metadata preservation, and bit-identical output across machines.

Based on the original ECM by Neill Corlett, reloaded by Daniel Carrasco.

For the original readme, see [README_original.md](README_original.md).  
For a full list of changes, see [CHANGELOG.md](CHANGELOG.md).

## GUI

ecm3 ships with a full **wxWidgets 3.2+** graphical interface (`ecm3-gui`) alongside the CLI tool. It provides the same encode/decode, batch, and CUE split/combine functionality without needing command-line flags.

| Platform | Build method | Script |
|---|---|---|
| Windows | MSYS2 MinGW64 (g++ 16.1.0+) | `build_gui.ps1 -Static` |
| Linux | wx-config / wx-config-static | `./build_gui.sh` or `STATIC=1 ./build_gui.sh` |

The GUI is built as a separate executable (`ecm3-gui`) from the same source tree. The CLI `ecm3` executable has no dependency on wxWidgets.

**Windows**: Run `powershell -File build_gui.ps1 -Static` from the project root. Produces a fully static `release/win64_gui/ecm3-gui.exe` (~9.8 MB standalone, no DLL dependencies).

**Linux**: Run `./build_gui.sh` (dynamic, ~1.2 MB + wx DLLs) or `STATIC=1 ./build_gui.sh` (fully static). Requires `wx-config` (or `wx-config-static` for static builds).

### GUI Tabs

| Tab | Function |
|---|---|
| **Encode** | Select BIN or CUE, compression settings, output path. Run encode. |
| **Decode** | Select `.ecm3`, output path, split option. Run decode. |
| **Batch Encode** | Directory + compression settings. Recursively encodes all `.cue` files. |
| **Batch Decode** | Directory. Recursively decodes all `.ecm3` files. |
| **CUE Split/Combine** | Split a combined `.cue`/`.bin` into per-track files, or combine per-track files into a single image. Requires output directory. |
| **Settings** | Register/unregister `.ecm3` file association (Windows). |
| **About** | Version and license info. |

All tabs show real-time progress in a log pane at the bottom of the window.

## Features

- **Lossless** — decoded output is bit-identical to the original image
- **Deterministic** — same disc content always produces the same .ecm3 file, regardless of original filename, machine, run, or number of threads
- **Parallel encoding and decoding** — `-j` flag uses multiple cores; streams are processed independently then assembled in fixed order, guaranteeing bit-identical output with any `-j` value
- **Internal compression** — zlib, ZSTD, LZMA, LZMA2, LZ4, FLAC, and WavPack per-stream
- **CUE sheet support** — embed track metadata on encode; auto-reconstruct `.cue` on decode
- **CUE split/combine utility** — `--split-cue` and `--combine-cue` flags (and GUI tab) for splitting combined images into per-track files or merging per-track files back into a single image. Bit-identical round-trip verified.
- **Split-BIN handling** — concatenate multi-file CUE images on encode; optionally split on decode
- **Auto CUE detection** — pass a `.cue` file as input and the referenced BIN is found automatically
- **Seekable output** — compression reset points every N sectors for fast seeking without full decompression
- **GUI** — wxWidgets-based graphical interface covering all encode, decode, batch, and CUE utility operations

## Usage

### Encode

```
ecm3 -i cdimage.bin
ecm3 -i cdimage.bin -o output.ecm3
ecm3 -i cdimage.bin --cue cdimage.cue
ecm3 -i cdimage.cue                    # auto-detects BIN reference
```

### Decode

```
ecm3 -i cdimage.ecm3
ecm3 -i cdimage.ecm3 -o cdimage.bin
ecm3 -i cdimage.ecm3 -S               # split into per-track BINs + multi-FILE .cue
```

When a `.cue` file contains track metadata, the decoded output automatically gets an accompanying `.cue` file. No extra flags needed.

### CUE Split

```
ecm3 --split-cue combined.cue -o output_dir
```

Splits a combined single-FILE `.cue`/`.bin` into per-track `.bin` files and a multi-FILE `.cue`. Single-track images are a no-op.

### CUE Combine

```
ecm3 --combine-cue split.cue -o output_dir
```

Combines per-track `.bin` files (from a multi-FILE `.cue`) into a single `.bin` and a single-FILE `.cue`. Strips `(Track NN)` suffixes from filenames and adjusts INDEX positions from relative to absolute.

`-f` controls overwrite of files in the output directory.

### Options

| Option | Description |
|---|---|
| `-i` / `--input` | Input file (BIN or ECM3; `.cue` also accepted for encode) |
| `-o` / `--output` | Output file (encode/decode) or output directory (split/combine) |
| `--cue` | CUE sheet for track metadata (encode only; auto-detected if input is `.cue`) |
| `-S` / `--split` | Split output into per-track BIN files with multi-FILE .cue (decode only) |
| `--split-cue` | Split combined `.cue`/`.bin` into per-track files (requires `-o <dir>`) |
| `--combine-cue` | Combine per-track `.cue`/`.bin` into a single image (requires `-o <dir>`) |
| `-a` / `--acompression` | Audio compression: `zlib`, `lzma`, `lzma2`, `lz4`, `flac`, `wavpack` |
| `-d` / `--dcompression` | Data compression: `zlib`, `lzma`, `lzma2`, `lz4`, `zstd` |
| `-c` / `--clevel` | Compression level 0–9 |
| `-e` / `--extreme-compression` | Extreme compression mode for LZMA/FLAC/WavPack (slower, higher ratio) |
| `-s` / `--seekable` | Create a seekable file (reduces compression ratio) |
| `-p` / `--sectors-per-block` | End-of-block mark interval for seekable files (max 255) |
| `-f` / `--force` | Overwrite output file(s) if they exist |
| `-k` / `--keep-output` | Keep output file on error |
| `-j` / `--jobs` | Parallel streams for encoding and decoding (0 = auto-detect cores, default 0) |
| `-V` / `--verify` | Verify `.ecm3` integrity (decode to NUL + EDC check, no output written) |
| `--delete-source` | Delete source file after successful encode/decode |
| `--batch-cue <dir>` | Encode all `.cue` files in a directory tree |
| `--batch-decode <dir>` | Decode all `.ecm3` files in a directory tree |
| `--batch-jobs <n>` | Parallel batch files (0=auto, default 0). Forces `-j` to 1 |

## CUE Sheet Workflow

### Encode

```
ecm3 -i game.bin --cue game.cue -o game.ecm3
```

or simply:

```
ecm3 -i game.cue -o game.ecm3
```

Track metadata (number, mode, pregap, flags, catalog) is embedded in the `.ecm3` file.

### Decode

```
ecm3 -i game.ecm3 -o game.bin
```

A `game.cue` file is written automatically next to the output BIN.

### Decode with split tracks

```
ecm3 -i game.ecm3 -S -o game.bin
```

Writes `game (Track 01).bin`, `game (Track 02).bin`, … and a multi-FILE `.cue`.

### Split a combined image

```
ecm3 --split-cue game.cue -o split_output/
```

Reads the combined `.cue`/`.bin`, writes per-track `.bin` files and a multi-FILE `.cue` to `split_output/`.

### Combine per-track files

```
ecm3 --combine-cue game.cue -o combined/
```

Reads the multi-FILE `.cue` and referenced per-track `.bin` files, writes a single `game.bin` + `game.cue` to `combined/`.

### Round-trip guarantee

All CUE split/combine operations are bit-identical round-trip: combining split tracks and re-splitting produces the exact same per-track files, and vice versa.

## Compression Modes

| Mode | Audio | Data | Notes |
|---|---|---|---|
| zlib | yes | yes | Fast, good compatibility |
| LZMA | yes | yes | High ratio, uses X86 BCJ + LZMA2 |
| LZMA2 | yes | yes | High ratio, no BCJ filter — more deterministic |
| LZ4 | yes | yes | Very fast, lower ratio |
| Zstandard | yes | yes | High ratio, tunable speed/ratio via `-c` |
| FLAC | yes | no | Best ratio for lossless CDDA audio streams |
| WavPack | yes | no | Lossless CDDA compression; hybrid mode disabled, always lossless |

Recommended settings: `-c 9 -e -a flac -d lzma2`

LZMA2 (without the X86 BCJ filter) is recommended for CD-ROM images because it produces bit-identical output across machines, unlike LZMA which applies a platform-sensitive branch filter.

## Parallel Encoding and Decoding

The `-j`/`--jobs` flag controls how many streams are processed in parallel. The default is `0` (auto-detect all CPU cores). Both encoding and decoding support it.

```
ecm3 -i game.bin -a flac -d lzma -j 0    # use all cores (default)
ecm3 -i game.bin -a flac -d lzma -j 1    # single-threaded
ecm3 -i game.bin -a flac -d lzma -j 4    # four streams at once
ecm3 -i game.ecm3 -j 2                   # parallel decode
```

Each stream is compressed or decompressed independently — stream N's output never depends on stream M. Results are assembled in fixed stream order, so the `.ecm3` file is **bit-identical** whether you use `-j 1` on a single-core machine or `-j 16` on a 16-core machine. The same applies to decoding.

Large images are handled without excessive memory: parallel decode streams to temporary files, then concatenates and computes EDC in a single pass.

### Batch-Level Parallelism

When processing multiple files with `--batch-cue` or `--batch-decode`, use
`--batch-jobs <n>` to process files concurrently. In batch mode `-j` is
automatically set to `1` (per-file streams are sequential) — only
`--batch-jobs` controls parallelism, preventing thread oversubscription.

```
ecm3 --batch-cue ./games --batch-jobs 0       # auto-detect CPU count
ecm3 --batch-cue ./games --batch-jobs 8        # up to 8 files at once
ecm3 --batch-decode ./archives --batch-jobs 4  # decode 4 at a time
```

## Building

### Windows (CLI)

```
powershell -File build.ps1 -Static        # release
powershell -File build.ps1 -Static -Debug  # debug
```

Required static libraries: zlib, FLAC, liblzma, WavPack (all in-tree). The build script auto-detects library format (ELF vs PE) and rebuilds from source if needed.

### Windows (GUI)

Requires [MSYS2 MinGW64](https://www.msys2.org/) with wxWidgets 3.2+ installed:

```
pacman -S mingw-w64-x86_64-wxWidgets
```

Then from PowerShell:

```
powershell -File build_gui.ps1 -Static
```

MSYS2 MinGW64 g++ 16.1.0+ is required (system g++ 14.2.0 has a C++ ABI mismatch with MSYS2 wxWidgets DLLs).

Produces `release/win64_gui/ecm3-gui.exe` (fully static, ~9.8 MB).

### Linux (CLI)

```
./build.sh                        # release (dynamic linking)
STATIC=1 ./build.sh               # fully static binary
DEBUG=1 ./build.sh                # debug build
JOBS=8 ./build.sh                 # override parallel jobs
```

### Linux (GUI)

```
./build_gui.sh                     # dynamic linking (requires wxWidgets)
STATIC=1 ./build_gui.sh            # fully static (requires wx-config-static)
```

## File Format

ECM3 files use the `.ecm3` extension. The format stores error-coded sector data in compressed streams with a table-of-contents for random access, and optional metadata blocks for track information. No filenames are stored in the format — only sector-level content and structure — so two users with identical disc content but different filenames will produce identical `.ecm3` files. Files created by older ECM2-format tools (`.ecm2`) are not compatible — use the original ecmtool to decode those first.

## For Danixu's [Original Readme is referenced here](README_original.md).
For detailed information on changes made, please see the [Changelog](CHANGELOG.md).

## License

ecm3 is free software released under the **GNU Affero General Public License, version 3 or later**.

New source files are © 2026 Edward Sloter and licensed under AGPLv3. Modified original files retain the GPLv3 license of the original ecmtool project by Daniel Carrasco, with modifications © 2026 Edward Sloter additionally offered under AGPLv3 as permitted by GPLv3 §13.

See [LICENSE](LICENSE) (AGPLv3) and [LICENSE.GPLv3](LICENSE.GPLv3) (GPLv3, original ecmtool code).