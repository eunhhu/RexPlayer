#pragma once

#include "display_widget.h"
#include "settings_dialog.h"
#include "../gpu/display.h"
#include "../vmm/include/rex/vmm/vm.h"
#include "../vmm/include/rex/vmm/snapshot.h"

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include <memory>

namespace rex::gui {

/// MainWindow is the top-level window for the RexPlayer application.
///
/// It provides:
///  - Menu bar: File, VM, Tools, Help
///  - Toolbar: Power, Rotate, Volume, Screenshot, Nav buttons
///  - Central DisplayWidget for rendering the guest framebuffer
///  - Status bar showing VM state, FPS, and memory usage
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Provide VM and Display objects from outside (set by main.cpp)
    void setVm(rex::vmm::Vm* vm);
    void setDisplay(rex::gpu::Display* display);

    /// Apply CLI-provided config overrides
    void applyConfig(const RexConfig& config);

public slots:
    // --- VM operations ---
    void startVm();
    void stopVm();
    void pauseVm();
    void resetVm();

    // --- File / Tools operations ---
    void installApk();
    void takeScreenshot();
    void startScreenRecord();
    void openFridaConsole();

    // --- Settings ---
    void openSettings();

    // --- Display ---
    void rotateDisplay();

    // --- Android nav ---
    void sendHome();
    void sendBack();
    void sendRecent();
    void sendPower();
    void sendVolumeUp();
    void sendVolumeDown();

private slots:
    void updateStatusBar();
    void onConfigApplied(const RexConfig& config);
    void showAbout();

private:
    void setupMenuBar();
    void setupToolBar();
    void setupStatusBar();
    void updateVmStateLabel();

    // Central widget
    DisplayWidget* display_widget_ = nullptr;

    // External references (not owned)
    rex::vmm::Vm* vm_ = nullptr;
    rex::gpu::Display* display_ = nullptr;

    // Status bar labels
    QLabel* state_label_ = nullptr;
    QLabel* fps_label_ = nullptr;
    QLabel* memory_label_ = nullptr;

    // Status bar timer
    QTimer* status_timer_ = nullptr;

    // Current config
    RexConfig config_;

    // Rotation state (0, 90, 180, 270)
    int rotation_ = 0;

    // Screen recording state
    bool recording_ = false;
};

} // namespace rex::gui
