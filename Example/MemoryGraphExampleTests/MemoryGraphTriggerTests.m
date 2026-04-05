/*
 * MemoryGraphTriggerTests.m — XCTest for two-phase OOM trigger detection
 */

#import <XCTest/XCTest.h>
#include "../../src/mg_trigger.h"

static int s_fire_count;
static void *s_fire_ctx;

static void test_fire_fn(void *ctx) {
    s_fire_count++;
    s_fire_ctx = ctx;
}

@interface MemoryGraphTriggerTests : XCTestCase
@end

@implementation MemoryGraphTriggerTests

- (void)setUp {
    [super setUp];
    s_fire_count = 0;
    s_fire_ctx = NULL;
}

- (void)testCreateDestroy {
    mg_trigger_config_t cfg = {
        .memory_threshold = 100 * 1024 * 1024,
        .polling_interval_ms = 1000,
        .fire_fn = test_fire_fn,
        .fire_ctx = NULL,
    };
    mg_trigger_t *trig = mg_trigger_create(&cfg);
    XCTAssertTrue(trig != NULL);
    mg_trigger_destroy(trig);
}

- (void)testCreateNullConfig {
    mg_trigger_t *trig = mg_trigger_create(NULL);
    XCTAssertTrue(trig == NULL);
}

- (void)testCreateNullFireFn {
    mg_trigger_config_t cfg = {
        .memory_threshold = 100 * 1024 * 1024,
        .polling_interval_ms = 1000,
        .fire_fn = NULL,
        .fire_ctx = NULL,
    };
    mg_trigger_t *trig = mg_trigger_create(&cfg);
    XCTAssertTrue(trig == NULL);
}

- (void)testPhaseInitial {
    mg_trigger_config_t cfg = {
        .memory_threshold = 100 * 1024 * 1024,
        .polling_interval_ms = 1000,
        .fire_fn = test_fire_fn,
        .fire_ctx = NULL,
    };
    mg_trigger_t *trig = mg_trigger_create(&cfg);
    XCTAssertEqual(mg_trigger_phase(trig), 0);
    mg_trigger_destroy(trig);
}

- (void)testStartPhase {
    mg_trigger_config_t cfg = {
        .memory_threshold = 100 * 1024 * 1024,
        .polling_interval_ms = 1000,
        .fire_fn = test_fire_fn,
        .fire_ctx = NULL,
    };
    mg_trigger_t *trig = mg_trigger_create(&cfg);
    mg_trigger_start(trig);
    XCTAssertEqual(mg_trigger_phase(trig), 1);
    mg_trigger_destroy(trig);
}

- (void)testStopPhase {
    mg_trigger_config_t cfg = {
        .memory_threshold = 100 * 1024 * 1024,
        .polling_interval_ms = 1000,
        .fire_fn = test_fire_fn,
        .fire_ctx = NULL,
    };
    mg_trigger_t *trig = mg_trigger_create(&cfg);
    mg_trigger_start(trig);
    XCTAssertEqual(mg_trigger_phase(trig), 1);

    mg_trigger_stop(trig);
    XCTAssertEqual(mg_trigger_phase(trig), 0);

    mg_trigger_destroy(trig);
}

- (void)testStartStopStart {
    mg_trigger_config_t cfg = {
        .memory_threshold = 100 * 1024 * 1024,
        .polling_interval_ms = 1000,
        .fire_fn = test_fire_fn,
        .fire_ctx = NULL,
    };
    mg_trigger_t *trig = mg_trigger_create(&cfg);

    mg_trigger_start(trig);
    XCTAssertEqual(mg_trigger_phase(trig), 1);

    mg_trigger_stop(trig);
    XCTAssertEqual(mg_trigger_phase(trig), 0);

    mg_trigger_start(trig);
    XCTAssertEqual(mg_trigger_phase(trig), 1);

    mg_trigger_destroy(trig);
}

- (void)testFireCallbackContext {
    int ctx_value = 42;
    mg_trigger_config_t cfg = {
        .memory_threshold = 100 * 1024 * 1024,
        .polling_interval_ms = 1000,
        .fire_fn = test_fire_fn,
        .fire_ctx = &ctx_value,
    };
    mg_trigger_t *trig = mg_trigger_create(&cfg);
    XCTAssertTrue(trig != NULL);

    /* Directly call the fire function to verify context passing. */
    cfg.fire_fn(cfg.fire_ctx);
    XCTAssertEqual(s_fire_count, 1);
    XCTAssertEqual(s_fire_ctx, &ctx_value);

    mg_trigger_destroy(trig);
}

- (void)testDestroyNull {
    mg_trigger_destroy(NULL);
}

- (void)testPhaseNull {
    XCTAssertEqual(mg_trigger_phase(NULL), 0);
}

@end
