/*
 * Copyright 2014 LKC Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "functions.h"
#include "util.h"
#include "fatfs.h"
#include "mbr.h"
#include "fwfile.h"
#include "block_cache.h"
#include "uboot_env.h"
#include "sparse_file.h"
#include "progress.h"
#include "pad_to_block_writer.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#include <sodium.h>

#if defined(_WIN32) || defined(__CYGWIN__)
    const char *read_args = "rb";
    const char *write_args = "wb";
#else
    const char *read_args = "r";
    const char *write_args = "w";
    #define O_BINARY 0
#endif

#define DECLARE_FUN(FUN) \
    static int FUN ## _validate(struct fun_context *fctx); \
    static int FUN ## _compute_progress(struct fun_context *fctx); \
    static int FUN ## _run(struct fun_context *fctx)

DECLARE_FUN(raw_write);
DECLARE_FUN(raw_memset);
DECLARE_FUN(fat_attrib);
DECLARE_FUN(fat_mkfs);
DECLARE_FUN(fat_write);
DECLARE_FUN(fat_mv);
DECLARE_FUN(fat_rm);
DECLARE_FUN(fat_cp);
DECLARE_FUN(fat_mkdir);
DECLARE_FUN(fat_setlabel);
DECLARE_FUN(fat_touch);
DECLARE_FUN(mbr_write);
DECLARE_FUN(trim);
DECLARE_FUN(uboot_clearenv);
DECLARE_FUN(uboot_setenv);
DECLARE_FUN(uboot_unsetenv);
DECLARE_FUN(uboot_recover);
DECLARE_FUN(error);
DECLARE_FUN(info);
DECLARE_FUN(path_write);
DECLARE_FUN(pipe_write);
DECLARE_FUN(execute);

struct fun_info {
    const char *name;
    int (*validate)(struct fun_context *fctx);
    int (*compute_progress)(struct fun_context *fctx);
    int (*run)(struct fun_context *fctx);
};

#define FUN_INFO(FUN) {#FUN, FUN ## _validate, FUN ## _compute_progress, FUN ## _run}
#define FUN_BANG_INFO(FUN) {#FUN "!", FUN ## _validate, FUN ## _compute_progress, FUN ## _run}
static struct fun_info fun_table[] = {
    FUN_INFO(raw_write),
    FUN_INFO(raw_memset),
    FUN_INFO(fat_attrib),
    FUN_INFO(fat_mkfs),
    FUN_INFO(fat_write),
    FUN_INFO(fat_mv),
    FUN_BANG_INFO(fat_mv),
    FUN_INFO(fat_rm),
    FUN_BANG_INFO(fat_rm),
    FUN_INFO(fat_cp),
    FUN_INFO(fat_mkdir),
    FUN_INFO(fat_setlabel),
    FUN_INFO(fat_touch),
    FUN_INFO(mbr_write),
    FUN_INFO(trim),
    FUN_INFO(uboot_clearenv),
    FUN_INFO(uboot_setenv),
    FUN_INFO(uboot_unsetenv),
    FUN_INFO(uboot_recover),
    FUN_INFO(error),
    FUN_INFO(info),
    FUN_INFO(path_write),
    FUN_INFO(pipe_write),
    FUN_INFO(execute),
};

extern bool fwup_unsafe;

static struct fun_info *lookup(int argc, const char **argv)
{
    if (argc < 1) {
        set_last_error("Not enough parameters");
        return 0;
    }

    size_t i;
    for (i = 0; i < NUM_ELEMENTS(fun_table); i++) {
        if (strcmp(argv[0], fun_table[i].name) == 0) {
            return &fun_table[i];
        }
    }

    set_last_error("Unknown function");
    return 0;
}

/**
 * @brief Validate the parameters passed to the function
 *
 * This is called when creating the firmware file.
 *
 * @param fctx the function context
 * @return 0 if ok
 */
int fun_validate(struct fun_context *fctx)
{
    struct fun_info *fun = lookup(fctx->argc, fctx->argv);
    if (!fun)
        return -1;

    return fun->validate(fctx);
}

/**
 * @brief Compute the total progress units expected
 *
 * This is called before running.
 *
 * @param fctx the function context
 * @return 0 if ok
 */
int fun_compute_progress(struct fun_context *fctx)
{
    struct fun_info *fun = lookup(fctx->argc, fctx->argv);
    if (!fun)
        return -1;

    return fun->compute_progress(fctx);
}

/**
 * @brief Run a function
 *
 * This is called when applying the firmware.
 *
 * @param fctx the function context
 * @return 0 if ok
 */
int fun_run(struct fun_context *fctx)
{
    struct fun_info *fun = lookup(fctx->argc, fctx->argv);
    if (!fun)
        return -1;

    return fun->run(fctx);
}


