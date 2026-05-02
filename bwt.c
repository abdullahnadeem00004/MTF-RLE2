/*
 * bwt.c
 * -----
 * Burrows-Wheeler Transform for the BZip2 pipeline.
 *
 * Two forward implementations are provided:
 *
 *   bwt_encode()     – Matrix method (spec requirement)
 *                      Creates all n cyclic rotations, sorts them
 *                      lexicographically, emits the last column (L).
 *                      Time: O(n² log n)  Space: O(n) [index-only table]
 *
 *   bwt_encode_sa()  – Suffix array method (config: bwt_type=suffix_array)
 *                      Builds a suffix array with prefix-doubling (Manber &
 *                      Myers style), derives the BWT from it.
 *                      Time: O(n log² n)  Space: O(n)
 *                      Produces IDENTICAL output to bwt_encode().
 *
 * One inverse implementation:
 *
 *   bwt_decode()     – LF-mapping (last-to-first column walk).
 *                      Time: O(n log n)  Space: O(n)
 *                      Works for output of both encode variants.
 *
 * Notation used in comments:
 *   n             = length of input string
 *   L[i]          = last column of the sorted rotation matrix  (BWT output)
 *   F[i]          = first column of the sorted rotation matrix
 *   primary_index = row i such that rotation[i] == original string
 */

#include "bzip2.h"

/* ═══════════════════════════════════════════════════════════════
 *  Helpers shared by both encode variants
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Cyclic character access.
 * rotation starting at index `start` has character at position `pos`:
 *   base[(start + pos) % len]
 */
static inline unsigned char rot_char(const unsigned char *base,
                                     size_t len, int start, size_t pos)
{
    return base[((size_t)start + pos) % len];
}

/* ═══════════════════════════════════════════════════════════════
 *  Matrix-based BWT  (O(n² log n))
 * ═══════════════════════════════════════════════════════════════ */

/*
 * compare_rotations – comparator for qsort.
 *
 * Both a and b are pointers to Rotation structs.  We compare the
 * two cyclic rotations they represent character-by-character without
 * materialising the full rotation strings (saves O(n²) memory).
 */
int compare_rotations(const void *a, const void *b)
{
    const Rotation *ra = (const Rotation *)a;
    const Rotation *rb = (const Rotation *)b;

    size_t len = ra->len;   /* both have the same len */

    for (size_t i = 0; i < len; i++) {
        unsigned char ca = rot_char(ra->base, len, ra->index, i);
        unsigned char cb = rot_char(rb->base, len, rb->index, i);
        if (ca != cb) return (ca < cb) ? -1 : 1;
    }
    return 0;   /* rotations are identical (only possible if all chars same) */
}

/*
 * bwt_encode – forward BWT using the rotation matrix.
 */
