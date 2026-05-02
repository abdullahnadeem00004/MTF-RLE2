/*
 * verify.c
 * --------
 * Standalone tool to manually verify the pipeline transforms.
 * Compares two files byte-by-byte and prints a detailed report.
 *
 * Usage:
 *   ./verify <original_file>
 *
 * How to use:
 *   1. Run the main pipeline:      ./bzip2_phase2 sample_input.txt
 *   2. Run the transform verifier: ./verify sample_input.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bzip2.h"

/* Print the first N bytes of a buffer as hex + ASCII (like xxd) */
static void hexdump(const char *label, unsigned char *buf,
                    size_t len, size_t max_show)
{
    printf("\n%s (first %zu of %zu bytes):\n", label,
           len < max_show ? len : max_show, len);
    printf("  Offset   Hex                                      ASCII\n");
    printf("  ------   -------                                  -----\n");

    size_t show = len < max_show ? len : max_show;
    for (size_t i = 0; i < show; i += 16) {
        printf("  %06zu   ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < show) printf("%02X ", buf[i+j]);
            else               printf("   ");
        }
        printf("  |");
        for (size_t j = 0; j < 16 && i + j < show; j++) {
            unsigned char c = buf[i+j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
    if (len > max_show) printf("  ... (%zu more bytes)\n", len - max_show);
}

/* Read entire file into a heap buffer */
static unsigned char *read_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open: %s\n", path); return NULL; }

    fseek(fp, 0, SEEK_END);
    size_t len = (size_t)ftell(fp);
    rewind(fp);

    unsigned char *buf = malloc(len + 1);
    if (fread(buf, 1, len, fp) != len) { free(buf); fclose(fp); return NULL; }
    fclose(fp);
    *out_len = len;
    return buf;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <input_file>\n\n", argv[0]);
        printf("This tool:\n");
        printf("  1. Reads the input file\n");
        printf("  2. Runs RLE-1 -> BWT -> MTF -> RLE-2\n");
        printf("  3. Runs RLE-2 -> MTF -> BWT -> RLE-1 in reverse\n");
        printf("  4. Compares result with original byte-by-byte\n");
        printf("  5. Shows a hex dump of each stage\n");
        return 1;
    }

    /* ── Read input file ──────────────────────────────────── */
    size_t orig_len = 0;
    unsigned char *original = read_file(argv[1], &orig_len);
    if (!original) return 1;

    printf("=================================================\n");
    printf("  BZip2 Pipeline — Manual Verification Tool\n");
    printf("=================================================\n");
    printf("File : %s\n", argv[1]);
    printf("Size : %zu bytes\n\n", orig_len);

    hexdump("STAGE 0 — Original data", original, orig_len, 64);

    /* ── RLE-1 encode ─────────────────────────────────────── */
    size_t rle_max = orig_len + orig_len / 4 + 8;
    unsigned char *rle_enc = malloc(rle_max);
    size_t rle_len = 0;
    rle1_encode(original, orig_len, rle_enc, &rle_len);

    printf("\n-------------------------------------------------\n");
    printf("STAGE 1 — After RLE-1 encode\n");
    printf("  Input : %zu bytes\n", orig_len);
    printf("  Output: %zu bytes\n", rle_len);
    printf("  Saved : %ld bytes (%.1f%% of original)\n",
           (long)orig_len - (long)rle_len,
           (double)rle_len / orig_len * 100.0);
    hexdump("RLE-1 encoded bytes", rle_enc, rle_len, 64);

    /* ── BWT encode ───────────────────────────────────────── */
    unsigned char *bwt_enc = malloc(rle_len);
    int primary_index = -1;
    bwt_encode(rle_enc, rle_len, bwt_enc, &primary_index);

    printf("\n-------------------------------------------------\n");
    printf("STAGE 2 — After BWT encode\n");
    printf("  Input : %zu bytes (RLE output)\n", rle_len);
    printf("  Output: %zu bytes (same — BWT is length-preserving)\n", rle_len);
    printf("  primary_index: %d\n", primary_index);
    printf("  (primary_index is stored alongside data for decoding)\n");
    hexdump("BWT encoded bytes", bwt_enc, rle_len, 64);

    /* ── MTF encode ───────────────────────────────────────── */
    unsigned char *mtf_enc = malloc(rle_len + 1);
    size_t mtf_len = 0;
    mtf_encode(bwt_enc, rle_len, mtf_enc, &mtf_len);

    printf("\n-------------------------------------------------\n");
    printf("STAGE 3 — After MTF encode\n");
    printf("  Input : %zu bytes (BWT output)\n", rle_len);
    printf("  Output: %zu bytes (same — MTF is length-preserving)\n", mtf_len);
    hexdump("MTF encoded bytes", mtf_enc, mtf_len, 64);

    /* ── RLE-2 encode ─────────────────────────────────────── */
    unsigned char *rle2_enc = malloc(mtf_len * 2 + 8);
    size_t rle2_len = 0;
    rle2_encode(mtf_enc, mtf_len, rle2_enc, &rle2_len);

    printf("\n-------------------------------------------------\n");
    printf("STAGE 4 — After RLE-2 encode\n");
    printf("  Input : %zu bytes (MTF output)\n", mtf_len);
    printf("  Output: %zu bytes\n", rle2_len);
    printf("  Saved : %ld bytes (%.1f%% of MTF stream)\n",
           (long)mtf_len - (long)rle2_len,
           mtf_len ? (double)rle2_len / mtf_len * 100.0 : 0.0);
    hexdump("RLE-2 encoded bytes", rle2_enc, rle2_len, 64);

    /* ── RLE-2 decode ─────────────────────────────────────── */
    unsigned char *rle2_dec = malloc(mtf_len + 8);
    size_t rle2_dec_len = 0;
    rle2_decode(rle2_enc, rle2_len, rle2_dec, &rle2_dec_len);

    printf("\n-------------------------------------------------\n");
    printf("STAGE 5 — After RLE-2 decode\n");
    printf("  This should match STAGE 3 output exactly\n");
    hexdump("RLE-2 decoded bytes", rle2_dec, rle2_dec_len, 64);

    int rle2_ok = (rle2_dec_len == mtf_len) &&
                  (memcmp(mtf_enc, rle2_dec, mtf_len) == 0);
    printf("  RLE-2 round-trip check: %s\n",
           rle2_ok ? "PASSED — inv-RLE-2 output == MTF output" :
                     "FAILED — mismatch!");

    /* ── MTF decode ───────────────────────────────────────── */
    unsigned char *mtf_dec = malloc(rle_len + 8);
    size_t mtf_dec_len = 0;
    mtf_decode(rle2_dec, rle2_dec_len, mtf_dec, &mtf_dec_len);

    printf("\n-------------------------------------------------\n");
    printf("STAGE 6 — After MTF decode\n");
    printf("  This should match STAGE 2 output exactly\n");
    hexdump("MTF decoded bytes", mtf_dec, mtf_dec_len, 64);

    int mtf_ok = (mtf_dec_len == rle_len) &&
                 (memcmp(bwt_enc, mtf_dec, rle_len) == 0);
    printf("  MTF round-trip check: %s\n",
           mtf_ok ? "PASSED — inv-MTF output == BWT output" :
                    "FAILED — mismatch!");

    /* ── BWT decode ───────────────────────────────────────── */
    size_t bwt_dec_len = mtf_dec_len;
    unsigned char *bwt_dec = malloc(bwt_dec_len + 4);
    bwt_decode(mtf_dec, mtf_dec_len, primary_index, bwt_dec);

    printf("\n-------------------------------------------------\n");
    printf("STAGE 7 — After BWT decode (inverse BWT)\n");
    printf("  This should match STAGE 1 output exactly\n");
    hexdump("BWT decoded bytes", bwt_dec, bwt_dec_len, 64);

    /* Verify BWT round-trip */
    int bwt_ok = (mtf_dec_len == rle_len) &&
                 (memcmp(rle_enc, bwt_dec, rle_len) == 0);
    printf("  BWT round-trip check: %s\n",
           bwt_ok ? "PASSED — inv-BWT output == RLE-1 output" :
                    "FAILED — mismatch!");

    /* ── RLE-1 decode ─────────────────────────────────────── */
    unsigned char *rle_dec = malloc(orig_len + 8);
    size_t rle_dec_len = 0;
    rle1_decode(bwt_dec, bwt_dec_len, rle_dec, &rle_dec_len);

    printf("\n-------------------------------------------------\n");
    printf("STAGE 8 — After RLE-1 decode (inverse RLE)\n");
    printf("  Input : %zu bytes (inv-BWT output)\n", bwt_dec_len);
    printf("  Output: %zu bytes\n", rle_dec_len);
    printf("  This should match STAGE 0 (the original) exactly\n");
    hexdump("RLE-1 decoded bytes", rle_dec, rle_dec_len, 64);

    /* ── Final comparison ─────────────────────────────────── */
    printf("\n=================================================\n");
    printf("  FINAL VERIFICATION\n");
    printf("=================================================\n");

    int size_ok = (rle_dec_len == orig_len);
    printf("Size match  : %s  (original=%zu, decoded=%zu)\n",
           size_ok ? "YES" : "NO", orig_len, rle_dec_len);

    int content_ok = size_ok && (memcmp(original, rle_dec, orig_len) == 0);
    printf("Content match: %s\n", content_ok ? "YES" : "NO");

    if (!content_ok && size_ok) {
        /* Find and report first mismatch */
        for (size_t i = 0; i < orig_len; i++) {
            if (original[i] != rle_dec[i]) {
                printf("\nFirst mismatch at byte %zu:\n", i);
                printf("  Original : 0x%02X ('%c')\n",
                       original[i],
                       original[i] >= 32 && original[i] < 127 ? original[i] : '?');
                printf("  Decoded  : 0x%02X ('%c')\n",
                       rle_dec[i],
                       rle_dec[i] >= 32 && rle_dec[i] < 127 ? rle_dec[i] : '?');
                break;
            }
        }
    }

    printf("\n");
    if (content_ok)
        printf("  RESULT: PERFECT ROUND-TRIP — original == decoded\n");
    else
        printf("  RESULT: FAILED — data was corrupted!\n");
    printf("=================================================\n");

    /* ── Character frequency analysis ────────────────────── */
    printf("\n--- Character Frequency (original vs BWT output) ---\n");
    printf("Shows how BWT clusters identical bytes together\n\n");

    size_t orig_freq[256] = {0};
    size_t bwt_freq[256]  = {0};
    for (size_t i = 0; i < orig_len; i++) orig_freq[original[i]]++;
    for (size_t i = 0; i < rle_len;  i++) bwt_freq[bwt_enc[i]]++;

    printf("  %-6s  %-12s  %-12s\n", "Char", "Original", "After BWT");
    printf("  %-6s  %-12s  %-12s\n", "------", "--------", "---------");
    for (int c = 0; c < 256; c++) {
        if (orig_freq[c] > 0 || bwt_freq[c] > 0) {
            char label[8];
            if (c >= 32 && c < 127) snprintf(label, sizeof(label), "'%c'", c);
            else                    snprintf(label, sizeof(label), "0x%02X", c);
            printf("  %-6s  %-12zu  %-12zu\n",
                   label, orig_freq[c], bwt_freq[c]);
        }
    }

    /* ── Run-length analysis on BWT output ───────────────── */
    printf("\n--- Run Analysis on BWT output ---\n");
    printf("Longer runs = BWT is doing its job clustering bytes\n\n");

    size_t max_run = 1, cur_run = 1;
    size_t total_runs = 1;
    for (size_t i = 1; i < rle_len; i++) {
        if (bwt_enc[i] == bwt_enc[i-1]) {
            cur_run++;
            if (cur_run > max_run) max_run = cur_run;
        } else {
            cur_run = 1;
            total_runs++;
        }
    }
    printf("  Total runs in BWT output : %zu\n", total_runs);
    printf("  Longest run              : %zu identical bytes\n", max_run);
    printf("  Average run length       : %.2f bytes\n",
           (double)rle_len / total_runs);
    printf("  (For comparison, random data averages ~1.0)\n");

    free(original);
    free(rle_enc);
    free(bwt_enc);
    free(mtf_enc);
    free(rle2_enc);
    free(rle2_dec);
    free(mtf_dec);
    free(bwt_dec);
    free(rle_dec);
    return content_ok ? 0 : 1;
}
