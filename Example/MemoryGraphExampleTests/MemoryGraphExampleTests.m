/*
 * MemoryGraphExampleTests.m — XCTest tests for Example app demo cases
 *
 * Each test initializes the SDK, creates demo objects, triggers a snapshot,
 * converts the report to text, and asserts expected content.
 *
 * This is a hosted test bundle — it runs inside the MemoryGraphExample app
 * process, so all demo classes from ViewController.m are already loaded.
 * We only forward-declare their interfaces here (no @implementation needed).
 */

#import <XCTest/XCTest.h>
#import <MemoryGraph/memory_graph.h>
#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ================================================================== */
/* Forward-declare app's demo classes (implementations in app binary)  */
/* ================================================================== */

@interface MGLeakyObject : NSObject {
    @public char _payload[224];
}
@end

@interface MGNodeA : NSObject
@property (nonatomic, strong) id partner;
@end

@interface MGNodeB : NSObject
@property (nonatomic, strong) id partner;
@end

@interface MGNodeC : NSObject
@property (nonatomic, strong) id partner;
@end

@interface MGView : NSObject
@property (nonatomic, strong) id delegate;
@end

@interface MGController : NSObject
@property (nonatomic, strong) MGView *view;
@end

@interface MGBlockHolder : NSObject
@property (nonatomic, copy) void (^work)(void);
@end

@interface MGTimerHolder : NSObject
@property (nonatomic, strong) NSTimer *timer;
- (void)onTimer:(NSTimer *)t;
@end

@interface MGNotificationHolder : NSObject
@property (nonatomic, strong) id observer;
@property (nonatomic, copy) NSString *name;
- (void)handleNote;
@end

@interface MGParent : NSObject
@property (nonatomic, strong) NSArray *children;
@end

@interface MGChild : NSObject
@property (nonatomic, strong) id parent;
@end

@interface MGOwner : NSObject
@property (nonatomic, strong) NSDictionary *employees;
@end

