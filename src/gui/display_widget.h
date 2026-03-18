#pragma once

#include <QWidget>
#include <QImage>

namespace rex::vnc { class VncClient; }

namespace rex::gui {

class DisplayWidget : public QWidget {
    Q_OBJECT

public:
    explicit DisplayWidget(QWidget* parent = nullptr);

    void setVncClient(rex::vnc::VncClient* vnc);

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void onFrameReady();
    QRect computeViewport() const;
    QPoint mapToGuest(const QPoint& widgetPos) const;

    rex::vnc::VncClient* vnc_ = nullptr;
    QImage current_frame_;
};

} // namespace rex::gui
