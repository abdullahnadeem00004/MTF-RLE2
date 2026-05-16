/*
 * main.c
 * ------
 * Pipeline driver: RLE-1 -> BWT -> MTF -> RLE-2 -> ANS.
 *
 * Usage:
 *   ./bzip2_phase2 <input_file>
 *
 * Steps performed:
 *   1. Load config.ini
 *   2. Run self-tests (RLE-1 + BWT + MTF + RLE-2 + ANS)
 *   3. Divide input file into blocks
 *   4. Per block: RLE-1 -> BWT -> MTF -> RLE-2 -> ANS encode
 *   5. Reassemble encoded output
 *   6. Round-trip verification (ANS -> RLE-2 -> MTF -> BWT -> RLE-1 decode)
 *   7. Print metrics
 */

#include "bzip2.h"
#include <time.h>
#include <stdint.h>

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
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s <input_file>              (full pipeline + verify)\n", argv[0]);
        fprintf(stderr, "  %s -c <input_file>           (encode only)\n", argv[0]);
        fprintf(stderr, "  %s -d <compressed_file>      (decode only)\n", argv[0]);
        return 1;
    }

    /* Parse command-line flags */
    int encode_only = 0;
    int decode_only = 0;
    const char *input_file = NULL;

    if (strcmp(argv[1], "-c") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s -c <input_file>\n", argv[0]);
            return 1;
        }
        encode_only = 1;
        input_file = argv[2];
    } else if (strcmp(argv[1], "-d") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s -d <compressed_file>\n", argv[0]);
            return 1;
        }
        decode_only = 1;
        input_file = argv[2];
    } else {
        input_file = argv[1];
    }

    /* 1. Config */
    Config *cfg = load_config("config.ini");
    if (!cfg) {
        cfg = calloc(1, sizeof(Config));
        cfg->block_size     = DEFAULT_BLOCK_SIZE;
        cfg->rle1_enabled   = true;
        cfg->mtf_enabled    = true;
        cfg->rle2_enabled   = true;
        cfg->ans_enabled = true;
        cfg->output_metrics = true;
        strncpy(cfg->bwt_type, "matrix", sizeof(cfg->bwt_type) - 1);
    }
    
    /* Skip config/test printing in encode/decode only mode */
    if (!encode_only && !decode_only) {
        print_config(cfg);

        /* 2. Self-tests */
        run_rle1_tests();
        run_bwt_tests();
        run_mtf_tests();
        run_rle2_tests();
        run_ans_tests();
    }

    /* ENCODE-ONLY MODE */
    if (encode_only) {
        printf("[encode] Compressing: %s\n", input_file);
        
        /* 3. Block division */
        clock_t t0 = clock();
        BlockManager *manager = divide_into_blocks(input_file, cfg->block_size);
        if (!manager) { free_config(cfg); return 1; }

        size_t total_orig = 0, total_ans = 0;
        int *primary_idxs = (int*)malloc(manager->num_blocks * sizeof(int));
        for (int i = 0; i < manager->num_blocks; i++) primary_idxs[i] = -1;

        printf("[pipeline] Processing %d block(s), bwt_type='%s'...\n",
               manager->num_blocks, cfg->bwt_type);

        /* Show pipeline configuration */
        printf("[stages] Stage 1 (RLE-1)  : %s\n", cfg->rle1_enabled ? "ENABLED" : "DISABLED");
        printf("[stages] Stage 2 (BWT)    : ENABLED (always required)\n");
        printf("[stages] Stage 3 (MTF)    : %s\n", cfg->mtf_enabled ? "ENABLED" : "DISABLED");
        printf("[stages] Stage 4 (RLE-2)  : %s\n", cfg->rle2_enabled ? "ENABLED" : "DISABLED");
        printf("[stages] Stage 5 (ANS)    : %s\n", cfg->ans_enabled ? "ENABLED" : "DISABLED");
        printf("\n");

        /* Stage 1: RLE-1 */
        for (int i = 0; i < manager->num_blocks; i++) {
            Block *b = &manager->blocks[i];
            if (b->size == 0) continue;
            total_orig += b->original_size;
            if (cfg->rle1_enabled) {
                size_t enc_max = b->size + b->size / 4 + 8;
                unsigned char *enc = malloc(enc_max);
                size_t enc_len = 0;
                rle1_encode(b->data, b->size, enc, &enc_len);
                free(b->data);
                b->data = enc;
                b->size = enc_len;
            }
        }

        /* Stage 2: BWT */
        for (int i = 0; i < manager->num_blocks; i++) {
            Block *b = &manager->blocks[i];
            if (b->size == 0) continue;
            unsigned char *bwt_buf = malloc(b->size);
            int pi = -1;
            bwt_encode_auto(cfg->bwt_type, b->data, b->size, bwt_buf, &pi);
            primary_idxs[i] = pi;
            free(b->data);
            b->data = bwt_buf;
        }

        /* Stage 3: MTF */
        if (cfg->mtf_enabled) {
            for (int i = 0; i < manager->num_blocks; i++) {
                Block *b = &manager->blocks[i];
                if (b->size == 0) continue;
                unsigned char *mtf_buf = malloc(b->size + 1);
                size_t mtf_len = 0;
                mtf_encode(b->data, b->size, mtf_buf, &mtf_len);
                free(b->data);
                b->data = mtf_buf;
                b->size = mtf_len;
            }
        }

        /* Stage 4: RLE-2 */
        if (cfg->rle2_enabled) {
            for (int i = 0; i < manager->num_blocks; i++) {
                Block *b = &manager->blocks[i];
                if (b->size == 0) continue;
                size_t enc_max = b->size * 2 + 8;
                unsigned char *enc = malloc(enc_max);
                size_t enc_len = 0;
                rle2_encode(b->data, b->size, enc, &enc_len);
                free(b->data);
                b->data = enc;
                b->size = enc_len;
            }
        }

        /* Stage 5: ANS */
        if (cfg->ans_enabled) {
            for (int i = 0; i < manager->num_blocks; i++) {
                Block *b = &manager->blocks[i];
                if (b->size == 0) continue;
                unsigned char *ans_buf = NULL;
                size_t ans_len = 0;
                if (ans_encode(b->data, b->size, &ans_buf, &ans_len) == 0) {
                    free(b->data);
                    b->data = ans_buf;
                    b->size = ans_len;
                    total_ans += b->size;
                }
            }
        }

        clock_t t1 = clock();

        /* Write output with metadata for decoding */
        char out_path[512];
        snprintf(out_path, sizeof(out_path), "%s.ans", input_file);
        
        FILE *out_fp = fopen(out_path, "wb");
        if (!out_fp) {
            fprintf(stderr, "[encode] Cannot open output file: %s\n", out_path);
            free(primary_idxs);
            free_block_manager(manager);
            free_config(cfg);
            return 1;
        }

        /* Write metadata header */
        unsigned char header[16];
        uint32_t magic = 0x5A495042; /* "BZIP" */
        uint32_t num_blocks = (uint32_t)manager->num_blocks;
        
        header[0] = (unsigned char)(magic & 0xFF);
        header[1] = (unsigned char)((magic >> 8) & 0xFF);
        header[2] = (unsigned char)((magic >> 16) & 0xFF);
        header[3] = (unsigned char)((magic >> 24) & 0xFF);
        
        header[4] = (unsigned char)(total_orig & 0xFF);
        header[5] = (unsigned char)((total_orig >> 8) & 0xFF);
        header[6] = (unsigned char)((total_orig >> 16) & 0xFF);
        header[7] = (unsigned char)((total_orig >> 24) & 0xFF);
        
        header[8] = (unsigned char)(num_blocks & 0xFF);
        header[9] = (unsigned char)((num_blocks >> 8) & 0xFF);
        header[10] = (unsigned char)((num_blocks >> 16) & 0xFF);
        header[11] = (unsigned char)((num_blocks >> 24) & 0xFF);
        
        header[12] = 0; header[13] = 0; header[14] = 0; header[15] = 0;
        
        if (fwrite(header, 1, 16, out_fp) != 16) {
            fprintf(stderr, "[encode] Write error\n");
            fclose(out_fp);
            free(primary_idxs);
            free_block_manager(manager);
            free_config(cfg);
            return 1;
        }

        /* Write block metadata and data */
        for (int i = 0; i < manager->num_blocks; i++) {
            Block *b = &manager->blocks[i];
            
            unsigned char block_header[12];
            int32_t pi = primary_idxs[i];
            uint32_t bs = (uint32_t)b->size;
            
            block_header[0] = (unsigned char)(pi & 0xFF);
            block_header[1] = (unsigned char)((pi >> 8) & 0xFF);
            block_header[2] = (unsigned char)((pi >> 16) & 0xFF);
            block_header[3] = (unsigned char)((pi >> 24) & 0xFF);
            
            block_header[4] = (unsigned char)(bs & 0xFF);
            block_header[5] = (unsigned char)((bs >> 8) & 0xFF);
            block_header[6] = (unsigned char)((bs >> 16) & 0xFF);
            block_header[7] = (unsigned char)((bs >> 24) & 0xFF);
            
            block_header[8] = 0; block_header[9] = 0;
            block_header[10] = 0; block_header[11] = 0;
            
            if (fwrite(block_header, 1, 12, out_fp) != 12) {
                fprintf(stderr, "[encode] Write error\n");
                fclose(out_fp);
                free(primary_idxs);
                free_block_manager(manager);
                free_config(cfg);
                return 1;
            }
            
            if (fwrite(b->data, 1, b->size, out_fp) != b->size) {
                fprintf(stderr, "[encode] Write error\n");
                fclose(out_fp);
                free(primary_idxs);
                free_block_manager(manager);
                free_config(cfg);
                return 1;
            }
        }
        
        fclose(out_fp);

        /* Metrics */
        printf("\n[metrics] Original size : %zu bytes\n", total_orig);
        printf("[metrics] Compressed size: %zu bytes\n", total_ans);
        printf("[metrics] Ratio         : %.1f%%\n", total_orig ? (double)total_ans/total_orig*100 : 0);
        printf("[metrics] Time          : %.2f ms\n", (double)(t1-t0)/CLOCKS_PER_SEC*1000.0);
        printf("[encode] Output saved to: %s\n", out_path);

        free(primary_idxs);
        free_block_manager(manager);
        free_config(cfg);
        return 0;
    }

    /* DECODE-ONLY MODE */
    if (decode_only) {
        printf("[decode] Decompressing: %s\n", input_file);
        
        clock_t t0 = clock();
        
        /* Read the compressed file */
        FILE *fp = fopen(input_file, "rb");
        if (!fp) {
            fprintf(stderr, "[decode] Cannot open file: %s\n", input_file);
            free_config(cfg);
            return 1;
        }

        /* Read metadata header */
        unsigned char header[16];
        if (fread(header, 1, 16, fp) != 16) {
            fprintf(stderr, "[decode] Invalid file format\n");
            fclose(fp);
            free_config(cfg);
            return 1;
        }

        /* Parse metadata */
        uint32_t magic = ((uint32_t)header[0]) | (((uint32_t)header[1])<<8) | 
                         (((uint32_t)header[2])<<16) | (((uint32_t)header[3])<<24);
        uint32_t total_orig_size = ((uint32_t)header[4]) | (((uint32_t)header[5])<<8) | 
                                   (((uint32_t)header[6])<<16) | (((uint32_t)header[7])<<24);
        uint32_t num_blocks = ((uint32_t)header[8]) | (((uint32_t)header[9])<<8) | 
                              (((uint32_t)header[10])<<16) | (((uint32_t)header[11])<<24);

        if (magic != 0x5A495042) { /* "BZIP" in little-endian */
            fprintf(stderr, "[decode] Invalid magic number\n");
            fclose(fp);
            free_config(cfg);
            return 1;
        }

        printf("[decode] Format: %d block(s), %u bytes total\n", num_blocks, total_orig_size);

        /* Read and decompress blocks */
        unsigned char *output_buffer = (unsigned char*)malloc(total_orig_size + 8);
        size_t output_pos = 0;

        for (uint32_t i = 0; i < num_blocks; i++) {
            /* Read block metadata */
            unsigned char block_header[12];
            if (fread(block_header, 1, 12, fp) != 12) {
                fprintf(stderr, "[decode] Cannot read block %u metadata\n", i);
                free(output_buffer);
                fclose(fp);
                free_config(cfg);
                return 1;
            }
            
            int32_t primary_idx = ((int32_t)block_header[0]) | (((int32_t)block_header[1])<<8) | 
                                  (((int32_t)block_header[2])<<16) | (((int32_t)block_header[3])<<24);
            uint32_t block_size = ((uint32_t)block_header[4]) | (((uint32_t)block_header[5])<<8) | 
                                  (((uint32_t)block_header[6])<<16) | (((uint32_t)block_header[7])<<24);

            /* Read compressed block data */
            unsigned char *compressed = (unsigned char*)malloc(block_size);
            if (fread(compressed, 1, block_size, fp) != block_size) {
                fprintf(stderr, "[decode] Cannot read block %u data\n", i);
                free(compressed);
                free(output_buffer);
                fclose(fp);
                free_config(cfg);
                return 1;
            }

            /* Decompress: ANS -> RLE-2 -> MTF -> BWT -> RLE-1 */
            unsigned char *stage = NULL;
            size_t stage_len = block_size;
            int stage_is_compressed = 0;  /* Track if stage points to compressed */

            /* ANS decode */
            if (cfg->ans_enabled) {
                unsigned char *ans_out = NULL;
                size_t ans_out_len = 0;
                if (ans_decode(compressed, stage_len, &ans_out, &ans_out_len) != 0) {
                    fprintf(stderr, "[decode] ANS decode failed on block %u\n", i);
                    free(compressed);
                    free(output_buffer);
                    fclose(fp);
                    free_config(cfg);
                    return 1;
                }
                stage = ans_out;
                stage_len = ans_out_len;
                stage_is_compressed = 0;
            } else {
                stage = compressed;
                stage_is_compressed = 1;
            }

            /* RLE-2 decode */
            if (cfg->rle2_enabled) {
                unsigned char *r2 = (unsigned char*)malloc(stage_len * 10 + 8);
                size_t r2_len = 0;
                rle2_decode(stage, stage_len, r2, &r2_len);
                if (!stage_is_compressed) free(stage);
                stage = r2;
                stage_len = r2_len;
                stage_is_compressed = 0;
            }

            /* MTF decode */
            if (cfg->mtf_enabled) {
                unsigned char *after_mtf = (unsigned char*)malloc(stage_len + 8);
                size_t after_mtf_len = 0;
                mtf_decode(stage, stage_len, after_mtf, &after_mtf_len);
                if (!stage_is_compressed) free(stage);
                stage = after_mtf;
                stage_len = after_mtf_len;
                stage_is_compressed = 0;
            }

            /* BWT decode */
            unsigned char *after_bwt = (unsigned char*)malloc(stage_len + 4);
            bwt_decode(stage, stage_len, primary_idx, after_bwt);
            if (!stage_is_compressed) free(stage);

            /* RLE-1 decode */
            unsigned char *final;
            size_t final_len;
            if (cfg->rle1_enabled) {
                final = (unsigned char*)malloc(stage_len * 2 + 8);
                rle1_decode(after_bwt, stage_len, final, &final_len);
                free(after_bwt);
            } else {
                final = after_bwt;
                final_len = stage_len;
            }

            /* Copy to output buffer */
            if (output_pos + final_len > total_orig_size) {
                fprintf(stderr, "[decode] Output buffer overflow\n");
                free(final);
                free(compressed);
                free(output_buffer);
                fclose(fp);
                free_config(cfg);
                return 1;
            }
            memcpy(output_buffer + output_pos, final, final_len);
            output_pos += final_len;
            free(final);
            free(compressed);
        }

        fclose(fp);

        clock_t t1 = clock();

        /* Write output file */
        char out_file[512];
        snprintf(out_file, sizeof(out_file), "%s.recovered", input_file);
        
        FILE *out_fp = fopen(out_file, "wb");
        if (!out_fp) {
            fprintf(stderr, "[decode] Cannot write output file: %s\n", out_file);
            free(output_buffer);
            free_config(cfg);
            return 1;
        }
        
        if (fwrite(output_buffer, 1, output_pos, out_fp) != output_pos) {
            fprintf(stderr, "[decode] Write error\n");
            fclose(out_fp);
            free(output_buffer);
            free_config(cfg);
            return 1;
        }
        fclose(out_fp);
        free(output_buffer);

        printf("[decode] Decompressed %zu bytes\n", output_pos);
        printf("[decode] Output saved to: %s\n", out_file);
        printf("[decode] Time: %.2f ms\n", (double)(t1-t0)/CLOCKS_PER_SEC*1000.0);
        free_config(cfg);
        return 0;
    }

    /* 3. Block division */
    clock_t t0 = clock();
    BlockManager *manager = divide_into_blocks(input_file, cfg->block_size);
    if (!manager) { free_config(cfg); return 1; }

    /* Storage for round-trip verification */
    unsigned char **originals    = calloc((size_t)manager->num_blocks, sizeof(unsigned char *));
    size_t         *orig_sizes   = calloc((size_t)manager->num_blocks, sizeof(size_t));
    size_t         *bwt_sizes    = calloc((size_t)manager->num_blocks, sizeof(size_t));
    size_t         *mtf_sizes    = calloc((size_t)manager->num_blocks, sizeof(size_t));
    size_t         *rle2_sizes   = calloc((size_t)manager->num_blocks, sizeof(size_t));
    int            *primary_idxs = malloc((size_t)manager->num_blocks * sizeof(int));
    for (int i = 0; i < manager->num_blocks; i++) primary_idxs[i] = -1;

    size_t total_orig = 0, total_rle = 0, total_bwt = 0;
    size_t total_mtf = 0, total_rle2 = 0, total_ans = 0;

    printf("[pipeline] Processing %d block(s), bwt_type='%s'...\n",
           manager->num_blocks, cfg->bwt_type);

    /* 4. Per-block stage-by-stage processing with intermediate file output */

    /* Stage 0: Save originals */
    for (int i = 0; i < manager->num_blocks; i++) {
        Block *b = &manager->blocks[i];
        if (b->size == 0) continue;

        total_orig += b->original_size;
        originals[i]  = malloc(b->size);
        orig_sizes[i] = b->size;
        if (originals[i]) memcpy(originals[i], b->data, b->size);
    }

    /* Stage 1: RLE-1 Encoding */
    printf("[stage] RLE-1 encoding...\n");
    for (int i = 0; i < manager->num_blocks; i++) {
        Block *b = &manager->blocks[i];
        if (b->size == 0) continue;

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
    }
    char out_rle1[512];
    snprintf(out_rle1, sizeof(out_rle1), "%s_stage1_rle1.bin", input_file);
    reassemble_blocks(manager, out_rle1);
    printf("  → Saved: %s\n", out_rle1);

    /* Stage 2: BWT Encoding */
    printf("[stage] BWT encoding...\n");
    for (int i = 0; i < manager->num_blocks; i++) {
        Block *b = &manager->blocks[i];
        if (b->size == 0) continue;

        unsigned char *bwt_buf = malloc(b->size);
        int pi = -1;
        bwt_encode_auto(cfg->bwt_type, b->data, b->size, bwt_buf, &pi);
        primary_idxs[i] = pi;
        free(b->data);
        b->data = bwt_buf;
        bwt_sizes[i] = b->size;
        total_bwt += b->size;
    }
    char out_bwt[512];
    snprintf(out_bwt, sizeof(out_bwt), "%s_stage2_bwt.bin", input_file);
    reassemble_blocks(manager, out_bwt);
    printf("  → Saved: %s\n", out_bwt);

    /* Stage 3: MTF Encoding */
    printf("[stage] MTF encoding...\n");
    for (int i = 0; i < manager->num_blocks; i++) {
        Block *b = &manager->blocks[i];
        if (b->size == 0) continue;

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
    }
    char out_mtf[512];
    snprintf(out_mtf, sizeof(out_mtf), "%s_stage3_mtf.bin", input_file);
    reassemble_blocks(manager, out_mtf);
    printf("  → Saved: %s\n", out_mtf);

    /* Stage 4: RLE-2 Encoding */
    printf("[stage] RLE-2 encoding...\n");
    for (int i = 0; i < manager->num_blocks; i++) {
        Block *b = &manager->blocks[i];
        if (b->size == 0) continue;

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
        rle2_sizes[i] = b->size;
    }
    char out_rle2[512];
    snprintf(out_rle2, sizeof(out_rle2), "%s_stage4_rle2.bin", input_file);
    reassemble_blocks(manager, out_rle2);
    printf("  → Saved: %s\n", out_rle2);

    /* Stage 5: ANS Encoding */
    printf("[stage] ANS encoding...\n");
    for (int i = 0; i < manager->num_blocks; i++) {
        Block *b = &manager->blocks[i];
        if (b->size == 0) continue;

        if (cfg->ans_enabled) {
            unsigned char *ans_buf = NULL;
            size_t ans_len = 0;
            if (ans_encode(b->data, b->size, &ans_buf, &ans_len) != 0) {
                fprintf(stderr, "[ANS] encode failed on block %d\n", i);
            } else {
                free(b->data);
                b->data = ans_buf;
                b->size = ans_len;
            }
        }
        total_ans += b->size;
    }
    char out_ans[512];
    snprintf(out_ans, sizeof(out_ans), "%s_stage5_ans.bin", input_file);
    reassemble_blocks(manager, out_ans);
    printf("  → Saved: %s\n", out_ans);

    clock_t t1 = clock();

    /* 5. Reassemble (Note: intermediate stage files already created above) */
    /* Create a final summary output file */
    char out_path[512];
    if (cfg->ans_enabled)
        snprintf(out_path, sizeof(out_path), "%s_final.ans", input_file);
    else if (cfg->rle2_enabled)
        snprintf(out_path, sizeof(out_path), "%s_final.rle2", input_file);
    else if (cfg->mtf_enabled)
        snprintf(out_path, sizeof(out_path), "%s_final.mtf", input_file);
    else
        snprintf(out_path, sizeof(out_path), "%s_final.bwt", input_file);
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
        if (cfg->ans_enabled)
            printf("[metrics] After ANS    : %zu bytes  (%.1f%%)\n",
                   total_ans, total_orig ? (double)total_ans/total_orig*100 : 0);
        printf("[metrics] Pipeline     : %.2f ms\n",
               (double)(t1-t0)/CLOCKS_PER_SEC*1000.0);
        printf("[metrics] Total        : %.2f ms\n",
               (double)(t2-t0)/CLOCKS_PER_SEC*1000.0);
        printf("\n[files] Intermediate stage files created:\n");
        printf("        %s_stage1_rle1.bin\n", input_file);
        printf("        %s_stage2_bwt.bin\n", input_file);
        printf("        %s_stage3_mtf.bin\n", input_file);
        printf("        %s_stage4_rle2.bin\n", input_file);
        printf("        %s_stage5_ans.bin\n", input_file);
        printf("        %s (final copy)\n", out_path);
    }

    /* 7. Round-trip verification */
    printf("\n[verify] ANS-decode -> RLE-2-decode -> MTF-decode -> BWT-decode -> RLE-1-decode -> compare\n");
    int all_ok = 1;

    for (int i = 0; i < manager->num_blocks; i++) {
        Block *b = &manager->blocks[i];
        if (b->size == 0 || !originals[i]) continue;

        unsigned char *stage = NULL;
        size_t stage_len = b->size;

        /* ANS decode */
        if (cfg->ans_enabled) {
            unsigned char *ans_out = NULL;
            size_t ans_out_len = 0;
            if (ans_decode(b->data, b->size, &ans_out, &ans_out_len) != 0) {
                printf("  Block %d ANS decode failed  X\n", i);
                all_ok = 0;
                continue;
            }
            stage = ans_out;
            stage_len = ans_out_len;
        } else {
            stage = malloc(b->size + 1);
            memcpy(stage, b->data, b->size);
            stage_len = b->size;
        }

        if (stage_len != rle2_sizes[i]) {
            printf("  Block %d ANS size mismatch: expected=%zu decoded=%zu  X\n",
                   i, rle2_sizes[i], stage_len);
            all_ok = 0;
        }

        /* RLE-2 decode */
        if (cfg->rle2_enabled) {
            unsigned char *r2 = malloc(mtf_sizes[i] + 8);
            size_t r2_len = 0;
            rle2_decode(stage, stage_len, r2, &r2_len);
            free(stage);
            stage = r2;
            stage_len = r2_len;
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
    free(rle2_sizes);
    free(primary_idxs);
    free_block_manager(manager);
    free_config(cfg);
    return all_ok ? 0 : 1;
}
