#import "ViewController.h"
#import "ReportViewController.h"
#import <MemoryGraph/memory_graph.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import "MemoryGraphExample-Swift.h"

/* ================================================================== */
/* Demo classes for leak & retain-cycle detection                      */
/* ================================================================== */

/* --- Memory Leak --- */

/* ~256 bytes per instance; 15 000 instances exceed the SDK's
 * count_threshold (10 000), triggering MG_LEAK_ABNORMAL_COUNT. */
@interface MGLeakyObject : NSObject {
    @public char _payload[224];
}
@end
@implementation MGLeakyObject
@end

/* --- Retain Cycles: Object --- */

@interface MGNodeA : NSObject
@property (nonatomic, strong) id partner;
@end
@implementation MGNodeA
@end

@interface MGNodeB : NSObject
@property (nonatomic, strong) id partner;
@end
@implementation MGNodeB
@end

/* Three-object cycle: A→B→C→A */
@interface MGNodeC : NSObject
@property (nonatomic, strong) id partner;
@end
@implementation MGNodeC
@end

/* Strong delegate anti-pattern:
 * MGController → _view → MGView → _delegate(strong) → MGController */
@interface MGView : NSObject
@property (nonatomic, strong) id delegate;  /* BUG: should be weak */
@end
@implementation MGView
@end

@interface MGController : NSObject
@property (nonatomic, strong) MGView *view;
@end
@implementation MGController
@end

/* Swift retain cycle classes are defined in SwiftCycleDemo.swift:
 *   class SwiftViewModel: NSObject { var service: SwiftService? }
 *   class SwiftService: NSObject { var delegate: SwiftViewModel? }
 * Imported via "MemoryGraphExample-Swift.h" (auto-generated). */

/* --- Retain Cycles: Timer & RunLoop --- */

@interface MGTimerHolder : NSObject
@property (nonatomic, strong) NSTimer *timer;
- (void)onTimer:(NSTimer *)t;
@end
@implementation MGTimerHolder
- (void)onTimer:(NSTimer *)t { (void)t; }
@end

@interface MGDisplayLinkHolder : NSObject
@property (nonatomic, strong) CADisplayLink *displayLink;
- (void)onDisplayLink:(CADisplayLink *)dl;
@end
@implementation MGDisplayLinkHolder
- (void)onDisplayLink:(CADisplayLink *)dl { (void)dl; }
@end

/* performSelector:withObject:afterDelay: retains target+object
 * until the selector fires.  By storing a strong back-reference
 * we create a cycle even if the selector eventually fires. */
@interface MGPerformSelectorHolder : NSObject
@property (nonatomic, strong) id retainedSelf;
- (void)delayedWork;
@end
@implementation MGPerformSelectorHolder
- (void)delayedWork {}
@end

/* --- Retain Cycles: Block --- */

@interface MGBlockHolder : NSObject
@property (nonatomic, copy) void (^work)(void);
@end
@implementation MGBlockHolder
@end

@interface MGNestedBlockHolder : NSObject
@property (nonatomic, copy) void (^work)(void);
- (void)doWork;
@end
@implementation MGNestedBlockHolder
- (void)doWork {}
@end

@interface MGNotificationHolder : NSObject
@property (nonatomic, strong) id observer;   /* retains the opaque token */
@property (nonatomic, copy) NSString *name;
- (void)handleNote;
@end
@implementation MGNotificationHolder
- (void)handleNote {}
@end

/* --- Retain Cycles: Collection --- */

@interface MGParent : NSObject
@property (nonatomic, strong) NSArray *children;
@end
@implementation MGParent
@end

@interface MGChild : NSObject
@property (nonatomic, strong) id parent;
@end
@implementation MGChild
@end

@interface MGOwner : NSObject
@property (nonatomic, strong) NSDictionary *employees;
@end
@implementation MGOwner
@end

@interface MGEmployee : NSObject
@property (nonatomic, strong) id owner;
@end
@implementation MGEmployee
@end

/* ================================================================== */
/* Case descriptor                                                     */
/* ================================================================== */

