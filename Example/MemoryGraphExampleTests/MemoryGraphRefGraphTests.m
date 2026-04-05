/*
 * MemoryGraphRefGraphTests.m — XCTest for reference graph & retain cycle detection
 */

#import <XCTest/XCTest.h>
#include "../../src/mg_ref_graph.h"
#include "../../src/mg_heap.h"
#include "../../src/mg_assoc_tracker.h"
#include "../../src/mg_pool.h"
#include <string.h>

#define TEST_POOL_SIZE (4 * 1024 * 1024)
#define OBJ_SIZE 64

static uint64_t alloc_fake_obj(mg_pool_t *pool) {
    void *p = mg_pool_alloc(pool, OBJ_SIZE);
    NSCAssert(p != NULL, @"pool alloc failed");
    memset(p, 0, OBJ_SIZE);
    return (uint64_t)(uintptr_t)p;
}

static void plant_ptr(uint64_t obj_addr, uint32_t off, uint64_t target_addr) {
    uintptr_t *slot = (uintptr_t *)(uintptr_t)(obj_addr + off);
    *slot = (uintptr_t)target_addr;
}

@interface MemoryGraphRefGraphTests : XCTestCase
@end

@implementation MemoryGraphRefGraphTests

#pragma mark - Creation

- (void)testCreateBasic {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);
    XCTAssertTrue(g != NULL);
    mg_pool_destroy(pool);
}

- (void)testCreateNullPool {
    mg_ref_graph_t *g = mg_ref_graph_create(NULL, 100);
    XCTAssertTrue(g == NULL);
}

#pragma mark - Object overflow

- (void)testAddObjectOverflow {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 2);
    XCTAssertTrue(g != NULL);

    uint64_t a1 = alloc_fake_obj(pool);
    uint64_t a2 = alloc_fake_obj(pool);
    uint64_t a3 = alloc_fake_obj(pool);

    mg_ref_graph_add_object(g, a1, OBJ_SIZE, "ObjA", 0);
    mg_ref_graph_add_object(g, a2, OBJ_SIZE, "ObjB", 0);
    mg_ref_graph_add_object(g, a3, OBJ_SIZE, "ObjC", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertTrue(result.objects_dropped >= 1);

    mg_pool_destroy(pool);
}

#pragma mark - Edge filtering

- (void)testTaggedPointerNoEdge {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 10);

    uint64_t a1 = alloc_fake_obj(pool);
    uint64_t a2 = alloc_fake_obj(pool);

#if defined(__arm64__)
    uint64_t tagged = a2 | 0x8000000000000000ULL;
#elif defined(__x86_64__)
    uint64_t tagged = a2 | 0x1ULL;
#else
    uint64_t tagged = a2 | 0x1ULL;
#endif
    plant_ptr(a1, sizeof(void *), tagged);

    mg_ref_graph_add_object(g, a1, OBJ_SIZE, "ObjA", 0);
    mg_ref_graph_add_object(g, a2, OBJ_SIZE, "ObjB", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.total_edges, (uint64_t)0);

    mg_pool_destroy(pool);
}

- (void)testUnalignedValueNoEdge {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 10);

    uint64_t a1 = alloc_fake_obj(pool);
    uint64_t a2 = alloc_fake_obj(pool);

    plant_ptr(a1, sizeof(void *), a2 + 3);

    mg_ref_graph_add_object(g, a1, OBJ_SIZE, "ObjA", 0);
    mg_ref_graph_add_object(g, a2, OBJ_SIZE, "ObjB", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.total_edges, (uint64_t)0);

    mg_pool_destroy(pool);
}

#pragma mark - Cycle detection

- (void)testSimpleTwoNodeCycle {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj(pool);
    uint64_t b = alloc_fake_obj(pool);

    plant_ptr(a, sizeof(void *), b);
    plant_ptr(b, sizeof(void *), a);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "MGNodeA", 0);
    mg_ref_graph_add_object(g, b, OBJ_SIZE, "MGNodeB", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)1);
    XCTAssertEqual(result.cycles[0].length, (uint32_t)2);
    XCTAssertEqual(result.cycles[0].instance_count, (uint32_t)1);

    mg_pool_destroy(pool);
}

