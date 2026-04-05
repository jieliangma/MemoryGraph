/*
 * MemoryGraphReachTests.m — XCTest for reachability-based leak detection
 */

#import <XCTest/XCTest.h>
#include "../../src/mg_reach.h"
#include "../../src/mg_ref_graph.h"
#include "../../src/mg_pool.h"
#include <string.h>

#define TEST_POOL_SIZE (4 * 1024 * 1024)
#define OBJ_SIZE 64

static uint64_t alloc_fake_obj_reach(mg_pool_t *pool) {
    void *p = mg_pool_alloc(pool, OBJ_SIZE);
    NSCAssert(p != NULL, @"pool alloc failed");
    memset(p, 0, OBJ_SIZE);
    return (uint64_t)(uintptr_t)p;
}

static void plant_ptr_reach(uint64_t obj_addr, uint32_t off, uint64_t target) {
    uintptr_t *slot = (uintptr_t *)(uintptr_t)(obj_addr + off);
    *slot = (uintptr_t)target;
}

static void bit_set(uint8_t *bits, uint32_t i) {
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

@interface MemoryGraphReachTests : XCTestCase
@end

@implementation MemoryGraphReachTests

- (void)testReachAllReachable {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj_reach(pool);
    uint64_t b = alloc_fake_obj_reach(pool);
    uint64_t c = alloc_fake_obj_reach(pool);

    plant_ptr_reach(a, sizeof(void *), b);
    plant_ptr_reach(b, sizeof(void *), c);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "ObjA", 0);
    mg_ref_graph_add_object(g, b, OBJ_SIZE, "ObjB", 0);
    mg_ref_graph_add_object(g, c, OBJ_SIZE, "ObjC", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    uint8_t root_bits[1] = {0};
    bit_set(root_bits, 0);

    mg_reach_result_t result;
    int ret = mg_reach_analyze(pool, g, root_bits, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.total_reachable, (uint32_t)3);
    XCTAssertEqual(result.total_unreachable, (uint32_t)0);
    XCTAssertEqual(result.suspect_count, (uint32_t)0);

    mg_pool_destroy(pool);
}

- (void)testReachOrphanObjects {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj_reach(pool);
    uint64_t b = alloc_fake_obj_reach(pool);
    uint64_t c = alloc_fake_obj_reach(pool);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "RootObj", 0);
    mg_ref_graph_add_object(g, b, OBJ_SIZE, "LeakyClass", 0);
    mg_ref_graph_add_object(g, c, OBJ_SIZE, "LeakyClass", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    uint8_t root_bits[1] = {0};
    bit_set(root_bits, 0);

    mg_reach_result_t result;
    int ret = mg_reach_analyze(pool, g, root_bits, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.total_reachable, (uint32_t)1);
    XCTAssertEqual(result.total_unreachable, (uint32_t)2);
    XCTAssertEqual(result.suspect_count, (uint32_t)1);
    XCTAssertEqual(strcmp(result.suspects[0].class_name, "LeakyClass"), 0);
    XCTAssertEqual(result.suspects[0].count, (uint32_t)2);

    mg_pool_destroy(pool);
}

- (void)testReachCycleUnreachable {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t r = alloc_fake_obj_reach(pool);
    uint64_t x = alloc_fake_obj_reach(pool);
    uint64_t y = alloc_fake_obj_reach(pool);

    plant_ptr_reach(x, sizeof(void *), y);
    plant_ptr_reach(y, sizeof(void *), x);

    mg_ref_graph_add_object(g, r, OBJ_SIZE, "RootObj", 0);
    mg_ref_graph_add_object(g, x, OBJ_SIZE, "CycleNodeX", 0);
    mg_ref_graph_add_object(g, y, OBJ_SIZE, "CycleNodeY", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    uint8_t root_bits[1] = {0};
    bit_set(root_bits, 0);

    mg_reach_result_t result;
    int ret = mg_reach_analyze(pool, g, root_bits, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.total_reachable, (uint32_t)1);
    XCTAssertEqual(result.total_unreachable, (uint32_t)2);
    XCTAssertEqual(result.suspect_count, (uint32_t)2);

    mg_pool_destroy(pool);
}

- (void)testReachPartial {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj_reach(pool);
    uint64_t b = alloc_fake_obj_reach(pool);
    uint64_t c = alloc_fake_obj_reach(pool);
    uint64_t d = alloc_fake_obj_reach(pool);

    plant_ptr_reach(a, sizeof(void *), b);
    plant_ptr_reach(c, sizeof(void *), d);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "ReachA", 0);
    mg_ref_graph_add_object(g, b, OBJ_SIZE, "ReachB", 0);
    mg_ref_graph_add_object(g, c, OBJ_SIZE, "LeakC", 0);
    mg_ref_graph_add_object(g, d, OBJ_SIZE, "LeakD", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    uint8_t root_bits[1] = {0};
    bit_set(root_bits, 0);

    mg_reach_result_t result;
    int ret = mg_reach_analyze(pool, g, root_bits, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.total_reachable, (uint32_t)2);
    XCTAssertEqual(result.total_unreachable, (uint32_t)2);
    XCTAssertEqual(result.suspect_count, (uint32_t)2);

    BOOL found_c = NO, found_d = NO;
    for (uint32_t i = 0; i < result.suspect_count; i++) {
        if (strcmp(result.suspects[i].class_name, "LeakC") == 0) found_c = YES;
        if (strcmp(result.suspects[i].class_name, "LeakD") == 0) found_d = YES;
    }
    XCTAssertTrue(found_c && found_d);

    mg_pool_destroy(pool);
}

- (void)testReachNullRootBits {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj_reach(pool);
    mg_ref_graph_add_object(g, a, OBJ_SIZE, "ObjA", 0);
    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_reach_result_t result;
    int ret = mg_reach_analyze(pool, g, NULL, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.suspect_count, (uint32_t)0);

    mg_pool_destroy(pool);
}

- (void)testReachSortBySize {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t r = alloc_fake_obj_reach(pool);

    void *s_mem = mg_pool_alloc(pool, OBJ_SIZE);
    memset(s_mem, 0, OBJ_SIZE);
    uint64_t s = (uint64_t)(uintptr_t)s_mem;

    void *l_mem = mg_pool_alloc(pool, 512);
    memset(l_mem, 0, 512);
    uint64_t l = (uint64_t)(uintptr_t)l_mem;

    mg_ref_graph_add_object(g, r, OBJ_SIZE, "Root", 0);
    mg_ref_graph_add_object(g, s, OBJ_SIZE, "SmallLeak", 0);
    mg_ref_graph_add_object(g, l, 512, "LargeLeak", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    uint8_t root_bits[1] = {0};
    bit_set(root_bits, 0);

    mg_reach_result_t result;
    int ret = mg_reach_analyze(pool, g, root_bits, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.suspect_count, (uint32_t)2);
    XCTAssertEqual(strcmp(result.suspects[0].class_name, "LargeLeak"), 0);
    XCTAssertEqual(result.suspects[0].total_size, (uint64_t)512);
    XCTAssertEqual(strcmp(result.suspects[1].class_name, "SmallLeak"), 0);
    XCTAssertEqual(result.suspects[1].total_size, (uint64_t)OBJ_SIZE);

    mg_pool_destroy(pool);
}

@end
