#include "mainwindow.h"
#include "display_widget.h"
#include "settings_dialog.h"
#include "../qemu/qemu_process.h"
#include "../vnc/vnc_client.h"
#include "../emu/emulator_process.h"
#include "../emu/grpc_display.h"

#include <QMenuBar>
#include <QStatusBar>
#include <QMessageBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QStyle>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace rex::gui {

static QIcon makeIcon(std::function<void(QPainter&, QRect)> draw) {
    QPixmap pix(48, 48);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(190, 190, 190), 2.5));
    p.setBrush(Qt::NoBrush);
    draw(p, QRect(8, 8, 32, 32));
    p.end();
    pix.setDevicePixelRatio(2.0);
    return QIcon(pix);
}

static QIcon iconPower() {
    return makeIcon([](QPainter& p, QRect r) {
        p.drawArc(r.adjusted(4,6,-4,-2), 210*16, -240*16);
        p.drawLine(r.center().x(), r.top(), r.center().x(), r.center().y());
    });
}
static QIcon iconVolUp() {
    return makeIcon([](QPainter& p, QRect r) {
        p.drawRect(r.x()+4, r.y()+10, 8, 12);
        QPolygon tri;
        tri << QPoint(r.x()+12,r.y()+10) << QPoint(r.x()+22,r.y()+4)
            << QPoint(r.x()+22,r.y()+28) << QPoint(r.x()+12,r.y()+22);
        p.drawPolyline(tri);
        p.drawLine(r.x()+26, r.y()+16, r.x()+30, r.y()+16);
        p.drawLine(r.x()+28, r.y()+14, r.x()+28, r.y()+18);
    });
}
static QIcon iconVolDown() {
    return makeIcon([](QPainter& p, QRect r) {
        p.drawRect(r.x()+4, r.y()+10, 8, 12);
        QPolygon tri;
        tri << QPoint(r.x()+12,r.y()+10) << QPoint(r.x()+22,r.y()+4)
            << QPoint(r.x()+22,r.y()+28) << QPoint(r.x()+12,r.y()+22);
        p.drawPolyline(tri);
        p.drawLine(r.x()+26, r.y()+16, r.x()+30, r.y()+16);
    });
}
static QIcon iconHome() {
    return makeIcon([](QPainter& p, QRect r) {
        QPolygon roof;
        roof << QPoint(r.center().x(), r.top()+2)
             << QPoint(r.right()-2, r.center().y())
             << QPoint(r.left()+2, r.center().y());
        p.drawPolygon(roof);
        p.drawRect(r.x()+8, r.center().y(), 16, 14);
    });
}
static QIcon iconBack() {
    return makeIcon([](QPainter& p, QRect r) {
        p.drawLine(r.left()+6, r.center().y(), r.right()-6, r.center().y());
        p.drawLine(r.left()+6, r.center().y(), r.left()+14, r.top()+8);
        p.drawLine(r.left()+6, r.center().y(), r.left()+14, r.bottom()-8);
    });
}
static QIcon iconRecents() {
    return makeIcon([](QPainter& p, QRect r) {
        p.drawRect(r.x()+4, r.y()+4, 16, 16);
        p.drawRect(r.x()+12, r.y()+12, 16, 16);
    });
}
static QIcon iconRotate() {
    return makeIcon([](QPainter& p, QRect r) {
        p.drawArc(r.adjusted(4,4,-4,-4), 45*16, 270*16);
        p.drawLine(r.right()-6, r.top()+6, r.right()-4, r.top()+14);
        p.drawLine(r.right()-6, r.top()+6, r.right()-14, r.top()+8);
    });
}
static QIcon iconScreenshot() {
    return makeIcon([](QPainter& p, QRect r) {
        p.drawRoundedRect(r.adjusted(2,8,-2,-2), 3, 3);
        p.drawEllipse(r.center(), 6, 6);
        p.drawLine(r.x()+10, r.y()+8, r.x()+10, r.y()+4);
        p.drawLine(r.x()+10, r.y()+4, r.x()+22, r.y()+4);
        p.drawLine(r.x()+22, r.y()+4, r.x()+22, r.y()+8);
    });
}
static QIcon iconGps() {
    return makeIcon([](QPainter& p, QRect r) {
        QPainterPath path;
        path.addEllipse(r.center().x()-8, r.top()+4, 16, 16);
        path.moveTo(r.center().x()-8, r.center().y());
        path.lineTo(r.center().x(), r.bottom()-2);
        path.lineTo(r.center().x()+8, r.center().y());
        p.drawPath(path);
    });
}
static QIcon iconKeymap() {
    return makeIcon([](QPainter& p, QRect r) {
        p.drawRoundedRect(r.adjusted(2,8,-2,-4), 3, 3);
        for (int i = 0; i < 4; i++)
            p.drawRect(r.x()+5+i*7, r.y()+11, 4, 4);
        for (int i = 0; i < 3; i++)
            p.drawRect(r.x()+8+i*7, r.y()+17, 4, 4);
        p.drawRect(r.x()+10, r.y()+23, 12, 3);
    });
}
static QIcon iconFrida() {
    return makeIcon([](QPainter& p, QRect r) {
        p.drawEllipse(r.x()+4, r.y()+4, 18, 18);
        p.drawLine(r.x()+19, r.y()+19, r.right()-4, r.bottom()-4);
        p.drawLine(r.x()+9, r.y()+13, r.x()+12, r.y()+10);
        p.drawLine(r.x()+9, r.y()+13, r.x()+12, r.y()+16);
        p.drawLine(r.x()+18, r.y()+13, r.x()+15, r.y()+10);
        p.drawLine(r.x()+18, r.y()+13, r.x()+15, r.y()+16);
    });
}
static QIcon iconSettings() {
    return makeIcon([](QPainter& p, QRect r) {
        int cx = r.center().x(), cy = r.center().y();
        p.drawEllipse(QPoint(cx, cy), 7, 7);
        for (int i = 0; i < 8; i++) {
            double angle = i * 3.14159 / 4.0;
            int x1 = cx + static_cast<int>(10 * std::cos(angle));
            int y1 = cy + static_cast<int>(10 * std::sin(angle));
            int x2 = cx + static_cast<int>(14 * std::cos(angle));
            int y2 = cy + static_cast<int>(14 * std::sin(angle));
            p.drawLine(x1, y1, x2, y2);
        }
    });
}

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

