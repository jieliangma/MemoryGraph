/*
 * MemoryGraphBinaryTests.m — XCTest for MGRP binary format
 */

#import <XCTest/XCTest.h>
#include "../../src/mg_binary.h"
#include "../../src/mg_binary_reader.h"
#include "../../src/mg_pool.h"
#include "../../src/mg_report.h"
#include "../../src/mg_vm_region.h"
#include "../../src/mg_heap.h"
#include "../../include/memory_graph.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int tmp_open(char *path, size_t pathlen) {
    snprintf(path, pathlen, "/tmp/mg_test_binary_XXXXXX");
    return mkstemp(path);
}

static uint8_t *read_file_bytes(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = malloc(size);
    ssize_t n = read(fd, buf, size);
    close(fd);
    if (n <= 0) { free(buf); return NULL; }
    if (out_size) *out_size = (size_t)n;
    return buf;
}

@interface MemoryGraphBinaryTests : XCTestCase
@end

@implementation MemoryGraphBinaryTests

- (void)testBinaryWriterPrimitives {
    char path[256];
    int fd = tmp_open(path, sizeof(path));
    XCTAssertTrue(fd >= 0);

    mg_binary_writer_t w;
    mg_binary_init(&w, fd);

    mg_binary_u8(&w, 0xAB);
    mg_binary_u16(&w, 0x1234);
    mg_binary_u32(&w, 0xDEADBEEF);
    mg_binary_u64(&w, 0x0102030405060708ULL);

    bool ok = mg_binary_finish(&w);
    close(fd);
    XCTAssertTrue(ok);

    size_t size = 0;
    uint8_t *data = read_file_bytes(path, &size);
    XCTAssertTrue(data != NULL);
    XCTAssertEqual(size, (size_t)(1 + 2 + 4 + 8));

    XCTAssertEqual(data[0], (uint8_t)0xAB);

    uint16_t v16;
    memcpy(&v16, data + 1, 2);
    XCTAssertEqual(v16, (uint16_t)0x1234);

    uint32_t v32;
    memcpy(&v32, data + 3, 4);
    XCTAssertEqual(v32, (uint32_t)0xDEADBEEF);

    uint64_t v64;
    memcpy(&v64, data + 7, 8);
    XCTAssertEqual(v64, 0x0102030405060708ULL);

    free(data);
    unlink(path);
}

- (void)testStringTableDedup {
    mg_pool_t *pool = mg_pool_create(64 * 1024);
    XCTAssertTrue(pool != NULL);

    mg_string_table_t t;
    bool ok = mg_string_table_init(&t, pool, 100);
    XCTAssertTrue(ok);

    uint16_t idx0 = mg_string_table_add(&t, "hello");
    uint16_t idx1 = mg_string_table_add(&t, "world");
    uint16_t idx2 = mg_string_table_add(&t, "hello");

    XCTAssertEqual(idx0, (uint16_t)0);
    XCTAssertEqual(idx1, (uint16_t)1);
    XCTAssertEqual(idx2, (uint16_t)0);
    XCTAssertEqual(t.count, (uint16_t)2);

    XCTAssertEqual(mg_string_table_add(&t, NULL), MG_STRING_TABLE_INVALID);

    mg_pool_destroy(pool);
}

- (void)testStringTableOverflow {
    mg_pool_t *pool = mg_pool_create(64 * 1024);
    mg_string_table_t t;
    bool ok = mg_string_table_init(&t, pool, 3);
    XCTAssertTrue(ok);

    mg_string_table_add(&t, "a");
    mg_string_table_add(&t, "b");
    mg_string_table_add(&t, "c");
    XCTAssertEqual(t.count, (uint16_t)3);

    uint16_t idx = mg_string_table_add(&t, "d");
    XCTAssertEqual(idx, MG_STRING_TABLE_INVALID);
    XCTAssertEqual(t.count, (uint16_t)3);

    mg_pool_destroy(pool);
}

- (void)testSectionHeader {
    char path[256];
    int fd = tmp_open(path, sizeof(path));
    XCTAssertTrue(fd >= 0);

    mg_binary_writer_t w;
    mg_binary_init(&w, fd);

    mg_binary_section_header(&w, MG_SEC_METADATA, 42);
    bool ok = mg_binary_finish(&w);
    close(fd);
    XCTAssertTrue(ok);

    size_t size = 0;
    uint8_t *data = read_file_bytes(path, &size);
    XCTAssertEqual(size, (size_t)5);
    XCTAssertEqual(data[0], (uint8_t)MG_SEC_METADATA);

    uint32_t len;
    memcpy(&len, data + 1, 4);
    XCTAssertEqual(len, (uint32_t)42);

    free(data);
    unlink(path);
}

- (void)testValidateBadMagic {
    char path[256];
    int fd = tmp_open(path, sizeof(path));
    XCTAssertTrue(fd >= 0);

    uint8_t garbage[16] = {0};
    write(fd, garbage, 16);
    close(fd);

    XCTAssertTrue(mg_report_validate(path) < 0);

    unlink(path);
}

- (void)testRoundTripText {
    mg_pool_t *pool = mg_pool_create(2 * 1024 * 1024);
    XCTAssertTrue(pool != NULL);

    mg_vm_snapshot_t vm_snap;
    memset(&vm_snap, 0, sizeof(vm_snap));

    mg_heap_snapshot_t heap_snap;
    memset(&heap_snap, 0, sizeof(heap_snap));

    mg_leak_result_t leak_result;
    memset(&leak_result, 0, sizeof(leak_result));

    mg_ref_graph_result_t ref_result;
    memset(&ref_result, 0, sizeof(ref_result));

    mg_report_data_t report = {
        .trigger_reason = "manual",
        .vm_snap        = &vm_snap,
        .heap_snap      = &heap_snap,
        .leak_result    = &leak_result,
        .ref_result     = &ref_result,
        .truncated      = false,
    };

    char bin_path[256];
    snprintf(bin_path, sizeof(bin_path), "/tmp/mg_test_roundtrip.mgbin");

    int ret = mg_report_write(bin_path, &report, pool);
    XCTAssertEqual(ret, 0);

    XCTAssertEqual(mg_report_validate(bin_path), 0);

    char txt_path[256];
    snprintf(txt_path, sizeof(txt_path), "/tmp/mg_test_roundtrip.txt");

    ret = mg_report_to_text(bin_path, txt_path);
    XCTAssertEqual(ret, 0);

    size_t text_size = 0;
    uint8_t *text = read_file_bytes(txt_path, &text_size);
    XCTAssertTrue(text != NULL);
    XCTAssertTrue(text_size > 0);

    char *txt = (char *)text;
    XCTAssertTrue(strstr(txt, "=== MemoryGraph Report ===") != NULL);
    XCTAssertTrue(strstr(txt, "Trigger:   manual") != NULL);
    XCTAssertTrue(strstr(txt, MEMORY_GRAPH_VERSION) != NULL);
    XCTAssertTrue(strstr(txt, "--- Device ---") != NULL);
    XCTAssertTrue(strstr(txt, "--- App ---") != NULL);
    XCTAssertTrue(strstr(txt, "--- Heap ---") != NULL);

    free(text);
    unlink(bin_path);
    unlink(txt_path);
    mg_pool_destroy(pool);
}

@end
