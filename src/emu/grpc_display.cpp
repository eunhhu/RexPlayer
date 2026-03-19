#include "grpc_display.h"

#include <grpcpp/grpcpp.h>
#include "emulator_controller.grpc.pb.h"

#include <QDateTime>
#include <cstdio>

using android::emulation::control::EmulatorController;
using android::emulation::control::ImageFormat;
using android::emulation::control::Image;
using android::emulation::control::KeyboardEvent;
using android::emulation::control::TouchEvent;
using android::emulation::control::Touch;
using android::emulation::control::MouseEvent;

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

    ImageFormat format;
    format.set_format(ImageFormat::RGBA8888);
    // 0 = native resolution
    format.set_width(0);
    format.set_height(0);

    while (!should_stop_) {
        grpc::ClientContext ctx;
        auto reader = stub->streamScreenshot(&ctx, format);

        Image image;
        while (reader->Read(&image) && !should_stop_) {
            const auto& data = image.image();
            int w = image.format().width();
            int h = image.format().height();

            if (w > 0 && h > 0 && !data.empty()) {
                QImage frame(reinterpret_cast<const uchar*>(data.data()),
                             w, h, w * 4, QImage::Format_RGBA8888);

                {
                    QMutexLocker lock(&mutex_);
                    frame_ = frame.copy(); // deep copy since data is temporary
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

        auto status = reader->Finish();
        if (!status.ok() && !should_stop_) {
            fprintf(stderr, "grpc: stream ended: %s, reconnecting...\n",
                    status.error_message().c_str());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    fprintf(stderr, "grpc: stream loop exited\n");
}

} // namespace rex::emu