typedef NS_ENUM(NSInteger, MGCaseAction) {
    /* Section 0: Memory Leaks */
    MGCaseAllocate10MB,
    MGCaseLeak15KObjects,
    MGCaseMallocLeak,
    MGCaseCFBridgeLeak,
    /* Section 1: Retain Cycles - Object */
    MGCaseTwoObjectCycle,
    MGCaseThreeObjectCycle,
    MGCaseStrongDelegate,
    MGCaseSwiftNSObjectCycle,
    MGCasePureSwiftCycle,
    /* Section 2: Retain Cycles - Timer & RunLoop */
    MGCaseNSTimerTarget,
    MGCaseCADisplayLink,
    MGCasePerformSelector,
    /* Section 3: Retain Cycles - Block */
    MGCaseBlockCapture,
    MGCaseNestedBlockCapture,
    MGCaseNotificationBlock,
    /* Section 4: Retain Cycles - Collection */
    MGCaseArrayCycle,
    MGCaseDictionaryCycle,
    /* Section 5: Snapshot */
    MGCaseTriggerSnapshot,
    MGCaseClearAll,
};

/* ------------------------------------------------------------------ */
/* Section / Row tables                                                */
/* ------------------------------------------------------------------ */

static NSString * const kSectionTitles[] = {
    @"Memory Leaks",
    @"Retain Cycles \u2014 Object",
    @"Retain Cycles \u2014 Timer & RunLoop",
    @"Retain Cycles \u2014 Block",
    @"Retain Cycles \u2014 Collection",
    @"Snapshot",
};
static const NSUInteger kSectionCount = 6;

typedef struct {
    NSString     *title;
    NSString     *subtitle;
    MGCaseAction  action;
} MGCaseRow;

/* Section 0: Memory Leaks */
static const MGCaseRow kSection0[] = {
    { @"Allocate 10 MB",
      @"NSMutableData 10 MB, touch every page",
      MGCaseAllocate10MB },
    { @"Leak 15K ObjC Objects",
      @"MGLeakyObject \u00d7 15 000 (~3.6 MB)",
      MGCaseLeak15KObjects },
    { @"C malloc Leak",
      @"malloc(1 MB) \u00d7 5, never free \u2014 raw heap leak",
      MGCaseMallocLeak },
    { @"CF/Bridge Leak",
      @"CFStringCreateMutableCopy + __bridge (no CFRelease)",
      MGCaseCFBridgeLeak },
};

/* Section 1: Retain Cycles - Object */
static const MGCaseRow kSection1[] = {
    { @"Two-Object (A\u2194B)",
      @"MGNodeA.partner \u2194 MGNodeB.partner \u00d7 200",
      MGCaseTwoObjectCycle },
    { @"Three-Object (A\u2192B\u2192C\u2192A)",
      @"MGNodeA \u2192 B \u2192 C \u2192 A \u00d7 100",
      MGCaseThreeObjectCycle },
    { @"Strong Delegate",
      @"MGController \u2192 MGView.delegate(strong) \u2192 Controller",
      MGCaseStrongDelegate },
    { @"Swift Cycle (: NSObject)",
      @"SwiftViewModel.service \u2194 SwiftService.delegate \u00d7 100",
      MGCaseSwiftNSObjectCycle },
    { @"Swift Cycle (pure class)",
      @"PureSwiftNodeA \u2194 PureSwiftNodeB, no NSObject \u00d7 100",
      MGCasePureSwiftCycle },
};

/* Section 2: Retain Cycles - Timer & RunLoop */
static const MGCaseRow kSection2[] = {
    { @"NSTimer Target",
      @"repeating timer retains target \u2192 holder.timer \u2192 timer.target cycle",
      MGCaseNSTimerTarget },
    { @"CADisplayLink",
      @"displayLink retains target \u2192 same pattern as NSTimer",
      MGCaseCADisplayLink },
    { @"performSelector:afterDelay:",
      @"delayed selector retains target + back-ref \u2192 cycle",
      MGCasePerformSelector },
};

/* Section 3: Retain Cycles - Block */
static const MGCaseRow kSection3[] = {
    { @"Block Capture Self",
      @"self.block = ^{ [self work]; } \u00d7 100",
      MGCaseBlockCapture },
    { @"Nested Block Cycle",
      @"self.block = ^{ dispatch_async(^{ self.doWork }) } \u00d7 100",
      MGCaseNestedBlockCapture },
    { @"Notification Block",
      @"addObserverForName: block captures self, token retained",
      MGCaseNotificationBlock },
};