- (void)testThreeNodeCycle {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj(pool);
    uint64_t b = alloc_fake_obj(pool);
    uint64_t c = alloc_fake_obj(pool);

    plant_ptr(a, sizeof(void *), b);
    plant_ptr(b, sizeof(void *), c);
    plant_ptr(c, sizeof(void *), a);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "AppNodeA", 0);
    mg_ref_graph_add_object(g, b, OBJ_SIZE, "AppNodeB", 0);
    mg_ref_graph_add_object(g, c, OBJ_SIZE, "AppNodeC", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)1);
    XCTAssertEqual(result.cycles[0].length, (uint32_t)3);

    mg_pool_destroy(pool);
}

- (void)testNoCycleLinear {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj(pool);
    uint64_t b = alloc_fake_obj(pool);
    uint64_t c = alloc_fake_obj(pool);

    plant_ptr(a, sizeof(void *), b);
    plant_ptr(b, sizeof(void *), c);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "ObjA", 0);
    mg_ref_graph_add_object(g, b, OBJ_SIZE, "ObjB", 0);
    mg_ref_graph_add_object(g, c, OBJ_SIZE, "ObjC", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)0);

    mg_pool_destroy(pool);
}

- (void)testSelfReferenceIgnored {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj(pool);
    plant_ptr(a, sizeof(void *), a);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "ObjA", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.total_edges, (uint64_t)0);
    XCTAssertEqual(result.cycle_count, (uint32_t)0);

    mg_pool_destroy(pool);
}

#pragma mark - Dedup & filtering

- (void)testDedupInstanceCount {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a1 = alloc_fake_obj(pool);
    uint64_t b1 = alloc_fake_obj(pool);
    plant_ptr(a1, sizeof(void *), b1);
    plant_ptr(b1, sizeof(void *), a1);

    uint64_t a2 = alloc_fake_obj(pool);
    uint64_t b2 = alloc_fake_obj(pool);
    plant_ptr(a2, sizeof(void *), b2);
    plant_ptr(b2, sizeof(void *), a2);

    mg_ref_graph_add_object(g, a1, OBJ_SIZE, "MGNodeA", 0);
    mg_ref_graph_add_object(g, b1, OBJ_SIZE, "MGNodeB", 0);
    mg_ref_graph_add_object(g, a2, OBJ_SIZE, "MGNodeA", 0);
    mg_ref_graph_add_object(g, b2, OBJ_SIZE, "MGNodeB", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)1);
    XCTAssertEqual(result.cycles[0].instance_count, (uint32_t)2);
    XCTAssertEqual(result.cycles[0].total_size, (uint64_t)(OBJ_SIZE * 4));

    mg_pool_destroy(pool);
}

- (void)testSystemClassFiltering {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj(pool);
    uint64_t b = alloc_fake_obj(pool);

    plant_ptr(a, sizeof(void *), b);
    plant_ptr(b, sizeof(void *), a);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "NSObject", 0);
    mg_ref_graph_add_object(g, b, OBJ_SIZE, "UIView", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = true };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)0);
    XCTAssertEqual(result.system_cycles_filtered, (uint32_t)1);

    mg_pool_destroy(pool);
}

- (void)testMixedCycleNotFiltered {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj(pool);
    uint64_t b = alloc_fake_obj(pool);

    plant_ptr(a, sizeof(void *), b);
    plant_ptr(b, sizeof(void *), a);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "NSObject", 0);
    mg_ref_graph_add_object(g, b, OBJ_SIZE, "MyAppClass", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = true };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)1);
    XCTAssertTrue(result.cycles[0].is_app_cycle == true);

    mg_pool_destroy(pool);
}

#pragma mark - Impact & path

- (void)testImpactSort {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t x = alloc_fake_obj(pool);
    uint64_t y = alloc_fake_obj(pool);
    plant_ptr(x, sizeof(void *), y);
    plant_ptr(y, sizeof(void *), x);

    void *p_mem = mg_pool_alloc(pool, 512);
    void *q_mem = mg_pool_alloc(pool, 512);
    memset(p_mem, 0, 512);
    memset(q_mem, 0, 512);
    uint64_t p = (uint64_t)(uintptr_t)p_mem;
    uint64_t q = (uint64_t)(uintptr_t)q_mem;
    plant_ptr(p, sizeof(void *), q);
    plant_ptr(q, sizeof(void *), p);

    mg_ref_graph_add_object(g, x, OBJ_SIZE, "SmallA", 0);
    mg_ref_graph_add_object(g, y, OBJ_SIZE, "SmallB", 0);
    mg_ref_graph_add_object(g, p, 512, "LargeA", 0);
    mg_ref_graph_add_object(g, q, 512, "LargeB", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)2);
    XCTAssertTrue(result.cycles[0].total_size >= result.cycles[1].total_size);
    XCTAssertEqual(result.cycles[0].total_size, (uint64_t)1024);

    mg_pool_destroy(pool);
}

