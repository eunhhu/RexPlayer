#pragma once

#include <QWidget>
#include <QImage>

namespace rex::spice { class SpiceDisplay; }

namespace rex::gui {

class DisplayWidget : public QWidget {
    Q_OBJECT

public:
    explicit DisplayWidget(QWidget* parent = nullptr);

    void setSpiceDisplay(rex::spice::SpiceDisplay* display);

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

    rex::spice::SpiceDisplay* spice_display_ = nullptr;
    QImage current_frame_;
};

} // namespace rex::gui
