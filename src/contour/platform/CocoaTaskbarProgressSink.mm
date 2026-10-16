// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/TaskbarProgressSink.hpp>

#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>

#import <AppKit/AppKit.h>

/// Draws the application icon and progress together; Dock tiles require explicit display calls.
@interface ContourTaskbarProgressView: NSView
{
    contour::platform::TaskbarProgress _progress;
    double _phase;
}
- (void)applyProgress:(contour::platform::TaskbarProgress)progress phase:(double)phase;
@end

@implementation ContourTaskbarProgressView
- (void)applyProgress:(contour::platform::TaskbarProgress)progress phase:(double)phase
{
    _progress = progress;
    _phase = phase;
}

- (void)drawRect:(NSRect)dirtyRect
{
    [super drawRect:dirtyRect];
    auto const bounds = self.bounds;
    [NSApp.applicationIconImage drawInRect:bounds];
    auto const track = NSMakeRect(
        NSWidth(bounds) * 0.1, NSHeight(bounds) * 0.08, NSWidth(bounds) * 0.8, NSHeight(bounds) * 0.1);
    auto const radius = NSHeight(track) / 2.0;
    [[NSColor colorWithWhite:0.0 alpha:0.75] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:track xRadius:radius yRadius:radius] fill];

    auto const inset = NSInsetRect(track, NSHeight(track) * 0.15, NSHeight(track) * 0.15);
    auto fill = inset;
    using vtbackend::ProgressState;
    auto* color = NSColor.controlAccentColor;
    switch (_progress.state)
    {
        case ProgressState::Inactive: return;
        case ProgressState::Normal: break;
        case ProgressState::Error: color = NSColor.systemRedColor; break;
        case ProgressState::Paused: color = NSColor.systemOrangeColor; break;
        case ProgressState::Indeterminate: break;
    }
    if (_progress.state == ProgressState::Indeterminate)
    {
        fill.size.width *= 0.25;
        fill.origin.x += (NSWidth(inset) - NSWidth(fill)) * _phase;
    }
    else
        fill.size.width *= _progress.percentage / 100.0;
    [color setFill];
    [[NSBezierPath bezierPathWithRoundedRect:fill xRadius:NSHeight(fill) / 2.0
                                     yRadius:NSHeight(fill) / 2.0] fill];
}
@end

namespace contour::platform
{
namespace
{
    class CocoaTaskbarProgressSink final: public TaskbarProgressSink
    {
      public:
        CocoaTaskbarProgressSink()
        {
            _animation.setInterval(100);
            QObject::connect(&_animation, &QTimer::timeout, &_animation, [this] {
                _phase += 0.1;
                if (_phase >= 1.0)
                    _phase = 0.0;
                draw();
            });
        }
        ~CocoaTaskbarProgressSink() override { clear(); }

        [[nodiscard]] bool isSupported() const noexcept override
        {
            return QGuiApplication::platformName() == QStringLiteral("cocoa");
        }
        [[nodiscard]] TaskbarProgressScope scope() const noexcept override
        {
            return TaskbarProgressScope::Application;
        }
        void bindWindow(uint64_t, QWindow&) override {}
        void setProgress(uint64_t, TaskbarProgress) override {}
        void removeWindow(uint64_t) override {}

        void setApplicationProgress(TaskbarProgress progress) override
        {
            if (!isSupported() || _progress == progress)
                return;
            _progress = progress;
            if (progress.state == vtbackend::ProgressState::Inactive)
            {
                clear();
                return;
            }
            if (!_view)
            {
                auto* tile = NSApp.dockTile;
                _previousContent = tile.contentView;
                _view = [[ContourTaskbarProgressView alloc]
                    initWithFrame:NSMakeRect(0, 0, tile.size.width, tile.size.height)];
                _view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
                tile.contentView = _view;
            }
            if (progress.state == vtbackend::ProgressState::Indeterminate)
                _animation.start();
            else
                _animation.stop();
            draw();
        }

      private:
        void draw()
        {
            [_view applyProgress:_progress phase:_phase];
            [NSApp.dockTile display];
        }

        void clear()
        {
            _animation.stop();
            if (_view && NSApp.dockTile.contentView == _view)
            {
                NSApp.dockTile.contentView = _previousContent;
                [NSApp.dockTile display];
            }
            _view = nil;
            _previousContent = nil;
        }

        // ARC retains both views until restoration; no raw owning Objective-C pointers.
        ContourTaskbarProgressView* __strong _view = nil;
        NSView* __strong _previousContent = nil;
        TaskbarProgress _progress;
        QTimer _animation;
        double _phase = 0.0;
    };
} // namespace

std::unique_ptr<TaskbarProgressSink> makeTaskbarProgressSink()
{
    return std::make_unique<CocoaTaskbarProgressSink>();
}
} // namespace contour::platform
