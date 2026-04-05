/*
 * MemoryGraphVMRegionTests.m — XCTest for VM region enumeration
 */

#import <XCTest/XCTest.h>
#include "../../src/mg_vm_region.h"
#include "../../src/mg_pool.h"
#include <string.h>

#define TEST_POOL_SIZE (4 * 1024 * 1024)

@interface MemoryGraphVMRegionTests : XCTestCase
@end

@implementation MemoryGraphVMRegionTests

- (void)testCollectBasic {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    XCTAssertTrue(pool != NULL);

    mg_vm_snapshot_t snap;
    int ret = mg_vm_region_collect(pool, &snap);
    XCTAssertEqual(ret, 0);

    XCTAssertTrue(snap.region_count > 0);
    XCTAssertTrue(snap.total_virtual > 0);
    XCTAssertTrue(snap.total_resident > 0);
    XCTAssertTrue(snap.regions != NULL);

    mg_pool_destroy(pool);
}

- (void)testTagStats {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_vm_snapshot_t snap;
    int ret = mg_vm_region_collect(pool, &snap);
    XCTAssertEqual(ret, 0);

    XCTAssertTrue(snap.tag_count > 0);
    XCTAssertTrue(snap.tag_stats != NULL);

    if (snap.tag_count > 1) {
        XCTAssertTrue(snap.tag_stats[0].total_resident >=
                       snap.tag_stats[1].total_resident);
    }

    for (uint32_t i = 0; i < snap.tag_count; i++) {
        const char *name = mg_vm_tag_name(snap.tag_stats[i].tag);
        XCTAssertTrue(name != NULL);
        XCTAssertTrue(strlen(name) > 0);
    }

    mg_pool_destroy(pool);
}

- (void)testRegionFields {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_vm_snapshot_t snap;
    mg_vm_region_collect(pool, &snap);

    for (uint32_t i = 0; i < snap.region_count && i < 10; i++) {
        mg_vm_region_t *r = &snap.regions[i];
        XCTAssertTrue(r->address > 0);
        XCTAssertTrue(r->virtual_size > 0);
    }

    mg_pool_destroy(pool);
}

- (void)testTagNameKnown {
    XCTAssertEqualObjects(@(mg_vm_tag_name(1)), @"MALLOC");
    XCTAssertEqualObjects(@(mg_vm_tag_name(2)), @"MALLOC_SMALL");
    XCTAssertEqualObjects(@(mg_vm_tag_name(30)), @"STACK");
}

- (void)testTagNameUnknown {
    XCTAssertEqualObjects(@(mg_vm_tag_name(255)), @"UNKNOWN");
}

- (void)testProtString {
    char buf[4];
    mg_vm_prot_string(0x01 | 0x02, buf);
    XCTAssertEqual(strcmp(buf, "rw-"), 0);

    mg_vm_prot_string(0x01 | 0x04, buf);
    XCTAssertEqual(strcmp(buf, "r-x"), 0);

    mg_vm_prot_string(0, buf);
    XCTAssertEqual(strcmp(buf, "---"), 0);

    mg_vm_prot_string(0x07, buf);
    XCTAssertEqual(strcmp(buf, "rwx"), 0);
}

- (void)testShareModeName {
    XCTAssertEqual(strcmp(mg_vm_share_mode_name(1), "cow"), 0);
    XCTAssertEqual(strcmp(mg_vm_share_mode_name(2), "private"), 0);
    XCTAssertEqual(strcmp(mg_vm_share_mode_name(4), "shared"), 0);
    XCTAssertEqual(strcmp(mg_vm_share_mode_name(99), "unknown"), 0);
}

- (void)testPrintSummary {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_vm_snapshot_t snap;
    mg_vm_region_collect(pool, &snap);

    NSLog(@"VM Regions: %u  Virtual: %.1f MB  Resident: %.1f MB",
          snap.region_count,
          snap.total_virtual / (1024.0 * 1024.0),
          snap.total_resident / (1024.0 * 1024.0));

    XCTAssertTrue(snap.region_count > 0);
    mg_pool_destroy(pool);
}

@end
