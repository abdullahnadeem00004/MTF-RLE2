/*
 * rle2.c
 * ------
 * Second run-length pass for the BZip2-style pipeline.
 *
 * This simplified RLE-2 targets runs of zero bytes produced by MTF.
 * Format:
 *   - Non-zero bytes 1..254 are emitted literally.
 *   - Byte 255 is an escape marker.
 *   - [255][0] means literal byte 255.
 *   - [255][count] where count is 1..255 means count zero bytes.
 *   - Short zero runs of 1..3 are emitted literally to avoid expansion.
 *
 * This is not the exact RUNA/RUNB symbol stream used by production bzip2,
 * but it keeps the current byte-buffer pipeline simple and fully reversible.
 */

#include "bzip2.h"

#define RLE2_ESCAPE       255
#define RLE2_MIN_ZERO_RUN 4
#define RLE2_MAX_RUN      255

void rle2_encode(unsigned char *input, size_t len,
                 unsigned char *output, size_t *out_len)
{
    if (!input || !output || !out_len) return;

    size_t i = 0;
    size_t out = 0;

    while (i < len) {
        if (input[i] == 0) {
            size_t run = 1;
            while (i + run < len && input[i + run] == 0)
                run++;

            if (run >= RLE2_MIN_ZERO_RUN) {
                size_t remaining = run;
                while (remaining >= RLE2_MIN_ZERO_RUN) {
                    unsigned char chunk = (remaining > RLE2_MAX_RUN)
                                        ? RLE2_MAX_RUN
                                        : (unsigned char)remaining;
                    output[out++] = RLE2_ESCAPE;
                    output[out++] = chunk;
                    remaining -= chunk;
                }
                for (size_t k = 0; k < remaining; k++)
                    output[out++] = 0;
            } else {
                for (size_t k = 0; k < run; k++)
                    output[out++] = 0;
            }

            i += run;
        } else if (input[i] == RLE2_ESCAPE) {
            output[out++] = RLE2_ESCAPE;
            output[out++] = 0;
            i++;
        } else {
            output[out++] = input[i++];
        }
    }

    *out_len = out;
}

void rle2_decode(unsigned char *input, size_t len,
                 unsigned char *output, size_t *out_len)
{
    if (!input || !output || !out_len) return;

    size_t i = 0;
    size_t out = 0;

    while (i < len) {
        unsigned char c = input[i++];

        if (c != RLE2_ESCAPE) {
            output[out++] = c;
            continue;
        }

        if (i >= len) {
            output[out++] = RLE2_ESCAPE;
            break;
        }

        unsigned char count = input[i++];
        if (count == 0) {
            output[out++] = RLE2_ESCAPE;
        } else {
            for (unsigned int k = 0; k < (unsigned int)count; k++)
                output[out++] = 0;
        }
    }

    *out_len = out;
}
