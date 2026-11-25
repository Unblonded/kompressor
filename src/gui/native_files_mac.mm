// macOS side of native_files.hpp: files on the pasteboard, and dragging files out of the window as
// file promises - Finder tells us where the drop landed and the files are written there, so nothing
// is extracted until the mouse is released over a folder.
#import <AppKit/AppKit.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>

#include "kz_common.hpp"
#include "native_files.hpp"

namespace fs = std::filesystem;

namespace {

// Shared by the drag's file promises: the files are made once (by `prepare`), whichever promise asks first.
struct DropState {
    std::function<std::vector<std::string>()> prepare;
    std::mutex mu;
    std::condition_variable cv;
    bool preparing = false, prepared = false;
    std::vector<std::string> made;
    std::atomic<int> started{0}, finished{0}, written{0};

    std::vector<std::string> prepareOnce() {
        std::unique_lock<std::mutex> l(mu);
        if (!preparing) {
            preparing = true;
            l.unlock();
            std::vector<std::string> r = prepare();
            l.lock();
            made = std::move(r);
            prepared = true;
            cv.notify_all();
        } else {
            cv.wait(l, [this] { return prepared; });
        }
        return made;
    }
};

}  // namespace

@interface KzPromiseDelegate : NSObject <NSFilePromiseProviderDelegate> {
@public
    std::shared_ptr<DropState> state;
    NSOperationQueue* queue;
}
@end

@implementation KzPromiseDelegate
- (NSString*)filePromiseProvider:(NSFilePromiseProvider*)provider fileNameForType:(NSString*)fileType {
    return (NSString*)provider.userInfo;
}
- (void)filePromiseProvider:(NSFilePromiseProvider*)provider
          writePromiseToURL:(NSURL*)url
          completionHandler:(void (^)(NSError* errorOrNil))completionHandler {
    state->started++;
    std::vector<std::string> made = state->prepareOnce();
    std::string name = [(NSString*)provider.userInfo UTF8String];
    bool ok = false;
    for (auto& m : made) {
        if (kz::fileNameOf(m) != name) continue;
        std::error_code ec;
        fs::copy(kz::u8path(m), fs::path(url.path.UTF8String), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        ok = !ec;
        break;
    }
    if (ok) state->written++;
    state->finished++;
    glfwPostEmptyEvent();
    completionHandler(ok ? nil : [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError userInfo:nil]);
}
- (NSOperationQueue*)operationQueueForFilePromiseProvider:(NSFilePromiseProvider*)provider {
    return queue;  // off the main thread, so the window keeps drawing while files are extracted
}
@end

@interface KzDragSource : NSObject <NSDraggingSource> {
@public
    BOOL ended;
    NSDragOperation operation;
}
@end

@implementation KzDragSource
- (NSDragOperation)draggingSession:(NSDraggingSession*)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    return context == NSDraggingContextOutsideApplication ? NSDragOperationCopy : NSDragOperationNone;
}
- (void)draggingSession:(NSDraggingSession*)session endedAtPoint:(NSPoint)screenPoint operation:(NSDragOperation)op {
    ended = YES;
    operation = op;
}
@end

namespace kzgui::native {

bool fileTransferSupported() { return true; }

std::vector<std::string> clipboardFiles() {
    std::vector<std::string> out;
    @autoreleasepool {
        NSArray* urls = [[NSPasteboard generalPasteboard] readObjectsForClasses:@[ [NSURL class] ]
                                                                        options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
        for (NSURL* u in urls)
            if (u.path) out.push_back(u.path.UTF8String);
    }
    return out;
}

bool setClipboardFiles(GLFWwindow*, const std::vector<std::string>& paths) {
    if (paths.empty()) return false;
    @autoreleasepool {
        NSMutableArray* urls = [NSMutableArray array];
        for (auto& p : paths) [urls addObject:[NSURL fileURLWithPath:[NSString stringWithUTF8String:p.c_str()]]];
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        return [pb writeObjects:urls];
    }
}

bool dragFilesOut(GLFWwindow* window, const std::vector<std::string>& paths,
                  const std::function<std::vector<std::string>()>& prepare, const std::function<void()>& cancel) {
    if (paths.empty()) return false;
    NSWindow* w = glfwGetCocoaWindow(window);
    NSView* view = w.contentView;
    if (!w || !view) return false;

    auto state = std::make_shared<DropState>();
    state->prepare = prepare;
    KzPromiseDelegate* del = [KzPromiseDelegate new];
    del->state = state;
    del->queue = [NSOperationQueue new];
    KzDragSource* source = [KzDragSource new];

    // The drag starts from a mouse event; GLFW has just handled the one that moved the row.
    NSEvent* ev = [NSApp currentEvent];
    if (!ev || (ev.type != NSEventTypeLeftMouseDragged && ev.type != NSEventTypeLeftMouseDown)) {
        ev = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged
                                location:[w mouseLocationOutsideOfEventStream]
                           modifierFlags:0
                               timestamp:NSProcessInfo.processInfo.systemUptime
                            windowNumber:w.windowNumber
                                 context:nil
                             eventNumber:0
                              clickCount:1
                                pressure:1];
    }
    NSPoint at = [view convertPoint:ev.locationInWindow fromView:nil];
    NSImage* icon = [[NSWorkspace sharedWorkspace] iconForFileType:@"public.data"];
    NSMutableArray* items = [NSMutableArray array];
    int i = 0;
    for (auto& p : paths) {
        NSFilePromiseProvider* prov = [[NSFilePromiseProvider alloc] initWithFileType:@"public.data" delegate:del];
        prov.userInfo = [NSString stringWithUTF8String:kz::fileNameOf(p).c_str()];
        NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:prov];
        [item setDraggingFrame:NSMakeRect(at.x - 16 + i * 4, at.y - 16 - i * 4, 32, 32) contents:icon];
        [items addObject:item];
        ++i;
    }
    [view beginDraggingSessionWithItems:items event:ev source:source];

    // Wait (drawing all the while) for the drop, then for every promised file to be written.
    const int count = (int)paths.size();
    auto endedAt = std::chrono::steady_clock::time_point();
    while (true) {
        @autoreleasepool {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate dateWithTimeIntervalSinceNow:1.0 / 60.0]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (e) {
                if (e.type == NSEventTypeKeyDown && e.keyCode == 53 && cancel) cancel();  // Esc
                [NSApp sendEvent:e];
            }
        }
        redraw();
        if (!source->ended) continue;
        if (source->operation == NSDragOperationNone) break;
        if (endedAt == std::chrono::steady_clock::time_point()) endedAt = std::chrono::steady_clock::now();
        if (state->finished.load() >= count) break;
        // Dropped somewhere that never asked for the files.
        if (state->started.load() == 0 && std::chrono::steady_clock::now() - endedAt > std::chrono::seconds(3)) break;
    }
    // The drag ate the button release; give GLFW one so it doesn't think the button is still down.
    NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                     location:[w mouseLocationOutsideOfEventStream]
                                modifierFlags:0
                                    timestamp:NSProcessInfo.processInfo.systemUptime
                                 windowNumber:w.windowNumber
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:0];
    [NSApp postEvent:up atStart:NO];
    return state->written.load() > 0;
}

}  // namespace kzgui::native