/**
 * @brief Run all of the functions in a funlist
 * @param fctx the context to use (argc and argv will be updated in it)
 * @param funlist the list
 * @param fun the function to execute (either fun_run or fun_compute_progress)
 * @return 0 if ok
 */
int fun_apply_funlist(struct fun_context *fctx, cfg_opt_t *funlist, int (*fun)(struct fun_context *fctx))
{
    int ix = 0;
    char *aritystr;
    while ((aritystr = cfg_opt_getnstr(funlist, ix++)) != NULL) {
        fctx->argc = strtoul(aritystr, NULL, 0);
        if (fctx->argc <= 0 || fctx->argc > FUN_MAX_ARGS) {
            set_last_error("Unexpected argc value in funlist");
            return -1;
        }
        int i;
        for (i = 0; i < fctx->argc; i++) {
            fctx->argv[i] = cfg_opt_getnstr(funlist, ix++);
            if (fctx->argv[i] == NULL) {
                set_last_error("Unexpected error with funlist");
                return -1;
            }
        }
        // Clear out the rest of the argv entries to avoid confusion when debugging.
        for (; i < FUN_MAX_ARGS; i++)
            fctx->argv[i] = 0;

        if (fun(fctx) < 0)
            return -1;
    }
    return 0;
}

int raw_write_validate(struct fun_context *fctx)
{
    if (fctx->type != FUN_CONTEXT_FILE)
        ERR_RETURN("raw_write only usable in on-resource");

    if (fctx->argc != 2)
        ERR_RETURN("raw_write requires a block offset");
        
    CHECK_ARG_UINT64(fctx->argv[1], "raw_write requires a non-negative integer block offset");

    return 0;
}
int block_write_compute_progress(struct fun_context *fctx)
{
    assert(fctx->type == FUN_CONTEXT_FILE);
    assert(fctx->on_event);

    struct sparse_file_map sfm;
    sparse_file_init(&sfm);
    OK_OR_RETURN(sparse_file_get_map_from_config(fctx->cfg, fctx->on_event->title, &sfm));
    off_t expected_length = sparse_file_data_size(&sfm);
    sparse_file_free(&sfm);

    // Count each byte as a progress unit
    fctx->progress->total_units += expected_length;

    return 0;
}

int raw_write_compute_progress(struct fun_context *fctx)
{
    return block_write_compute_progress(fctx);
}
int raw_write_run(struct fun_context *fctx)
{
    assert(fctx->type == FUN_CONTEXT_FILE);
    assert(fctx->on_event);

    int rc = 0;
    struct sparse_file_map sfm;
    sparse_file_init(&sfm);

    cfg_t *resource = cfg_gettsec(fctx->cfg, "file-resource", fctx->on_event->title);
    if (!resource)
        ERR_CLEANUP_MSG("raw_write can't find matching file-resource");

    char *expected_hash = cfg_getstr(resource, "blake2b-256");
    if (!expected_hash || strlen(expected_hash) != crypto_generichash_BYTES * 2)
        ERR_CLEANUP_MSG("invalid blake2b-256 hash for '%s'", fctx->on_event->title);

    OK_OR_CLEANUP(sparse_file_get_map_from_resource(resource, &sfm));
    off_t expected_length = sparse_file_data_size(&sfm);

    off_t dest_offset = strtoull(fctx->argv[1], NULL, 0) * FWUP_BLOCK_SIZE;
    off_t len_written = 0;

    crypto_generichash_state hash_state;
    crypto_generichash_init(&hash_state, NULL, 0, crypto_generichash_BYTES);

    struct pad_to_block_writer ptbw;
    ptbw_init(&ptbw, fctx->output);

    for (;;) {
        off_t offset;
        size_t len;
        const void *buffer;

        OK_OR_CLEANUP(fctx->read(fctx, &buffer, &len, &offset));

        // Check if done.
        if (len == 0)
            break;

        crypto_generichash_update(&hash_state, (unsigned char*) buffer, len);

        OK_OR_CLEANUP(ptbw_pwrite(&ptbw, buffer, len, dest_offset + offset));

        len_written += len;
        progress_report(fctx->progress, len);
    }

    off_t ending_hole = sparse_ending_hole_size(&sfm);
    if (ending_hole > 0) {
        // If this is a regular file, seeking is insufficient in making the file
        // the right length, so write a block of zeros to the end.
        char zeros[FWUP_BLOCK_SIZE];
        memset(zeros, 0, sizeof(zeros));
        off_t to_write = sizeof(zeros);
        if (ending_hole < to_write)
            to_write = ending_hole;
        off_t offset = sparse_file_size(&sfm) - to_write;
        OK_OR_CLEANUP(ptbw_pwrite(&ptbw, zeros, to_write, dest_offset + offset));
    }

    OK_OR_CLEANUP(ptbw_flush(&ptbw));

    if (len_written != expected_length) {
        if (len_written == 0)
            ERR_CLEANUP_MSG("raw_write didn't write anything. Was it called twice in an on-resource for '%s'?", fctx->on_event->title);
        else
            ERR_CLEANUP_MSG("raw_write wrote %" PRId64" bytes, but should have written %" PRId64, len_written, expected_length);
    }

    // Verify hash
    unsigned char hash[crypto_generichash_BYTES];
    crypto_generichash_final(&hash_state, hash, sizeof(hash));
    char hash_str[sizeof(hash) * 2 + 1];
    bytes_to_hex(hash, hash_str, sizeof(hash));
    if (memcmp(hash_str, expected_hash, sizeof(hash_str)) != 0)
        ERR_CLEANUP_MSG("raw_write detected blake2b digest mismatch");

cleanup:
    sparse_file_free(&sfm);
    return rc;
}

