/*
 * main.c
 * ------
 * Pipeline driver: RLE-1 -> BWT -> MTF -> RLE-2.
 *
 * Usage:
 *   ./bzip2_phase2 <input_file>
 *
 * Steps performed:
 *   1. Load config.ini
 *   2. Run self-tests (RLE-1 + BWT + MTF + RLE-2)
 *   3. Divide input file into blocks
 *   4. Per block: RLE-1 encode -> BWT encode -> MTF encode -> RLE-2 encode
 *   5. Reassemble encoded output
 *   6. Round-trip verification (RLE-2 decode -> MTF decode -> BWT decode
 *      -> RLE-1 decode -> compare)
 *   7. Print metrics
 */

#include "bzip2.h"
#include <time.h>

/* ── utility ────────────────────────────────────────────────── */
static int bytes_equal(const unsigned char *a, size_t alen,
                       const unsigned char *b, size_t blen)
{
    if (alen != blen) return 0;
    return memcmp(a, b, alen) == 0;
}

/* ── RLE-1 self-tests ───────────────────────────────────────── */
static void run_rle1_tests(void)
{
    printf("=== RLE-1 Self-Tests ===\n");

    struct { const char *label; const char *input; } cases[] = {
        { "All unique",        "ABCDEFGH"              },
        { "All same",          "AAAAAAAA"              },
        { "Mixed",             "ABBBCCCCD"             },
        { "Run of exactly 4",  "AAAA"                  },
        { "Run of 5",          "AAAAA"                 },
        { "Long run (20 A's)", "AAAAAAAAAAAAAAAAAAAA"  },
        { "Single byte",       "Z"                     },
        { "Two same",          "XX"                    },
    };
    int n = (int)(sizeof(cases) / sizeof(cases[0]));

    for (int t = 0; t < n; t++) {
        size_t         ilen    = strlen(cases[t].input);
        unsigned char *in      = (unsigned char *)cases[t].input;
        unsigned char *enc     = malloc(ilen * 2 + 8);
        unsigned char *dec     = malloc(ilen + 8);
        size_t         enc_len = 0, dec_len = 0;

        rle1_encode(in, ilen, enc, &enc_len);
        rle1_decode(enc, enc_len, dec, &dec_len);
        int ok = bytes_equal(in, ilen, dec, dec_len);
        printf("  [%s] %-25s  in=%zu enc=%zu dec=%zu  %s\n",
               ok ? "PASS" : "FAIL", cases[t].label,
               ilen, enc_len, dec_len, ok ? "v" : "X");
        free(enc); free(dec);
    }
    printf("========================\n\n");
}

/* ── BWT self-tests ─────────────────────────────────────────── */
static void run_bwt_tests(void)
{
    printf("=== BWT Self-Tests ===\n");

    struct {
        const char *label;
        const char *input;
        const char *expected_L; /* NULL = round-trip only */
        int         expected_pi;/* -1  = round-trip only */
    } cases[] = {
        { "BANANA (classic)",   "BANANA",      "NNBAAA", 3  },
        { "Single char A",      "A",           "A",      0  },
        { "Two same AA",        "AA",          "AA",     0  },
        { "Two diff AB",        "AB",          "BA",     0  },
        { "All same CCCC",      "CCCC",        "CCCC",   0  },
        { "ABRACADABRA",        "ABRACADABRA", NULL,    -1  },
        { "mississippi",        "mississippi", NULL,    -1  },
        { "abcabc",             "abcabc",      NULL,    -1  },
        { "Repeated ABABABAB",  "ABABABAB",    NULL,    -1  },
    };
    int n = (int)(sizeof(cases) / sizeof(cases[0]));
    int total = 0, passed = 0;

    for (int t = 0; t < n; t++) {
        size_t         ilen    = strlen(cases[t].input);
        unsigned char *in      = (unsigned char *)cases[t].input;
        unsigned char *m_out   = malloc(ilen);
        unsigned char *m_dec   = malloc(ilen);
        unsigned char *s_out   = malloc(ilen);
        unsigned char *s_dec   = malloc(ilen);

        /* Matrix method */
        int pi_m = -1;
        bwt_encode(in, ilen, m_out, &pi_m);
        bwt_decode(m_out, ilen, pi_m, m_dec);
        int m_rt  = bytes_equal(in, ilen, m_dec, ilen);
        int m_vec = 1;
        if (cases[t].expected_L)
            m_vec = (memcmp(m_out, cases[t].expected_L, ilen) == 0)
                    && (cases[t].expected_pi < 0 || pi_m == cases[t].expected_pi);
        int m_ok = m_rt && m_vec;

        /* Suffix-array method */
        int pi_s = -1;
        bwt_encode_sa(in, ilen, s_out, &pi_s);
        bwt_decode(s_out, ilen, pi_s, s_dec);
        int s_rt    = bytes_equal(in, ilen, s_dec, ilen);
        int s_match = bytes_equal(m_out, ilen, s_out, ilen) && (pi_s == pi_m);
        int s_ok    = s_rt && s_match;

        printf("  [matrix %-5s] %-22s pi=%-3d  rt=%s vec=%s\n",
               m_ok ? "PASS" : "FAIL", cases[t].label, pi_m,
               m_rt ? "v" : "X", m_vec ? "v" : "X");
        printf("  [sa     %-5s] %-22s pi=%-3d  rt=%s match=%s\n",
               s_ok ? "PASS" : "FAIL", cases[t].label, pi_s,
               s_rt ? "v" : "X", s_match ? "v" : "X");

        total += 2;
        if (m_ok) passed++;
        if (s_ok) passed++;

        free(m_out); free(m_dec); free(s_out); free(s_dec);
    }

    printf("  Result: %d / %d passed\n", passed, total);
    printf("======================\n\n");
}