- (void)testCyclePathDirection {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj(pool);
    uint64_t b = alloc_fake_obj(pool);

    plant_ptr(a, sizeof(void *), b);
    plant_ptr(b, sizeof(void *), a);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "CycleA", 0);
    mg_ref_graph_add_object(g, b, OBJ_SIZE, "CycleB", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)1);

    mg_retain_cycle_t *cyc = &result.cycles[0];
    XCTAssertEqual(cyc->length, (uint32_t)2);

    XCTAssertEqual(strcmp(cyc->steps[0].to_class, cyc->steps[1].from_class), 0);
    XCTAssertEqual(strcmp(cyc->steps[cyc->length - 1].to_class,
                          cyc->steps[0].from_class), 0);

    mg_pool_destroy(pool);
}

- (void)testIvarNameDefault {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj(pool);
    uint64_t b = alloc_fake_obj(pool);

    plant_ptr(a, sizeof(void *), b);
    plant_ptr(b, sizeof(void *), a);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "TestA", 0);
    mg_ref_graph_add_object(g, b, OBJ_SIZE, "TestB", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)1);

    for (uint32_t i = 0; i < result.cycles[0].length; i++) {
        XCTAssertEqual(strcmp(result.cycles[0].steps[i].ivar_name, "?"), 0);
    }

    mg_pool_destroy(pool);
}

#pragma mark - Edge packing

- (void)testEdgePacking {
    uint32_t target = 12345;
    uint32_t offset = 128;
    uint32_t packed = MG_EDGE_PACK(target, offset);
    XCTAssertEqual(MG_EDGE_TARGET(packed), target);
    XCTAssertEqual(MG_EDGE_OFFSET(packed), offset);

    packed = MG_EDGE_PACK(MG_EDGE_TARGET_MAX, MG_EDGE_OFFSET_MAX);
    XCTAssertEqual(MG_EDGE_TARGET(packed), MG_EDGE_TARGET_MAX);
    XCTAssertEqual(MG_EDGE_OFFSET(packed), MG_EDGE_OFFSET_MAX);

    packed = MG_EDGE_PACK(0, 0);
    XCTAssertEqual(MG_EDGE_TARGET(packed), (uint32_t)0);
    XCTAssertEqual(MG_EDGE_OFFSET(packed), (uint32_t)0);
}

#pragma mark - Block edge types

- (void)testBlockCaptureEdgeType {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t ctrl = alloc_fake_obj(pool);
    uint64_t blk  = alloc_fake_obj(pool);

    plant_ptr(ctrl, sizeof(void *), blk);
    plant_ptr(blk, 32, ctrl);

    mg_ref_graph_add_object(g, ctrl, OBJ_SIZE, "MyController", 0);
    mg_ref_graph_add_object(g, blk,  OBJ_SIZE, "__NSMallocBlock__",
                            MG_OBJ_FLAG_BLOCK);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)1);
    XCTAssertEqual(result.cycles[0].length, (uint32_t)2);

    mg_retain_cycle_t *cyc = &result.cycles[0];
    BOOL found_block_edge = NO;
    for (uint32_t i = 0; i < cyc->length; i++) {
        if (strcmp(cyc->steps[i].from_class, "__NSMallocBlock__") == 0) {
            XCTAssertEqual(cyc->steps[i].edge_type, MG_EDGE_BLOCK);
            XCTAssertEqual(strcmp(cyc->steps[i].ivar_name, "block_capture"), 0);
            found_block_edge = YES;
        }
    }
    XCTAssertTrue(found_block_edge);

    mg_pool_destroy(pool);
}