int raw_memset_validate(struct fun_context *fctx)
{
    if (fctx->argc != 4)
        ERR_RETURN("raw_memset requires a block offset, count, and value");

    CHECK_ARG_UINT64(fctx->argv[1], "raw_memset requires a non-negative integer block offset");
    CHECK_ARG_UINT64_MAX(fctx->argv[2], INT32_MAX / FWUP_BLOCK_SIZE, "raw_memset requires a non-negative integer block count");
    CHECK_ARG_UINT64_MAX(fctx->argv[3], 255, "raw_memset requires value to be between 0 and 255");

    return 0;
}
int raw_memset_compute_progress(struct fun_context *fctx)
{
    int count = strtol(fctx->argv[2], NULL, 0);

    // Count each byte as a progress unit
    fctx->progress->total_units += count * FWUP_BLOCK_SIZE;

    return 0;
}
int raw_memset_run(struct fun_context *fctx)
{
    const size_t block_size = FWUP_BLOCK_SIZE;

    off_t dest_offset = strtoull(fctx->argv[1], NULL, 0) * FWUP_BLOCK_SIZE;
    int count = strtol(fctx->argv[2], NULL, 0) * FWUP_BLOCK_SIZE;
    int value = strtol(fctx->argv[3], NULL, 0);
    char buffer[block_size];
    memset(buffer, value, sizeof(buffer));

    off_t len_written = 0;
    off_t offset;
    for (offset = 0; offset < count; offset += block_size) {
        OK_OR_RETURN_MSG(block_cache_pwrite(fctx->output, buffer, block_size, dest_offset + offset, true),
                         "raw_memset couldn't write %d bytes to offset %" PRId64, block_size, dest_offset + offset);

        len_written += block_size;
        progress_report(fctx->progress, block_size);
    }

    return 0;
}

int fat_mkfs_validate(struct fun_context *fctx)
{
    if (fctx->argc != 3)
        ERR_RETURN("fat_mkfs requires a block offset and block count");

    CHECK_ARG_UINT64(fctx->argv[1], "fat_mkfs requires a non-negative integer block offset");
    CHECK_ARG_UINT64(fctx->argv[2], "fat_mkfs requires a non-negative integer block count");

    return 0;
}
int fat_mkfs_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int fat_mkfs_run(struct fun_context *fctx)
{
    off_t block_offset = strtoull(fctx->argv[1], NULL, 0);
    size_t block_count = strtoul(fctx->argv[2], NULL, 0);

    if (fatfs_mkfs(fctx->output, block_offset, block_count) < 0)
        return -1;

    progress_report(fctx->progress, 1);
    return 0;
}

int fat_attrib_validate(struct fun_context *fctx)
{
    if (fctx->argc != 4)
        ERR_RETURN("fat_attrib requires a block offset, filename, and attributes (SHR)");

    CHECK_ARG_UINT64(fctx->argv[1], "fat_mkfs requires a non-negative integer block offset");

    const char *attrib = fctx->argv[3];
    while (*attrib) {
        switch (*attrib) {
        case 'S':
        case 's':
        case 'H':
        case 'h':
        case 'R':
        case 'r':
            break;

        default:
            ERR_RETURN("fat_attrib only supports R, H, and S attributes");
        }
        attrib++;
    }
    return 0;
}
int fat_attrib_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int fat_attrib_run(struct fun_context *fctx)
{
    if (fatfs_attrib(fctx->output, strtoull(fctx->argv[1], NULL, 0), fctx->argv[2], fctx->argv[3]) < 0)
        return 1;

    progress_report(fctx->progress, 1);
    return 0;
}