/* -- MTF self-tests ------------------------------------------- */
static void run_mtf_tests(void)
{
    printf("=== MTF Self-Tests ===\n");

    const char *cases[] = {
        "BANANA",
        "AAAAAA",
        "ABRACADABRA",
        "the quick brown fox jumps over the lazy dog",
        ""
    };

    int total = 0, passed = 0;
    for (int t = 0; cases[t][0] != '\0'; t++) {
        size_t len = strlen(cases[t]);
        unsigned char *in = (unsigned char *)cases[t];
        unsigned char *enc = malloc(len + 1);
        unsigned char *dec = malloc(len + 1);
        size_t enc_len = 0, dec_len = 0;

        mtf_encode(in, len, enc, &enc_len);
        mtf_decode(enc, enc_len, dec, &dec_len);

        int ok = bytes_equal(in, len, dec, dec_len);
        printf("  [%s] %-30s in=%zu enc=%zu dec=%zu\n",
               ok ? "PASS" : "FAIL", cases[t], len, enc_len, dec_len);
        total++;
        if (ok) passed++;

        free(enc);
        free(dec);
    }

    unsigned char all_bytes[256];
    for (int i = 0; i < 256; i++) all_bytes[i] = (unsigned char)i;
    unsigned char enc[256];
    unsigned char dec[256];
    size_t enc_len = 0, dec_len = 0;
    mtf_encode(all_bytes, sizeof(all_bytes), enc, &enc_len);
    mtf_decode(enc, enc_len, dec, &dec_len);
    int ok = bytes_equal(all_bytes, sizeof(all_bytes), dec, dec_len);
    printf("  [%s] %-30s in=%zu enc=%zu dec=%zu\n",
           ok ? "PASS" : "FAIL", "All 256 byte values",
           sizeof(all_bytes), enc_len, dec_len);
    total++;
    if (ok) passed++;

    printf("  Result: %d / %d passed\n", passed, total);
    printf("======================\n\n");
}

/* -- RLE-2 self-tests ----------------------------------------- */
static void run_rle2_tests(void)
{
    printf("=== RLE-2 Self-Tests ===\n");

    unsigned char no_zero[]      = { 1, 2, 3, 4, 5 };
    unsigned char short_zero[]   = { 7, 0, 0, 8, 0, 9 };
    unsigned char long_zero[]    = { 1, 0, 0, 0, 0, 0, 2 };
    unsigned char escape_byte[]  = { 1, 255, 2, 0, 0, 0, 0, 255 };
    unsigned char all_zero[300];
    memset(all_zero, 0, sizeof(all_zero));

    struct {
        const char *label;
        unsigned char *input;
        size_t len;
    } cases[] = {
        { "No zeros", no_zero, sizeof(no_zero) },
        { "Short zero runs", short_zero, sizeof(short_zero) },
        { "Long zero run", long_zero, sizeof(long_zero) },
        { "Literal escape byte", escape_byte, sizeof(escape_byte) },
        { "300 zeros", all_zero, sizeof(all_zero) },
    };

    int total = 0, passed = 0;
    int n = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int t = 0; t < n; t++) {
        unsigned char *enc = malloc(cases[t].len * 2 + 8);
        unsigned char *dec = malloc(cases[t].len + 8);
        size_t enc_len = 0, dec_len = 0;

        rle2_encode(cases[t].input, cases[t].len, enc, &enc_len);
        rle2_decode(enc, enc_len, dec, &dec_len);

        int ok = bytes_equal(cases[t].input, cases[t].len, dec, dec_len);
        printf("  [%s] %-20s in=%zu enc=%zu dec=%zu\n",
               ok ? "PASS" : "FAIL", cases[t].label,
               cases[t].len, enc_len, dec_len);
        total++;
        if (ok) passed++;

        free(enc);
        free(dec);
    }

    printf("  Result: %d / %d passed\n", passed, total);
    printf("========================\n\n");
}