void MainWindow::setVncClient(rex::vnc::VncClient* vnc) {
    vnc_ = vnc;
    display_->setVncClient(vnc);
}

void MainWindow::setEmulatorProcess(rex::emu::EmulatorProcess* emu) {
    emu_ = emu;
    connect(emu_, &rex::emu::EmulatorProcess::stateChanged,
            this, &MainWindow::onVmStateChanged);
    connect(emu_, &rex::emu::EmulatorProcess::error,
            this, [this](const QString& msg) {
        statusBar()->showMessage(msg, 5000);
    });
}

void MainWindow::setGrpcDisplay(rex::emu::GrpcDisplay* grpc) {
    grpc_ = grpc;
    display_->setGrpcDisplay(grpc);
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
    sidebar_->setIconSize(QSize(22, 22));
    sidebar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    sidebar_->setStyleSheet(
        "QToolBar { background: #2b2b2b; border: none; spacing: 2px; padding: 3px; }"
        "QToolButton { background: transparent; border: none; border-radius: 4px; "
        "  padding: 3px; min-width: 30px; min-height: 30px; }"
        "QToolButton:hover { background: #3c3c3c; }"
        "QToolButton:pressed { background: #505050; }"
    );
    addToolBar(Qt::RightToolBarArea, sidebar_);

    auto addBtn = [this](const QIcon& icon, const QString& tooltip,
                         void (MainWindow::*slot)(), const QKeySequence& shortcut = {}) -> QAction* {
        auto* action = sidebar_->addAction(icon, "");
        connect(action, &QAction::triggered, this, slot);
        QString tip = tooltip;
        if (!shortcut.isEmpty())
            tip += QString(" (%1)").arg(shortcut.toString());
        action->setToolTip(tip);
        if (!shortcut.isEmpty()) action->setShortcut(shortcut);
        return action;
    };

    power_action_ = addBtn(iconPower(), "Power", &MainWindow::onPower);
    sidebar_->addSeparator();
    addBtn(iconVolUp(), "Volume Up", &MainWindow::onVolumeUp, QKeySequence("Ctrl+Up"));
    addBtn(iconVolDown(), "Volume Down", &MainWindow::onVolumeDown, QKeySequence("Ctrl+Down"));
    sidebar_->addSeparator();
    addBtn(iconHome(), "Home", &MainWindow::onHome, QKeySequence("Ctrl+H"));
    addBtn(iconBack(), "Back", &MainWindow::onBack, QKeySequence("Ctrl+B"));
    addBtn(iconRecents(), "Recents", &MainWindow::onRecents, QKeySequence("Ctrl+R"));
    sidebar_->addSeparator();
    addBtn(iconRotate(), "Rotate", &MainWindow::onRotate, QKeySequence("Ctrl+Shift+R"));
    addBtn(iconScreenshot(), "Screenshot", &MainWindow::onScreenshot);
    addBtn(iconGps(), "GPS", &MainWindow::onGps);
    sidebar_->addSeparator();
    addBtn(iconKeymap(), "Keymap Editor", &MainWindow::onKeymapToggle, QKeySequence("Ctrl+K"));
    addBtn(iconFrida(), "Frida", &MainWindow::onFridaToggle, QKeySequence("Ctrl+F"));
    addBtn(iconSettings(), "Settings", &MainWindow::onSettings, QKeySequence("Ctrl+,"));
}

