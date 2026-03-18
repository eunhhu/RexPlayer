#include "mainwindow.h"
#include "display_widget.h"
#include "../qemu/qemu_process.h"
#include "../spice/spice_client.h"
#include "../spice/spice_display.h"
#include "../spice/spice_input.h"

#include <QMenuBar>
#include <QStatusBar>
#include <QMessageBox>
#include <QFileDialog>
#include <QInputDialog>
#include <cstdio>

namespace rex::gui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("RexPlayer");
    resize(480, 860);

    display_ = new DisplayWidget(this);
    setCentralWidget(display_);

    createMenus();
    createSidebar();
    createStatusBar_();

    status_timer_ = new QTimer(this);
    connect(status_timer_, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    status_timer_->start(1000);
}

MainWindow::~MainWindow() = default;

void MainWindow::setQemuProcess(rex::qemu::QemuProcess* qemu) {
    qemu_ = qemu;
    connect(qemu_, &rex::qemu::QemuProcess::stateChanged,
            this, &MainWindow::onVmStateChanged);
    connect(qemu_, &rex::qemu::QemuProcess::error,
            this, [this](const QString& msg) {
        statusBar()->showMessage(msg, 5000);
    });
}

void MainWindow::setSpiceClient(rex::spice::SpiceClient* spice) {
    spice_ = spice;
    display_->setSpiceDisplay(spice->display());
}

void MainWindow::createMenus() {
    auto* file = menuBar()->addMenu("&File");
    file->addAction("&Screenshot", this, &MainWindow::onScreenshot, QKeySequence("Ctrl+S"));
    file->addSeparator();
    file->addAction("&Quit", this, &QWidget::close, QKeySequence::Quit);

    auto* vm = menuBar()->addMenu("&VM");
    vm->addAction("&Pause/Resume", this, &MainWindow::onPause, QKeySequence("Ctrl+P"));
    vm->addAction("&Reset", this, &MainWindow::onReset);
    vm->addAction("Power &Off", this, &MainWindow::onPower);
    vm->addSeparator();
    vm->addAction("Save Snapshot...", this, [this]() {
        if (!qemu_) return;
        QString name = QInputDialog::getText(this, "Save Snapshot", "Name:");
        if (!name.isEmpty()) qemu_->snapshotSave(name);
    });
    vm->addAction("Load Snapshot...", this, [this]() {
        if (!qemu_) return;
        QString name = QInputDialog::getText(this, "Load Snapshot", "Name:");
        if (!name.isEmpty()) qemu_->snapshotLoad(name);
    });

    auto* tools = menuBar()->addMenu("&Tools");
    tools->addAction("&Keymap Editor", this, &MainWindow::onKeymapToggle, QKeySequence("Ctrl+K"));
    tools->addAction("&Frida", this, &MainWindow::onFridaToggle, QKeySequence("Ctrl+F"));
    tools->addSeparator();
    tools->addAction("&Settings", this, &MainWindow::onSettings, QKeySequence("Ctrl+,"));

    auto* help = menuBar()->addMenu("&Help");
    help->addAction("&About", this, [this]() {
        QMessageBox::about(this, "RexPlayer",
            "RexPlayer v0.2.0\nAndroid app player powered by QEMU");
    });
}

void MainWindow::createSidebar() {
    sidebar_ = new QToolBar("Actions", this);
    sidebar_->setOrientation(Qt::Vertical);
    sidebar_->setMovable(false);
    sidebar_->setIconSize(QSize(28, 28));
    sidebar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    sidebar_->setStyleSheet(
        "QToolBar { background: #2b2b2b; border: none; spacing: 4px; padding: 4px; }"
        "QToolButton { background: transparent; border: none; border-radius: 4px; "
        "  padding: 4px; color: #b0b0b0; min-width: 36px; min-height: 36px; font-size: 18px; }"
        "QToolButton:hover { background: #3c3c3c; color: white; }"
        "QToolButton:pressed { background: #4a4a4a; }"
    );
    addToolBar(Qt::RightToolBarArea, sidebar_);

    auto addBtn = [this](const QString& text, const QString& tooltip,
                         void (MainWindow::*slot)(), const QKeySequence& shortcut = {}) -> QAction* {
        auto* action = sidebar_->addAction(text);
        connect(action, &QAction::triggered, this, slot);
        QString tip = tooltip;
        if (!shortcut.isEmpty())
            tip += QString(" (%1)").arg(shortcut.toString());
        action->setToolTip(tip);
        if (!shortcut.isEmpty()) action->setShortcut(shortcut);
        return action;
    };

    power_action_ = addBtn("PWR", "Power", &MainWindow::onPower);
    sidebar_->addSeparator();
    addBtn("V+", "Volume Up", &MainWindow::onVolumeUp, QKeySequence("Ctrl+Up"));
    addBtn("V-", "Volume Down", &MainWindow::onVolumeDown, QKeySequence("Ctrl+Down"));
    sidebar_->addSeparator();
    addBtn("HOM", "Home", &MainWindow::onHome, QKeySequence("Ctrl+H"));
    addBtn("BCK", "Back", &MainWindow::onBack, QKeySequence("Ctrl+B"));
    addBtn("RCT", "Recents", &MainWindow::onRecents, QKeySequence("Ctrl+R"));
    sidebar_->addSeparator();
    addBtn("ROT", "Rotate", &MainWindow::onRotate, QKeySequence("Ctrl+Shift+R"));
    addBtn("CAP", "Screenshot", &MainWindow::onScreenshot);
    addBtn("GPS", "GPS", &MainWindow::onGps);
    sidebar_->addSeparator();
    addBtn("KEY", "Keymap", &MainWindow::onKeymapToggle, QKeySequence("Ctrl+K"));
    addBtn("FRD", "Frida", &MainWindow::onFridaToggle);
    addBtn("SET", "Settings", &MainWindow::onSettings, QKeySequence("Ctrl+,"));
}