int fat_write_validate(struct fun_context *fctx)
{
    if (fctx->type != FUN_CONTEXT_FILE)
        ERR_RETURN("fat_write only usable in on-resource");

    if (fctx->argc != 3)
        ERR_RETURN("fat_write requires a block offset and destination filename");

    CHECK_ARG_UINT64(fctx->argv[1], "fat_write requires a non-negative integer block offset");

    return 0;
}
int fat_write_compute_progress(struct fun_context *fctx)
{
    assert(fctx->type == FUN_CONTEXT_FILE);
    assert(fctx->on_event);

    struct sparse_file_map sfm;
    sparse_file_init(&sfm);
    OK_OR_RETURN(sparse_file_get_map_from_config(fctx->cfg, fctx->on_event->title, &sfm));
    off_t expected_length = sparse_file_data_size(&sfm);
    sparse_file_free(&sfm);

    // Zero-length files still do something
    if (expected_length == 0)
        expected_length = 1;

    // Count each byte as a progress unit
    fctx->progress->total_units += expected_length;

    return 0;
}
int fat_write_run(struct fun_context *fctx)
{
    int rc = 0;
    assert(fctx->on_event);

    struct sparse_file_map sfm;
    sparse_file_init(&sfm);

    cfg_t *resource = cfg_gettsec(fctx->cfg, "file-resource", fctx->on_event->title);
    if (!resource)
        ERR_CLEANUP_MSG("fat_write can't find file-resource '%s'", fctx->on_event->title);
    char *expected_hash = cfg_getstr(resource, "blake2b-256");
    if (!expected_hash || strlen(expected_hash) != crypto_generichash_BYTES * 2)
        ERR_CLEANUP_MSG("invalid blake2b-256 hash for '%s'", fctx->on_event->title);

    off_t len_written = 0;
    off_t block_offset = strtoull(fctx->argv[1], NULL, 0);

    // Enforce truncation semantics if the file exists
    OK_OR_CLEANUP(fatfs_rm(fctx->output, block_offset, fctx->argv[0], fctx->argv[2], false));

    OK_OR_CLEANUP(sparse_file_get_map_from_resource(resource, &sfm));
    off_t expected_data_length = sparse_file_data_size(&sfm);
    off_t expected_length = sparse_file_size(&sfm);

    // Handle zero-length file
    if (expected_length == 0) {
        OK_OR_CLEANUP(fatfs_touch(fctx->output, block_offset, fctx->argv[2]));

        sparse_file_free(&sfm);
        progress_report(fctx->progress, 1);
        goto cleanup;
    }

    crypto_generichash_state hash_state;
    crypto_generichash_init(&hash_state, NULL, 0, crypto_generichash_BYTES);
    for (;;) {
        off_t offset;
        size_t len;
        const void *buffer;

        OK_OR_CLEANUP(fctx->read(fctx, &buffer, &len, &offset));

        // Check if done.
        if (len == 0)
            break;

        crypto_generichash_update(&hash_state, (unsigned char*) buffer, len);

        OK_OR_CLEANUP(fatfs_pwrite(fctx->output, block_offset, fctx->argv[2], (int) offset, buffer, len));

        len_written += len;
        progress_report(fctx->progress, len);
    }

    size_t ending_hole = sparse_ending_hole_size(&sfm);
    if (ending_hole) {
        // If the file ends in a hole, fatfs_pwrite can be used to grow it.
        OK_OR_CLEANUP(fatfs_pwrite(fctx->output, block_offset, fctx->argv[2], (int) expected_length, NULL, 0));
    }

    if (len_written != expected_data_length) {
        if (len_written == 0)
            ERR_CLEANUP_MSG("fat_write didn't write anything. Was it called twice in an on-resource for '%s'? Try fat_cp instead.", fctx->on_event->title);
        else
            ERR_CLEANUP_MSG("fat_write didn't write the expected amount for '%s'", fctx->on_event->title);
    }

    unsigned char hash[crypto_generichash_BYTES];
    crypto_generichash_final(&hash_state, hash, sizeof(hash));
    char hash_str[sizeof(hash) * 2 + 1];
    bytes_to_hex(hash, hash_str, sizeof(hash));
    if (memcmp(hash_str, expected_hash, sizeof(hash_str)) != 0)
        ERR_CLEANUP_MSG("fat_write detected blake2b hash mismatch on '%s'", fctx->on_event->title);

cleanup:
    sparse_file_free(&sfm);
    return rc;
}

int fat_mv_validate(struct fun_context *fctx)
{
    if (fctx->argc != 4)
        ERR_RETURN("fat_mv requires a block offset, old filename, new filename");

    CHECK_ARG_UINT64(fctx->argv[1], "fat_mv requires a non-negative integer block offset");
    return 0;
}
int fat_mv_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int fat_mv_run(struct fun_context *fctx)
{
    off_t block_offset = strtoull(fctx->argv[1], NULL, 0);

    bool force = (fctx->argv[0][6] == '!');
    OK_OR_RETURN(fatfs_mv(fctx->output, block_offset, fctx->argv[0], fctx->argv[2], fctx->argv[3], force));

    progress_report(fctx->progress, 1);
    return 0;
}

