#include "spice_display.h"

// Include GLib/SPICE headers only in .cpp — avoids Qt "signals" macro conflict
#undef signals
#include <spice-client.h>
#define signals Q_SIGNALS

#include <QDateTime>
#include <cstdio>
#include <cstring>

namespace rex::spice {

SpiceDisplay::SpiceDisplay(QObject* parent) : QObject(parent) {}

SpiceDisplay::~SpiceDisplay() {
    detachChannel();
}

void SpiceDisplay::attachChannel(SpiceDisplayChannel* channel) {
    channel_ = channel;

    g_signal_connect(channel, "display-primary-create",
                     G_CALLBACK(onDisplayPrimaryCreate), this);
    g_signal_connect(channel, "display-primary-destroy",
                     G_CALLBACK(onDisplayPrimaryDestroy), this);
    g_signal_connect(channel, "display-invalidate",
                     G_CALLBACK(onDisplayInvalidate), this);
}

void SpiceDisplay::detachChannel() {
    if (channel_) {
        g_signal_handlers_disconnect_by_data(channel_, this);
        channel_ = nullptr;
    }
    primary_data_ = nullptr;
}

QImage SpiceDisplay::currentFrame() const {
    QMutexLocker lock(&mutex_);
    return frame_.copy();
}

void SpiceDisplay::onDisplayPrimaryCreate(SpiceChannel*,
    gint format, gint width, gint height, gint stride,
    gint, gpointer imgdata, gpointer user_data) {
    auto* self = static_cast<SpiceDisplay*>(user_data);
    Q_UNUSED(format);

    QMutexLocker lock(&self->mutex_);
    self->primary_data_ = static_cast<const uint8_t*>(imgdata);
    self->width_ = width;
    self->height_ = height;
    self->stride_ = stride;
    self->frame_ = QImage(self->primary_data_, width, height, stride,
                           QImage::Format_RGB32);

    fprintf(stderr, "spice: display created %dx%d\n", width, height);
    emit self->resolutionChanged(width, height);
}

void SpiceDisplay::onDisplayPrimaryDestroy(SpiceChannel*, gpointer user_data) {
    auto* self = static_cast<SpiceDisplay*>(user_data);
    QMutexLocker lock(&self->mutex_);
    self->primary_data_ = nullptr;
    self->frame_ = QImage();
}

void SpiceDisplay::onDisplayInvalidate(SpiceChannel*,
    gint, gint, gint, gint, gpointer user_data) {
    auto* self = static_cast<SpiceDisplay*>(user_data);

    self->frame_count_++;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (self->last_fps_time_ == 0) {
        self->last_fps_time_ = now;
    } else if (now - self->last_fps_time_ >= 1000) {
        self->fps_ = self->frame_count_ * 1000.0 / (now - self->last_fps_time_);
        self->frame_count_ = 0;
        self->last_fps_time_ = now;
    }

    {
        QMutexLocker lock(&self->mutex_);
        if (self->primary_data_) {
            self->frame_ = QImage(self->primary_data_,
                                   self->width_, self->height_,
                                   self->stride_, QImage::Format_RGB32);
        }
    }
    emit self->frameReady();
}

} // namespace rex::spice