/* Section 4: Retain Cycles - Collection */
static const MGCaseRow kSection4[] = {
    { @"NSArray Cycle",
      @"Parent \u2192 NSArray \u2192 Child \u2192 Parent \u00d7 100",
      MGCaseArrayCycle },
    { @"NSDictionary Cycle",
      @"Owner \u2192 NSDictionary \u2192 Employee \u2192 Owner \u00d7 100",
      MGCaseDictionaryCycle },
};

/* Section 5: Snapshot */
static const MGCaseRow kSection5[] = {
    { @"Trigger Snapshot",
      @"Capture current memory state",
      MGCaseTriggerSnapshot },
    { @"Clear All",
      @"Clear report and reset counters",
      MGCaseClearAll },
};

static const MGCaseRow * const kSections[] = {
    kSection0, kSection1, kSection2, kSection3, kSection4, kSection5,
};
static const NSUInteger kSectionSizes[] = { 4, 5, 3, 3, 2, 2 };

/* ================================================================== */

@interface CaseListViewController () <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, strong) UITableView    *tableView;
@property (nonatomic, strong) UILabel        *statusLabel;
@property (nonatomic, strong) NSMutableArray *leakedMemory;
@property (nonatomic, assign) NSUInteger      leakedDataMB;
@property (nonatomic, assign) NSUInteger      leakedObjectCount;
@property (nonatomic, assign) NSUInteger      retainCycleCount;
@end

@implementation CaseListViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"MemoryGraph";
    self.view.backgroundColor = [UIColor systemBackgroundColor];
    self.leakedMemory = [NSMutableArray new];

    [self setupTableView];
    [self setupStatusBar];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self updateStatus];
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

- (void)setupTableView {
    self.tableView = [[UITableView alloc] initWithFrame:CGRectZero
                                                  style:UITableViewStyleInsetGrouped];
    self.tableView.dataSource = self;
    self.tableView.delegate   = self;
    self.tableView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.tableView];

    [NSLayoutConstraint activateConstraints:@[
        [self.tableView.topAnchor
            constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [self.tableView.leadingAnchor
            constraintEqualToAnchor:self.view.leadingAnchor],
        [self.tableView.trailingAnchor
            constraintEqualToAnchor:self.view.trailingAnchor],
    ]];
}

- (void)setupStatusBar {
    self.statusLabel = [[UILabel alloc] init];
    self.statusLabel.font =
        [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    self.statusLabel.textColor     = [UIColor secondaryLabelColor];
    self.statusLabel.textAlignment = NSTextAlignmentCenter;
    self.statusLabel.numberOfLines = 2;
    self.statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.statusLabel.backgroundColor = [UIColor secondarySystemBackgroundColor];
    [self.view addSubview:self.statusLabel];

    [NSLayoutConstraint activateConstraints:@[
        [self.statusLabel.topAnchor
            constraintEqualToAnchor:self.tableView.bottomAnchor],
        [self.statusLabel.leadingAnchor
            constraintEqualToAnchor:self.view.leadingAnchor],
        [self.statusLabel.trailingAnchor
            constraintEqualToAnchor:self.view.trailingAnchor],
        [self.statusLabel.bottomAnchor
            constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [self.statusLabel.heightAnchor constraintEqualToConstant:44],
    ]];
}

/* ------------------------------------------------------------------ */
/* UITableViewDataSource                                               */
/* ------------------------------------------------------------------ */

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return (NSInteger)kSectionCount;
}

- (NSInteger)tableView:(UITableView *)tableView
 numberOfRowsInSection:(NSInteger)section {
    return (NSInteger)kSectionSizes[section];
}

- (NSString *)tableView:(UITableView *)tableView
titleForHeaderInSection:(NSInteger)section {
    return kSectionTitles[section];
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    static NSString *reuseID = @"Cell";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:reuseID];
    if (!cell) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                      reuseIdentifier:reuseID];
    }

    const MGCaseRow *row = &kSections[indexPath.section][indexPath.row];
    cell.textLabel.text       = row->title;
    cell.detailTextLabel.text = row->subtitle;
    cell.detailTextLabel.textColor = [UIColor secondaryLabelColor];
    cell.detailTextLabel.numberOfLines = 2;

    if (row->action == MGCaseClearAll) {
        cell.accessoryType       = UITableViewCellAccessoryNone;
        cell.textLabel.textColor = [UIColor systemRedColor];
    } else {
        cell.accessoryType       = UITableViewCellAccessoryDisclosureIndicator;
        cell.textLabel.textColor = [UIColor labelColor];
    }

    return cell;
}

