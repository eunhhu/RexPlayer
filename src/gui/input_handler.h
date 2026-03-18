#pragma once

#include <QObject>
#include <QKeyEvent>
#include <QMouseEvent>

namespace rex::vnc { class VncClient; }

namespace rex::gui {

class InputHandler : public QObject {
    Q_OBJECT

public:
    explicit InputHandler(QObject* parent = nullptr);

    void setVncClient(rex::vnc::VncClient* vnc);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void handleKeyPress(QKeyEvent* event);
    void handleKeyRelease(QKeyEvent* event);
    void handleMousePress(QMouseEvent* event);
    void handleMouseMove(QMouseEvent* event);
    void handleMouseRelease(QMouseEvent* event);

    rex::vnc::VncClient* vnc_ = nullptr;
};

} // namespace rex::gui
