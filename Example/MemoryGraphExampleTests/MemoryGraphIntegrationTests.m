/*
 * MemoryGraphIntegrationTests.m — XCTest for end-to-end SDK lifecycle
 */

#import <XCTest/XCTest.h>
#include "../../include/memory_graph.h"
#include "../../src/mg_pool.h"
#include "../../src/mg_leak.h"
#include "../../src/mg_heap.h"
#include "../../src/mg_binary.h"
#include "../../src/mg_binary_reader.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <objc/runtime.h>
#include <objc/message.h>

static uint8_t *read_file_bytes_integ(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(size);
    ssize_t n = read(fd, buf, size);
    close(fd);
    if (n <= 0) { free(buf); return NULL; }
    if (out_size) *out_size = (size_t)n;
    return buf;
}

static const char *s_callback_path = NULL;

static void test_callback_handler(const char *path) {
    s_callback_path = path;
}

@interface MemoryGraphIntegrationTests : XCTestCase
@end

@implementation MemoryGraphIntegrationTests

- (void)setUp {
    [super setUp];
    s_callback_path = NULL;
}

- (void)tearDown {
    mg_destroy();
    [super tearDown];
}

- (void)testDefaultConfig {
    mg_config_t cfg = mg_config_default();
    XCTAssertEqual(cfg.memory_threshold, (uint64_t)(100 * 1024 * 1024));
    XCTAssertEqual(cfg.polling_interval_ms, (uint32_t)1000);
    XCTAssertEqual(cfg.suspension_timeout_ms, (uint32_t)30000);
    XCTAssertEqual(cfg.pool_size, (uint64_t)(50ULL * 1024 * 1024));
    XCTAssertEqual(cfg.top_n_classes, (uint32_t)50);
    XCTAssertEqual(cfg.enable_reference_graph, true);
    XCTAssertEqual(cfg.enable_assoc_tracking, true);
    XCTAssertEqual(cfg.filter_system_cycles, true);
    XCTAssertEqual(cfg.enable_reachability_leak, true);
    XCTAssertTrue(cfg.output_dir == NULL);
}

- (void)testInitDestroy {
    mg_config_t cfg = mg_config_default();
    cfg.pool_size = 4 * 1024 * 1024;

    int ret = mg_init(&cfg);
    XCTAssertEqual(ret, MG_OK);

    ret = mg_init(&cfg);
    XCTAssertEqual(ret, MG_ERR_ALREADY_INIT);

    mg_destroy();

    ret = mg_init(&cfg);
    XCTAssertEqual(ret, MG_OK);
    /* tearDown calls mg_destroy() */
}

- (void)testManualTrigger {
    mg_config_t cfg = mg_config_default();
    cfg.pool_size = 8 * 1024 * 1024;
    cfg.output_dir = "/tmp/memorygraph_test";

    int ret = mg_init(&cfg);
    XCTAssertEqual(ret, MG_OK);

    mg_trigger_now();
    XCTAssertTrue(mg_has_pending_report());

    const char *path = mg_pending_report_path();
    XCTAssertTrue(path != NULL);
    XCTAssertTrue(strlen(path) > 0);

    const char *ext = strrchr(path, '.');
    XCTAssertTrue(ext != NULL);
    XCTAssertEqual(strcmp(ext, ".mgbin"), 0);

    size_t size = 0;
    uint8_t *data = read_file_bytes_integ(path, &size);
    XCTAssertTrue(data != NULL);
    XCTAssertTrue(size >= 16);

    uint32_t magic;
    memcpy(&magic, data, 4);
    XCTAssertEqual(magic, MG_BINARY_MAGIC);

    uint16_t version;
    memcpy(&version, data + 4, 2);
    XCTAssertEqual(version, MG_BINARY_VERSION);

    uint32_t section_count;
    memcpy(&section_count, data + 8, 4);
    XCTAssertTrue(section_count >= 7);

    XCTAssertEqual(mg_report_validate(path), 0);

    NSLog(@"Binary report: %zu bytes at %s", size, path);

    free(data);

    char text_path[512];
    snprintf(text_path, sizeof(text_path), "%s.txt", path);
    ret = mg_report_to_text(path, text_path);
    XCTAssertEqual(ret, 0);

    size_t text_size = 0;
    uint8_t *text = read_file_bytes_integ(text_path, &text_size);
    XCTAssertTrue(text != NULL);
    XCTAssertTrue(text_size > 50);
    XCTAssertTrue(strstr((char *)text, "=== MemoryGraph Report ===") != NULL);
    XCTAssertTrue(strstr((char *)text, "Trigger:   manual") != NULL);
    XCTAssertTrue(strstr((char *)text, "--- Device ---") != NULL);
    XCTAssertTrue(strstr((char *)text, "--- Heap ---") != NULL);

    free(text);
    unlink(text_path);

    mg_clear_pending_report();
    XCTAssertFalse(mg_has_pending_report());
}

