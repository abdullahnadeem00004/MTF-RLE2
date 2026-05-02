/*
 * block.c
 * -------
 * Block Division and File Handling for the BZip2 pipeline.
 *
 * Responsibilities:
 *   - divide_into_blocks()  : read an arbitrarily large file and split it
 *                             into fixed-size Block chunks.
 *   - reassemble_blocks()   : write all blocks back into a single file.
 *   - free_block_manager()  : release every byte of heap memory.
 *
 * Design notes:
 *   • Uses streaming I/O (fread/fwrite) so it works on files larger than RAM.
 *   • Each Block owns its data buffer; the BlockManager owns the Block array.
 *   • block_size is clamped to [MIN_BLOCK_SIZE, MAX_BLOCK_SIZE] at call-site
 *     (see config.c), so no clamping is repeated here.
 */

#include "bzip2.h"

/* ─────────────────────────────────────────────────────────────
 *  divide_into_blocks
 * ───────────────────────────────────────────────────────────── */
BlockManager *divide_into_blocks(const char *filename, size_t block_size)
{
    if (!filename || block_size == 0) {
        fprintf(stderr, "[block] Invalid arguments to divide_into_blocks\n");
        return NULL;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "[block] Cannot open input file: %s\n", filename);
        return NULL;
    }

    /* ── Determine file size ─────────────────────────────────── */
    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "[block] fseek failed on: %s\n", filename);
        fclose(fp);
        return NULL;
    }
    long file_size_signed = ftell(fp);
    if (file_size_signed < 0) {
        fprintf(stderr, "[block] ftell failed on: %s\n", filename);
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    size_t file_size  = (size_t)file_size_signed;
    int    num_blocks = (file_size == 0) ? 1
                      : (int)((file_size + block_size - 1) / block_size);

    /* ── Allocate BlockManager ───────────────────────────────── */
    BlockManager *manager = malloc(sizeof(BlockManager));
    if (!manager) {
        fprintf(stderr, "[block] Out of memory (BlockManager)\n");
        fclose(fp);
        return NULL;
    }
    manager->blocks     = calloc((size_t)num_blocks, sizeof(Block));
    manager->num_blocks = num_blocks;
    manager->block_size = block_size;

    if (!manager->blocks) {
        fprintf(stderr, "[block] Out of memory (Block array, %d blocks)\n",
                num_blocks);
        free(manager);
        fclose(fp);
        return NULL;
    }

    /* ── Read file in chunks ─────────────────────────────────── */
    for (int i = 0; i < num_blocks; i++) {
        /* Last block may be smaller than block_size */
        size_t remaining  = file_size - (size_t)i * block_size;
        size_t this_size  = (remaining < block_size) ? remaining : block_size;

        /* Handle empty file edge-case: create one zero-length block */
        if (file_size == 0) this_size = 0;

        manager->blocks[i].data = malloc(this_size == 0 ? 1 : this_size);
        if (!manager->blocks[i].data) {
            fprintf(stderr, "[block] Out of memory (block %d data)\n", i);
            /* Free already-allocated blocks */
            for (int j = 0; j < i; j++) free(manager->blocks[j].data);
            free(manager->blocks);
            free(manager);
            fclose(fp);
            return NULL;
        }

        size_t bytes_read = 0;
        if (this_size > 0) {
            bytes_read = fread(manager->blocks[i].data, 1, this_size, fp);
            if (bytes_read != this_size) {
                fprintf(stderr, "[block] fread error on block %d "
                        "(expected %zu, got %zu)\n", i, this_size, bytes_read);
                for (int j = 0; j <= i; j++) free(manager->blocks[j].data);
                free(manager->blocks);
                free(manager);
                fclose(fp);
                return NULL;
            }
        }

        manager->blocks[i].size          = bytes_read;
        manager->blocks[i].original_size = bytes_read;
    }

    fclose(fp);

    printf("[block] '%s' → %d block(s), block_size=%zu bytes, "
           "file_size=%zu bytes\n",
           filename, num_blocks, block_size, file_size);

    return manager;
}

/* ─────────────────────────────────────────────────────────────
 *  reassemble_blocks
 * ───────────────────────────────────────────────────────────── */
int reassemble_blocks(BlockManager *manager, const char *output_filename)
{
    if (!manager || !output_filename) {
        fprintf(stderr, "[block] Invalid arguments to reassemble_blocks\n");
        return -1;
    }

    FILE *fp = fopen(output_filename, "wb");
    if (!fp) {
        fprintf(stderr, "[block] Cannot open output file: %s\n",
                output_filename);
        return -1;
    }

    size_t total_written = 0;

    for (int i = 0; i < manager->num_blocks; i++) {
        Block *b = &manager->blocks[i];
        if (b->size == 0) continue;          /* skip empty blocks */

        size_t written = fwrite(b->data, 1, b->size, fp);
        if (written != b->size) {
            fprintf(stderr, "[block] fwrite error on block %d "
                    "(expected %zu, wrote %zu)\n", i, b->size, written);
            fclose(fp);
            return -1;
        }
        total_written += written;
    }

    fclose(fp);

    printf("[block] Reassembled %d block(s) → '%s' (%zu bytes)\n",
           manager->num_blocks, output_filename, total_written);

    return 0;
}

/* ─────────────────────────────────────────────────────────────
 *  free_block_manager
 * ───────────────────────────────────────────────────────────── */
void free_block_manager(BlockManager *manager)
{
    if (!manager) return;

    if (manager->blocks) {
        for (int i = 0; i < manager->num_blocks; i++)
            free(manager->blocks[i].data);
        free(manager->blocks);
    }
    free(manager);
}
