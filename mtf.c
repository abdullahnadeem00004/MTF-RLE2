/*
 * mtf.c
 * -----
 * Move-to-Front transform for the BZip2-style pipeline.
 *
 * MTF is length-preserving. It replaces each input byte with the current
 * position of that byte in a 256-entry list, then moves the byte to the
 * front. After BWT clusters equal bytes together, MTF turns many repeated
 * bytes into zeros, preparing the stream for RLE-2.
 */

#include "bzip2.h"

#define MTF_ALPHABET_SIZE 256

static void mtf_init(unsigned char table[MTF_ALPHABET_SIZE])
{
    for (int i = 0; i < MTF_ALPHABET_SIZE; i++)
        table[i] = (unsigned char)i;
}

static void mtf_move_to_front(unsigned char table[MTF_ALPHABET_SIZE], int pos)
{
    unsigned char value = table[pos];
    for (int i = pos; i > 0; i--)
        table[i] = table[i - 1];
    table[0] = value;
}

void mtf_encode(unsigned char *input, size_t len,
                unsigned char *output, size_t *out_len)
{
    if (!input || !output || !out_len) return;

    unsigned char table[MTF_ALPHABET_SIZE];
    mtf_init(table);

    for (size_t i = 0; i < len; i++) {
        unsigned char c = input[i];
        int pos = 0;

        while (pos < MTF_ALPHABET_SIZE && table[pos] != c)
            pos++;

        output[i] = (unsigned char)pos;
        mtf_move_to_front(table, pos);
    }

    *out_len = len;
}

void mtf_decode(unsigned char *input, size_t len,
                unsigned char *output, size_t *out_len)
{
    if (!input || !output || !out_len) return;

    unsigned char table[MTF_ALPHABET_SIZE];
    mtf_init(table);

    for (size_t i = 0; i < len; i++) {
        int pos = (int)input[i];
        unsigned char c = table[pos];

        output[i] = c;
        mtf_move_to_front(table, pos);
    }

    *out_len = len;
}
