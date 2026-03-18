#pragma once

#include <QObject>
#include <QTimer>
#include <QImage>

// Forward-declare GLib/SPICE types to avoid Qt/GLib macro conflict
// (GLib uses "signals" as a field name, Qt defines it as a macro)
typedef struct _SpiceSession SpiceSession;
typedef struct _SpiceChannel SpiceChannel;
typedef void* gpointer;

namespace rex::spice {

class SpiceDisplay;
class SpiceInput;

class SpiceClient : public QObject {
    Q_OBJECT

public:
    explicit SpiceClient(QObject* parent = nullptr);
    ~SpiceClient() override;

    void connectToSocket(const QString& path);
    void connectToHost(const QString& host, int port);
    void disconnect();

    bool isConnected() const { return connected_; }

    SpiceDisplay* display() { return display_; }
    SpiceInput* input() { return input_; }

Q_SIGNALS:
    void connected();
    void disconnected();
    void error(const QString& message);

private:
    void setupGlibIntegration();
    static void onChannelNew(SpiceSession* session, SpiceChannel* channel, gpointer user_data);
    static void onChannelDestroy(SpiceSession* session, SpiceChannel* channel, gpointer user_data);

    SpiceSession* session_ = nullptr;
    void* glib_ctx_ = nullptr;  // GMainContext* — avoid including glib.h here
    QTimer* glib_timer_ = nullptr;
    SpiceDisplay* display_ = nullptr;
    SpiceInput* input_ = nullptr;
    bool connected_ = false;
};

} // namespace rex::spice
