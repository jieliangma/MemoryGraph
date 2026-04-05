/*
 * MemoryGraphIvarMapTests.m — XCTest for ObjC ivar offset-to-name lookup
 */

#import <XCTest/XCTest.h>
#include "../../src/mg_ivar_map.h"
#include "../../src/mg_pool.h"
#include <objc/runtime.h>
#include <string.h>

#define TEST_POOL_SIZE (8 * 1024 * 1024)

@interface MemoryGraphIvarMapTests : XCTestCase
@end

@implementation MemoryGraphIvarMapTests

- (void)testBuildBasic {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    XCTAssertTrue(pool != NULL);

    mg_ivar_map_t *map = mg_ivar_map_build(pool);
    XCTAssertTrue(map != NULL);

    mg_pool_destroy(pool);
}

- (void)testBuildNullPool {
    mg_ivar_map_t *map = mg_ivar_map_build(NULL);
    XCTAssertTrue(map == NULL);
}

- (void)testLookupCustomClass {
    /* Dynamically create a class with an object-typed ivar. */
    Class cls = objc_allocateClassPair([NSObject class],
                                       "MGTestIvarClass", 0);
    XCTAssertTrue(cls != nil);

    BOOL added = class_addIvar(cls, "_testObject", sizeof(id),
                                log2(sizeof(id)), "@");
    XCTAssertTrue(added);
    objc_registerClassPair(cls);

    /* Build the ivar map — should include our new class. */
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ivar_map_t *map = mg_ivar_map_build(pool);
    XCTAssertTrue(map != NULL);

    /* Find the ivar offset. */
    Ivar ivar = class_getInstanceVariable(cls, "_testObject");
    XCTAssertTrue(ivar != NULL);
    ptrdiff_t offset = ivar_getOffset(ivar);

    const char *name = mg_ivar_map_lookup(map, "MGTestIvarClass",
                                           (uint16_t)offset);
    XCTAssertTrue(name != NULL);
    XCTAssertEqual(strcmp(name, "_testObject"), 0);

    mg_pool_destroy(pool);
    /* Note: objc_disposeClassPair not safe after instances exist,
       but we created no instances — safe to leave registered. */
}

- (void)testLookupInheritedIvar {
    /* Create parent with object ivar. */
    Class parent = objc_allocateClassPair([NSObject class],
                                          "MGTestIvarParent", 0);
    XCTAssertTrue(parent != nil);
    class_addIvar(parent, "_parentObj", sizeof(id),
                  log2(sizeof(id)), "@");
    objc_registerClassPair(parent);

    /* Create child that inherits parent's ivar. */
    Class child = objc_allocateClassPair(parent,
                                         "MGTestIvarChild", 0);
    XCTAssertTrue(child != nil);
    objc_registerClassPair(child);

    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ivar_map_t *map = mg_ivar_map_build(pool);
    XCTAssertTrue(map != NULL);

    /* Get offset from parent class. */
    Ivar ivar = class_getInstanceVariable(parent, "_parentObj");
    XCTAssertTrue(ivar != NULL);
    ptrdiff_t offset = ivar_getOffset(ivar);

    /* Lookup on child class should find inherited ivar via superclass chain. */
    const char *name = mg_ivar_map_lookup(map, "MGTestIvarChild",
                                           (uint16_t)offset);
    XCTAssertTrue(name != NULL);
    XCTAssertEqual(strcmp(name, "_parentObj"), 0);

    mg_pool_destroy(pool);
}

- (void)testLookupWrongOffset {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ivar_map_t *map = mg_ivar_map_build(pool);
    XCTAssertTrue(map != NULL);

    /* Offset 9999 is unlikely to match any real ivar. */
    const char *name = mg_ivar_map_lookup(map, "NSObject", 9999);
    XCTAssertTrue(name == NULL);

    mg_pool_destroy(pool);
}

- (void)testLookupNonObjectIvar {
    /* Create class with an int ivar (not @-encoded). */
    Class cls = objc_allocateClassPair([NSObject class],
                                       "MGTestIntIvarClass", 0);
    XCTAssertTrue(cls != nil);
    class_addIvar(cls, "_intValue", sizeof(int),
                  log2(sizeof(int)), "i");
    objc_registerClassPair(cls);

    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ivar_map_t *map = mg_ivar_map_build(pool);
    XCTAssertTrue(map != NULL);

    Ivar ivar = class_getInstanceVariable(cls, "_intValue");
    XCTAssertTrue(ivar != NULL);
    ptrdiff_t offset = ivar_getOffset(ivar);

    /* Int ivars should NOT be in the map (only @ encoding). */
    const char *name = mg_ivar_map_lookup(map, "MGTestIntIvarClass",
                                           (uint16_t)offset);
    XCTAssertTrue(name == NULL);

    mg_pool_destroy(pool);
}

- (void)testLookupNullMap {
    const char *name = mg_ivar_map_lookup(NULL, "NSObject", 8);
    XCTAssertTrue(name == NULL);
}

- (void)testLookupNullClassName {
    mg_pool_t *pool = mg_pool_create(TEST_POOL_SIZE);
    mg_ivar_map_t *map = mg_ivar_map_build(pool);
    XCTAssertTrue(map != NULL);

    const char *name = mg_ivar_map_lookup(map, NULL, 8);
    XCTAssertTrue(name == NULL);

    mg_pool_destroy(pool);
}

@end