void MainWindow::createStatusBar_() {
    status_vm_ = new QLabel("Stopped");
    status_spice_ = new QLabel("VNC: ---");
    status_fps_ = new QLabel("0 FPS");
    status_cpu_ = new QLabel("");
    status_ram_ = new QLabel("");
    status_adb_ = new QLabel("ADB :5555");

    auto* sb = statusBar();
    sb->addWidget(status_vm_);
    sb->addWidget(status_spice_);
    sb->addWidget(status_fps_);
    sb->addPermanentWidget(status_adb_);
}

void MainWindow::onVmStateChanged() {
    if (emu_) {
        using S = rex::emu::EmulatorProcess::State;
        switch (emu_->state()) {
            case S::Stopped:  status_vm_->setText("Stopped"); break;
            case S::Starting: status_vm_->setText("Booting Android..."); break;
            case S::Running:  status_vm_->setText("Running"); break;
            case S::Error:    status_vm_->setText("Error"); break;
        }
        return;
    }
    if (!qemu_) return;
    using S = rex::qemu::QemuProcess::State;
    switch (qemu_->state()) {
        case S::Stopped:  status_vm_->setText("Stopped"); break;
        case S::Starting: status_vm_->setText("Starting..."); break;
        case S::Running:  status_vm_->setText("Running"); break;
        case S::Paused:   status_vm_->setText("Paused"); break;
        case S::Error:    status_vm_->setText("Error"); break;
    }
}

void MainWindow::onPower() {
    if (qemu_ && qemu_->isRunning()) qemu_->poweroff();
    else statusBar()->showMessage("VM is not running", 2000);
}

void MainWindow::onPause() {
    if (!qemu_) { statusBar()->showMessage("VM is not running", 2000); return; }
    if (qemu_->state() == rex::qemu::QemuProcess::State::Running)
        qemu_->pause();
    else if (qemu_->state() == rex::qemu::QemuProcess::State::Paused)
        qemu_->resume();
    else
        statusBar()->showMessage("VM is not running", 2000);
}

void MainWindow::onReset() {
    if (qemu_ && qemu_->isRunning()) qemu_->reset();
    else statusBar()->showMessage("VM is not running", 2000);
}

void MainWindow::onVolumeUp() {
    if (vnc_ && vnc_->isConnected()) {
        vnc_->sendKeyEvent(true, 0x1008FF13);
        vnc_->sendKeyEvent(false, 0x1008FF13);
    } else statusBar()->showMessage("Not connected to VM", 2000);
}

void MainWindow::onVolumeDown() {
    if (vnc_ && vnc_->isConnected()) {
        vnc_->sendKeyEvent(true, 0x1008FF11);
        vnc_->sendKeyEvent(false, 0x1008FF11);
    } else statusBar()->showMessage("Not connected to VM", 2000);
}

void MainWindow::onHome() {
    if (vnc_ && vnc_->isConnected()) {
        vnc_->sendKeyEvent(true, 0xFF50);
        vnc_->sendKeyEvent(false, 0xFF50);
    } else statusBar()->showMessage("Not connected to VM", 2000);
}

void MainWindow::onBack() {
    if (vnc_ && vnc_->isConnected()) {
        vnc_->sendKeyEvent(true, 0xFF1B);
        vnc_->sendKeyEvent(false, 0xFF1B);
    } else statusBar()->showMessage("Not connected to VM", 2000);
}

void MainWindow::onRecents() {
    if (vnc_ && vnc_->isConnected()) {
        vnc_->sendKeyEvent(true, 0xFF67);
        vnc_->sendKeyEvent(false, 0xFF67);
    } else statusBar()->showMessage("Not connected to VM", 2000);
}

void MainWindow::onRotate() {
    statusBar()->showMessage("Rotate: not yet implemented", 2000);
}

void MainWindow::onScreenshot() {
    if (!vnc_ || !vnc_->isConnected()) {
        statusBar()->showMessage("Not connected to VM", 2000);
        return;
    }
    QImage frame = vnc_->currentFrame();
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
    SettingsDialog dlg(this);
    dlg.exec();
}

void MainWindow::updateStatusBar() {
    if (grpc_) {
        status_fps_->setText(QString("%1 FPS").arg(grpc_->fps(), 0, 'f', 0));
        status_spice_->setText(grpc_->isConnected() ? "gRPC OK" : "gRPC ---");
        return;
    }
    if (vnc_) {
        status_fps_->setText(QString("%1 FPS").arg(vnc_->fps(), 0, 'f', 0));
        status_spice_->setText(vnc_->isConnected() ? "VNC OK" : "VNC ---");
    }
}

} // namespace rex::gui
