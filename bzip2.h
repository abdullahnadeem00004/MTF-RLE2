#ifndef BZIP2_H
#define BZIP2_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ─────────────────────────────────────────────────────────────
 *  Constants
 * ───────────────────────────────────────────────────────────── */
#define MIN_BLOCK_SIZE  100000   /* 100 KB */
#define MAX_BLOCK_SIZE  900000   /* 900 KB */
#define DEFAULT_BLOCK_SIZE 500000

/* ─────────────────────────────────────────────────────────────
 *  Configuration
 * ───────────────────────────────────────────────────────────── */
typedef struct {
    size_t  block_size;
    bool    rle1_enabled;
    char    bwt_type[32];        /* "matrix" or "suffix_array" */
    bool    mtf_enabled;
    bool    rle2_enabled;
    bool    huffman_enabled;

    bool    benchmark_mode;
    bool    output_metrics;

    char    input_directory[256];
    char    output_directory[256];
} Config;

/* ─────────────────────────────────────────────────────────────
 *  Block structures
 * ───────────────────────────────────────────────────────────── */

/*
 * Structure to hold a single block of data
 */
typedef struct {
    unsigned char *data;         /* Pointer to block data          */
    size_t         size;         /* Current size of block          */
    size_t         original_size;/* Original size before compression*/
} Block;

/*
 * Structure to manage multiple blocks
 */
typedef struct {
    Block  *blocks;              /* Array of blocks                */
    int     num_blocks;          /* Number of blocks               */
    size_t  block_size;          /* Configurable block size        */
} BlockManager;

/* ─────────────────────────────────────────────────────────────
 *  BWT structures
 * ───────────────────────────────────────────────────────────── */

/*
 * Used by the matrix-based BWT to sort cyclic rotations.
 * Stores only the starting index — avoids O(n²) memory for copies.
 */
typedef struct {
    int            index;        /* Starting position of this rotation */
    unsigned char *base;         /* Pointer back to the original data  */
    size_t         len;          /* Length of the string               */
} Rotation;

/*
 * Bundles the BWT output with its primary index so the pipeline
 * can pass encode results cleanly to the decode stage.
 */
typedef struct {
    unsigned char *data;         /* BWT last-column output         */
    size_t         len;          /* Length (== input length)       */
    int            primary_index;/* Row of the original string     */
} BWTResult;

/* ─────────────────────────────────────────────────────────────
 *  config.c
 * ───────────────────────────────────────────────────────────── */
Config *load_config(const char *filename);
void    free_config(Config *cfg);
void    print_config(const Config *cfg);

/* ─────────────────────────────────────────────────────────────
 *  block.c  –  Block Division & File Handling
 * ───────────────────────────────────────────────────────────── */

/*
 * Reads input file and divides into blocks.
 * @param filename   : Input file path
 * @param block_size : Size of each block in bytes
 * @return           : BlockManager containing all blocks, or NULL on error
 */
BlockManager *divide_into_blocks(const char *filename, size_t block_size);

/*
 * Reassembles blocks back into the original file.
 * @param manager         : BlockManager containing processed blocks
 * @param output_filename : Path for the output file
 * @return                : 0 on success, -1 on failure
 */
int reassemble_blocks(BlockManager *manager, const char *output_filename);

/*
 * Frees all memory allocated for a BlockManager.
 * @param manager : Pointer to BlockManager to free
 */
void free_block_manager(BlockManager *manager);

/* ─────────────────────────────────────────────────────────────
 *  rle1.c  –  Run-Length Encoding (Stage 1 / pass 1)
 * ───────────────────────────────────────────────────────────── */

/*
 * Encodes data using BZip2-style first-pass RLE.
 * Runs of 1-3 bytes are emitted literally. Runs of 4-258 bytes are
 * stored as four literal bytes plus one extra-count byte.
 *
 * @param input   : Input byte array
 * @param len     : Length of input array
 * @param output  : Output buffer (must be pre-allocated)
 * @param out_len : Pointer to store output length
 */