int fat_rm_validate(struct fun_context *fctx)
{
    if (fctx->argc != 3)
        ERR_RETURN("fat_rm requires a block offset and filename");

    CHECK_ARG_UINT64(fctx->argv[1], "fat_rm requires a non-negative integer block offset");

    return 0;
}
int fat_rm_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int fat_rm_run(struct fun_context *fctx)
{
    off_t block_offset = strtoull(fctx->argv[1], NULL, 0);

    bool file_must_exist = (fctx->argv[0][6] == '!');
    OK_OR_RETURN(fatfs_rm(fctx->output, block_offset, fctx->argv[0], fctx->argv[2], file_must_exist));

    progress_report(fctx->progress, 1);
    return 0;
}

int fat_cp_validate(struct fun_context *fctx)
{
    if (fctx->argc != 4)
        ERR_RETURN("fat_cp requires a block offset, from filename, and to filename");

    CHECK_ARG_UINT64(fctx->argv[1], "fat_cp requires a non-negative integer block offset");

    return 0;
}
int fat_cp_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int fat_cp_run(struct fun_context *fctx)
{
    off_t block_offset = strtoull(fctx->argv[1], NULL, 0);

    OK_OR_RETURN(fatfs_cp(fctx->output, block_offset, fctx->argv[2], fctx->argv[3]));

    progress_report(fctx->progress, 1);
    return 0;
}

int fat_mkdir_validate(struct fun_context *fctx)
{
    if (fctx->argc != 3)
        ERR_RETURN("fat_mkdir requires a block offset and directory name");

    CHECK_ARG_UINT64(fctx->argv[1], "fat_mkdir requires a non-negative integer block offset");

    return 0;
}
int fat_mkdir_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int fat_mkdir_run(struct fun_context *fctx)
{
    off_t block_offset = strtoull(fctx->argv[1], NULL, 0);

    OK_OR_RETURN(fatfs_mkdir(fctx->output, block_offset, fctx->argv[2]));

    progress_report(fctx->progress, 1);
    return 0;
}

int fat_setlabel_validate(struct fun_context *fctx)
{
    if (fctx->argc != 3)
        ERR_RETURN("fat_setlabel requires a block offset and name");

    CHECK_ARG_UINT64(fctx->argv[1], "fat_setlabel requires a non-negative integer block offset");

    return 0;
}
int fat_setlabel_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int fat_setlabel_run(struct fun_context *fctx)
{
    off_t block_offset = strtoull(fctx->argv[1], NULL, 0);

    OK_OR_RETURN(fatfs_setlabel(fctx->output, block_offset, fctx->argv[2]));

    return 0;
}

int fat_touch_validate(struct fun_context *fctx)
{
    if (fctx->argc != 3)
        ERR_RETURN("fat_touch requires a block offset and filename");

    CHECK_ARG_UINT64(fctx->argv[1], "fat_touch requires a non-negative integer block offset");

    return 0;
}
int fat_touch_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int fat_touch_run(struct fun_context *fctx)
{
    off_t block_offset = strtoull(fctx->argv[1], NULL, 0);

    OK_OR_RETURN(fatfs_touch(fctx->output, block_offset, fctx->argv[2]));

    progress_report(fctx->progress, 1);
    return 0;
}

int mbr_write_validate(struct fun_context *fctx)
{
    if (fctx->argc != 2)
        ERR_RETURN("mbr_write requires an mbr");

    const char *mbr_name = fctx->argv[1];
    cfg_t *mbrsec = cfg_gettsec(fctx->cfg, "mbr", mbr_name);

    if (!mbrsec)
        ERR_RETURN("mbr_write can't find mbr reference");

    return 0;
}
int mbr_write_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int mbr_write_run(struct fun_context *fctx)
{
    const char *mbr_name = fctx->argv[1];
    cfg_t *mbrsec = cfg_gettsec(fctx->cfg, "mbr", mbr_name);
    uint8_t buffer[FWUP_BLOCK_SIZE];

    OK_OR_RETURN(mbr_create_cfg(mbrsec, buffer));

    OK_OR_RETURN_MSG(block_cache_pwrite(fctx->output, buffer, FWUP_BLOCK_SIZE, 0, false),
                     "unexpected error writing mbr: %s", strerror(errno));

    progress_report(fctx->progress, 1);
    return 0;
}

int trim_validate(struct fun_context *fctx)
{
    if (fctx->argc != 3)
        ERR_RETURN("trim requires a block offset and count");

    CHECK_ARG_UINT64(fctx->argv[1], "trim requires a non-negative integer block offset");
    CHECK_ARG_UINT64_MAX(fctx->argv[2], INT32_MAX / FWUP_BLOCK_SIZE, "trim requires a non-negative integer block count");

    return 0;
}
int trim_compute_progress(struct fun_context *fctx)
{
    off_t block_count = strtoull(fctx->argv[2], NULL, 0);

    // Use a heuristic for counting trim progress units -> 1 per 128KB
    fctx->progress->total_units += block_count / 256;

    return 0;
}
int trim_run(struct fun_context *fctx)
{
    off_t block_offset = strtoull(fctx->argv[1], NULL, 0);
    off_t block_count = strtoull(fctx->argv[2], NULL, 0);

    off_t offset = block_offset * FWUP_BLOCK_SIZE;
    off_t count = block_offset * FWUP_BLOCK_SIZE;

    OK_OR_RETURN(block_cache_trim(fctx->output, offset, count, true));

    progress_report(fctx->progress, block_count / 256);
    return 0;
}