/* ------------------------------------------------------------------ */
/* UITableViewDelegate                                                 */
/* ------------------------------------------------------------------ */

- (void)tableView:(UITableView *)tableView
didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];

    const MGCaseRow *row = &kSections[indexPath.section][indexPath.row];

    [self executeAction:row->action];

    if (row->action == MGCaseClearAll) {
        [self updateStatus];
        return;
    }

    mg_trigger_now();
    [self updateStatus];

    NSString *report = [self readCurrentReport];
    if (!report) report = @"No report generated.";

    ReportViewController *vc =
        [[ReportViewController alloc] initWithReportText:report
                                                   title:row->title];
    [self.navigationController pushViewController:vc animated:YES];
}

/* ------------------------------------------------------------------ */
/* Action dispatcher                                                   */
/* ------------------------------------------------------------------ */

- (void)executeAction:(MGCaseAction)action {
    switch (action) {
        /* Memory Leaks */
        case MGCaseAllocate10MB:       [self allocateMemory];          break;
        case MGCaseLeak15KObjects:     [self createLeakObjects];       break;
        case MGCaseMallocLeak:         [self createMallocLeak];        break;
        case MGCaseCFBridgeLeak:       [self createCFBridgeLeak];      break;
        /* Retain Cycles - Object */
        case MGCaseTwoObjectCycle:     [self createTwoObjectCycles];   break;
        case MGCaseThreeObjectCycle:   [self createThreeObjectCycles]; break;
        case MGCaseStrongDelegate:     [self createStrongDelegateCycles]; break;
        case MGCaseSwiftNSObjectCycle: [self createSwiftNSObjectCycles]; break;
        case MGCasePureSwiftCycle:     [self createPureSwiftCycles];    break;
        /* Retain Cycles - Timer & RunLoop */
        case MGCaseNSTimerTarget:      [self createTimerCycles];       break;
        case MGCaseCADisplayLink:      [self createDisplayLinkCycles]; break;
        case MGCasePerformSelector:    [self createPerformSelectorCycles]; break;
        /* Retain Cycles - Block */
        case MGCaseBlockCapture:       [self createBlockCycles];       break;
        case MGCaseNestedBlockCapture: [self createNestedBlockCycles]; break;
        case MGCaseNotificationBlock:  [self createNotificationBlockCycles]; break;
        /* Retain Cycles - Collection */
        case MGCaseArrayCycle:         [self createArrayCycles];       break;
        case MGCaseDictionaryCycle:    [self createDictionaryCycles];  break;
        /* Snapshot */
        case MGCaseTriggerSnapshot:    /* snapshot taken after switch */ break;
        case MGCaseClearAll:           [self clearAll];                break;
    }
}

/* ================================================================== */
/* Section 0: Memory Leaks                                             */
/* ================================================================== */

- (void)allocateMemory {
    size_t size = 10 * 1024 * 1024;
    NSMutableData *data = [NSMutableData dataWithLength:size];
    memset(data.mutableBytes, 0xAB, size);
    [self.leakedMemory addObject:data];
    self.leakedDataMB += 10;
}

- (void)createLeakObjects {
    NSUInteger count = 15000;
    for (NSUInteger i = 0; i < count; i++) {
        MGLeakyObject *obj = [[MGLeakyObject alloc] init];
        [self.leakedMemory addObject:obj];
    }
    self.leakedObjectCount += count;
}

- (void)createMallocLeak {
    /*
     * Pure C heap leak: malloc without free.
     * These blocks appear in the heap summary as "raw allocations"
     * and increase the physical footprint.
     */
    for (int i = 0; i < 5; i++) {
        void *ptr = malloc(1024 * 1024);  /* 1 MB */
        if (ptr) {
            memset(ptr, 0xCD, 1024 * 1024);
            /* Intentionally leaked — never freed.
             * Store in NSValue to prevent compiler warning. */
            [self.leakedMemory addObject:[NSValue valueWithPointer:ptr]];
        }
    }
    self.leakedDataMB += 5;
}

