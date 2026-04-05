/*
 * MemoryGraphLeakTests.m — XCTest for statistical leak analysis
 */

#import <XCTest/XCTest.h>
#include "../../src/mg_leak.h"
#include "../../src/mg_pool.h"
#include "../../include/memory_graph.h"
#include <string.h>

@interface MemoryGraphLeakTests : XCTestCase
@end

@implementation MemoryGraphLeakTests

#pragma mark - Config defaults

- (void)testConfigDefault {
    mg_leak_config_t cfg = mg_leak_config_default();
    XCTAssertEqual(cfg.count_threshold, (uint32_t)10000);
    XCTAssertEqualWithAccuracy(cfg.size_ratio, 0.30, 0.001);
    XCTAssertEqualWithAccuracy(cfg.count_ratio, 0.50, 0.001);
}

#pragma mark - Reason strings

- (void)testReasonStringAbnormalCount {
    XCTAssertEqual(strcmp(mg_leak_reason_string(MG_LEAK_ABNORMAL_COUNT),
                          "abnormal_count"), 0);
}

- (void)testReasonStringAbnormalSize {
    XCTAssertEqual(strcmp(mg_leak_reason_string(MG_LEAK_ABNORMAL_SIZE),
                          "abnormal_size"), 0);
}

- (void)testReasonStringHighRatio {
    XCTAssertEqual(strcmp(mg_leak_reason_string(MG_LEAK_HIGH_RATIO),
                          "high_ratio"), 0);
}

- (void)testReasonStringUnknown {
    XCTAssertEqual(strcmp(mg_leak_reason_string((mg_leak_reason_t)99),
                          "unknown"), 0);
}

#pragma mark - Analyze scenarios

- (void)testAnalyzeAbnormalCount {
    mg_pool_t *pool = mg_pool_create(1024 * 1024);

    mg_class_stat_t classes[1] = {
        { .class_name = "LeakyClass", .count = 50000, .total_size = 1000000 },
    };
    mg_heap_snapshot_t snap = {
        .total_allocated = 100000000,
        .total_blocks = 60000,
        .total_objects = 100000,
        .top_classes = classes,
        .top_count = 1,
    };

    mg_leak_result_t result;
    mg_leak_config_t cfg = mg_leak_config_default();
    int ret = mg_leak_analyze(pool, &snap, &cfg, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.suspect_count, (uint32_t)1);
    XCTAssertEqual(strcmp(result.suspects[0].class_name, "LeakyClass"), 0);
    XCTAssertEqual(result.suspects[0].reason, MG_LEAK_ABNORMAL_COUNT);

    mg_pool_destroy(pool);
}

- (void)testAnalyzeAbnormalSize {
    mg_pool_t *pool = mg_pool_create(1024 * 1024);

    /* count=100 (below 10000), but size dominates heap (>30%). */
    mg_class_stat_t classes[1] = {
        { .class_name = "BigClass", .count = 100, .total_size = 40000000 },
    };
    mg_heap_snapshot_t snap = {
        .total_allocated = 100000000,
        .total_blocks = 1000,
        .total_objects = 1000,
        .top_classes = classes,
        .top_count = 1,
    };

    mg_leak_result_t result;
    mg_leak_config_t cfg = mg_leak_config_default();
    int ret = mg_leak_analyze(pool, &snap, &cfg, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.suspect_count, (uint32_t)1);
    XCTAssertEqual(result.suspects[0].reason, MG_LEAK_ABNORMAL_SIZE);

    mg_pool_destroy(pool);
}

- (void)testAnalyzeHighRatio {
    mg_pool_t *pool = mg_pool_create(1024 * 1024);

    /* count=600, total_objects=1000 → ratio 60% > 50%. */
    mg_class_stat_t classes[1] = {
        { .class_name = "DominantClass", .count = 600, .total_size = 5000 },
    };
    mg_heap_snapshot_t snap = {
        .total_allocated = 100000000,
        .total_blocks = 1000,
        .total_objects = 1000,
        .top_classes = classes,
        .top_count = 1,
    };

    mg_leak_result_t result;
    mg_leak_config_t cfg = mg_leak_config_default();
    int ret = mg_leak_analyze(pool, &snap, &cfg, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.suspect_count, (uint32_t)1);
    XCTAssertEqual(result.suspects[0].reason, MG_LEAK_HIGH_RATIO);

    mg_pool_destroy(pool);
}

- (void)testAnalyzeNoSuspects {
    mg_pool_t *pool = mg_pool_create(1024 * 1024);

    mg_class_stat_t classes[2] = {
        { .class_name = "NormalA", .count = 100, .total_size = 5000 },
        { .class_name = "NormalB", .count = 50, .total_size = 2000 },
    };
    mg_heap_snapshot_t snap = {
        .total_allocated = 100000000,
        .total_blocks = 10000,
        .total_objects = 10000,
        .top_classes = classes,
        .top_count = 2,
    };

    mg_leak_result_t result;
    mg_leak_config_t cfg = mg_leak_config_default();
    int ret = mg_leak_analyze(pool, &snap, &cfg, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.suspect_count, (uint32_t)0);

    mg_pool_destroy(pool);
}

- (void)testAnalyzeNullArgs {
    mg_pool_t *pool = mg_pool_create(1024 * 1024);
    mg_heap_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    mg_leak_result_t result;

    XCTAssertEqual(mg_leak_analyze(NULL, &snap, NULL, &result), MG_ERR_INVALID_ARG);
    XCTAssertEqual(mg_leak_analyze(pool, NULL, NULL, &result), MG_ERR_INVALID_ARG);
    XCTAssertEqual(mg_leak_analyze(pool, &snap, NULL, NULL), MG_ERR_INVALID_ARG);

    mg_pool_destroy(pool);
}

@end
