#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

namespace rex::gui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("RexPlayer");

    // Central display widget
    display_widget_ = new DisplayWidget(this);
    setCentralWidget(display_widget_);
    connect(display_widget_, &DisplayWidget::touchInput,
            this, &MainWindow::onGuestTouchInput);
    connect(display_widget_, &DisplayWidget::keyInput,
            this, &MainWindow::onGuestKeyInput);

    setupMenuBar();
    setupToolBar();
    setupStatusBar();

    // Status bar update timer (1 Hz)
    status_timer_ = new QTimer(this);
    connect(status_timer_, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    status_timer_->start(1000);
}

MainWindow::~MainWindow() {
    stopVm();
}

void MainWindow::setVm(rex::vmm::Vm* vm) {
    vm_ = vm;
    updateVmStateLabel();
}

void MainWindow::setDisplay(rex::gpu::Display* display) {
    display_ = display;
    if (display_widget_) {
        display_widget_->attachDisplay(display);
    }
}

void MainWindow::applyConfig(const RexConfig& config) {
    config_ = config;
}

// ============================================================================
// Menu bar
// ============================================================================

void MainWindow::setupMenuBar() {
    // --- File menu ---
    auto* fileMenu = menuBar()->addMenu(tr("&File"));

    auto* installAction = fileMenu->addAction(tr("&Install APK..."));
    connect(installAction, &QAction::triggered, this, &MainWindow::installApk);

    fileMenu->addSeparator();

    auto* settingsAction = fileMenu->addAction(tr("&Settings..."));
    settingsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::openSettings);

    fileMenu->addSeparator();

    auto* exitAction = fileMenu->addAction(tr("E&xit"));
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    // --- VM menu ---
    auto* vmMenu = menuBar()->addMenu(tr("&VM"));

    auto* startAction = vmMenu->addAction(tr("&Start"));
    connect(startAction, &QAction::triggered, this, &MainWindow::startVm);

    auto* pauseAction = vmMenu->addAction(tr("&Pause"));
    connect(pauseAction, &QAction::triggered, this, &MainWindow::pauseVm);

    auto* stopAction = vmMenu->addAction(tr("S&top"));
    connect(stopAction, &QAction::triggered, this, &MainWindow::stopVm);

    auto* resetAction = vmMenu->addAction(tr("&Reset"));
    connect(resetAction, &QAction::triggered, this, &MainWindow::resetVm);

    // --- Tools menu ---
    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));

    auto* screenshotAction = toolsMenu->addAction(tr("&Screenshot"));
    screenshotAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    connect(screenshotAction, &QAction::triggered, this, &MainWindow::takeScreenshot);

    auto* recordAction = toolsMenu->addAction(tr("Screen &Record"));
    connect(recordAction, &QAction::triggered, this, &MainWindow::startScreenRecord);

    toolsMenu->addSeparator();

    auto* fridaAction = toolsMenu->addAction(tr("&Frida Console"));
    connect(fridaAction, &QAction::triggered, this, &MainWindow::openFridaConsole);

    // --- Help menu ---
    auto* helpMenu = menuBar()->addMenu(tr("&Help"));

    auto* aboutAction = helpMenu->addAction(tr("&About RexPlayer"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
}

// ============================================================================
// Toolbar
// ============================================================================

void MainWindow::setupToolBar() {
    auto* toolbar = addToolBar(tr("Controls"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(24, 24));

    toolbar->addAction(tr("Power"), this, &MainWindow::sendPower);
    toolbar->addAction(tr("Rotate"), this, &MainWindow::rotateDisplay);
    toolbar->addSeparator();
    toolbar->addAction(tr("Vol+"), this, &MainWindow::sendVolumeUp);
    toolbar->addAction(tr("Vol-"), this, &MainWindow::sendVolumeDown);
    toolbar->addSeparator();
    toolbar->addAction(tr("Screenshot"), this, &MainWindow::takeScreenshot);
    toolbar->addSeparator();
    toolbar->addAction(tr("Home"), this, &MainWindow::sendHome);
    toolbar->addAction(tr("Back"), this, &MainWindow::sendBack);
    toolbar->addAction(tr("Recent"), this, &MainWindow::sendRecent);
}

// ============================================================================
// Status bar
// ============================================================================

void MainWindow::setupStatusBar() {
    state_label_ = new QLabel(tr("Stopped"));
    fps_label_ = new QLabel(tr("0 FPS"));
    memory_label_ = new QLabel(tr("0 MB"));

    statusBar()->addWidget(state_label_);
    statusBar()->addPermanentWidget(fps_label_);
    statusBar()->addPermanentWidget(memory_label_);
}

void MainWindow::updateStatusBar() {
    updateVmStateLabel();

    double fps = display_widget_ ? display_widget_->fps() : 0.0;
    fps_label_->setText(tr("%1 FPS").arg(fps, 0, 'f', 1));

    // TODO: read actual memory usage from VM
    if (vm_) {
        auto mb = vm_->memory_manager().total_allocated() / (1024 * 1024);
        memory_label_->setText(QString("%1 MB").arg(mb));
    }
}

void MainWindow::updateVmStateLabel() {
    if (!vm_) {
        state_label_->setText(tr("No VM"));
        return;
    }

    switch (vm_->state()) {
        case rex::vmm::VmState::Created: state_label_->setText(tr("Created")); break;
        case rex::vmm::VmState::Running: state_label_->setText(tr("Running")); break;
        case rex::vmm::VmState::Paused:  state_label_->setText(tr("Paused"));  break;
        case rex::vmm::VmState::Stopped: state_label_->setText(tr("Stopped")); break;
    }
}

// ============================================================================
// VM operations
// ============================================================================

void MainWindow::startVm() {
    if (!vm_) return;

    if (vm_->state() == rex::vmm::VmState::Paused) {
        vm_->resume();
    } else {
        auto result = vm_->start();
        if (!result) {
            QMessageBox::critical(this, tr("Error"),
                tr("Failed to start VM: %1").arg(
                    rex::hal::hal_error_str(result.error())));
        }
    }
    updateVmStateLabel();
}

void MainWindow::stopVm() {
    if (vm_) {
        vm_->stop();
        updateVmStateLabel();
    }
}

void MainWindow::pauseVm() {
    if (vm_) {
        vm_->pause();
        updateVmStateLabel();
    }
}

void MainWindow::resetVm() {
    showUnavailableFeature(tr("VM reset"));
}

// ============================================================================
// File / Tools operations
// ============================================================================

void MainWindow::installApk() {
    showUnavailableFeature(tr("APK installation"));
}

void MainWindow::takeScreenshot() {
    if (!display_widget_) return;

    QString path = QFileDialog::getSaveFileName(
        this, tr("Save Screenshot"), "screenshot.png",
        tr("Images (*.png *.jpg)"));
    if (path.isEmpty()) return;

    QPixmap pixmap = display_widget_->grab();
    pixmap.save(path);
    statusBar()->showMessage(tr("Screenshot saved to %1").arg(path), 3000);
}

void MainWindow::startScreenRecord() {
    showUnavailableFeature(tr("Screen recording"));
}

void MainWindow::openFridaConsole() {
    showUnavailableFeature(tr("Frida console"));
}

void MainWindow::openSettings() {
    SettingsDialog dialog(config_, this);
    connect(&dialog, &SettingsDialog::configApplied,
            this, &MainWindow::onConfigApplied);
    dialog.exec();
}

void MainWindow::onConfigApplied(const RexConfig& config) {
    config_ = config;
    statusBar()->showMessage(tr("Settings applied"), 3000);
}

// ============================================================================
// Display
// ============================================================================

void MainWindow::rotateDisplay() {
    rotation_ = (rotation_ + 90) % 360;
    if (display_widget_) {
        display_widget_->setRotation(rotation_);
    }
    statusBar()->showMessage(
        tr("Rotation: %1°").arg(rotation_), 2000);
}

// ============================================================================
// Android navigation keys
// ============================================================================

void MainWindow::sendHome() {
    if (display_widget_) {
        display_widget_->injectKey(linux_input::KEY_HOME, true);
        display_widget_->injectKey(linux_input::KEY_HOME, false);
    }
}

void MainWindow::sendBack() {
    if (display_widget_) {
        display_widget_->injectKey(linux_input::KEY_BACK, true);
        display_widget_->injectKey(linux_input::KEY_BACK, false);
    }
}

void MainWindow::sendRecent() {
    if (display_widget_) {
        display_widget_->injectKey(linux_input::KEY_APPSELECT, true);
        display_widget_->injectKey(linux_input::KEY_APPSELECT, false);
    }
}

void MainWindow::sendPower() {
    if (display_widget_) {
        display_widget_->injectKey(linux_input::KEY_POWER, true);
        display_widget_->injectKey(linux_input::KEY_POWER, false);
    }
}

void MainWindow::sendVolumeUp() {
    if (display_widget_) {
        display_widget_->injectKey(linux_input::KEY_VOLUMEUP, true);
        display_widget_->injectKey(linux_input::KEY_VOLUMEUP, false);
    }
}

void MainWindow::sendVolumeDown() {
    if (display_widget_) {
        display_widget_->injectKey(linux_input::KEY_VOLUMEDOWN, true);
        display_widget_->injectKey(linux_input::KEY_VOLUMEDOWN, false);
    }
}

void MainWindow::onGuestTouchInput(const TouchContact& /*contact*/) {
    notifyGuestInputUnavailable();
}

void MainWindow::onGuestKeyInput(uint16_t /*linux_keycode*/, bool /*pressed*/) {
    notifyGuestInputUnavailable();
}

void MainWindow::showUnavailableFeature(const QString& feature) {
    statusBar()->showMessage(
        tr("%1 is not available in the current build.").arg(feature),
        5000);
}

void MainWindow::notifyGuestInputUnavailable() {
    if (guest_input_notice_timer_.isValid() && guest_input_notice_timer_.elapsed() < 1500) {
        return;
    }

    guest_input_notice_timer_.start();
    if (!vm_) {
        statusBar()->showMessage(tr("Input requires a running VM."), 3000);
        return;
    }

    showUnavailableFeature(tr("Guest input injection"));
}

// ============================================================================
// About
// ============================================================================

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About RexPlayer"),
        tr("<h3>RexPlayer v%1</h3>"
           "<p>Lightweight high-performance Android app player</p>"
           "<p>Uses native hypervisor APIs (KVM/HVF/WHPX) for maximum performance.</p>"
           "<p>Built-in Frida for security research.</p>")
        .arg(QApplication::applicationVersion()));
}

} // namespace rex::gui
