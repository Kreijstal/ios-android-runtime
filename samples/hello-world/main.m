#import <UIKit/UIKit.h>

// Minimal device-app fixture for the general runtime: no Unreal Engine or storyboard.
@interface HelloViewController : UIViewController
@end

@implementation HelloViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor whiteColor];

    UILabel *label = [[UILabel alloc] initWithFrame:self.view.bounds];
    label.text = @"Hello World!";
    label.textAlignment = NSTextAlignmentCenter;
    label.textColor = [UIColor blackColor];
    label.font = [UIFont systemFontOfSize:32];
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:label];
    NSLog(@"HelloWorld: view loaded");
}

@end

@interface HelloAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation HelloAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    NSLog(@"HelloWorld: didFinishLaunchingWithOptions");
    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.rootViewController = [[HelloViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

- (void)applicationDidBecomeActive:(UIApplication *)application {
    NSLog(@"HelloWorld: didBecomeActive");
}

- (void)applicationDidEnterBackground:(UIApplication *)application {
    NSLog(@"HelloWorld: didEnterBackground");
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([HelloAppDelegate class]));
    }
}