- (void)createCFBridgeLeak {
    /*
     * CF/ARC bridge misuse: CFCreate → __bridge cast (no ownership transfer).
     * ARC does not manage the CF side, so CFRelease is never called.
     * Each iteration leaks one CFMutableStringRef.
     */
    for (int i = 0; i < 1000; i++) {
        CFMutableStringRef cfStr =
            CFStringCreateMutableCopy(kCFAllocatorDefault, 0,
                                      CFSTR("leaked_cf_string_payload_"));

        /* __bridge does NOT transfer ownership to ARC — the CF retain is leaked.
         * The correct fix is __bridge_transfer (or manual CFRelease). */
        NSString *str = (__bridge NSString *)cfStr;
        [self.leakedMemory addObject:str];
        /* cfStr is never released → CF retain count leak */
    }
    self.leakedObjectCount += 1000;
}

/* ================================================================== */
/* Section 1: Retain Cycles — Object                                   */
/* ================================================================== */

- (void)createTwoObjectCycles {
    NSUInteger pairs = 200;
    for (NSUInteger i = 0; i < pairs; i++) {
        MGNodeA *a = [[MGNodeA alloc] init];
        MGNodeB *b = [[MGNodeB alloc] init];
        a.partner = b;   /* A → B */
        b.partner = a;   /* B → A  (cycle!) */
        [self.leakedMemory addObject:a];
    }
    self.retainCycleCount += pairs;
}

- (void)createThreeObjectCycles {
    /* A → B → C → A */
    NSUInteger count = 100;
    for (NSUInteger i = 0; i < count; i++) {
        MGNodeA *a = [[MGNodeA alloc] init];
        MGNodeB *b = [[MGNodeB alloc] init];
        MGNodeC *c = [[MGNodeC alloc] init];
        a.partner = b;
        b.partner = c;
        c.partner = a;   /* closes the 3-node ring */
        [self.leakedMemory addObject:a];
    }
    self.retainCycleCount += count;
}

- (void)createStrongDelegateCycles {
    /*
     * Classic iOS anti-pattern: delegate declared as strong instead of weak.
     * Cycle: MGController →(_view)→ MGView →(_delegate)→ MGController
     */
    NSUInteger count = 100;
    for (NSUInteger i = 0; i < count; i++) {
        MGController *ctrl = [[MGController alloc] init];
        MGView *v = [[MGView alloc] init];
        ctrl.view   = v;
        v.delegate  = ctrl;  /* should be weak! */
        [self.leakedMemory addObject:ctrl];
    }
    self.retainCycleCount += count;
}

- (void)createSwiftNSObjectCycles {
    /*
     * Swift classes inheriting from NSObject (SwiftCycleDemo.swift):
     *   class SwiftViewModel: NSObject { var service: SwiftService? }
     *   class SwiftService: NSObject { var delegate: SwiftViewModel? }
     *
     * These are ObjC runtime objects — detected via ISA + ivar scanning.
     * Cycle: SwiftViewModel →(_service)→ SwiftService →(_delegate)→ SwiftViewModel
     */
    NSUInteger count = 100;
    for (NSUInteger i = 0; i < count; i++) {
        SwiftViewModel *vm = [[SwiftViewModel alloc] init];
        SwiftService *svc = [[SwiftService alloc] init];
        vm.service    = svc;
        svc.delegate  = vm;   /* should be weak! */
        [self.leakedMemory addObject:vm];
    }
    self.retainCycleCount += count;
}

- (void)createPureSwiftCycles {
    /*
     * Pure Swift classes with NO NSObject inheritance (SwiftCycleDemo.swift):
     *   class PureSwiftNodeA { var partner: PureSwiftNodeB? }
     *   class PureSwiftNodeB { var partner: PureSwiftNodeA? }
     *
     * On Apple platforms, even pure Swift classes are heap-allocated and
     * registered in the ObjC class list, so the SDK's heap walker can
     * discover them.  Their mangled names appear as _TtC... in reports.
     *
     * Created via SwiftCycleFactory since ObjC can't instantiate
     * non-@objc Swift classes directly.
     */
    NSArray *objects = [SwiftCycleFactory createPureSwiftCycles:100];
    [self.leakedMemory addObjectsFromArray:objects];
    self.retainCycleCount += 100;
}

