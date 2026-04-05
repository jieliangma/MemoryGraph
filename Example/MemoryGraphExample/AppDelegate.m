#import "AppDelegate.h"
#import "ViewController.h"  /* CaseListViewController */
#import <MemoryGraph/memory_graph.h>

/* ---- MemoryGraph report callback ---- */
static void on_report_ready(const char *report_path) {
    NSString *path = [NSString stringWithUTF8String:report_path];
    NSLog(@"[MemoryGraph] OOM report ready: %@", path);

    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter]
            postNotificationName:@"MemoryGraphReportReady"
                          object:path];
    });
}

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {

    /* --- Initialize MemoryGraph SDK --- */
    mg_config_t cfg = mg_config_default();

    /*
     * Pool must be large enough for the reference-graph node table.
     * 16 MB supports ~65 000 objects; threshold kept low for easy testing.
     */
    cfg.pool_size        = 16 * 1024 * 1024;
    cfg.memory_threshold = 50 * 1024 * 1024;
    cfg.top_n_classes    = 20;
    cfg.enable_reference_graph = true;
    cfg.enable_assoc_tracking  = true;

    int ret = mg_init(&cfg);
    if (ret != MG_OK) {
        NSLog(@"[MemoryGraph] mg_init failed: %d", ret);
    } else {
        NSLog(@"[MemoryGraph] SDK initialized, pool = %.1f MB, threshold = %.1f MB",
              cfg.pool_size / (1024.0 * 1024.0),
              cfg.memory_threshold / (1024.0 * 1024.0));
    }

    /* Register callback — fires on every report (manual trigger, OOM, or pending from previous run). */
    mg_set_callback(on_report_ready);

    /* Start monitoring. */
    mg_start();
    NSLog(@"[MemoryGraph] Monitoring started");

    /* --- Set up window (pre-scene) --- */
    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    UINavigationController *nav = [[UINavigationController alloc]
        initWithRootViewController:[[CaseListViewController alloc] init]];
    self.window.rootViewController = nav;
    [self.window makeKeyAndVisible];

    return YES;
}

- (void)applicationWillTerminate:(UIApplication *)application {
    mg_stop();
    mg_destroy();
    NSLog(@"[MemoryGraph] SDK destroyed");
}

@end
