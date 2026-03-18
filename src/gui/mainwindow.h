#pragma once

#include <QMainWindow>
#include <QToolBar>
#include <QLabel>
#include <QTimer>
#include <QAction>

namespace rex::qemu { class QemuProcess; }
namespace rex::spice { class SpiceClient; }

namespace rex::gui {

class DisplayWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void setQemuProcess(rex::qemu::QemuProcess* qemu);
    void setSpiceClient(rex::spice::SpiceClient* spice);

private slots:
    void onVmStateChanged();
    void onPower();
    void onPause();
    void onReset();
    void onVolumeUp();
    void onVolumeDown();
    void onHome();
    void onBack();
    void onRecents();
    void onRotate();
    void onScreenshot();
    void onGps();
    void onKeymapToggle();
    void onFridaToggle();
    void onSettings();
    void updateStatusBar();

private:
    void createMenus();
    void createSidebar();
    void createStatusBar_();

    rex::qemu::QemuProcess* qemu_ = nullptr;
    rex::spice::SpiceClient* spice_ = nullptr;
    DisplayWidget* display_ = nullptr;

    QToolBar* sidebar_ = nullptr;
    QAction* power_action_ = nullptr;

    QLabel* status_vm_ = nullptr;
    QLabel* status_spice_ = nullptr;
    QLabel* status_fps_ = nullptr;
    QLabel* status_cpu_ = nullptr;
    QLabel* status_ram_ = nullptr;
    QLabel* status_adb_ = nullptr;
    QTimer* status_timer_ = nullptr;
};

} // namespace rex::gui