/* ================================================================== */
/* Section 2: Retain Cycles — Timer & RunLoop                          */
/* ================================================================== */

- (void)createTimerCycles {
    /*
     * NSTimer retains its target.  If the target also retains the timer,
     * you get a retain cycle:
     *   MGTimerHolder →(_timer)→ NSTimer →(target)→ MGTimerHolder
     *
     * The RunLoop also retains the timer, but that doesn't form a cycle
     * with the holder — only the holder↔timer pair does.
     */
    NSUInteger count = 100;
    for (NSUInteger i = 0; i < count; i++) {
        MGTimerHolder *holder = [[MGTimerHolder alloc] init];
        holder.timer = [NSTimer timerWithTimeInterval:999
                                               target:holder
                                             selector:@selector(onTimer:)
                                             userInfo:nil
                                              repeats:YES];
        /* Don't actually schedule on RunLoop — just create the cycle. */
        [self.leakedMemory addObject:holder];
    }
    self.retainCycleCount += count;
}

- (void)createDisplayLinkCycles {
    /*
     * CADisplayLink retains its target, same pattern as NSTimer.
     *   MGDisplayLinkHolder →(_displayLink)→ CADisplayLink →(target)→ Holder
     */
    NSUInteger count = 100;
    for (NSUInteger i = 0; i < count; i++) {
        MGDisplayLinkHolder *holder = [[MGDisplayLinkHolder alloc] init];
        holder.displayLink =
            [CADisplayLink displayLinkWithTarget:holder
                                        selector:@selector(onDisplayLink:)];
        /* Don't add to RunLoop — just create the retain cycle. */
        [self.leakedMemory addObject:holder];
    }
    self.retainCycleCount += count;
}

- (void)createPerformSelectorCycles {
    /*
     * performSelector:withObject:afterDelay: retains target until fire time.
     * Combined with a strong back-reference, this forms a cycle:
     *   Holder →(_retainedSelf)→ Holder (self-cycle through delayed perform)
     *
     * Here we simulate it as a self-referencing object since the RunLoop
     * retain is temporary.  The real-world bug is when the holder is
     * deallocated before the perform fires — this keeps it alive forever.
     */
    NSUInteger count = 100;
    for (NSUInteger i = 0; i < count; i++) {
        MGPerformSelectorHolder *holder = [[MGPerformSelectorHolder alloc] init];
        holder.retainedSelf = holder;  /* self-referencing cycle */
        [self.leakedMemory addObject:holder];
    }
    self.retainCycleCount += count;
}

/* ================================================================== */
/* Section 3: Retain Cycles — Block                                    */
/* ================================================================== */