- (void)testNonBlockEdgeType {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj(pool);
    uint64_t b = alloc_fake_obj(pool);

    plant_ptr(a, sizeof(void *), b);
    plant_ptr(b, sizeof(void *), a);

    mg_ref_graph_add_object(g, a, OBJ_SIZE, "ObjA", 0);
    mg_ref_graph_add_object(g, b, OBJ_SIZE, "ObjB", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)1);

    for (uint32_t i = 0; i < result.cycles[0].length; i++) {
        XCTAssertEqual(result.cycles[0].steps[i].edge_type, MG_EDGE_UNKNOWN);
        XCTAssertEqual(strcmp(result.cycles[0].steps[i].ivar_name, "?"), 0);
    }

    mg_pool_destroy(pool);
}

- (void)testBlockInternalEdge {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t blk  = alloc_fake_obj(pool);
    uint64_t obj  = alloc_fake_obj(pool);

    plant_ptr(blk, sizeof(void *), obj);
    plant_ptr(obj, sizeof(void *), blk);

    mg_ref_graph_add_object(g, blk, OBJ_SIZE, "__NSAutoBlock__",
                            MG_OBJ_FLAG_BLOCK);
    mg_ref_graph_add_object(g, obj, OBJ_SIZE, "SomeObject", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)1);

    mg_retain_cycle_t *cyc = &result.cycles[0];
    for (uint32_t i = 0; i < cyc->length; i++) {
        if (strcmp(cyc->steps[i].from_class, "__NSAutoBlock__") == 0) {
            XCTAssertEqual(strcmp(cyc->steps[i].ivar_name, "block_internal"), 0);
            XCTAssertEqual(cyc->steps[i].edge_type, MG_EDGE_UNKNOWN);
        }
    }

    mg_pool_destroy(pool);
}

#pragma mark - Indirect / collection edges

- (void)testIndirectCollectionEdge {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t parent = alloc_fake_obj(pool);
    uint64_t child  = alloc_fake_obj(pool);

    void *buf_mem = mg_pool_alloc(pool, 64);
    memset(buf_mem, 0, 64);
    uint64_t buf_addr = (uint64_t)(uintptr_t)buf_mem;

    plant_ptr(parent, sizeof(void *), buf_addr);
    *(uintptr_t *)buf_mem = (uintptr_t)child;
    plant_ptr(child, sizeof(void *), parent);

    mg_ref_graph_add_object(g, parent, OBJ_SIZE, "ParentObj", 0);
    mg_ref_graph_add_object(g, child,  OBJ_SIZE, "ChildObj", 0);

    mg_ref_graph_build_index(g);

    mg_buffer_table_t bt;
    mg_buffer_entry_t entry = { .addr = buf_addr, .size = 64 };
    bt.entries  = &entry;
    bt.count    = 1;
    bt.capacity = 1;

    mg_ref_graph_scan_references(g, &bt);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)1);
    XCTAssertEqual(result.cycles[0].length, (uint32_t)2);

    mg_retain_cycle_t *cyc = &result.cycles[0];
    BOOL found_collection = NO;
    for (uint32_t i = 0; i < cyc->length; i++) {
        if (strcmp(cyc->steps[i].from_class, "ParentObj") == 0 &&
            strcmp(cyc->steps[i].to_class, "ChildObj") == 0) {
            XCTAssertEqual(cyc->steps[i].edge_type, MG_EDGE_COLLECTION);
            XCTAssertEqual(strcmp(cyc->steps[i].ivar_name, "collection"), 0);
            found_collection = YES;
        }
    }
    XCTAssertTrue(found_collection);

    mg_pool_destroy(pool);
}

- (void)testIndirectEdgePacking {
    uint32_t target = 42;
    uint32_t offset = 64;

    uint32_t direct = MG_EDGE_PACK(target, offset);
    XCTAssertEqual(MG_EDGE_TARGET(direct), target);
    XCTAssertEqual(MG_EDGE_OFFSET(direct), offset);
    XCTAssertFalse(MG_EDGE_IS_INDIRECT(direct));

    uint32_t indirect = MG_EDGE_PACK_INDIRECT(target, offset);
    XCTAssertEqual(MG_EDGE_TARGET(indirect), target);
    XCTAssertEqual(MG_EDGE_OFFSET(indirect), offset);
    XCTAssertTrue(MG_EDGE_IS_INDIRECT(indirect));
}