int uboot_recover_validate(struct fun_context *fctx)
{
    if (fctx->argc != 2)
        ERR_RETURN("uboot_recover requires a uboot-environment reference");

    const char *uboot_env_name = fctx->argv[1];
    cfg_t *ubootsec = cfg_gettsec(fctx->cfg, "uboot-environment", uboot_env_name);

    if (!ubootsec)
        ERR_RETURN("uboot_recover can't find uboot-environment reference");

    return 0;
}
int uboot_recover_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int uboot_recover_run(struct fun_context *fctx)
{
    int rc = 0;
    const char *uboot_env_name = fctx->argv[1];
    cfg_t *ubootsec = cfg_gettsec(fctx->cfg, "uboot-environment", uboot_env_name);
    struct uboot_env env;
    struct uboot_env clean_env;

    if (uboot_env_create_cfg(ubootsec, &env) < 0 ||
        uboot_env_create_cfg(ubootsec, &clean_env) < 0)
        return -1;

    char *buffer = malloc(env.env_size);
    OK_OR_CLEANUP_MSG(block_cache_pread(fctx->output, buffer, env.env_size, env.block_offset * FWUP_BLOCK_SIZE),
                      "unexpected error reading uboot environment: %s", strerror(errno));

    if (uboot_env_read(&env, buffer) < 0) {
        // Corrupt, so make a clean environment and write it.

        OK_OR_CLEANUP(uboot_env_write(&clean_env, buffer));

        OK_OR_CLEANUP_MSG(block_cache_pwrite(fctx->output, buffer, env.env_size, env.block_offset * FWUP_BLOCK_SIZE, false),
                      "unexpected error writing uboot environment: %s", strerror(errno));
    }

    progress_report(fctx->progress, 1);

cleanup:
    uboot_env_free(&env);
    uboot_env_free(&clean_env);
    free(buffer);
    return rc;
}

int uboot_clearenv_validate(struct fun_context *fctx)
{
    if (fctx->argc != 2)
        ERR_RETURN("uboot_clearenv requires a uboot-environment reference");

    const char *uboot_env_name = fctx->argv[1];
    cfg_t *ubootsec = cfg_gettsec(fctx->cfg, "uboot-environment", uboot_env_name);

    if (!ubootsec)
        ERR_RETURN("uboot_clearenv can't find uboot-environment reference");

    return 0;
}
int uboot_clearenv_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int uboot_clearenv_run(struct fun_context *fctx)
{
    int rc = 0;
    const char *uboot_env_name = fctx->argv[1];
    cfg_t *ubootsec = cfg_gettsec(fctx->cfg, "uboot-environment", uboot_env_name);
    struct uboot_env env;

    if (uboot_env_create_cfg(ubootsec, &env) < 0)
        return -1;

    char *buffer = (char *) malloc(env.env_size);
    OK_OR_CLEANUP(uboot_env_write(&env, buffer));

    OK_OR_CLEANUP_MSG(block_cache_pwrite(fctx->output, buffer, env.env_size, env.block_offset * FWUP_BLOCK_SIZE, false),
                      "unexpected error writing uboot environment: %s", strerror(errno));

    progress_report(fctx->progress, 1);

cleanup:
    uboot_env_free(&env);
    free(buffer);
    return rc;
}

int uboot_setenv_validate(struct fun_context *fctx)
{
    if (fctx->argc != 4)
        ERR_RETURN("uboot_setenv requires a uboot-environment reference, variable name and value");

    const char *uboot_env_name = fctx->argv[1];
    cfg_t *ubootsec = cfg_gettsec(fctx->cfg, "uboot-environment", uboot_env_name);

    if (!ubootsec)
        ERR_RETURN("uboot_setenv can't find uboot-environment reference");

    return 0;
}
int uboot_setenv_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int uboot_setenv_run(struct fun_context *fctx)
{
    int rc = 0;
    const char *uboot_env_name = fctx->argv[1];
    cfg_t *ubootsec = cfg_gettsec(fctx->cfg, "uboot-environment", uboot_env_name);
    struct uboot_env env;

    if (uboot_env_create_cfg(ubootsec, &env) < 0)
        return -1;

    char *buffer = (char *) malloc(env.env_size);

    OK_OR_CLEANUP_MSG(block_cache_pread(fctx->output, buffer, env.env_size, env.block_offset * FWUP_BLOCK_SIZE),
                      "unexpected error reading uboot environment: %s", strerror(errno));

    OK_OR_CLEANUP(uboot_env_read(&env, buffer));

    OK_OR_CLEANUP(uboot_env_setenv(&env, fctx->argv[2], fctx->argv[3]));

    OK_OR_CLEANUP(uboot_env_write(&env, buffer));

    OK_OR_CLEANUP_MSG(block_cache_pwrite(fctx->output, buffer, env.env_size, env.block_offset * FWUP_BLOCK_SIZE, false),
                      "unexpected error writing uboot environment: %s", strerror(errno));

    progress_report(fctx->progress, 1);

cleanup:
    uboot_env_free(&env);
    free(buffer);
    return rc;
}