- (void)createBlockCycles {
    /*
     * Block captures self → retain cycle:
     *   MGBlockHolder →(_work)→ __NSMallocBlock__ →(block_capture)→ MGBlockHolder
     */
    NSUInteger count = 100;
    for (NSUInteger i = 0; i < count; i++) {
        MGBlockHolder *holder = [[MGBlockHolder alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-retain-cycles"
        holder.work = ^{
            [holder description];  /* captures holder strongly */
        };
#pragma clang diagnostic pop
        [self.leakedMemory addObject:holder];
    }
    self.retainCycleCount += count;
}

- (void)createNestedBlockCycles {
    /*
     * Nested blocks both capture self:
     *   MGNestedBlockHolder →(_work)→ Block₁ →(capture)→ Block₂ →(capture)→ Holder
     *
     * Even with only one block property, the inner dispatch_async block
     * is a separate heap block that also captures self.
     */
    NSUInteger count = 100;
    for (NSUInteger i = 0; i < count; i++) {
        MGNestedBlockHolder *holder = [[MGNestedBlockHolder alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-retain-cycles"
        holder.work = ^{
            dispatch_async(dispatch_get_main_queue(), ^{
                [holder doWork];  /* inner block also captures holder */
            });
        };
#pragma clang diagnostic pop
        [self.leakedMemory addObject:holder];
    }
    self.retainCycleCount += count;
}

- (void)createNotificationBlockCycles {
    /*
     * Block-based notification observer captures self.
     * The observer token is stored in self → cycle:
     *   MGNotificationHolder →(_observer)→ Token/Block →(capture)→ Holder
     *
     * Fix: use [NSNotificationCenter removeObserver:token] in dealloc,
     * or use __weak self inside the block.
     */
    NSUInteger count = 100;
    for (NSUInteger i = 0; i < count; i++) {
        MGNotificationHolder *h = [[MGNotificationHolder alloc] init];
        h.name = [NSString stringWithFormat:@"MGTestNote_%lu", (unsigned long)i];
        h.observer = [[NSNotificationCenter defaultCenter]
            addObserverForName:h.name
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification *note) {
                        (void)note;
                        [h handleNote];  /* captures h strongly */
                    }];
        [self.leakedMemory addObject:h];
    }
    self.retainCycleCount += count;
}

/* ================================================================== */
/* Section 4: Retain Cycles — Collection                               */
/* ================================================================== */

- (void)createArrayCycles {
    /*
     * Parent → NSArray → [buffer] → Child → Parent
     * The NSArray stores elements in an external buffer, detected by
     * the SDK's penetrating buffer scan (Phase 2 feature).
     */
    NSUInteger count = 100;
    for (NSUInteger i = 0; i < count; i++) {
        MGParent *parent = [[MGParent alloc] init];
        MGChild *child = [[MGChild alloc] init];
        parent.children = @[child];
        child.parent = parent;
        [self.leakedMemory addObject:parent];
    }
    self.retainCycleCount += count;
}

- (void)createDictionaryCycles {
    /*
     * Owner → NSDictionary → [buffer] → Employee → Owner
     * Same as NSArray cycle but through NSDictionary.
     */
    NSUInteger count = 100;
    for (NSUInteger i = 0; i < count; i++) {
        MGOwner *owner = [[MGOwner alloc] init];
        MGEmployee *emp = [[MGEmployee alloc] init];
        owner.employees = @{@"first": emp};
        emp.owner = owner;
        [self.leakedMemory addObject:owner];
    }
    self.retainCycleCount += count;
}

/* ================================================================== */
/* Clear All                                                           */
/* ================================================================== */

- (void)clearAll {
    /* Remove notification observers before releasing holders. */
    for (id obj in self.leakedMemory) {
        if ([obj isKindOfClass:[MGNotificationHolder class]]) {
            MGNotificationHolder *h = obj;
            if (h.observer) {
                [[NSNotificationCenter defaultCenter] removeObserver:h.observer];
            }
        }
    }

    /* Free C malloc'ed blocks. */
    for (id obj in self.leakedMemory) {
        if ([obj isKindOfClass:[NSValue class]]) {
            void *ptr = [(NSValue *)obj pointerValue];
            if (ptr) free(ptr);
        }
    }

    mg_clear_pending_report();
    [self.leakedMemory removeAllObjects];
    self.leakedDataMB      = 0;
    self.leakedObjectCount = 0;
    self.retainCycleCount  = 0;
}

/* ================================================================== */
/* Report                                                              */
/* ================================================================== */

- (NSString *)readCurrentReport {
    const char *path = mg_pending_report_path();
    if (!path) return nil;

    NSString *tmpPath = [NSTemporaryDirectory()
        stringByAppendingPathComponent:@"mg_report.crash"];
    int ret = mg_report_to_text(path, tmpPath.UTF8String);
    if (ret != MG_OK) return nil;

    NSString *text = [NSString stringWithContentsOfFile:tmpPath
                                              encoding:NSUTF8StringEncoding
                                                 error:nil];
    [[NSFileManager defaultManager] removeItemAtPath:tmpPath error:nil];
    return text;
}

/* ================================================================== */
/* Status                                                              */
/* ================================================================== */

- (void)updateStatus {
    struct task_vm_info info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count);

    self.statusLabel.text =
        [NSString stringWithFormat:
            @"Footprint: %.1f MB | Leaked: %lu MB | Objects: %lu | Cycles: %lu",
            info.phys_footprint / (1024.0 * 1024.0),
            (unsigned long)self.leakedDataMB,
            (unsigned long)self.leakedObjectCount,
            (unsigned long)self.retainCycleCount];
}

@end
