#include "grpc_display.h"

#include <grpcpp/grpcpp.h>
#include "emulator_controller.grpc.pb.h"

#include <QDateTime>
#include <cstdio>

#ifndef _WIN32
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

using android::emulation::control::EmulatorController;
using android::emulation::control::ImageFormat;
using android::emulation::control::Image;
using android::emulation::control::KeyboardEvent;
using android::emulation::control::TouchEvent;
using android::emulation::control::Touch;
using android::emulation::control::MouseEvent;
using android::emulation::control::ImageTransport;

namespace rex::emu {

GrpcDisplay::GrpcDisplay(QObject* parent) : QObject(parent) {}

GrpcDisplay::~GrpcDisplay() {
    disconnect();
}

void GrpcDisplay::connectToEmulator(const QString& host, uint16_t port) {
    if (connected_) disconnect();

    host_ = host;
    port_ = port;

    QString target = QString("%1:%2").arg(host).arg(port);
    fprintf(stderr, "grpc: connecting to %s\n", target.toUtf8().constData());

    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(64 * 1024 * 1024); // 64MB — enough for 4K RGBA
    channel_ = grpc::CreateCustomChannel(target.toStdString(),
                                          grpc::InsecureChannelCredentials(), args);

    should_stop_ = false;
    connected_ = true;

    // Start streaming in a std::thread (not QThread — avoids event loop issues)
    stream_thread_ = std::make_unique<std::thread>(&GrpcDisplay::streamLoop, this);
    emit connected();
}

void GrpcDisplay::disconnect() {
    should_stop_ = true;
    connected_ = false;

    if (stream_thread_ && stream_thread_->joinable()) {
        stream_thread_->join();
    }
    stream_thread_.reset();
    channel_.reset();
    emit disconnected();
}

QImage GrpcDisplay::currentFrame() const {
    QMutexLocker lock(&mutex_);
    return frame_.copy();
}

void GrpcDisplay::sendKey(int keyCode, bool down) {
    if (!channel_) return;

    auto stub = EmulatorController::NewStub(channel_);
    KeyboardEvent event;
    event.set_codetype(KeyboardEvent::Evdev);
    event.set_eventtype(down ? KeyboardEvent::keydown : KeyboardEvent::keyup);
    event.set_keycode(keyCode);

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(100));
    google::protobuf::Empty empty;
    stub->sendKey(&ctx, event, &empty);
}

void GrpcDisplay::sendTouch(int x, int y, int id, bool down, int pressure) {
    if (!channel_) return;

    auto stub = EmulatorController::NewStub(channel_);
    TouchEvent event;
    auto* touch = event.add_touches();
    touch->set_x(x);
    touch->set_y(y);
    touch->set_identifier(id);
    touch->set_pressure(down ? pressure : 0);

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(100));
    google::protobuf::Empty empty;
    stub->sendTouch(&ctx, event, &empty);
}

void GrpcDisplay::sendMouse(int x, int y, int buttons) {
    if (!channel_) return;

    auto stub = EmulatorController::NewStub(channel_);
    MouseEvent event;
    event.set_x(x);
    event.set_y(y);
    event.set_buttons(buttons);

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(100));
    google::protobuf::Empty empty;
    stub->sendMouse(&ctx, event, &empty);
}

void GrpcDisplay::streamLoop() {
    auto stub = EmulatorController::NewStub(channel_);

    // Use getScreenshot in a tight loop — faster than streamScreenshot
    // because we control the pacing and avoid server-side buffering
    ImageFormat format;
    format.set_format(ImageFormat::RGB888); // 3 bpp — 30% less data than RGBA
    format.set_width(540);   // half res: 540x1200 = 1.9MB vs 10MB at native
    format.set_height(1200);

    fprintf(stderr, "grpc: starting frame loop (540x1200 RGB888)\n");

    while (!should_stop_) {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        Image image;
        auto status = stub->getScreenshot(&ctx, format, &image);

        if (!status.ok()) {
            if (!should_stop_) {
                fprintf(stderr, "grpc: getScreenshot failed: %s\n",
                        status.error_message().c_str());
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            continue;
        }

        const auto& data = image.image();
        int w = image.format().width();
        int h = image.format().height();

        if (w > 0 && h > 0 && !data.empty()) {
            QImage frame(reinterpret_cast<const uchar*>(data.data()),
                         w, h, w * 3, QImage::Format_RGB888);

            {
                QMutexLocker lock(&mutex_);
                frame_ = frame.copy();
                if (width_ != w || height_ != h) {
                    width_ = w;
                    height_ = h;
                    QMetaObject::invokeMethod(this, [this, w, h]() {
                        emit resolutionChanged(w, h);
                    }, Qt::QueuedConnection);
                }
            }

            // FPS tracking
            frame_count_++;
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (last_fps_time_ == 0) {
                last_fps_time_ = now;
            } else if (now - last_fps_time_ >= 1000) {
                fps_ = frame_count_.load() * 1000.0 / (now - last_fps_time_);
                frame_count_ = 0;
                last_fps_time_ = now;
            }

            QMetaObject::invokeMethod(this, [this]() {
                emit frameReady();
            }, Qt::QueuedConnection);
        }
    }

    fprintf(stderr, "grpc: stream loop exited\n");
}

} // namespace rex::emu