void rle1_encode(unsigned char *input, size_t len,
                 unsigned char *output, size_t *out_len);

/*
 * Decodes RLE-1 encoded data.
 * @param input   : Encoded byte array
 * @param len     : Length of encoded data
 * @param output  : Output buffer for decoded data
 * @param out_len : Pointer to store decoded length
 */
void rle1_decode(unsigned char *input, size_t len,
                 unsigned char *output, size_t *out_len);

/* Alternative simple count+byte scheme */
void rle1_encode_simple(unsigned char *input, size_t len,
                        unsigned char *output, size_t *out_len);
void rle1_decode_simple(unsigned char *input, size_t len,
                        unsigned char *output, size_t *out_len);

/* ─────────────────────────────────────────────────────────────
 *  mtf.c  –  Move-to-Front Transform
 * ───────────────────────────────────────────────────────────── */

/*
 * Forward MTF transform.
 * Length-preserving: output length equals input length.
 */
void mtf_encode(unsigned char *input, size_t len,
                unsigned char *output, size_t *out_len);

/*
 * Inverse MTF transform.
 * Length-preserving: output length equals input length.
 */
void mtf_decode(unsigned char *input, size_t len,
                unsigned char *output, size_t *out_len);

/* ─────────────────────────────────────────────────────────────
 *  rle2.c  –  Run-Length Encoding (Stage 2 / zero runs after MTF)
 * ───────────────────────────────────────────────────────────── */

/*
 * Encodes runs of zero MTF values using an escape-byte format.
 * Output buffer should be at least (2 * len + 8) bytes.
 */
void rle2_encode(unsigned char *input, size_t len,
                 unsigned char *output, size_t *out_len);

/*
 * Decodes the RLE-2 escape-byte format.
 * Output buffer must be large enough for the decoded MTF stream.
 */
void rle2_decode(unsigned char *input, size_t len,
                 unsigned char *output, size_t *out_len);

/* ─────────────────────────────────────────────────────────────
 *  bwt.c  –  Burrows-Wheeler Transform
 * ───────────────────────────────────────────────────────────── */

/*
 * Compares two Rotation entries for qsort.
 * Performs a full cyclic comparison using only the stored index.
 * @param a, b : pointers to Rotation structs
 * @return     : negative / 0 / positive
 */
int compare_rotations(const void *a, const void *b);

/*
 * Forward BWT – matrix method.
 * Builds all n cyclic rotations, sorts them, returns last column.
 * O(n² log n) time, O(n) extra space (index-only rotation table).
 *
 * @param input         : Input byte array
 * @param len           : Length of input
 * @param output        : Pre-allocated buffer for BWT output (size == len)
 * @param primary_index : Set to the row index of the original string
 */
void bwt_encode(unsigned char *input, size_t len,
                unsigned char *output, int *primary_index);

/*
 * Forward BWT – suffix array method (O(n log n)).
 * Produces identical output to bwt_encode; faster for large blocks.
 *
 * @param input         : Input byte array
 * @param len           : Length of input
 * @param output        : Pre-allocated buffer for BWT output (size == len)
 * @param primary_index : Set to the row index of the original string
 */
void bwt_encode_sa(unsigned char *input, size_t len,
                   unsigned char *output, int *primary_index);

/*
 * Inverse BWT.
 * Uses the LF-mapping (last-to-first) technique.
 * O(n log n) time (one sort), O(n) extra space.
 *
 * @param input         : BWT-encoded byte array (last column L)
 * @param len           : Length of encoded data
 * @param primary_index : Primary index from encoding
 * @param output        : Pre-allocated buffer for recovered original data
 */
void bwt_decode(unsigned char *input, size_t len,
                int primary_index, unsigned char *output);

/*
 * Dispatcher: calls bwt_encode or bwt_encode_sa based on bwt_type string.
 * @param bwt_type : "matrix" or "suffix_array"
 */
void bwt_encode_auto(const char *bwt_type,
                     unsigned char *input, size_t len,
                     unsigned char *output, int *primary_index);

#endif /* BZIP2_H */