/* ── main ───────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }
    const char *input_file = argv[1];

    /* 1. Config */
    Config *cfg = load_config("config.ini");
    if (!cfg) {
        cfg = calloc(1, sizeof(Config));
        cfg->block_size     = DEFAULT_BLOCK_SIZE;
        cfg->rle1_enabled   = true;
        cfg->mtf_enabled    = true;
        cfg->rle2_enabled   = true;
        cfg->huffman_enabled = true;
        cfg->output_metrics = true;
        strncpy(cfg->bwt_type, "matrix", sizeof(cfg->bwt_type) - 1);
    }
    print_config(cfg);

    /* 2. Self-tests */
    run_rle1_tests();
    run_bwt_tests();
    run_mtf_tests();
    run_rle2_tests();

    /* 3. Block division */
    clock_t t0 = clock();
    BlockManager *manager = divide_into_blocks(input_file, cfg->block_size);
    if (!manager) { free_config(cfg); return 1; }

    /* Storage for round-trip verification */
    unsigned char **originals    = calloc((size_t)manager->num_blocks, sizeof(unsigned char *));
    size_t         *orig_sizes   = calloc((size_t)manager->num_blocks, sizeof(size_t));
    size_t         *bwt_sizes    = calloc((size_t)manager->num_blocks, sizeof(size_t));
    size_t         *mtf_sizes    = calloc((size_t)manager->num_blocks, sizeof(size_t));
    int            *primary_idxs = malloc((size_t)manager->num_blocks * sizeof(int));
    for (int i = 0; i < manager->num_blocks; i++) primary_idxs[i] = -1;

    size_t total_orig = 0, total_rle = 0, total_bwt = 0;
    size_t total_mtf = 0, total_rle2 = 0;

    printf("[pipeline] Processing %d block(s), bwt_type='%s'...\n",
           manager->num_blocks, cfg->bwt_type);

    /* 4. Per-block: RLE-1 -> BWT -> MTF -> RLE-2 */
    for (int i = 0; i < manager->num_blocks; i++) {
        Block *b = &manager->blocks[i];
        if (b->size == 0) continue;

        total_orig += b->original_size;

        /* Save original for verification */
        originals[i]  = malloc(b->size);
        orig_sizes[i] = b->size;
        if (originals[i]) memcpy(originals[i], b->data, b->size);

        /* RLE-1 */
        if (cfg->rle1_enabled) {
            size_t enc_max = b->size + b->size / 4 + 8;
            unsigned char *enc = malloc(enc_max);
            size_t enc_len = 0;
            rle1_encode(b->data, b->size, enc, &enc_len);
            free(b->data);
            b->data = enc;
            b->size = enc_len;
        }
        total_rle += b->size;

        /* BWT */
        unsigned char *bwt_buf = malloc(b->size);
        int pi = -1;
        bwt_encode_auto(cfg->bwt_type, b->data, b->size, bwt_buf, &pi);
        primary_idxs[i] = pi;
        free(b->data);
        b->data = bwt_buf;
        bwt_sizes[i] = b->size;
        total_bwt += b->size;

        /* MTF */
        if (cfg->mtf_enabled) {
            unsigned char *mtf_buf = malloc(b->size + 1);
            size_t mtf_len = 0;
            mtf_encode(b->data, b->size, mtf_buf, &mtf_len);
            free(b->data);
            b->data = mtf_buf;
            b->size = mtf_len;
        }
        mtf_sizes[i] = b->size;
        total_mtf += b->size;

        /* RLE-2 */
        if (cfg->rle2_enabled) {
            size_t enc_max = b->size * 2 + 8;
            unsigned char *enc = malloc(enc_max);
            size_t enc_len = 0;
            rle2_encode(b->data, b->size, enc, &enc_len);
            free(b->data);
            b->data = enc;
            b->size = enc_len;
        }
        total_rle2 += b->size;
    }

    clock_t t1 = clock();

    /* 5. Reassemble */
    char out_path[512];
    if (cfg->rle2_enabled)
        snprintf(out_path, sizeof(out_path), "%s_phase5.rle2", input_file);
    else if (cfg->mtf_enabled)
        snprintf(out_path, sizeof(out_path), "%s_phase4.mtf", input_file);
    else
        snprintf(out_path, sizeof(out_path), "%s_phase2.bwt", input_file);
    reassemble_blocks(manager, out_path);

    clock_t t2 = clock();

    /* 6. Metrics */
    if (cfg->output_metrics) {
        printf("\n[metrics] Original     : %zu bytes\n", total_orig);
        if (cfg->rle1_enabled)
            printf("[metrics] After RLE-1  : %zu bytes  (%.1f%%)\n",
                   total_rle, total_orig ? (double)total_rle/total_orig*100 : 0);
        printf("[metrics] After BWT    : %zu bytes  (length-preserving)\n", total_bwt);
        if (cfg->mtf_enabled)
            printf("[metrics] After MTF    : %zu bytes  (length-preserving)\n", total_mtf);
        if (cfg->rle2_enabled)
            printf("[metrics] After RLE-2  : %zu bytes  (%.1f%%)\n",
                   total_rle2, total_orig ? (double)total_rle2/total_orig*100 : 0);
        printf("[metrics] Pipeline     : %.2f ms\n",
               (double)(t1-t0)/CLOCKS_PER_SEC*1000.0);
        printf("[metrics] Total        : %.2f ms\n",
               (double)(t2-t0)/CLOCKS_PER_SEC*1000.0);
        printf("[metrics] Output       : %s\n", out_path);
    }

    /* 7. Round-trip verification */
    printf("\n[verify] RLE-2-decode -> MTF-decode -> BWT-decode -> RLE-1-decode -> compare original\n");
    int all_ok = 1;

    for (int i = 0; i < manager->num_blocks; i++) {
        Block *b = &manager->blocks[i];
        if (b->size == 0 || !originals[i]) continue;

        unsigned char *stage = NULL;
        size_t stage_len = b->size;

        /* RLE-2 decode */
        if (cfg->rle2_enabled) {
            stage = malloc(mtf_sizes[i] + 8);
            rle2_decode(b->data, b->size, stage, &stage_len);
        } else {
            stage = malloc(b->size + 1);
            memcpy(stage, b->data, b->size);
        }

        if (stage_len != mtf_sizes[i]) {
            printf("  Block %d RLE-2 size mismatch: expected=%zu decoded=%zu  X\n",
                   i, mtf_sizes[i], stage_len);
            all_ok = 0;
        }

        /* MTF decode */
        if (cfg->mtf_enabled) {
            unsigned char *after_mtf = malloc(stage_len + 8);
            size_t after_mtf_len = 0;
            mtf_decode(stage, stage_len, after_mtf, &after_mtf_len);
            free(stage);
            stage = after_mtf;
            stage_len = after_mtf_len;
        }

        if (stage_len != bwt_sizes[i]) {
            printf("  Block %d MTF size mismatch: expected=%zu decoded=%zu  X\n",
                   i, bwt_sizes[i], stage_len);
            all_ok = 0;
        }

        /* BWT decode */
        unsigned char *after_bwt = malloc(stage_len + 4);
        bwt_decode(stage, stage_len, primary_idxs[i], after_bwt);
        free(stage);

        /* RLE-1 decode */
        unsigned char *final;
        size_t final_len;
        if (cfg->rle1_enabled) {
            final = malloc(orig_sizes[i] + 8);
            rle1_decode(after_bwt, stage_len, final, &final_len);
            free(after_bwt);
        } else {
            final     = after_bwt;
            final_len = stage_len;
        }

        int ok = bytes_equal(originals[i], orig_sizes[i], final, final_len);
        if (!ok) {
            printf("  Block %d MISMATCH: orig=%zu decoded=%zu  X\n",
                   i, orig_sizes[i], final_len);
            all_ok = 0;
        }
        free(final);
        free(originals[i]);
    }

    printf("[verify] Round-trip: %s\n", all_ok ? "PASSED v" : "FAILED X");

    free(originals);
    free(orig_sizes);
    free(bwt_sizes);
    free(mtf_sizes);
    free(primary_idxs);
    free_block_manager(manager);
    free_config(cfg);
    return all_ok ? 0 : 1;
}
