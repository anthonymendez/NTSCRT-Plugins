// NTSCRT.app — the wrapper application FxPlug 4 requires. Launching it once
// registers the embedded .pluginkit with PlugInKit (Launch Services), and it
// installs the Final Cut Pro effect template into
//   ~/Movies/Motion Templates.localized/Effects.localized/NTSCRT/
// (FCP lists FxPlug effects only through Motion templates).
#import <AppKit/AppKit.h>

#include <pwd.h>
#include <unistd.h>

static NSString *RealHome(void) {
    // Sandboxed: NSHomeDirectory() is the container; the entitlement grants the real ~/Movies path.
    const struct passwd *pw = getpwuid(getuid());
    return pw ? [NSString stringWithUTF8String:pw->pw_dir] : NSHomeDirectory();
}

static NSString *TemplateDestination(void) {
    return [RealHome() stringByAppendingPathComponent:@"Movies/Motion Templates.localized/Effects.localized/NTSCRT"];
}

static BOOL InstallTemplate(NSString **message) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *src = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"Motion Templates/NTSCRT"];
    NSString *dst = TemplateDestination();
    if (![fm fileExistsAtPath:src]) { *message = @"The template is missing from the app bundle."; return NO; }
    NSError *err = nil;
    if (![fm createDirectoryAtPath:dst.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:&err]) {
        *message = err.localizedDescription; return NO;
    }
    if ([fm fileExistsAtPath:dst] && ![fm removeItemAtPath:dst error:&err]) { *message = err.localizedDescription; return NO; }
    if (![fm copyItemAtPath:src toPath:dst error:&err]) { *message = err.localizedDescription; return NO; }
    *message = [NSString stringWithFormat:@"Installed the NTSCRT effect template to\n%@\n\nIn Final Cut Pro, find it in the Effects browser under NTSCRT.", dst];
    return YES;
}

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    NSString *msg = nil;
    BOOL ok = InstallTemplate(&msg);
    NSAlert *a = [NSAlert new];
    a.messageText = ok ? @"NTSCRT for Final Cut Pro is ready" : @"Couldn't install the Motion template";
    a.informativeText = [msg stringByAppendingString:
        @"\n\nKeep NTSCRT.app in /Applications: Final Cut Pro loads the plug-in from inside it. "
        @"If the effect doesn't appear, quit and reopen Final Cut Pro."];
    a.alertStyle = ok ? NSAlertStyleInformational : NSAlertStyleWarning;
    [a addButtonWithTitle:@"OK"];
    [NSApp activateIgnoringOtherApps:YES];
    [a runModal];
    [NSApp terminate:nil];
}
@end

int main(int argc, const char *argv[]) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        AppDelegate *delegate = [AppDelegate new];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
