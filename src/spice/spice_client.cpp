#include "spice_client.h"
#include "spice_display.h"
#include "spice_input.h"

// Include GLib/SPICE headers only in .cpp — avoids Qt "signals" macro conflict
#undef signals
#include <glib.h>
#include <spice-client.h>
#define signals Q_SIGNALS

#include <cstdio>

namespace rex::spice {

SpiceClient::SpiceClient(QObject* parent) : QObject(parent) {
    display_ = new SpiceDisplay(this);
    input_ = new SpiceInput(this);
    setupGlibIntegration();
}

SpiceClient::~SpiceClient() {
    disconnect();
    if (glib_ctx_) {
        g_main_context_unref(static_cast<GMainContext*>(glib_ctx_));
    }
}

void SpiceClient::setupGlibIntegration() {
    glib_ctx_ = g_main_context_new();

    glib_timer_ = new QTimer(this);
    glib_timer_->setInterval(16);
    connect(glib_timer_, &QTimer::timeout, this, [this]() {
        while (g_main_context_iteration(static_cast<GMainContext*>(glib_ctx_), FALSE)) {}
    });
    glib_timer_->start();
}

void SpiceClient::connectToSocket(const QString& path) {
    if (session_) disconnect();

    session_ = spice_session_new();
    g_object_set(session_,
                 "unix-path", path.toUtf8().constData(),
                 NULL);

    g_signal_connect(session_, "channel-new",
                     G_CALLBACK(onChannelNew), this);
    g_signal_connect(session_, "channel-destroy",
                     G_CALLBACK(onChannelDestroy), this);

    if (!spice_session_connect(session_)) {
        emit error("Failed to connect to SPICE socket");
        g_object_unref(session_);
        session_ = nullptr;
        return;
    }

    connected_ = true;
    emit connected();
    fprintf(stderr, "spice: connecting to %s\n", path.toUtf8().constData());
}

void SpiceClient::connectToHost(const QString& host, int port) {
    if (session_) disconnect();

    session_ = spice_session_new();
    g_object_set(session_,
                 "host", host.toUtf8().constData(),
                 "port", QString::number(port).toUtf8().constData(),
                 NULL);

    g_signal_connect(session_, "channel-new",
                     G_CALLBACK(onChannelNew), this);
    g_signal_connect(session_, "channel-destroy",
                     G_CALLBACK(onChannelDestroy), this);

    if (!spice_session_connect(session_)) {
        emit error("Failed to connect to SPICE host");
        g_object_unref(session_);
        session_ = nullptr;
        return;
    }

    connected_ = true;
    emit connected();
}

void SpiceClient::disconnect() {
    if (session_) {
        spice_session_disconnect(session_);
        g_object_unref(session_);
        session_ = nullptr;
    }
    connected_ = false;
    emit disconnected();
}

void SpiceClient::onChannelNew(SpiceSession*, SpiceChannel* channel, gpointer user_data) {
    auto* self = static_cast<SpiceClient*>(user_data);

    int type;
    g_object_get(channel, "channel-type", &type, NULL);

    if (type == SPICE_CHANNEL_DISPLAY) {
        fprintf(stderr, "spice: display channel connected\n");
        self->display_->attachChannel(SPICE_DISPLAY_CHANNEL(channel));
    } else if (type == SPICE_CHANNEL_INPUTS) {
        fprintf(stderr, "spice: inputs channel connected\n");
        self->input_->attachChannel(SPICE_INPUTS_CHANNEL(channel));
    }

    spice_channel_connect(channel);
}

void SpiceClient::onChannelDestroy(SpiceSession*, SpiceChannel* channel, gpointer user_data) {
    auto* self = static_cast<SpiceClient*>(user_data);

    int type;
    g_object_get(channel, "channel-type", &type, NULL);

    if (type == SPICE_CHANNEL_DISPLAY) {
        self->display_->detachChannel();
    } else if (type == SPICE_CHANNEL_INPUTS) {
        self->input_->detachChannel();
    }
}

} // namespace rex::spice
