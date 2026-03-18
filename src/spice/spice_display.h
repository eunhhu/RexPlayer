#pragma once

#include <QObject>
#include <QImage>
#include <QMutex>

// Forward-declare SPICE types to avoid Qt/GLib macro conflict
typedef struct _SpiceDisplayChannel SpiceDisplayChannel;
typedef struct _SpiceChannel SpiceChannel;
typedef void* gpointer;
typedef int gint;

namespace rex::spice {

class SpiceDisplay : public QObject {
    Q_OBJECT

public:
    explicit SpiceDisplay(QObject* parent = nullptr);
    ~SpiceDisplay() override;

    void attachChannel(SpiceDisplayChannel* channel);
    void detachChannel();

    QImage currentFrame() const;

    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return fps_; }

Q_SIGNALS:
    void frameReady();
    void resolutionChanged(int width, int height);

private:
    static void onDisplayPrimaryCreate(SpiceChannel* channel,
        gint format, gint width, gint height, gint stride,
        gint shmid, gpointer imgdata, gpointer user_data);
    static void onDisplayPrimaryDestroy(SpiceChannel* channel, gpointer user_data);
    static void onDisplayInvalidate(SpiceChannel* channel,
        gint x, gint y, gint w, gint h, gpointer user_data);

    SpiceDisplayChannel* channel_ = nullptr;
    mutable QMutex mutex_;
    QImage frame_;
    const uint8_t* primary_data_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;

    int frame_count_ = 0;
    double fps_ = 0.0;
    qint64 last_fps_time_ = 0;
};

} // namespace rex::spice