#pragma mark - Buffer table

- (void)testBufferTableLookup {
    mg_buffer_entry_t entries[3] = {
        { .addr = 1000, .size = 100 },
        { .addr = 2000, .size = 200 },
        { .addr = 5000, .size = 50 },
    };
    mg_buffer_table_t bt = {
        .entries  = entries,
        .count    = 3,
        .capacity = 3,
    };

    const mg_buffer_entry_t *e = mg_buffer_table_lookup(&bt, 1000);
    XCTAssertTrue(e != NULL);
    XCTAssertEqual(e->addr, (uint64_t)1000);

    e = mg_buffer_table_lookup(&bt, 2050);
    XCTAssertTrue(e != NULL);
    XCTAssertEqual(e->addr, (uint64_t)2000);

    e = mg_buffer_table_lookup(&bt, 5049);
    XCTAssertTrue(e != NULL);
    XCTAssertEqual(e->addr, (uint64_t)5000);

    e = mg_buffer_table_lookup(&bt, 5050);
    XCTAssertTrue(e == NULL);

    e = mg_buffer_table_lookup(&bt, 1500);
    XCTAssertTrue(e == NULL);

    e = mg_buffer_table_lookup(&bt, 500);
    XCTAssertTrue(e == NULL);

    mg_buffer_table_t empty = { .entries = NULL, .count = 0, .capacity = 0 };
    e = mg_buffer_table_lookup(&empty, 1000);
    XCTAssertTrue(e == NULL);
}

#pragma mark - Associated objects

- (void)testAssociatedObjectEdge {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t ctrl = alloc_fake_obj(pool);
    uint64_t obs  = alloc_fake_obj(pool);

    plant_ptr(obs, sizeof(void *), ctrl);

    mg_ref_graph_add_object(g, ctrl, OBJ_SIZE, "MyController", 0);
    mg_ref_graph_add_object(g, obs,  OBJ_SIZE, "MyObserver", 0);

    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)0);

    mg_assoc_entry_t assoc = {
        .object = (uintptr_t)ctrl,
        .value  = (uintptr_t)obs,
    };

    mg_pool_reset(pool);
    g = mg_ref_graph_create(pool, 100);
    mg_ref_graph_add_object(g, ctrl, OBJ_SIZE, "MyController", 0);
    mg_ref_graph_add_object(g, obs,  OBJ_SIZE, "MyObserver", 0);
    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);
    mg_ref_graph_inject_associations(g, &assoc, 1);

    memset(&result, 0, sizeof(result));
    ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)1);
    XCTAssertEqual(result.cycles[0].length, (uint32_t)2);

    mg_retain_cycle_t *cyc = &result.cycles[0];
    BOOL found_assoc = NO;
    for (uint32_t i = 0; i < cyc->length; i++) {
        if (strcmp(cyc->steps[i].from_class, "MyController") == 0 &&
            strcmp(cyc->steps[i].to_class, "MyObserver") == 0) {
            XCTAssertEqual(cyc->steps[i].edge_type, MG_EDGE_ASSOCIATED);
            XCTAssertEqual(strcmp(cyc->steps[i].ivar_name,
                                  "associated_object"), 0);
            found_assoc = YES;
        }
    }
    XCTAssertTrue(found_assoc);

    mg_pool_destroy(pool);
}

- (void)testAssociatedNoMatch {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ref_graph_t *g = mg_ref_graph_create(pool, 100);

    uint64_t a = alloc_fake_obj(pool);
    mg_ref_graph_add_object(g, a, OBJ_SIZE, "ObjA", 0);
    mg_ref_graph_build_index(g);
    mg_ref_graph_scan_references(g, NULL);

    mg_assoc_entry_t assoc = { .object = 0xDEAD, .value = 0xBEEF };
    mg_ref_graph_inject_associations(g, &assoc, 1);

    mg_ref_graph_result_t result;
    mg_ref_graph_options_t opts = { .filter_system_cycles = false };
    int ret = mg_ref_graph_find_cycles(pool, g, NULL, &opts, &result);
    XCTAssertEqual(ret, 0);
    XCTAssertEqual(result.cycle_count, (uint32_t)0);

    mg_pool_destroy(pool);
}

@end