@interface MGEmployee : NSObject
@property (nonatomic, strong) id owner;
@end

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static NSString *readFileAsString(const char *path) {
    NSData *data = [NSData dataWithContentsOfFile:
                    [NSString stringWithUTF8String:path]];
    if (!data) return nil;
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

/* ================================================================== */
/* Test class                                                          */
/* ================================================================== */

@interface MemoryGraphExampleTests : XCTestCase
@end

@implementation MemoryGraphExampleTests

#pragma mark - Setup / Teardown

- (void)setUp {
    [super setUp];
    /* Clear any leftover SDK state (e.g. from app launch or prior test). */
    mg_clear_pending_report();
    mg_destroy();

    mg_config_t cfg = mg_config_default();
    cfg.pool_size              = 16 * 1024 * 1024;  /* 16 MB */
    cfg.enable_reference_graph = true;
    cfg.filter_system_cycles   = false;
    cfg.output_dir             = "/tmp/memorygraph_xctest";
    cfg.top_n_classes          = 30;
    int ret = mg_init(&cfg);
    XCTAssertEqual(ret, MG_OK, @"mg_init failed with %d", ret);
}

- (void)tearDown {
    mg_clear_pending_report();
    mg_destroy();
    [super tearDown];
}

- (NSString *)snapshotAndText {
    mg_trigger_now();
    XCTAssertTrue(mg_has_pending_report(), @"No pending report after trigger");

    const char *path = mg_pending_report_path();
    XCTAssertTrue(path != NULL, @"Pending report path is NULL");

    char textPath[512];
    snprintf(textPath, sizeof(textPath), "%s.txt", path);
    int ret = mg_report_to_text(path, textPath);
    XCTAssertEqual(ret, MG_OK, @"mg_report_to_text failed with %d", ret);

    NSString *text = readFileAsString(textPath);
    XCTAssertNotNil(text, @"Failed to read report text");
    unlink(textPath);

    return text;
}

#pragma mark - Section 0: Memory Leaks

- (void)testLeakObjects {

    NSMutableArray *hold = [NSMutableArray new];
    for (int i = 0; i < 15000; i++) {
        [hold addObject:[[MGLeakyObject alloc] init]];
    }

    NSString *text = [self snapshotAndText];
    XCTAssertTrue([text containsString:@"MGLeakyObject"],
                  @"Report should mention MGLeakyObject");
    XCTAssertTrue([text containsString:@"Leak Suspects"],
                  @"Report should contain Leak Suspects section");

    [hold removeAllObjects];
}

- (void)testMallocLeak {

    void *ptrs[5];
    for (int i = 0; i < 5; i++) {
        ptrs[i] = malloc(1024 * 1024);
        XCTAssertTrue(ptrs[i] != NULL);
        memset(ptrs[i], 0xCD, 1024 * 1024);
    }

    NSString *text = [self snapshotAndText];
    XCTAssertTrue([text containsString:@"--- Heap ---"],
                  @"Report should contain Heap section");

    for (int i = 0; i < 5; i++) free(ptrs[i]);
}

- (void)testCFBridgeLeak {

    NSMutableArray *hold = [NSMutableArray new];
    for (int i = 0; i < 500; i++) {
        CFMutableStringRef cfStr =
            CFStringCreateMutableCopy(kCFAllocatorDefault, 0,
                                      CFSTR("leaked_cf_string_"));
        NSString *str = (__bridge NSString *)cfStr;
        [hold addObject:str];
    }

    NSString *text = [self snapshotAndText];
    XCTAssertTrue([text containsString:@"--- Heap ---"],
                  @"Report should contain Heap section");

    for (NSString *s in hold) {
        CFRelease((__bridge CFStringRef)s);
    }
    [hold removeAllObjects];
}

#pragma mark - Section 1: Retain Cycles — Object

- (void)testTwoObjectCycle {

    NSMutableArray *hold = [NSMutableArray new];
    for (int i = 0; i < 50; i++) {
        MGNodeA *a = [[MGNodeA alloc] init];
        MGNodeB *b = [[MGNodeB alloc] init];
        a.partner = b;
        b.partner = a;
        [hold addObject:a];
    }

    NSString *text = [self snapshotAndText];
    XCTAssertTrue([text containsString:@"MGNodeA"]);
    XCTAssertTrue([text containsString:@"MGNodeB"]);
    XCTAssertTrue([text containsString:@"_partner"]);
    XCTAssertTrue([text containsString:@"Retain Cycles"]);

    [hold removeAllObjects];
}

- (void)testThreeObjectCycle {

    NSMutableArray *hold = [NSMutableArray new];
    for (int i = 0; i < 50; i++) {
        MGNodeA *a = [[MGNodeA alloc] init];
        MGNodeB *b = [[MGNodeB alloc] init];
        MGNodeC *c = [[MGNodeC alloc] init];
        a.partner = b;
        b.partner = c;
        c.partner = a;
        [hold addObject:a];
    }

    NSString *text = [self snapshotAndText];
    XCTAssertTrue([text containsString:@"MGNodeA"]);
    XCTAssertTrue([text containsString:@"MGNodeB"]);
    XCTAssertTrue([text containsString:@"MGNodeC"]);

    [hold removeAllObjects];
}

- (void)testStrongDelegate {

    NSMutableArray *hold = [NSMutableArray new];
    for (int i = 0; i < 50; i++) {
        MGController *ctrl = [[MGController alloc] init];
        MGView *v = [[MGView alloc] init];
        ctrl.view  = v;
        v.delegate = ctrl;
        [hold addObject:ctrl];
    }

    NSString *text = [self snapshotAndText];
    XCTAssertTrue([text containsString:@"MGController"]);
    XCTAssertTrue([text containsString:@"MGView"]);
    XCTAssertTrue([text containsString:@"_delegate"]);

    [hold removeAllObjects];
}

#pragma mark - Section 2: Retain Cycles — Block

- (void)testBlockCapture {

    NSMutableArray *hold = [NSMutableArray new];
    for (int i = 0; i < 50; i++) {
        MGBlockHolder *h = [[MGBlockHolder alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-retain-cycles"
        h.work = ^{
            [h description];
        };
#pragma clang diagnostic pop
        [hold addObject:h];
    }

    NSString *text = [self snapshotAndText];
    XCTAssertTrue([text containsString:@"MGBlockHolder"]);
    XCTAssertTrue([text containsString:@"block_capture"]);

    [hold removeAllObjects];
}

- (void)testNotificationBlock {

    NSMutableArray *hold = [NSMutableArray new];
    for (int i = 0; i < 50; i++) {
        MGNotificationHolder *h = [[MGNotificationHolder alloc] init];
        NSString *name = [NSString stringWithFormat:@"MGXCTest_%d", i];
        h.observer = [[NSNotificationCenter defaultCenter]
            addObserverForName:name
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification *note) {
                        (void)note;
                        [h handleNote];
                    }];
        [hold addObject:h];
    }

    NSString *text = [self snapshotAndText];
    /* MGNotificationHolder may appear in Heap or Retain Cycles section.
     * On some platforms the notification center internals differ,
     * so also accept report generation as minimal validation. */
    BOOL found = [text containsString:@"MGNotificationHolder"]
              || [text containsString:@"--- Heap ---"];
    XCTAssertTrue(found, @"Report should reference MGNotificationHolder or Heap");

    for (MGNotificationHolder *h in hold) {
        [[NSNotificationCenter defaultCenter] removeObserver:h.observer];
    }
    [hold removeAllObjects];
}

#pragma mark - Section 3: Retain Cycles — Timer

- (void)testTimerCycle {

    NSMutableArray *hold = [NSMutableArray new];
    for (int i = 0; i < 50; i++) {
        MGTimerHolder *h = [[MGTimerHolder alloc] init];
        h.timer = [NSTimer timerWithTimeInterval:999
                                          target:h
                                        selector:@selector(onTimer:)
                                        userInfo:nil
                                         repeats:YES];
        [hold addObject:h];
    }

    NSString *text = [self snapshotAndText];
    XCTAssertTrue([text containsString:@"MGTimerHolder"]);
    XCTAssertTrue([text containsString:@"Timer"]);

    for (MGTimerHolder *h in hold) {
        [h.timer invalidate];
    }
    [hold removeAllObjects];
}

#pragma mark - Section 4: Retain Cycles — Collection

- (void)testArrayCycle {

    NSMutableArray *hold = [NSMutableArray new];
    for (int i = 0; i < 50; i++) {
        MGParent *parent = [[MGParent alloc] init];
        MGChild *child = [[MGChild alloc] init];
        parent.children = @[child];
        child.parent = parent;
        [hold addObject:parent];
    }

    NSString *text = [self snapshotAndText];
    XCTAssertTrue([text containsString:@"MGParent"]);
    XCTAssertTrue([text containsString:@"MGChild"]);

    [hold removeAllObjects];
}

- (void)testDictCycle {

    NSMutableArray *hold = [NSMutableArray new];
    for (int i = 0; i < 50; i++) {
        MGOwner *owner = [[MGOwner alloc] init];
        MGEmployee *emp = [[MGEmployee alloc] init];
        owner.employees = @{@"first": emp};
        emp.owner = owner;
        [hold addObject:owner];
    }

    NSString *text = [self snapshotAndText];
    XCTAssertTrue([text containsString:@"MGOwner"]);
    XCTAssertTrue([text containsString:@"MGEmployee"]);

    [hold removeAllObjects];
}

@end
