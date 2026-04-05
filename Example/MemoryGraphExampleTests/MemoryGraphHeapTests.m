/*
 * MemoryGraphHeapTests.m — XCTest for heap enumeration + C malloc leak detection
 */

#import <XCTest/XCTest.h>
#include "../../src/mg_heap.h"
#include "../../src/mg_pool.h"
#include <string.h>
#include <objc/runtime.h>
#include <objc/message.h>

#define TEST_POOL_SIZE (8 * 1024 * 1024)

@interface MemoryGraphHeapTests : XCTestCase
@end

@implementation MemoryGraphHeapTests

#pragma mark - Basic heap enumeration

- (void)testClassSetBuild {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    XCTAssertTrue(pool != NULL);

    mg_class_set_t *set = mg_class_set_build(pool);
    XCTAssertTrue(set != NULL);

    mg_pool_destroy(pool);
}

- (void)testHeapCollectBasic {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set = mg_class_set_build(pool);
    XCTAssertTrue(set != NULL);

    mg_heap_snapshot_t snap;
    int ret = mg_heap_collect(pool, set, 50, &snap, NULL);
    XCTAssertEqual(ret, 0);

    XCTAssertTrue(snap.zone_count > 0);
    XCTAssertTrue(snap.total_blocks > 0);
    XCTAssertTrue(snap.total_allocated > 0);
    XCTAssertTrue(snap.total_objects > 0);

    mg_pool_destroy(pool);
}

- (void)testHeapZones {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set = mg_class_set_build(pool);
    mg_heap_snapshot_t snap;
    mg_heap_collect(pool, set, 50, &snap, NULL);

    BOOL found_named = NO;
    for (uint32_t i = 0; i < snap.zone_count; i++) {
        if (snap.zones[i].name && strlen(snap.zones[i].name) > 0) {
            found_named = YES;
            break;
        }
    }
    XCTAssertTrue(found_named);

    mg_pool_destroy(pool);
}

- (void)testHeapTopClasses {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set = mg_class_set_build(pool);
    mg_heap_snapshot_t snap;
    mg_heap_collect(pool, set, 10, &snap, NULL);

    XCTAssertTrue(snap.top_count > 0);
    XCTAssertTrue(snap.top_classes != NULL);

    for (uint32_t i = 1; i < snap.top_count; i++) {
        XCTAssertTrue(snap.top_classes[i - 1].total_size >=
                       snap.top_classes[i].total_size);
    }

    for (uint32_t i = 0; i < snap.top_count; i++) {
        XCTAssertTrue(snap.top_classes[i].class_name != NULL);
        XCTAssertTrue(strlen(snap.top_classes[i].class_name) > 0);
        XCTAssertTrue(snap.top_classes[i].count > 0);
        XCTAssertTrue(snap.top_classes[i].total_size > 0);
    }

    mg_pool_destroy(pool);
}

- (void)testHeapWithKnownObjects {
    Class nsobject = objc_getClass("NSObject");
    XCTAssertTrue(nsobject != NULL);

    SEL alloc_sel = sel_registerName("alloc");
    SEL init_sel  = sel_registerName("init");

    id objects[100];
    for (int i = 0; i < 100; i++) {
        id obj = ((id(*)(Class, SEL))objc_msgSend)(nsobject, alloc_sel);
        objects[i] = ((id(*)(id, SEL))objc_msgSend)(obj, init_sel);
    }

    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set = mg_class_set_build(pool);
    mg_heap_snapshot_t snap;
    mg_heap_collect(pool, set, 50, &snap, NULL);

    XCTAssertTrue(snap.total_objects > 0);

    SEL release_sel = sel_registerName("release");
    for (int i = 0; i < 100; i++) {
        ((void(*)(id, SEL))objc_msgSend)(objects[i], release_sel);
    }

    mg_pool_destroy(pool);
}

#pragma mark - C malloc leak detection

- (void)testMallocLeakCaptured {
    mg_pool_t *pool1 = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set1 = mg_class_set_build(pool1);
    mg_heap_snapshot_t before;
    mg_heap_collect(pool1, set1, 10, &before, NULL);
    mg_pool_destroy(pool1);

    #define MALLOC_LEAK_COUNT 1000
    #define MALLOC_LEAK_SIZE  4096
    void *ptrs[MALLOC_LEAK_COUNT];
    for (int i = 0; i < MALLOC_LEAK_COUNT; i++) {
        ptrs[i] = malloc(MALLOC_LEAK_SIZE);
        XCTAssertTrue(ptrs[i] != NULL);
        memset(ptrs[i], 0xAB, MALLOC_LEAK_SIZE);
    }

    mg_pool_t *pool2 = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set2 = mg_class_set_build(pool2);
    mg_heap_snapshot_t after;
    mg_heap_collect(pool2, set2, 10, &after, NULL);
    mg_pool_destroy(pool2);

    XCTAssertTrue(after.total_blocks >= before.total_blocks + MALLOC_LEAK_COUNT);
    XCTAssertTrue(after.total_allocated >=
                   before.total_allocated + (uint64_t)MALLOC_LEAK_COUNT * MALLOC_LEAK_SIZE);
    XCTAssertTrue(after.raw_allocations >= before.raw_allocations + MALLOC_LEAK_COUNT);

    for (int i = 0; i < MALLOC_LEAK_COUNT; i++) free(ptrs[i]);
}

