/*
 * config.c
 * --------
 * Parses config.ini into a Config struct.
 * Supports [Section] headers and key = value pairs.
 * Lines beginning with '#' or ';' are treated as comments.
 */

#include "bzip2.h"

/* ── helpers ─────────────────────────────────────────────────── */

/* Trim leading and trailing whitespace in-place; returns the pointer. */
static char *trim(char *s)
{
    /* leading */
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return s;

    /* trailing */
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' ||
                       *end == '\r' || *end == '\n'))
        end--;
    *(end + 1) = '\0';
    return s;
}

static bool parse_bool(const char *val)
{
    return (strcmp(val, "true") == 0 || strcmp(val, "1") == 0 ||
            strcmp(val, "yes")  == 0);
}

/* ── public API ──────────────────────────────────────────────── */

Config *load_config(const char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "[config] Cannot open '%s'\n", filename);
        return NULL;
    }

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) { fclose(fp); return NULL; }

    /* Sensible defaults */
    cfg->block_size    = DEFAULT_BLOCK_SIZE;
    cfg->rle1_enabled  = true;
    cfg->mtf_enabled   = true;
    cfg->rle2_enabled  = true;
    cfg->huffman_enabled = true;
    cfg->benchmark_mode  = false;
    cfg->output_metrics  = true;
    strncpy(cfg->bwt_type,        "matrix",       sizeof(cfg->bwt_type)        - 1);
    strncpy(cfg->input_directory,  "./benchmarks/", sizeof(cfg->input_directory)  - 1);
    strncpy(cfg->output_directory, "./results/",    sizeof(cfg->output_directory) - 1);

    char line[512];
    char section[64] = "";

    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);

        /* Skip blank lines and comments */
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        /* Section header */
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                *end = '\0';
                strncpy(section, p + 1, sizeof(section) - 1);
                trim(section);
            }
            continue;
        }

        /* key = value (strip inline comments) */
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = trim(p);
        char *val = eq + 1;

        /* Strip inline comment from value */
        char *comment = strchr(val, '#');
        if (comment) *comment = '\0';
        val = trim(val);

        /* ── [General] ── */
        if (strcmp(section, "General") == 0) {
            if (strcmp(key, "block_size") == 0) {
                size_t bs = (size_t)strtoul(val, NULL, 10);
                if (bs < MIN_BLOCK_SIZE) bs = MIN_BLOCK_SIZE;
                if (bs > MAX_BLOCK_SIZE) bs = MAX_BLOCK_SIZE;
                cfg->block_size = bs;
            } else if (strcmp(key, "rle1_enabled") == 0) {
                cfg->rle1_enabled = parse_bool(val);
            } else if (strcmp(key, "bwt_type") == 0) {
                strncpy(cfg->bwt_type, val, sizeof(cfg->bwt_type) - 1);
            } else if (strcmp(key, "mtf_enabled") == 0) {
                cfg->mtf_enabled = parse_bool(val);
            } else if (strcmp(key, "rle2_enabled") == 0) {
                cfg->rle2_enabled = parse_bool(val);
            } else if (strcmp(key, "huffman_enabled") == 0) {
                cfg->huffman_enabled = parse_bool(val);
            }
        }

        /* ── [Performance] ── */
        else if (strcmp(section, "Performance") == 0) {
            if (strcmp(key, "benchmark_mode") == 0)
                cfg->benchmark_mode = parse_bool(val);
            else if (strcmp(key, "output_metrics") == 0)
                cfg->output_metrics = parse_bool(val);
        }

        /* ── [Paths] ── */
        else if (strcmp(section, "Paths") == 0) {
            if (strcmp(key, "input_directory") == 0)
                strncpy(cfg->input_directory, val, sizeof(cfg->input_directory) - 1);
            else if (strcmp(key, "output_directory") == 0)
                strncpy(cfg->output_directory, val, sizeof(cfg->output_directory) - 1);
        }
    }

    fclose(fp);
    return cfg;
}

void free_config(Config *cfg)
{
    free(cfg);
}

void print_config(const Config *cfg)
{
    if (!cfg) return;
    printf("=== Configuration ===\n");
    printf("  block_size      : %zu bytes\n",  cfg->block_size);
    printf("  rle1_enabled    : %s\n",          cfg->rle1_enabled    ? "true" : "false");
    printf("  bwt_type        : %s\n",          cfg->bwt_type);
    printf("  mtf_enabled     : %s\n",          cfg->mtf_enabled     ? "true" : "false");
    printf("  rle2_enabled    : %s\n",          cfg->rle2_enabled    ? "true" : "false");
    printf("  huffman_enabled : %s\n",          cfg->huffman_enabled ? "true" : "false");
    printf("  benchmark_mode  : %s\n",          cfg->benchmark_mode  ? "true" : "false");
    printf("  output_metrics  : %s\n",          cfg->output_metrics  ? "true" : "false");
    printf("  input_dir       : %s\n",          cfg->input_directory);
    printf("  output_dir      : %s\n",          cfg->output_directory);
    printf("=====================\n");
}
