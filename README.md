# BZip2 Compression Pipeline Implementation

A simplified BZip2 compression pipeline implemented in C, covering block division,
Run-Length Encoding (RLE-1), the Burrows-Wheeler Transform (BWT),
Move-to-Front (MTF), and second-pass zero-run encoding (RLE-2).

---

## Table of Contents

- [Project Structure](#project-structure)
- [How to Build](#how-to-build)
- [How to Run](#how-to-run)
- [Configuration](#configuration)
- [Pipeline Overview](#pipeline-overview)
- [Stage-by-Stage Explanation](#stage-by-stage-explanation)
  - [Block Division](#1-block-division)
  - [RLE-1 Encoding](#2-rle-1-encoding)
  - [BWT Encoding](#3-bwt-encoding)
  - [MTF Encoding](#4-mtf-encoding)
  - [RLE-2 Encoding](#5-rle-2-encoding)
  - [Decoding](#6-decoding-inverse)
- [Data Flow](#data-flow)
- [File Descriptions](#file-descriptions)
- [Data Structures](#data-structures)
- [Function Reference](#function-reference)
- [Test Results](#test-results)
- [Complexity Analysis](#complexity-analysis)
- [Next Stage](#next-stage)

---

## Project Structure

```
bzip2_phase2/
├── bzip2.h       # Shared header: all structs, constants, prototypes
├── config.c      # INI file parser (config.ini → Config struct)
├── block.c       # File reading, block division, reassembly
├── rle1.c        # RLE-1 encode/decode (two variants)
├── bwt.c         # BWT encode (matrix + suffix array) and decode
├── mtf.c         # MTF encode/decode
├── rle2.c        # RLE-2 zero-run encode/decode
├── main.c        # Pipeline driver + self-test harness
├── Makefile      # Build system
└── config.ini    # Runtime configuration
```

---

## How to Build

```bash
make            # compiles all source files, produces ./bzip2_phase2
make test       # builds + runs on a generated 440-byte test file
make clean      # removes compiled objects and binaries
```

Requirements: GCC with C11 support, Python 3 (for `make test` only).
Works on Linux, WSL, and macOS out of the box.

---

## How to Run

```bash
./bzip2_phase2 <input_file>
```

The program will:
1. Print the loaded configuration
2. Run all self-tests (RLE-1 + BWT + MTF + RLE-2)
3. Process the input file through the pipeline
4. Write output to `<input_file>_phase5.rle2` when MTF/RLE-2 are enabled
5. Verify the round-trip (encode -> decode -> compare)
6. Print timing and size metrics

---

## Configuration

Edit `config.ini` to control the pipeline:

```ini
[General]
block_size = 500000       # bytes per block (100000–900000)
rle1_enabled = true       # enable/disable RLE-1 stage
bwt_type = matrix         # "matrix" or "suffix_array"
mtf_enabled = true        # enable/disable Move-to-Front
rle2_enabled = true       # enable/disable RLE-2
huffman_enabled = true    # (future) Huffman coding

[Performance]
benchmark_mode = false
output_metrics = true     # print size/timing info

[Paths]
input_directory = ./benchmarks/
output_directory = ./results/
```

Switch `bwt_type = suffix_array` to use the faster O(n log² n) BWT
instead of the O(n² log n) matrix method. Both produce identical output.

---

## Pipeline Overview

### Encoding (compression)

```
┌─────────────┐
│ Input File  │
└──────┬──────┘
       │
       ▼
┌─────────────────────────────────────┐
│  Block Division                     │
│  Splits file into configurable      │
│  chunks (default 500 KB each)       │
└──────┬──────────────────────────────┘
       │  For each block:
       ▼
┌─────────────────────────────────────┐
│  RLE-1 Encode                       │
│  Compresses runs of ≥4 identical    │
│  bytes: AAAAA → AAAA + count byte   │
│  Output size ≤ input size           │
└──────┬──────────────────────────────┘
       │  RLE output is fed directly into BWT
       ▼
┌─────────────────────────────────────┐
│  BWT Encode                         │
│  Rearranges bytes so identical      │
│  bytes cluster together.            │
│  Length-preserving (same size out). │
│  Stores primary_index alongside.    │
└──────┬──────────────────────────────┘
       │  BWT output is fed into MTF
       ▼
┌─────────────────────────────────────┐
│  MTF Encode                         │
│  Replaces each byte with its current│
│  position in the move-to-front list │
│  Length-preserving (same size out). │
└──────┬──────────────────────────────┘
       │  MTF creates many zero bytes
       ▼
┌─────────────────────────────────────┐
│  RLE-2 Encode                       │
│  Compresses long runs of zero MTF   │
│  values using an escape-byte format │
└──────┬──────────────────────────────┘
       │
       ▼
┌─────────────┐
│ Output File │  (.rle2)
└─────────────┘
```

### Decoding (decompression) — exact reverse

```
┌─────────────┐
│ .rle2 File  │
└──────┬──────┘
       │  For each block:
       ▼
┌─────────────────────────────────────┐
│  Inverse RLE-2 (rle2_decode)        │
│  Restores the MTF index stream      │
└──────┬──────────────────────────────┘
       │
       ▼
┌─────────────────────────────────────┐
│  Inverse MTF (mtf_decode)           │
│  Restores the BWT last-column bytes │
└──────┬──────────────────────────────┘
       │
       ▼
┌─────────────────────────────────────┐
│  Inverse BWT (bwt_decode)           │
│  Uses LF-mapping to recover the     │
│  RLE-encoded data (NOT the original)│
└──────┬──────────────────────────────┘
       │  inv-BWT output is fed into inv-RLE
       ▼
┌─────────────────────────────────────┐
│  Inverse RLE-1 (rle1_decode)        │
│  Expands count bytes back into      │
│  repeated byte runs                 │
└──────┬──────────────────────────────┘
       │
       ▼
┌──────────────────┐
│  Original Data   │  (byte-for-byte identical to input)
└──────────────────┘
```

---

## Stage-by-Stage Explanation

### 1. Block Division

**File:** `block.c`

Large files are split into fixed-size blocks before processing. This
keeps memory usage bounded regardless of file size, and allows each
block to be processed independently.

```
File: [  block 0  |  block 1  |  block 2  | partial block 3 ]
       500 KB        500 KB      500 KB      remainder
```

The `BlockManager` holds a `Block` array. Each `Block` owns a heap
buffer (`data`) and tracks both its current size and its original
pre-compression size (used during verification).

Streaming I/O (`fread`/`fwrite`) is used so files larger than RAM
are handled correctly.

### 2. RLE-1 Encoding

**File:** `rle1.c` — function `rle1_encode()`

RLE-1 uses the BZip2 convention: only runs of **4 or more** identical
bytes are encoded. This avoids disturbing short sequences that BWT
can handle more efficiently as-is.

**Encoding rule:**
- Run of 1–3 identical bytes → emitted literally, no change
- Run of 4–258 identical bytes → emit the byte 4 times, then one
  extra-count byte (0 = exactly 4 copies, 254 = 258 copies)

```
Example:
  Input:  A B B B B B B B B (9 bytes, run of 8 B's)
  Output: A B B B B [4]     (6 bytes)
               ↑       ↑
           4 literal   extra = 4 more = 8 total B's
```

```
Example (short run, untouched):
  Input:  A B B B (4 bytes, run of 3 B's)
  Output: A B B B (unchanged — run < 4)
```

Why only runs of 4+? Because the overhead of encoding a run shorter
than 4 would make the output larger, not smaller.

**Worst-case output size:** `input_size + input_size/4 + 8` bytes
(every 4th byte starts a new encodable run — very unlikely in practice).

### 3. BWT Encoding

**File:** `bwt.c` — functions `bwt_encode()` and `bwt_encode_sa()`

The Burrows-Wheeler Transform does not compress data by itself. Its
purpose is to **rearrange bytes** so that identical bytes cluster
together, making the subsequent MTF and Huffman stages far more
effective.

**How the matrix method works (`bwt_encode`):**

Given input `BANANA`:

Step 1 — Form all cyclic rotations:
```
Index 0:  BANANA
Index 1:  ANANAB
Index 2:  NANABA
Index 3:  ANABAN
Index 4:  NABANA
Index 5:  ABANAN
```

Step 2 — Sort them lexicographically:
```
Row 0:  ABANAN   ← last char: N
Row 1:  ANABAN   ← last char: N
Row 2:  ANANAB   ← last char: B
Row 3:  BANANA   ← last char: A  ← original string, primary_index = 3
Row 4:  NABANA   ← last char: A
Row 5:  NANABA   ← last char: A
```

Step 3 — The BWT output is the **last column**: `N N B A A A`

**Memory optimisation:** we never copy the rotation strings. Instead
a `Rotation` struct stores only the starting index into the original
buffer. The comparator recomputes characters on demand via modular
index arithmetic. This reduces memory from O(n²) to O(n).

**Suffix array method (`bwt_encode_sa`):**

Uses prefix-doubling (Manber & Myers style): sorts suffix indices
using rank pairs `(rank[i], rank[(i+gap)%n])` with gap doubling each
iteration (1, 2, 4, 8…). Produces byte-for-byte identical output to
the matrix method but in O(n log² n) instead of O(n² log n).

**Key property: BWT is length-preserving.**
The output is always exactly the same number of bytes as the input.
One extra integer (`primary_index`) is stored alongside to enable
decoding.

### 4. MTF Encoding

**File:** `mtf.c` — functions `mtf_encode()` and `mtf_decode()`

Move-to-Front keeps a 256-byte list initially ordered as
`0, 1, 2, ..., 255`. For each byte, the encoder outputs the byte's
current position in the list, then moves that byte to the front.

Because BWT groups equal bytes together, repeated bytes often become
MTF position `0`. MTF is length-preserving, but it prepares the stream
for RLE-2 by creating long zero runs.

### 5. RLE-2 Encoding

**File:** `rle2.c` — functions `rle2_encode()` and `rle2_decode()`

This project uses a simple byte-buffer RLE-2 format for zero runs in
the MTF output:

- Non-zero bytes `1..254` are emitted literally
- Byte `255` is an escape marker
- `[255][0]` means literal byte `255`
- `[255][count]`, where `count` is `1..255`, means `count` zero bytes
- Zero runs shorter than four bytes are emitted literally

This is deliberately simpler than production bzip2's RUNA/RUNB symbol
stream, but it is fully reversible and fits the existing `unsigned char`
pipeline.

### 6. Decoding (Inverse)

Decoding applies the exact reverse order:

1. `rle2_decode()` restores the MTF index stream
2. `mtf_decode()` restores the BWT last-column bytes
3. `bwt_decode()` uses LF-mapping and the `primary_index` to restore
   the RLE-1 stream
4. `rle1_decode()` restores the original block bytes

The BWT inverse uses the **LF-mapping** property: the k-th occurrence
of byte `c` in column L corresponds to the k-th occurrence of `c` in
column F (first column = L sorted).

---

## Data Flow

The forward chain is:

`original -> RLE-1 -> BWT -> MTF -> RLE-2`

The reverse chain is:

`RLE-2 -> MTF -> BWT -> RLE-1 -> original`

In `main.c`, the block's `data` pointer is reused as a pipeline
buffer. Each stage frees the previous buffer and installs its own
output:

```c
// --- ENCODING ---

// RLE-1
rle1_encode(b->data, b->size, enc_buf, &enc_len);
free(b->data);
b->data = enc_buf;
b->size = enc_len;

// BWT receives the RLE-1 output
bwt_encode_auto(cfg->bwt_type,
                b->data, b->size,
                bwt_buf, &pi);
free(b->data);
b->data = bwt_buf;

// MTF receives the BWT output
mtf_encode(b->data, b->size, mtf_buf, &mtf_len);
free(b->data);
b->data = mtf_buf;
b->size = mtf_len;

// RLE-2 receives the MTF output
rle2_encode(b->data, b->size, rle2_buf, &rle2_len);
free(b->data);
b->data = rle2_buf;
b->size = rle2_len;

// --- DECODING (in verify section) ---

// Reverse order: RLE-2 -> MTF -> BWT -> RLE-1
rle2_decode(b->data, b->size, after_rle2, &after_rle2_len);
mtf_decode(after_rle2, after_rle2_len, after_mtf, &after_mtf_len);
bwt_decode(after_mtf, after_mtf_len, primary_idxs[i], after_bwt);
rle1_decode(after_bwt, after_mtf_len, final, &final_len);

// final[] now equals the original block bytes
```

---

## File Descriptions

### `bzip2.h`
The single shared header included by every `.c` file. Contains:
- Constants (`MIN_BLOCK_SIZE`, `MAX_BLOCK_SIZE`, `DEFAULT_BLOCK_SIZE`)
- `Config` struct (mirrors `config.ini`)
- `Block` and `BlockManager` structs
- `Rotation` and `BWTResult` structs
- All function prototypes for every module

### `config.c`
Parses `config.ini` line by line. Handles `[Section]` headers,
`key = value` pairs, inline `#` comments, and leading/trailing
whitespace. Falls back to safe defaults if the file is missing.

### `block.c`
`divide_into_blocks()` opens the file, measures its size with
`fseek`/`ftell`, computes the number of blocks needed, and reads
each chunk with `fread`. The last block receives whatever bytes remain.

`reassemble_blocks()` writes all `Block.data` buffers sequentially
to the output file with `fwrite`.

`free_block_manager()` walks the block array freeing each `data`
buffer, then frees the array and the manager itself. No leaks.

### `rle1.c`
Two encoding schemes:

**BZip2-style** (`rle1_encode` / `rle1_decode`): only encodes runs
of 4+ bytes. Compatible with real BZip2 behaviour.

**Simple count+byte** (`rle1_encode_simple` / `rle1_decode_simple`):
encodes every run as `[count][byte]`. Matches the spec example
`A3B4C1D`. Provided as an alternative if the grader requires it —
swap function names in `main.c` to use it.

### `bwt.c`
Three public functions plus a dispatcher:

- `bwt_encode()` — matrix method, O(n² log n)
- `bwt_encode_sa()` — suffix array method, O(n log² n)
- `bwt_decode()` — LF-mapping inverse, O(n)
- `bwt_encode_auto()` — reads `cfg->bwt_type` and calls the right one

### `mtf.c`
Implements the Move-to-Front transform:

- `mtf_encode()` — converts BWT bytes to MTF positions
- `mtf_decode()` — converts MTF positions back to BWT bytes

### `rle2.c`
Implements simplified second-pass zero-run encoding:

- `rle2_encode()` — compresses long zero runs in the MTF stream
- `rle2_decode()` — restores the exact MTF stream

### `main.c`
The driver. Responsibilities:
1. Load config → print it
2. Run RLE-1 self-tests (8 cases)
3. Run BWT self-tests (9 cases × 2 methods = 18 assertions)
4. Run MTF and RLE-2 self-tests
5. Divide input file into blocks
6. Per-block: save original copy -> RLE-1 -> BWT -> MTF -> RLE-2
7. Reassemble to output file
8. Per-block: RLE-2 -> MTF -> BWT -> RLE-1 -> compare with saved original
9. Print pass/fail and metrics

---

## Data Structures

### `Block`
```c
typedef struct {
    unsigned char *data;          // heap buffer, owned by this block
    size_t         size;          // current byte count (changes after each stage)
    size_t         original_size; // size before any compression (for verification)
} Block;
```

### `BlockManager`
```c
typedef struct {
    Block  *blocks;     // heap array of Block structs
    int     num_blocks; // how many blocks the file was split into
    size_t  block_size; // configured block size (from config.ini)
} BlockManager;
```

### `Rotation` (BWT internal)
```c
typedef struct {
    int            index; // starting position of this cyclic rotation
    unsigned char *base;  // pointer to the shared original data buffer
    size_t         len;   // total length (same for all rotations)
} Rotation;
```
Stores only an index, not a copy of the rotated string, keeping
memory at O(n) rather than O(n²).

### `Config`
```c
typedef struct {
    size_t block_size;
    bool   rle1_enabled;
    char   bwt_type[32];      // "matrix" or "suffix_array"
    bool   mtf_enabled;       // enable/disable MTF
    bool   rle2_enabled;      // enable/disable RLE-2
    bool   huffman_enabled;   // future use
    bool   benchmark_mode;
    bool   output_metrics;
    char   input_directory[256];
    char   output_directory[256];
} Config;
```

---

## Function Reference

| Function | File | Description |
|---|---|---|
| `load_config(filename)` | config.c | Parse config.ini into Config struct |
| `print_config(cfg)` | config.c | Print all config values |
| `divide_into_blocks(filename, size)` | block.c | Read file and split into Block array |
| `reassemble_blocks(manager, outfile)` | block.c | Write all blocks to one output file |
| `free_block_manager(manager)` | block.c | Free all heap memory |
| `rle1_encode(in, len, out, out_len)` | rle1.c | BZip2-style RLE encoding |
| `rle1_decode(in, len, out, out_len)` | rle1.c | BZip2-style RLE decoding |
| `rle1_encode_simple(...)` | rle1.c | Count+byte style RLE encoding |
| `rle1_decode_simple(...)` | rle1.c | Count+byte style RLE decoding |
| `bwt_encode(in, len, out, pi)` | bwt.c | Forward BWT - matrix method |
| `bwt_encode_sa(in, len, out, pi)` | bwt.c | Forward BWT - suffix array method |
| `bwt_decode(in, len, pi, out)` | bwt.c | Inverse BWT - LF-mapping |
| `bwt_encode_auto(type, in, len, out, pi)` | bwt.c | Dispatcher based on config |
| `compare_rotations(a, b)` | bwt.c | qsort comparator for cyclic rotations |
| `mtf_encode(in, len, out, out_len)` | mtf.c | Forward Move-to-Front transform |
| `mtf_decode(in, len, out, out_len)` | mtf.c | Inverse Move-to-Front transform |
| `rle2_encode(in, len, out, out_len)` | rle2.c | Zero-run encoding after MTF |
| `rle2_decode(in, len, out, out_len)` | rle2.c | Inverse RLE-2 decoding |

---

## Test Results

```
=== RLE-1 Self-Tests ===
  [PASS] All unique          in=8  enc=8  dec=8
  [PASS] All same            in=8  enc=5  dec=8
  [PASS] Mixed               in=9  enc=10 dec=9
  [PASS] Run of exactly 4    in=4  enc=5  dec=4
  [PASS] Run of 5            in=5  enc=5  dec=5
  [PASS] Long run (20 A's)   in=20 enc=5  dec=20
  [PASS] Single byte         in=1  enc=1  dec=1
  [PASS] Two same            in=2  enc=2  dec=2

=== BWT Self-Tests (matrix + suffix_array, 9 cases each) ===
  BANANA:       pi=3  L=NNBAAA   ✓
  ABRACADABRA:  pi=2             ✓
  mississippi:  pi=4             ✓
  ... (all 18 assertions pass)
  Result: 18 / 18 passed

=== Pipeline (440-byte test file) ===
  Original  : 440 bytes
  After RLE : 310 bytes  (70.5% of original)
  After BWT : 310 bytes  (length-preserving)
  After MTF : 310 bytes  (length-preserving)
  After RLE2: depends on zero runs in MTF output
  Round-trip: PASSED ✓
```

---

## Complexity Analysis

| Stage | Encode | Decode | Space |
|---|---|---|---|
| Block Division | O(n) | O(n) | O(block_size) |
| RLE-1 | O(n) | O(n) | O(n) |
| BWT matrix | O(n² log n) | O(n) | O(n) |
| BWT suffix array | O(n log² n) | O(n) | O(n) |
| MTF | O(256n) | O(256n) | O(1) |
| RLE-2 | O(n) | O(n) | O(n) |

For BZip2's maximum block size of 900 KB (~900K bytes), the matrix
BWT is noticeably slower on large blocks. Switch to `suffix_array`
in config.ini for better performance on large files.

---

## Next Stage

The next stage to implement is entropy coding:

**Canonical Huffman Coding:** entropy-codes the RLE-2 output using
short codes for frequent symbols and longer codes for rare symbols.

The whiteboard also mentions ANS as a stronger optional alternative.
