/*
 * mg_binary_reader.h — MGRP binary reader + text formatter
 *
 * Offline tool: reads .mgbin files, produces human-readable text
 * (Apple .crash file style).  Uses malloc — NOT for OOM-time use.
 */

#ifndef MG_BINARY_READER_H
#define MG_BINARY_READER_H

#include <stdint.h>

/*
 * Read a binary report (.mgbin) and write human-readable text
 * to `output_fd`.  Uses malloc internally.
 * Returns 0 on success, negative on error.
 */
int mg_report_read_text(const char *binary_path, int output_fd);

/*
 * Validate a binary report file: checks magic, version, and
 * string table presence.
 * Returns 0 if valid, negative on error.
 */
int mg_report_validate(const char *binary_path);

#endif /* MG_BINARY_READER_H */