- (void)testCallback {
    mg_config_t cfg = mg_config_default();
    cfg.pool_size = 8 * 1024 * 1024;
    cfg.output_dir = "/tmp/memorygraph_test";

    int ret = mg_init(&cfg);
    XCTAssertEqual(ret, MG_OK);

    mg_trigger_now();
    XCTAssertTrue(mg_has_pending_report());

    s_callback_path = NULL;
    mg_set_callback(test_callback_handler);
    mg_start();

    XCTAssertTrue(s_callback_path != NULL);
    XCTAssertTrue(strlen(s_callback_path) > 0);

    mg_stop();
    mg_clear_pending_report();
}

- (void)testReportWithObjects {
    Class nsobject = objc_getClass("NSObject");
    SEL alloc_sel = sel_registerName("alloc");
    SEL init_sel  = sel_registerName("init");
    SEL release_sel = sel_registerName("release");

    id objects[500];
    for (int i = 0; i < 500; i++) {
        id obj = ((id(*)(Class, SEL))objc_msgSend)(nsobject, alloc_sel);
        objects[i] = ((id(*)(id, SEL))objc_msgSend)(obj, init_sel);
    }

    mg_config_t cfg = mg_config_default();
    cfg.pool_size = 8 * 1024 * 1024;
    cfg.output_dir = "/tmp/memorygraph_test";
    cfg.top_n_classes = 20;

    int ret = mg_init(&cfg);
    XCTAssertEqual(ret, MG_OK);

    mg_trigger_now();
    XCTAssertTrue(mg_has_pending_report());

    const char *path = mg_pending_report_path();
    XCTAssertEqual(mg_report_validate(path), 0);

    size_t size = 0;
    uint8_t *data = read_file_bytes_integ(path, &size);
    XCTAssertTrue(data != NULL);
    XCTAssertTrue(size > 0);

    free(data);
    mg_clear_pending_report();
    mg_destroy();

    for (int i = 0; i < 500; i++) {
        ((void(*)(id, SEL))objc_msgSend)(objects[i], release_sel);
    }

    /* Re-init so tearDown's mg_destroy() is safe. */
    mg_config_t cfg2 = mg_config_default();
    cfg2.pool_size = 4 * 1024 * 1024;
    mg_init(&cfg2);
}

- (void)testLeakAnalysis {
    mg_pool_t *pool = mg_pool_create(1024 * 1024);

    mg_class_stat_t fake_classes[3] = {
        { .class_name = "LeakyClass",  .count = 50000, .total_size = 25000000 },
        { .class_name = "NormalClass", .count = 100,   .total_size = 5000 },
        { .class_name = "SmallClass",  .count = 10,    .total_size = 500 },
    };

    mg_heap_snapshot_t fake_snap = {
        .total_allocated = 26000000,
        .total_blocks    = 60000,
        .total_objects   = 50110,
        .top_classes     = fake_classes,
        .top_count       = 3,
    };

    mg_leak_result_t result;
    mg_leak_config_t cfg = mg_leak_config_default();
    int ret = mg_leak_analyze(pool, &fake_snap, &cfg, &result);
    XCTAssertEqual(ret, 0);

    XCTAssertTrue(result.suspect_count >= 1);
    XCTAssertEqual(strcmp(result.suspects[0].class_name, "LeakyClass"), 0);
    XCTAssertEqual(result.suspects[0].reason, MG_LEAK_ABNORMAL_COUNT);

    mg_pool_destroy(pool);
}

#pragma mark - Error paths & edge cases

- (void)testDestroyWithoutInit {
    /* mg_destroy() before any mg_init() should not crash. */
    mg_destroy();
}

- (void)testTriggerWithoutInit {
    /* mg_trigger_now() before mg_init() should not crash. */
    mg_trigger_now();
}

- (void)testStartStopLifecycle {
    mg_config_t cfg = mg_config_default();
    cfg.pool_size = 4 * 1024 * 1024;

    int ret = mg_init(&cfg);
    XCTAssertEqual(ret, MG_OK);

    mg_start();
    mg_stop();

    /* Second cycle should also work. */
    mg_start();
    mg_stop();
}

- (void)testPendingReportBeforeTrigger {
    mg_config_t cfg = mg_config_default();
    cfg.pool_size = 4 * 1024 * 1024;
    cfg.output_dir = "/tmp/memorygraph_test_pending";

    int ret = mg_init(&cfg);
    XCTAssertEqual(ret, MG_OK);

    /* Before any trigger, there should be no pending report. */
    XCTAssertFalse(mg_has_pending_report());
    XCTAssertTrue(mg_pending_report_path() == NULL);
}

- (void)testReportToTextInvalidPath {
    /* Non-existent file should return an error code. */
    int ret = mg_report_to_text("/tmp/nonexistent_42.mgbin",
                                "/tmp/nonexistent_42.txt");
    XCTAssertTrue(ret != 0, @"Should fail for non-existent input");
}

@end