int uboot_unsetenv_validate(struct fun_context *fctx)
{
    if (fctx->argc != 3)
        ERR_RETURN("uboot_unsetenv requires a uboot-environment reference and a variable name");

    const char *uboot_env_name = fctx->argv[1];
    cfg_t *ubootsec = cfg_gettsec(fctx->cfg, "uboot-environment", uboot_env_name);

    if (!ubootsec)
        ERR_RETURN("uboot_unsetenv can't find uboot-environment reference");

    return 0;
}
int uboot_unsetenv_compute_progress(struct fun_context *fctx)
{
    fctx->progress->total_units++; // Arbitarily count as 1 unit
    return 0;
}
int uboot_unsetenv_run(struct fun_context *fctx)
{
    int rc = 0;
    const char *uboot_env_name = fctx->argv[1];
    cfg_t *ubootsec = cfg_gettsec(fctx->cfg, "uboot-environment", uboot_env_name);
    struct uboot_env env;

    if (uboot_env_create_cfg(ubootsec, &env) < 0)
        return -1;

    char *buffer = (char *) malloc(env.env_size);
    OK_OR_CLEANUP_MSG(block_cache_pread(fctx->output, buffer, env.env_size, env.block_offset * FWUP_BLOCK_SIZE),
                      "unexpected error reading uboot environment: %s", strerror(errno));

    OK_OR_CLEANUP(uboot_env_read(&env, buffer));

    OK_OR_CLEANUP(uboot_env_unsetenv(&env, fctx->argv[2]));

    OK_OR_CLEANUP(uboot_env_write(&env, buffer));

    OK_OR_CLEANUP_MSG(block_cache_pwrite(fctx->output, buffer, env.env_size, env.block_offset * FWUP_BLOCK_SIZE, false),
                      "unexpected error writing uboot environment: %s", strerror(errno));

    progress_report(fctx->progress, 1);

cleanup:
    uboot_env_free(&env);
    free(buffer);
    return rc;
}

int error_validate(struct fun_context *fctx)
{
    if (fctx->argc != 2)
        ERR_RETURN("error() requires a message parameter");

    return 0;
}
int error_compute_progress(struct fun_context *fctx)
{
    (void) fctx; // UNUSED
    return 0;
}
int error_run(struct fun_context *fctx)
{
    ERR_RETURN("%s", fctx->argv[1]);
}

int info_validate(struct fun_context *fctx)
{
    if (fctx->argc != 2)
        ERR_RETURN("info() requires a message parameter");

    return 0;
}
int info_compute_progress(struct fun_context *fctx)
{
    (void) fctx; // UNUSED
    return 0;
}
int info_run(struct fun_context *fctx)
{
    fwup_warnx("%s", fctx->argv[1]);
    return 0;
}

