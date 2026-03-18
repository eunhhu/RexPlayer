#pragma once

#include <QObject>
#include <QKeyEvent>
#include <QMouseEvent>

// Forward-declare SPICE types to avoid Qt/GLib macro conflict
typedef struct _SpiceInputsChannel SpiceInputsChannel;

namespace rex::spice {

class SpiceInput : public QObject {
    Q_OBJECT

public:
    explicit SpiceInput(QObject* parent = nullptr);
    ~SpiceInput() override;

    void attachChannel(SpiceInputsChannel* channel);
    void detachChannel();

    void sendKeyPress(int scancode);
    void sendKeyRelease(int scancode);
    void sendMouseMove(int x, int y);
    void sendMousePress(int button);
    void sendMouseRelease(int button);

    static int qtKeyToScancode(int qtKey);

private:
    SpiceInputsChannel* channel_ = nullptr;
};

} // namespace rex::spice
