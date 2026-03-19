#include "window_grabber.h"
#include <QLayout>
#include <QDateTime>
#include <cstdio>

#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
// CGWindowListCreateImage is obsoleted in macOS 15 SDK but still works at runtime.
// We suppress the error by declaring it ourselves if the SDK marks it unavailable.
extern "C" CGImageRef CGWindowListCreateImage(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption)
    __attribute__((availability(macos, introduced=10.5)));
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
    if (capture_timer_) {
        capture_timer_->stop();
    }
    embedded_ = false;
    captured_wid_ = 0;
}

QImage WindowGrabber::currentFrame() const {
    QMutexLocker lock(&frame_mutex_);
    return frame_.copy();
}

void WindowGrabber::searchForWindow() {
    search_retries_++;
    if (search_retries_ > kMaxRetries) {
        search_timer_->stop();
        emit error("Emulator window not found after timeout");
        return;
    }

    uint64_t wid = findEmulatorWindowId(avd_name_);
    if (wid == 0) return;

    search_timer_->stop();
    fprintf(stderr, "grabber: found emulator window ID %llu\n", wid);
    emit windowFound(wid);

    captured_wid_ = wid;
    embedded_ = true;
    fprintf(stderr, "grabber: capturing window at 60fps via CGWindowListCreateImage\n");

    capture_timer_ = new QTimer(this);
    capture_timer_->setInterval(16); // ~60fps
    connect(capture_timer_, &QTimer::timeout, this, &WindowGrabber::captureFrame);
    capture_timer_->start();
    emit embedded();
}

void WindowGrabber::captureFrame() {
#ifdef Q_OS_MACOS
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wavailability"
    if (captured_wid_ == 0) return;

    CGImageRef cgImage = CGWindowListCreateImage(
        CGRectNull,
        kCGWindowListOptionIncludingWindow,
        static_cast<CGWindowID>(captured_wid_),
        kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);

    if (!cgImage) return;

    size_t w = CGImageGetWidth(cgImage);
    size_t h = CGImageGetHeight(cgImage);

    if (w == 0 || h == 0) {
        CGImageRelease(cgImage);
        return;
    }

    // Convert CGImage to QImage
    QImage img(static_cast<int>(w), static_cast<int>(h), QImage::Format_ARGB32);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        img.bits(), w, h, 8, img.bytesPerLine(),
        colorSpace, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);

    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cgImage);
    CGContextRelease(ctx);
    CGColorSpaceRelease(colorSpace);
    CGImageRelease(cgImage);

    {
        QMutexLocker lock(&frame_mutex_);
        frame_ = img;
    }

    // FPS tracking
    frame_count_++;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (last_fps_time_ == 0) {
        last_fps_time_ = now;
    } else if (now - last_fps_time_ >= 1000) {
        fps_ = frame_count_ * 1000.0 / (now - last_fps_time_);
        frame_count_ = 0;
        last_fps_time_ = now;
    }

    emit frameReady();
#endif
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

        auto ownerRef = static_cast<CFStringRef>(
            CFDictionaryGetValue(dict, kCGWindowOwnerName));
        if (!ownerRef) continue;

        char owner[256] = {};
        CFStringGetCString(ownerRef, owner, sizeof(owner), kCFStringEncodingUTF8);
        if (strstr(owner, "qemu") == nullptr) continue;

        auto titleRef = static_cast<CFStringRef>(
            CFDictionaryGetValue(dict, kCGWindowName));
        if (!titleRef) continue;

        char title[512] = {};
        CFStringGetCString(titleRef, title, sizeof(title), kCFStringEncodingUTF8);

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
#else
    Q_UNUSED(avdName);
    return 0;
#endif
}

} // namespace rex::emu
