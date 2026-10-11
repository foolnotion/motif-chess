#include "motif/slint_app/macos_open_file_bridge.hpp"

#import <AppKit/AppKit.h>

#include <mutex>
#include <utility>

namespace
{

std::mutex handler_mutex;
auto handler = motif::slint_app::macos_open_file_handler {};

void dispatch_open_path(NSURL* const url)
{
    if (url == nil || !url.isFileURL || url.path == nil) {
        return;
    }

    auto current = motif::slint_app::macos_open_file_handler {};
    {
        auto const lock = std::scoped_lock {handler_mutex};
        current = handler;
    }
    if (current) {
        current(std::string {url.path.UTF8String});
    }
}

}  // namespace

@interface motif_open_file_delegate : NSObject
@end

@implementation motif_open_file_delegate

- (void)handleOpenDocuments:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply_event
{
    (void)reply_event;
    auto const descriptor = [event paramDescriptorForKeyword:keyDirectObject];
    for (NSInteger index = 1; index <= descriptor.numberOfItems; ++index) {
        dispatch_open_path([descriptor descriptorAtIndex:index].fileURLValue);
    }
}

@end

namespace motif::slint_app
{

void install_macos_open_file_handler(macos_open_file_handler next)
{
    {
        auto const lock = std::scoped_lock {handler_mutex};
        handler = std::move(next);
    }

    static auto* const delegate = [[motif_open_file_delegate alloc] init];
    auto* const manager = [NSAppleEventManager sharedAppleEventManager];
    [manager setEventHandler:delegate
                  andSelector:@selector(handleOpenDocuments:withReplyEvent:)
                forEventClass:kCoreEventClass
                   andEventID:kAEOpenDocuments];
}

}  // namespace motif::slint_app