int path_write_validate(struct fun_context *fctx)
{
    if (fctx->type != FUN_CONTEXT_FILE)
        ERR_RETURN("path_write only usable in on-resource");

    if (fctx->argc != 2)
        ERR_RETURN("path_write requires a file path");
        
    return 0;
}
int path_write_compute_progress(struct fun_context *fctx)
{
    return block_write_compute_progress(fctx);
}
int fd_write_run(char const * cmd_name, struct fun_context *fctx, int output_fd) 
{
    assert(fctx->type == FUN_CONTEXT_FILE);
    assert(fctx->on_event);

    int rc = 0;
    struct sparse_file_map sfm;
    sparse_file_init(&sfm);

    cfg_t *resource = cfg_gettsec(fctx->cfg, "file-resource", fctx->on_event->title);
    if (!resource)
        ERR_CLEANUP_MSG("%s can't find matching file-resource",cmd_name);

    char *expected_hash = cfg_getstr(resource, "blake2b-256");
    if (!expected_hash || strlen(expected_hash) != crypto_generichash_BYTES * 2)
        ERR_CLEANUP_MSG("invalid blake2b-256 hash for '%s'", fctx->on_event->title);

    OK_OR_CLEANUP(sparse_file_get_map_from_resource(resource, &sfm));
    off_t expected_length = sparse_file_data_size(&sfm);

    off_t len_written = 0;

    crypto_generichash_state hash_state;
    crypto_generichash_init(&hash_state, NULL, 0, crypto_generichash_BYTES);
    for (;;) {
        off_t offset;
        size_t len;
        const void *buffer;

        OK_OR_CLEANUP(fctx->read(fctx, &buffer, &len, &offset));

        // Check if done.
        if (len == 0)
            break;

        crypto_generichash_update(&hash_state, (unsigned char*) buffer, len);

        ssize_t written = write(output_fd, buffer, len);
        if (written < 0)
            ERR_CLEANUP_MSG("%s couldn't write %d bytes to offset %lld", cmd_name, len, offset);
        len_written += written;
        progress_report(fctx->progress, len);
    }

    off_t ending_hole = sparse_ending_hole_size(&sfm);
    if (ending_hole > 0) {
        // If this is a regular file, seeking is insufficient in making the file
        // the right length, so write a block of zeros to the end.
        char zeros[512];
        memset(zeros, 0, sizeof(zeros));
        off_t to_write = sizeof(zeros);
        if (ending_hole < to_write)
            to_write = ending_hole;
        off_t offset = sparse_file_size(&sfm) - to_write;
        ssize_t written = write(output_fd, zeros, to_write);
        if (written < 0)
            ERR_CLEANUP_MSG("%s couldn't write to hole at offset %lld", cmd_name, offset);

        // Unaccount for these bytes
        len_written += written - to_write;
    }

    // Verify hash
    unsigned char hash[crypto_generichash_BYTES];
    crypto_generichash_final(&hash_state, hash, sizeof(hash));
    char hash_str[sizeof(hash) * 2 + 1];
    bytes_to_hex(hash, hash_str, sizeof(hash));
    if (memcmp(hash_str, expected_hash, sizeof(hash_str)) != 0)
        ERR_CLEANUP_MSG("raw_write detected blake2b digest mismatch");

cleanup:
    sparse_file_free(&sfm);
    return rc;
}
int path_write_run(struct fun_context *fctx)
{
    assert(fctx->type == FUN_CONTEXT_FILE);
    assert(fctx->on_event);

    int rc = 0;

    if(!fwup_unsafe)
        ERR_CLEANUP_MSG("path_write requires --unsafe");

    char const* output_filename = fctx->argv[1];
   
    int output_fd = open(output_filename,O_WRONLY|O_CREAT|O_BINARY,0644);
    if(!output_fd)
        ERR_CLEANUP_MSG("raw_write can't open output file %s", fctx->argv[2]);

    rc = fd_write_run("path_write",fctx,output_fd);

cleanup:
    if(output_fd) {
        close(output_fd);
    }
    return rc;
}
int pipe_write_validate(struct fun_context *fctx)
{
    if (fctx->type != FUN_CONTEXT_FILE)
        ERR_RETURN("pipe_write only usable in on-resource");

    if (fctx->argc != 2)
        ERR_RETURN("pipe_write requires a command to execute");

    return 0;
}
int pipe_write_compute_progress(struct fun_context *fctx)
{
    return block_write_compute_progress(fctx);
}

int pipe_write_run(struct fun_context *fctx)
{
    assert(fctx->type == FUN_CONTEXT_FILE);
    assert(fctx->on_event);

    int rc = 0;

    if(!fwup_unsafe)
        ERR_CLEANUP_MSG("pipe_write requires --unsafe");

    char const *cmd_name = fctx->argv[1];
    FILE * cmd_pipe = popen(cmd_name,write_args);
    if(!cmd_pipe)
            ERR_CLEANUP_MSG("pipe_write can't run command %s", cmd_name);
    int output_fd = fileno(cmd_pipe);
    if(!output_fd)
        ERR_CLEANUP_MSG("pipe_write can't run command %s", cmd_name);

    rc = fd_write_run("pipe_write",fctx,output_fd);

cleanup:
    if(cmd_pipe) {
        pclose(cmd_pipe);
    }
    return rc;
}
int execute_validate(struct fun_context *fctx)
{
    if (fctx->argc != 2)
        ERR_RETURN("execute requires a command to execute");

    return 0;
}
int execute_compute_progress(struct fun_context *fctx)
{
    (void) fctx; // UNUSED
    return 0;
}

int execute_run(struct fun_context *fctx)
{
    assert(fctx->on_event);

    int rc = 0;

    if(!fwup_unsafe)
        ERR_CLEANUP_MSG("pipe_write requires --unsafe");
    
    char const *cmd_name = fctx->argv[1];
    FILE * cmd_pipe = popen(cmd_name,read_args);
    if(!cmd_pipe)
        ERR_CLEANUP_MSG("execute can't run command %s", cmd_name);

    char buffer[512];
    
    while(fread(buffer, 512, 1, cmd_pipe) == 512) {
        fwup_warnx("%s", buffer);
    }

cleanup:
    if(cmd_pipe) {
        pclose(cmd_pipe);
    }
    return rc;
}
