/*
 * mg2crash — Convert MemoryGraph binary reports (.mgbin) to text (.crash)
 *
 * Usage:
 *   mg2crash <file.mgbin>                  → writes file.crash
 *   mg2crash <file.mgbin> out.crash        → writes out.crash
 *   mg2crash <file.mgbin> -                → writes to stdout
 *   mg2crash --validate <file.mgbin>       → validate only
 *   mg2crash *.mgbin                       → batch convert
 *
 * Exit codes:
 *   0  success
 *   1  usage error
 *   2  one or more files failed
 */

#include "../src/mg_binary_reader.h"
#include "../include/memory_graph.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Output path generation                                              */
/* ------------------------------------------------------------------ */

/*
 * Replace the extension of `src` with `.crash`.
 * If src has no extension, appends `.crash`.
 * Writes into `dst` (must be at least `dstsz` bytes).
 */
static void make_crash_path(const char *src, char *dst, size_t dstsz) {
    /* Find last dot after last slash. */
    const char *slash = strrchr(src, '/');
    const char *dot   = strrchr(src, '.');
    if (dot && (!slash || dot > slash)) {
        size_t prefix = (size_t)(dot - src);
        if (prefix + 7 > dstsz) { /* ".crash\0" = 7 */
            snprintf(dst, dstsz, "%s.crash", src);
            return;
        }
        memcpy(dst, src, prefix);
        memcpy(dst + prefix, ".crash", 7);
    } else {
        snprintf(dst, dstsz, "%s.crash", src);
    }
}

/* ------------------------------------------------------------------ */
/* Convert one file                                                    */
/* ------------------------------------------------------------------ */

static int convert_one(const char *input, const char *output) {
    int fd;
    bool close_fd = false;

    if (output && strcmp(output, "-") == 0) {
        fd = STDOUT_FILENO;
    } else {
        char path_buf[1024];
        const char *out_path = output;
        if (!out_path) {
            make_crash_path(input, path_buf, sizeof(path_buf));
            out_path = path_buf;
        }

        fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "mg2crash: cannot create '%s': ", out_path);
            perror(NULL);
            return -1;
        }
        close_fd = true;

        /* Print output path to stderr so user can see it. */
        fprintf(stderr, "%s -> %s\n", input, out_path);
    }

    int rc = mg_report_read_text(input, fd);

    if (close_fd) close(fd);

    if (rc != MG_OK) {
        const char *reason = "unknown error";
        if (rc == MG_ERR_IO)          reason = "invalid or corrupt file";
        if (rc == MG_ERR_ALLOC)       reason = "out of memory";
        if (rc == MG_ERR_INVALID_ARG) reason = "invalid argument";
        fprintf(stderr, "mg2crash: %s: %s\n", input, reason);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Validate one file                                                   */
/* ------------------------------------------------------------------ */

static int validate_one(const char *path) {
    int rc = mg_report_validate(path);
    if (rc == MG_OK) {
        fprintf(stdout, "%s: valid MGRP\n", path);
    } else {
        fprintf(stderr, "%s: invalid (%d)\n", path, rc);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Usage                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <file.mgbin> [output.crash]\n"
        "       %s [options] <file1.mgbin> <file2.mgbin> ...\n"
        "\n"
        "Convert MemoryGraph binary reports to human-readable text.\n"
        "\n"
        "Options:\n"
        "  -v, --validate   Validate file format without converting\n"
        "  -h, --help       Show this help\n"
        "\n"
        "If output path is omitted, replaces .mgbin with .crash.\n"
        "Use '-' as output path to write to stdout.\n",
        prog, prog);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    /* Parse options. */
    bool validate_only = false;
    int first_file = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--validate") == 0) {
            validate_only = true;
            first_file = i + 1;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "mg2crash: unknown option '%s'\n", argv[i]);
            return 1;
        }
        break;
    }

    int nfiles = argc - first_file;
    if (nfiles == 0) {
        fprintf(stderr, "mg2crash: no input files\n");
        return 1;
    }

    int failures = 0;

    if (validate_only) {
        /* Validate mode: all remaining args are input files. */
        for (int i = first_file; i < argc; i++) {
            if (validate_one(argv[i]) != MG_OK) failures++;
        }
    } else if (nfiles == 1) {
        /* Single file: no explicit output → auto-generate .crash path. */
        if (convert_one(argv[first_file], NULL) != MG_OK) failures++;
    } else if (nfiles == 2) {
        /* Two args: could be (input, output) or (input1, input2).
         * Heuristic: if second arg ends with .mgbin, treat as batch. */
        const char *second = argv[first_file + 1];
        size_t slen = strlen(second);
        bool second_is_mgbin = (slen > 6 &&
            strcmp(second + slen - 6, ".mgbin") == 0);

        if (second_is_mgbin || strcmp(second, "-") == 0) {
            /* Ambiguous case with "-": single file to stdout. */
            if (strcmp(second, "-") == 0) {
                if (convert_one(argv[first_file], "-") != MG_OK)
                    failures++;
            } else {
                /* Both are .mgbin: batch mode. */
                for (int i = first_file; i < argc; i++) {
                    if (convert_one(argv[i], NULL) != MG_OK) failures++;
                }
            }
        } else {
            /* Second arg is explicit output path. */
            if (convert_one(argv[first_file], second) != MG_OK)
                failures++;
        }
    } else {
        /* 3+ files: batch mode, auto-generate output paths. */
        for (int i = first_file; i < argc; i++) {
            if (convert_one(argv[i], NULL) != MG_OK) failures++;
        }
    }

    return failures > 0 ? 2 : 0;
}
