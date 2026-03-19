#include "window_grabber.h"
#include <QLayout>
#include <cstdio>

#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace rex::emu {

WindowGrabber::WindowGrabber(QObject* parent) : QObject(parent) {
    search_timer_ = new QTimer(this);
    search_timer_->setInterval(500);
    connect(search_timer_, &QTimer::timeout, this, &WindowGrabber::searchForWindow);
}

WindowGrabber::~WindowGrabber() {
    release();
}

void WindowGrabber::startGrabbing(QWidget* container, const QString& avdName) {
    container_ = container;
    avd_name_ = avdName;
    search_retries_ = 0;
    embedded_ = false;
    search_timer_->start();
    fprintf(stderr, "grabber: searching for emulator window '%s'...\n",
            avdName.toUtf8().constData());
}

void WindowGrabber::release() {
    search_timer_->stop();
    if (embedded_widget_) {
        embedded_widget_->setParent(nullptr);
        embedded_widget_ = nullptr;
    }
    foreign_window_ = nullptr;
    embedded_ = false;
}

void WindowGrabber::searchForWindow() {
    search_retries_++;
    if (search_retries_ > kMaxRetries) {
        search_timer_->stop();
        emit error("Emulator window not found after timeout");
        return;
    }

    uint64_t wid = findEmulatorWindowId(avd_name_);
    if (wid == 0) return; // not found yet, keep trying

    search_timer_->stop();
    fprintf(stderr, "grabber: found emulator window ID %llu\n", wid);
    emit windowFound(wid);

    // Store the window ID for CGWindowListCreateImage capture
    // (cross-process window embedding crashes on macOS)
    captured_wid_ = wid;
    embedded_ = true;
    fprintf(stderr, "grabber: will capture emulator window ID %llu at 60fps\n", wid);

    // Start frame capture timer — GPU-accelerated window capture via CoreGraphics
    capture_timer_ = new QTimer(this);
    capture_timer_->setInterval(16); // ~60fps
    connect(capture_timer_, &QTimer::timeout, this, [this]() {
        captureFrame();
    });
    capture_timer_->start();
    emit embedded();
}

uint64_t WindowGrabber::findEmulatorWindowId(const QString& avdName) {
#ifdef Q_OS_MACOS
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionAll, kCGNullWindowID);
    if (!windowList) return 0;

    CFIndex count = CFArrayGetCount(windowList);
    uint64_t result = 0;

    for (CFIndex i = 0; i < count; i++) {
        auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windowList, i));

        // Get owner name
        auto ownerRef = static_cast<CFStringRef>(
            CFDictionaryGetValue(dict, kCGWindowOwnerName));
        if (!ownerRef) continue;

        char owner[256] = {};
        CFStringGetCString(ownerRef, owner, sizeof(owner), kCFStringEncodingUTF8);
        if (strstr(owner, "qemu") == nullptr) continue;

        // Get window title
        auto titleRef = static_cast<CFStringRef>(
            CFDictionaryGetValue(dict, kCGWindowName));
        if (!titleRef) continue;

        char title[512] = {};
        CFStringGetCString(titleRef, title, sizeof(title), kCFStringEncodingUTF8);

        // Match "Android Emulator - <avdName>:*"
        if (strstr(title, "Android Emulator") &&
            (avdName.isEmpty() || strstr(title, avdName.toUtf8().constData()))) {
            auto numRef = static_cast<CFNumberRef>(
                CFDictionaryGetValue(dict, kCGWindowNumber));
            if (numRef) {
                int32_t wid = 0;
                CFNumberGetValue(numRef, kCFNumberSInt32Type, &wid);
                result = static_cast<uint64_t>(wid);
                break;
            }
        }
    }

    CFRelease(windowList);
    return result;

#elif defined(Q_OS_LINUX)
    // X11: use XQueryTree + XFetchName to find emulator window
    // TODO: implement for Linux
    Q_UNUSED(avdName);
    return 0;

#elif defined(Q_OS_WIN)
    // Win32: EnumWindows + GetWindowText
    // TODO: implement for Windows
    Q_UNUSED(avdName);
    return 0;

#else
    Q_UNUSED(avdName);
    return 0;
#endif
}

} // namespace rex::emu
