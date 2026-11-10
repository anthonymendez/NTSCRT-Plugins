// XPC service entry point of the FxPlug 4 plug-in. The FxPrincipal from
// FxPlug.framework registers the classes listed in Info.plist ProPlugPlugInList
// (NTSCRTPlugIn) with the host and runs the service's run loop.
#import <Foundation/Foundation.h>
#import <FxPlug/FxPlugSDK.h>

int main(int argc, const char *argv[]) {
    (void)argc;
    (void)argv;
    [FxPrincipal startServicePrincipal];
    return 0;
}
