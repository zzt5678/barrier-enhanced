// Runs the real helper Cocoa loop in a disposable desktop test process.
// A helper accidentally promoted to a foreground app must fail this probe.
#import <Cocoa/Cocoa.h>
#include "platform/OSXDragSimulator.h"
#include <cstdio>

int main()
{
    __block int result = 1;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        std::fprintf(stderr, "helper Cocoa loop did not stop without input\n");
        ::exit(2);
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        const bool background =
            [NSApp activationPolicy] == NSApplicationActivationPolicyAccessory;
        const bool noKeyWindow = [NSApp keyWindow] == nil;
        bool noVisibleWindows = true;
        for (NSWindow* window in [NSApp windows]) {
            if ([window isVisible]) noVisibleWindows = false;
        }
        std::printf("background=%d noKeyWindow=%d noVisibleWindows=%d\n",
                    background, noKeyWindow, noVisibleWindows);
        result = background && noKeyWindow && noVisibleWindows ? 0 : 1;
        // Production shutdown comes from the input/event thread. No test-side
        // event should hide a missing wake-up in stopCocoaLoop().
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            stopCocoaLoop();
        });
    });
    runCocoaApp();
    return result;
}
