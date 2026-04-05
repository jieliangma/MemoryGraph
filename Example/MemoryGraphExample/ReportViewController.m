#import "ReportViewController.h"

@interface ReportViewController ()
@property (nonatomic, copy)   NSString   *reportText;
@property (nonatomic, strong) UITextView *textView;
@end

@implementation ReportViewController

- (instancetype)initWithReportText:(NSString *)text title:(NSString *)title {
    self = [super initWithNibName:nil bundle:nil];
    if (self) {
        _reportText = [text copy];
        self.title  = title;
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    /* Share button */
    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAction
                                                     target:self
                                                     action:@selector(shareReport)];

    /* Full-screen text view */
    self.textView = [[UITextView alloc] initWithFrame:self.view.bounds];
    self.textView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.textView.editable = NO;
    self.textView.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    self.textView.backgroundColor = [UIColor secondarySystemBackgroundColor];
    self.textView.textContainerInset = UIEdgeInsetsMake(12, 8, 12, 8);
    self.textView.text = self.reportText;
    [self.view addSubview:self.textView];
}

- (void)shareReport {
    if (!self.reportText.length) return;
    UIActivityViewController *avc =
        [[UIActivityViewController alloc] initWithActivityItems:@[self.reportText]
                                          applicationActivities:nil];
    avc.popoverPresentationController.barButtonItem = self.navigationItem.rightBarButtonItem;
    [self presentViewController:avc animated:YES completion:nil];
}

@end