void bwt_encode(unsigned char *input, size_t len,
                unsigned char *output, int *primary_index)
{
    if (!input || !output || !primary_index || len == 0) return;

    /* Build rotation table (indices only – no string copies) */
    Rotation *rotations = malloc(len * sizeof(Rotation));
    if (!rotations) {
        fprintf(stderr, "[bwt] Out of memory (rotation table, n=%zu)\n", len);
        return;
    }

    for (size_t i = 0; i < len; i++) {
        rotations[i].index = (int)i;
        rotations[i].base  = input;
        rotations[i].len   = len;
    }

    /* Sort all rotations lexicographically */
    qsort(rotations, len, sizeof(Rotation), compare_rotations);

    /*
     * Extract the last column L and find the primary index.
     * The last character of rotation starting at `start` is
     * input[(start + len - 1) % len].
     */
    *primary_index = -1;
    for (size_t i = 0; i < len; i++) {
        output[i] = input[((size_t)rotations[i].index + len - 1) % len];

        /* The row whose rotation == original string has index 0 start */
        if (rotations[i].index == 0)
            *primary_index = (int)i;
    }

    free(rotations);

    if (*primary_index == -1) {
        fprintf(stderr, "[bwt] primary_index not found – should never happen\n");
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Suffix Array BWT  (O(n log² n))
 *
 *  Algorithm: prefix-doubling (Manber & Myers 1990 style).
 *
 *  A suffix array SA[] stores the starting indices of all suffixes
 *  in lexicographic order.  For cyclic BWT we treat the string as
 *  circular, which is equivalent to appending a sentinel and then
 *  using the suffix array of the doubled string.
 *
 *  Simpler approach used here:
 *    We sort suffix indices with a comparator that wraps around
 *    (cyclic comparison), which gives the same ordering as the
 *    full rotation matrix but in O(n log² n) using std qsort +
 *    a cached rank array.
 *
 *  For the block sizes used in BZip2 (≤900KB ≈ 900K chars) this
 *  is fast enough.  A true O(n) SA-IS construction is left as a
 *  future optimisation.
 * ═══════════════════════════════════════════════════════════════ */

/* Global pointers used by the SA comparator (qsort is not re-entrant) */
static unsigned char *g_sa_base = NULL;
static size_t         g_sa_len  = 0;
static int           *g_rank    = NULL;  /* rank[i] = current rank of suffix i */
static int            g_gap     = 0;     /* current doubling gap               */

/*
 * Comparator for suffix array construction using prefix doubling.
 * Compares two suffixes at positions *a and *b using rank pairs
 * (rank[i], rank[(i+gap)%n]).
 */
static int sa_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    size_t n = g_sa_len;

    /* Compare first half */
    if (g_rank[ia] != g_rank[ib])
        return g_rank[ia] - g_rank[ib];

    /* Compare second half (wrap around for cyclic BWT) */
    int ra2 = g_rank[((size_t)ia + (size_t)g_gap) % n];
    int rb2 = g_rank[((size_t)ib + (size_t)g_gap) % n];
    return ra2 - rb2;
}

void bwt_encode_sa(unsigned char *input, size_t len,
                   unsigned char *output, int *primary_index)
{
    if (!input || !output || !primary_index || len == 0) return;

    int *sa   = malloc(len * sizeof(int));
    int *rank = malloc(len * sizeof(int));
    int *tmp  = malloc(len * sizeof(int));

    if (!sa || !rank || !tmp) {
        fprintf(stderr, "[bwt_sa] Out of memory (n=%zu)\n", len);
        free(sa); free(rank); free(tmp);
        return;
    }

    /* Initialise SA and rank from raw byte values */
    for (size_t i = 0; i < len; i++) {
        sa[i]   = (int)i;
        rank[i] = (int)input[i];
    }

    /* Set up globals for the comparator */
    g_sa_base = input;
    g_sa_len  = len;
    g_rank    = rank;

    /* Prefix doubling: gap = 1, 2, 4, 8, … until ≥ n */
    for (int gap = 1; (size_t)gap < len; gap <<= 1) {
        g_gap = gap;
        qsort(sa, len, sizeof(int), sa_cmp);

        /* Recompute ranks from sorted order */
        tmp[sa[0]] = 0;
        for (size_t i = 1; i < len; i++) {
            tmp[sa[i]] = tmp[sa[i-1]];
            /* Increment rank if this suffix differs from previous */
            int prev = sa[i-1], cur = sa[i];
            if (rank[prev] != rank[cur] ||
                rank[((size_t)prev + (size_t)gap) % len] !=
                rank[((size_t)cur  + (size_t)gap) % len])
                tmp[sa[i]]++;
        }
        memcpy(rank, tmp, len * sizeof(int));

        /* Early exit: all ranks unique means sort is complete */
        if (rank[sa[len-1]] == (int)len - 1) break;
    }

    /* Extract BWT last column and primary index from SA */
    *primary_index = -1;
    for (size_t i = 0; i < len; i++) {
        /* Last char of rotation starting at sa[i] */
        output[i] = input[((size_t)sa[i] + len - 1) % len];
        if (sa[i] == 0)
            *primary_index = (int)i;
    }

    /* Clear globals */
    g_sa_base = NULL;
    g_rank    = NULL;

    free(sa);
    free(rank);
    free(tmp);

    if (*primary_index == -1)
        fprintf(stderr, "[bwt_sa] primary_index not found\n");
}

/* ═══════════════════════════════════════════════════════════════
 *  Dispatcher
 * ═══════════════════════════════════════════════════════════════ */

void bwt_encode_auto(const char *bwt_type,
                     unsigned char *input, size_t len,
                     unsigned char *output, int *primary_index)
{
    if (bwt_type && strcmp(bwt_type, "suffix_array") == 0) {
        bwt_encode_sa(input, len, output, primary_index);
    } else {
        bwt_encode(input, len, output, primary_index);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Inverse BWT – LF-mapping
 *
 *  Given the last column L (= BWT output) and the primary_index,
 *  reconstruct the original string.
 *
 *  The LF-mapping property:
 *    The i-th occurrence of character c in L corresponds to the
 *    i-th occurrence of c in F (first column).
 *
 *  Steps:
 *    1. Build F by counting and sorting L (F is L sorted).
 *    2. Build next[] array: next[i] = the row in the matrix that
 *       the LF-mapping takes row i to.
 *    3. Starting from primary_index, walk next[] n times,
 *       reading F[row] each step → original string in reverse.
 * ═══════════════════════════════════════════════════════════════ */

void bwt_decode(unsigned char *input, size_t len,
                int primary_index, unsigned char *output)
{
    if (!input || !output || len == 0 || primary_index < 0) return;

    /* ── Step 1: frequency count of L ──────────────────────── */
    size_t freq[256] = {0};
    for (size_t i = 0; i < len; i++)
        freq[(unsigned char)input[i]]++;

    /* ── Step 2: cumulative start positions for each byte in F ─ */
    size_t F_start[256] = {0};
    size_t cumulative = 0;
    for (int c = 0; c < 256; c++) {
        F_start[c]  = cumulative;
        cumulative += freq[c];
    }

    /* ── Step 3: LF-mapping array ───────────────────────────── */
    int *lf = malloc(len * sizeof(int));
    if (!lf) {
        fprintf(stderr, "[bwt_decode] Out of memory (lf[], n=%zu)\n", len);
        return;
    }

    size_t occ[256] = {0};
    for (size_t i = 0; i < len; i++) {
        unsigned char c = input[i];
        lf[i] = (int)(F_start[c] + occ[c]);
        occ[c]++;
    }

    /*
     * ── Step 4: reconstruct original string ─────────────────
     *
     * Starting at row = primary_index (which holds the last character
     * of the original string in column L), walk the LF-mapping n times.
     * Each step gives us the PREVIOUS character of the original string,
     * so we fill output from back to front.
     *
     * output[n-1] = L[primary_index]  (last char of original)
     * output[n-2] = L[lf[primary_index]]
     * ...
     * output[0]   = last character recovered
     */
    int row = primary_index;
    for (size_t i = 0; i < len; i++) {
        output[len - 1 - i] = input[row];
        row = lf[row];
    }

    free(lf);
}
