/*
 * MemoryGraphThreadTests.m — XCTest for thread register capture
 *
 * Note: mg_threads_suspend/resume are NOT tested here because
 * suspending threads would freeze the XCTest runner itself.
 * Only mg_thread_capture_regs is safe to test.
 */

#import <XCTest/XCTest.h>
#include "../../src/mg_thread.h"

@interface MemoryGraphThreadTests : XCTestCase
@end

@implementation MemoryGraphThreadTests

- (void)testCaptureRegs {
    uint64_t pc = 0, sp = 0;
    mg_thread_capture_regs(&pc, &sp);

    XCTAssertTrue(pc != 0, @"PC should be non-zero");
    XCTAssertTrue(sp != 0, @"SP should be non-zero");
}

- (void)testCaptureRegsMultipleCalls {
    uint64_t pc1 = 0, sp1 = 0;
    uint64_t pc2 = 0, sp2 = 0;

    mg_thread_capture_regs(&pc1, &sp1);
    mg_thread_capture_regs(&pc2, &sp2);

    /* SP should be the same (same stack frame depth). */
    XCTAssertEqual(sp1, sp2);

    /* PC values should be close (same function, adjacent instructions). */
    uint64_t pc_diff = pc1 > pc2 ? pc1 - pc2 : pc2 - pc1;
    XCTAssertTrue(pc_diff < 4096, @"PC values should be in the same function");
}

- (void)testCaptureRegsSPAlignment {
    uint64_t pc = 0, sp = 0;
    mg_thread_capture_regs(&pc, &sp);

    /* ARM64 ABI requires 16-byte stack alignment. */
    XCTAssertEqual(sp % 16, (uint64_t)0,
                   @"SP must be 16-byte aligned per ARM64 ABI");
}

@end
