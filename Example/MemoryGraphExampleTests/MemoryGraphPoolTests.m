/*
 * MemoryGraphPoolTests.m — XCTest for mg_pool (bump allocator)
 */

#import <XCTest/XCTest.h>
#include "../../src/mg_pool.h"
#include <string.h>

#define POOL_SIZE (64 * 1024)

@interface MemoryGraphPoolTests : XCTestCase
@end

@implementation MemoryGraphPoolTests

- (void)testCreateDestroy {
    mg_pool_t *pool = mg_pool_create(POOL_SIZE);
    XCTAssertTrue(pool != NULL);
    XCTAssertTrue(mg_pool_capacity(pool) > 0);
    XCTAssertEqual(mg_pool_used(pool), (size_t)0);
    XCTAssertEqual(mg_pool_remaining(pool), mg_pool_capacity(pool));
    XCTAssertFalse(mg_pool_truncated(pool));
    mg_pool_destroy(pool);
}

- (void)testCreateTooSmall {
    mg_pool_t *pool = mg_pool_create(8);
    XCTAssertTrue(pool == NULL);
}

- (void)testAllocBasic {
    mg_pool_t *pool = mg_pool_create(POOL_SIZE);
    XCTAssertTrue(pool != NULL);

    void *p1 = mg_pool_alloc(pool, 100);
    XCTAssertTrue(p1 != NULL);
    XCTAssertTrue(mg_pool_used(pool) >= 100);

    void *p2 = mg_pool_alloc(pool, 200);
    XCTAssertTrue(p2 != NULL);
    XCTAssertTrue(p2 != p1);
    XCTAssertTrue(mg_pool_used(pool) >= 300);

    mg_pool_destroy(pool);
}

- (void)testAllocAlignment {
    mg_pool_t *pool = mg_pool_create(POOL_SIZE);

    void *p1 = mg_pool_alloc(pool, 1);
    void *p2 = mg_pool_alloc(pool, 3);
    void *p3 = mg_pool_alloc(pool, 7);

    XCTAssertTrue(p1 != NULL);
    XCTAssertTrue(p2 != NULL);
    XCTAssertTrue(p3 != NULL);

    XCTAssertEqual((uintptr_t)p1 % 8, (uintptr_t)0);
    XCTAssertEqual((uintptr_t)p2 % 8, (uintptr_t)0);
    XCTAssertEqual((uintptr_t)p3 % 8, (uintptr_t)0);

    mg_pool_destroy(pool);
}

- (void)testAllocExhaustion {
    mg_pool_t *pool = mg_pool_create(POOL_SIZE);
    size_t cap = mg_pool_capacity(pool);

    void *p = mg_pool_alloc(pool, cap - 8);
    XCTAssertTrue(p != NULL);
    XCTAssertFalse(mg_pool_truncated(pool));

    void *p2 = mg_pool_alloc(pool, cap);
    XCTAssertTrue(p2 == NULL);
    XCTAssertTrue(mg_pool_truncated(pool));

    mg_pool_destroy(pool);
}

- (void)testReset {
    mg_pool_t *pool = mg_pool_create(POOL_SIZE);

    mg_pool_alloc(pool, 1000);
    XCTAssertTrue(mg_pool_used(pool) >= 1000);

    mg_pool_reset(pool);
    XCTAssertEqual(mg_pool_used(pool), (size_t)0);
    XCTAssertFalse(mg_pool_truncated(pool));
    XCTAssertEqual(mg_pool_remaining(pool), mg_pool_capacity(pool));

    void *p = mg_pool_alloc(pool, 500);
    XCTAssertTrue(p != NULL);

    mg_pool_destroy(pool);
}

- (void)testZeroAlloc {
    mg_pool_t *pool = mg_pool_create(POOL_SIZE);

    void *p = mg_pool_alloc(pool, 0);
    XCTAssertTrue(p == NULL);
    XCTAssertEqual(mg_pool_used(pool), (size_t)0);

    mg_pool_destroy(pool);
}

- (void)testWriteToAllocated {
    mg_pool_t *pool = mg_pool_create(POOL_SIZE);

    char *buf = (char *)mg_pool_alloc(pool, 256);
    XCTAssertTrue(buf != NULL);

    const char *msg = "MemoryGraph test string";
    strcpy(buf, msg);
    XCTAssertEqual(strcmp(buf, msg), 0);

    mg_pool_destroy(pool);
}

- (void)testNullPool {
    XCTAssertTrue(mg_pool_alloc(NULL, 100) == NULL);
    XCTAssertEqual(mg_pool_used(NULL), (size_t)0);
    XCTAssertEqual(mg_pool_capacity(NULL), (size_t)0);
    XCTAssertEqual(mg_pool_remaining(NULL), (size_t)0);
    XCTAssertFalse(mg_pool_truncated(NULL));
    mg_pool_reset(NULL);
    mg_pool_destroy(NULL);
}

@end