void MainWindow::createStatusBar_() {
    status_vm_    = new QLabel("Stopped");
    status_spice_ = new QLabel("SPICE: ---");
    status_fps_   = new QLabel("0 FPS");
    status_cpu_   = new QLabel("");
    status_ram_   = new QLabel("");
    status_adb_   = new QLabel("ADB :5555");

    auto* sb = statusBar();
    sb->addWidget(status_vm_);
    sb->addWidget(status_spice_);
    sb->addWidget(status_fps_);
    sb->addPermanentWidget(status_adb_);
}

void MainWindow::onVmStateChanged() {
    if (!qemu_) return;
    using S = rex::qemu::QemuProcess::State;
    switch (qemu_->state()) {
        case S::Stopped:  status_vm_->setText("Stopped");    break;
        case S::Starting: status_vm_->setText("Starting..."); break;
        case S::Running:  status_vm_->setText("Running");    break;
        case S::Paused:   status_vm_->setText("Paused");     break;
        case S::Error:    status_vm_->setText("Error");      break;
    }
}

void MainWindow::onPower() {
    if (qemu_) qemu_->poweroff();
}

void MainWindow::onPause() {
    if (!qemu_) return;
    if (qemu_->state() == rex::qemu::QemuProcess::State::Running)
        qemu_->pause();
    else if (qemu_->state() == rex::qemu::QemuProcess::State::Paused)
        qemu_->resume();
}

void MainWindow::onReset() {
    if (qemu_) qemu_->reset();
}

void MainWindow::onVolumeUp() {
    if (spice_ && spice_->input()) {
        spice_->input()->sendKeyPress(115);
        spice_->input()->sendKeyRelease(115);
    }
}

void MainWindow::onVolumeDown() {
    if (spice_ && spice_->input()) {
        spice_->input()->sendKeyPress(114);
        spice_->input()->sendKeyRelease(114);
    }
}

void MainWindow::onHome() {
    if (spice_ && spice_->input()) {
        spice_->input()->sendKeyPress(102);
        spice_->input()->sendKeyRelease(102);
    }
}

void MainWindow::onBack() {
    if (spice_ && spice_->input()) {
        spice_->input()->sendKeyPress(158);
        spice_->input()->sendKeyRelease(158);
    }
}

void MainWindow::onRecents() {
    if (spice_ && spice_->input()) {
        spice_->input()->sendKeyPress(580);
        spice_->input()->sendKeyRelease(580);
    }
}

void MainWindow::onRotate() {
    statusBar()->showMessage("Rotate: not yet implemented", 2000);
}

void MainWindow::onScreenshot() {
    if (!spice_ || !spice_->display()) return;
    QImage frame = spice_->display()->currentFrame();
    if (frame.isNull()) {
        statusBar()->showMessage("No display frame available", 2000);
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, "Save Screenshot",
        "screenshot.png", "Images (*.png *.jpg)");
    if (!path.isEmpty()) {
        frame.save(path);
        statusBar()->showMessage("Screenshot saved: " + path, 3000);
    }
}

void MainWindow::onGps() {
    statusBar()->showMessage("GPS: not yet implemented", 2000);
}

void MainWindow::onKeymapToggle() {
    statusBar()->showMessage("Keymap editor: coming soon", 2000);
}

void MainWindow::onFridaToggle() {
    statusBar()->showMessage("Frida panel: coming soon", 2000);
}

void MainWindow::onSettings() {
    statusBar()->showMessage("Settings: coming soon", 2000);
}

void MainWindow::updateStatusBar() {
    if (spice_ && spice_->display()) {
        status_fps_->setText(QString("%1 FPS").arg(spice_->display()->fps(), 0, 'f', 0));
        status_spice_->setText(spice_->isConnected() ? "SPICE OK" : "SPICE ---");
    }
}

} // namespace rex::gui