- (void)testCallocLeakCaptured {
    mg_pool_t *pool1 = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set1 = mg_class_set_build(pool1);
    mg_heap_snapshot_t before;
    mg_heap_collect(pool1, set1, 10, &before, NULL);
    mg_pool_destroy(pool1);

    #define CALLOC_COUNT 500
    #define CALLOC_SIZE  8192
    void *cptrs[CALLOC_COUNT];
    for (int i = 0; i < CALLOC_COUNT; i++) {
        cptrs[i] = calloc(1, CALLOC_SIZE);
        XCTAssertTrue(cptrs[i] != NULL);
    }

    mg_pool_t *pool2 = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set2 = mg_class_set_build(pool2);
    mg_heap_snapshot_t after;
    mg_heap_collect(pool2, set2, 10, &after, NULL);
    mg_pool_destroy(pool2);

    XCTAssertTrue(after.total_blocks >= before.total_blocks + CALLOC_COUNT);
    XCTAssertTrue(after.total_allocated >=
                   before.total_allocated + (uint64_t)CALLOC_COUNT * CALLOC_SIZE);
    XCTAssertTrue(after.raw_allocations >= before.raw_allocations + CALLOC_COUNT);

    for (int i = 0; i < CALLOC_COUNT; i++) free(cptrs[i]);
}

- (void)testReallocLeakCaptured {
    mg_pool_t *pool1 = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set1 = mg_class_set_build(pool1);
    mg_heap_snapshot_t before;
    mg_heap_collect(pool1, set1, 10, &before, NULL);
    mg_pool_destroy(pool1);

    #define REALLOC_COUNT 200
    #define REALLOC_SIZE  16384
    void *rptrs[REALLOC_COUNT];
    for (int i = 0; i < REALLOC_COUNT; i++) {
        rptrs[i] = malloc(64);
        XCTAssertTrue(rptrs[i] != NULL);
        rptrs[i] = realloc(rptrs[i], REALLOC_SIZE);
        XCTAssertTrue(rptrs[i] != NULL);
        memset(rptrs[i], 0xCD, REALLOC_SIZE);
    }

    mg_pool_t *pool2 = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set2 = mg_class_set_build(pool2);
    mg_heap_snapshot_t after;
    mg_heap_collect(pool2, set2, 10, &after, NULL);
    mg_pool_destroy(pool2);

    XCTAssertTrue(after.total_blocks >= before.total_blocks + REALLOC_COUNT);
    XCTAssertTrue(after.total_allocated >=
                   before.total_allocated + (uint64_t)REALLOC_COUNT * REALLOC_SIZE);

    for (int i = 0; i < REALLOC_COUNT; i++) free(rptrs[i]);
}

- (void)testMallocNotInTopClasses {
    #define BIG_ALLOC_COUNT 100
    #define BIG_ALLOC_SIZE  (64 * 1024)
    void *bptrs[BIG_ALLOC_COUNT];
    for (int i = 0; i < BIG_ALLOC_COUNT; i++) {
        bptrs[i] = malloc(BIG_ALLOC_SIZE);
        XCTAssertTrue(bptrs[i] != NULL);
        memset(bptrs[i], 0xEF, BIG_ALLOC_SIZE);
    }

    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set = mg_class_set_build(pool);
    mg_heap_snapshot_t snap;
    mg_heap_collect(pool, set, 50, &snap, NULL);

    for (uint32_t i = 0; i < snap.top_count; i++) {
        XCTAssertTrue(snap.top_classes[i].class_name != NULL);
        XCTAssertTrue(strlen(snap.top_classes[i].class_name) > 0);
        Class cls = objc_getClass(snap.top_classes[i].class_name);
        XCTAssertTrue(cls != NULL, @"top_classes should only contain real ObjC classes, got: %s",
                       snap.top_classes[i].class_name);
    }

    mg_pool_destroy(pool);
    for (int i = 0; i < BIG_ALLOC_COUNT; i++) free(bptrs[i]);
}

- (void)testMallocZoneStats {
    mg_pool_t *pool1 = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set1 = mg_class_set_build(pool1);
    mg_heap_snapshot_t before;
    mg_heap_collect(pool1, set1, 10, &before, NULL);

    uint64_t zone_total_before = 0;
    for (uint32_t i = 0; i < before.zone_count; i++) {
        zone_total_before += before.zones[i].allocated;
    }
    mg_pool_destroy(pool1);

    #define ZONE_TEST_COUNT 256
    #define ZONE_TEST_SIZE  8192
    void *zptrs[ZONE_TEST_COUNT];
    for (int i = 0; i < ZONE_TEST_COUNT; i++) {
        zptrs[i] = malloc(ZONE_TEST_SIZE);
        XCTAssertTrue(zptrs[i] != NULL);
        memset(zptrs[i], 0x55, ZONE_TEST_SIZE);
    }

    mg_pool_t *pool2 = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set2 = mg_class_set_build(pool2);
    mg_heap_snapshot_t after;
    mg_heap_collect(pool2, set2, 10, &after, NULL);

    uint64_t zone_total_after = 0;
    for (uint32_t i = 0; i < after.zone_count; i++) {
        zone_total_after += after.zones[i].allocated;
    }
    mg_pool_destroy(pool2);

    XCTAssertEqual(zone_total_after, after.total_allocated);
    XCTAssertTrue(zone_total_after >=
                   zone_total_before + (uint64_t)ZONE_TEST_COUNT * ZONE_TEST_SIZE);

    for (int i = 0; i < ZONE_TEST_COUNT; i++) free(zptrs[i]);
}

- (void)testPrintSummary {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_class_set_t *set = mg_class_set_build(pool);
    mg_heap_snapshot_t snap;
    mg_heap_collect(pool, set, 10, &snap, NULL);

    NSLog(@"Heap: Zones=%u Blocks=%llu Allocated=%.1f MB ObjC=%llu Raw=%llu",
          snap.zone_count, snap.total_blocks,
          snap.total_allocated / (1024.0 * 1024.0),
          snap.total_objects, snap.raw_allocations);

    XCTAssertTrue(snap.zone_count > 0);
    mg_pool_destroy(pool);
}

@end
