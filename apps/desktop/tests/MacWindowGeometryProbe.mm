#import <AppKit/AppKit.h>
#include <QWidget>

// The fixture must observe completed native zoom, not just Qt's requested
// state, before requesting the opposite transition. No private Qt APIs.
bool macWindowIsZoomed(QWidget *window) {
    if (!window || !window->internalWinId()) return false;
    NSView *view = reinterpret_cast<NSView *>(window->internalWinId());
    return view.window && view.window.isZoomed;
}
