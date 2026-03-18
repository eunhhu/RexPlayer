# RexPlayer QEMU Backend Migration — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace RexPlayer's custom VMM with QEMU as a subprocess, using SPICE for low-latency display and QMP for VM control, while upgrading the GUI with game-style keymap editor, modern settings, Frida script editor, and Android Studio-style action sidebar.

**Architecture:** QEMU runs as a child process managed by `QProcess`. Display frames arrive via `libspice-client-glib` over unix socket (Linux/macOS) or TCP (Windows). VM state is controlled through QMP JSON protocol over `QLocalSocket`. The Qt GUI is refactored from direct HAL/VMM calls to signal/slot communication with the QEMU process manager.

**Tech Stack:** C++23, Qt 6 (Widgets/Gui/Core), libspice-client-glib, GLib 2.0, Rust (rex-config, rex-frida, rex-update), CMake 3.25+, GoogleTest

**Spec:** `docs/superpowers/specs/2026-03-18-qemu-backend-migration-design.md`

---

## File Structure

### New Files

| Path | Responsibility |
|------|---------------|
| `src/qemu/qemu_process.h` | QemuProcess class — spawn, monitor, kill QEMU subprocess |
| `src/qemu/qemu_process.cpp` | QemuProcess implementation |
| `src/qemu/qemu_config.h` | QemuConfig — translate RexPlayer config to QEMU CLI args |
| `src/qemu/qemu_config.cpp` | QemuConfig implementation |
| `src/qemu/qmp_client.h` | QmpClient — JSON-RPC over QLocalSocket for VM control |
| `src/qemu/qmp_client.cpp` | QmpClient implementation |
| `src/spice/spice_client.h` | SpiceClient — session management, GLib event loop integration |
| `src/spice/spice_client.cpp` | SpiceClient implementation |
| `src/spice/spice_display.h` | SpiceDisplay — display channel → QImage conversion |
| `src/spice/spice_display.cpp` | SpiceDisplay implementation |
| `src/spice/spice_input.h` | SpiceInput — Qt events → SPICE input channel |
| `src/spice/spice_input.cpp` | SpiceInput implementation |
| `tests/qemu_tests/test_qemu_config.cpp` | Unit tests for QEMU CLI arg generation |
| `tests/qemu_tests/test_qmp_client.cpp` | Unit tests for QMP protocol with mock socket |
| `tests/spice_tests/test_spice_display.cpp` | Unit tests for SPICE display frame handling |

### Modified Files

| Path | Changes |
|------|---------|
| `src/gui/main.cpp` | Remove HAL/VMM init, launch QemuProcess instead |
| `src/gui/mainwindow.h` | Replace VM* with QemuProcess*, add sidebar, status bar |
| `src/gui/mainwindow.cpp` | Rewire signals/slots to QemuProcess/QMP/SPICE |
| `src/gui/display_widget.h` | Remove framebuffer attachment, add SpiceDisplay sink |
| `src/gui/display_widget.cpp` | Render from SPICE frames instead of GPU Display |
| `src/gui/input_handler.h` | Route to SpiceInput instead of direct key injection |
| `src/gui/input_handler.cpp` | SpiceInput integration |
| `src/gui/settings_dialog.h` | Sidebar layout, 7 categories, QEMU-specific settings |
| `src/gui/settings_dialog.cpp` | Full rewrite to sidebar style |
| `src/gui/keymap_editor.h` | Drag-and-drop widget system, overlay mode |
| `src/gui/keymap_editor.cpp` | Full rewrite with game-specific widgets |
| `CMakeLists.txt` | Remove old libs, add rex_qemu/rex_spice, link spice-client-glib |
| `tests/CMakeLists.txt` | Remove old tests, add qemu/spice tests |
| `middleware/Cargo.toml` | Remove unused crate members |
| `middleware/rex-config/src/lib.rs` | Add QEMU-specific config fields |

### Deleted Directories

| Path | Reason |
|------|--------|
| `src/hal/` | QEMU manages hypervisor directly |
| `src/vmm/` | QEMU manages VM lifecycle |
| `src/devices/` | QEMU provides all device emulation |
| `src/gpu/` | SPICE replaces display pipeline |
| `src/platform/` | No longer needed (async I/O, platform abstraction) |
| `middleware/rex-ffi/` | FFI bridge obsolete |
| `middleware/rex-devices/` | QEMU provides all virtio devices |
| `middleware/rex-network/` | QEMU SLIRP handles networking |
| `middleware/rex-filesync/` | Replaced by SPICE shared folder |

---

## Task 1: Delete Old Code & Clean Up

**Files:**
- Delete: `src/hal/`, `src/vmm/`, `src/devices/`, `src/gpu/`, `src/platform/`
- Delete: `middleware/rex-ffi/`, `middleware/rex-devices/`, `middleware/rex-network/`, `middleware/rex-filesync/`
- Modify: `middleware/Cargo.toml`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Delete C++ source directories**

```bash
rm -rf src/hal src/vmm src/devices src/gpu src/platform
```

- [ ] **Step 2: Delete unused Rust crates**

```bash
rm -rf middleware/rex-ffi middleware/rex-devices middleware/rex-network middleware/rex-filesync
```

- [ ] **Step 3: Update middleware/Cargo.toml**

Replace the workspace members list with only the retained crates:

```toml
[workspace]
resolver = "2"
members = [
    "rex-frida",
    "rex-config",
    "rex-update",
]

[workspace.package]
version = "0.1.0"
edition = "2021"
license = "MIT"
authors = ["RexPlayer Team"]

[workspace.dependencies]
# Async runtime
tokio = { version = "1", features = ["full"] }

# Serialization
serde = { version = "1", features = ["derive"] }
toml = "0.8"

# Error handling
thiserror = "2"
anyhow = "1"

# Logging
tracing = "0.1"
tracing-subscriber = "0.3"
```

Remove workspace deps no longer used: `vm-memory`, `vm-virtio`, `virtio-queue`, `virtio-bindings`, `vmm-sys-util`, `prost`, `bytes`, `libc`, `cxx`, `cxx-build`.

- [ ] **Step 4: Strip CMakeLists.txt to skeleton**

Replace `CMakeLists.txt` with a minimal skeleton (just the project header, Qt find, empty targets). The full targets will be added in later tasks as files are created.

```cmake
cmake_minimum_required(VERSION 3.25)
project(RexPlayer
    VERSION 0.2.0
    DESCRIPTION "Lightweight high-performance Android app player"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(REX_ENABLE_TESTS "Build tests" ON)

# --- Find dependencies ---
find_package(Qt6 COMPONENTS Widgets Gui Core Network REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SPICE_CLIENT REQUIRED spice-client-glib-2.0)
pkg_check_modules(GLIB REQUIRED glib-2.0)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# --- Libraries will be added by subsequent tasks ---

# --- Tests ---
if(REX_ENABLE_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(tests)
endif()
```

- [ ] **Step 5: Strip tests/CMakeLists.txt**

Replace with empty placeholder:

```cmake
# Tests will be added as new components are implemented
```

- [ ] **Step 6: Delete old test files**

```bash
rm -rf tests/hal_tests tests/gpu_tests tests/benchmarks
```

- [ ] **Step 7: Verify Rust workspace builds**

Run: `cd middleware && cargo check --workspace`
Expected: compiles with 0 errors (only rex-config, rex-frida, rex-update remain)

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "refactor: remove custom VMM, keep only QEMU-compatible components

Delete HAL, VMM, device emulation, GPU, platform layers.
Delete rex-ffi, rex-devices, rex-network, rex-filesync crates.
Retain rex-config, rex-frida, rex-update for QEMU frontend."
```

---

## Task 2: QMP Client

**Files:**
- Create: `src/qemu/qmp_client.h`
- Create: `src/qemu/qmp_client.cpp`
- Create: `tests/qemu_tests/test_qmp_client.cpp`
- Modify: `CMakeLists.txt` (add rex_qemu library)
- Modify: `tests/CMakeLists.txt` (add qemu tests)

- [ ] **Step 1: Write QMP client header**

Create `src/qemu/qmp_client.h`:

```cpp
#pragma once

#include <QObject>
#include <QLocalSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QString>
#include <QQueue>
#include <functional>
#include <memory>

namespace rex::qemu {

/// Callback for QMP command responses
using QmpCallback = std::function<void(bool success, const QJsonObject& response)>;

/// QMP (QEMU Machine Protocol) client
///
/// Connects to QEMU's QMP socket, negotiates capabilities,
/// and provides async command execution with callbacks.
class QmpClient : public QObject {
    Q_OBJECT

public:
    explicit QmpClient(QObject* parent = nullptr);
    ~QmpClient() override;

    /// Connect to QMP socket at the given path (unix) or host:port (TCP)
    void connectToSocket(const QString& path);
    void connectToHost(const QString& host, quint16 port);

    /// Disconnect from QMP
    void disconnect();

    /// Is the QMP connection established and capabilities negotiated?
    bool isReady() const;

    /// Execute a QMP command asynchronously
    void execute(const QString& command,
                 const QJsonObject& arguments = {},
                 QmpCallback callback = nullptr);

    // Convenience methods for common commands
    void stop(QmpCallback cb = nullptr);       // pause VM
    void cont(QmpCallback cb = nullptr);       // resume VM
    void systemReset(QmpCallback cb = nullptr);
    void systemPowerdown(QmpCallback cb = nullptr);
    void quit(QmpCallback cb = nullptr);
    void queryStatus(QmpCallback cb = nullptr);
    void snapshotSave(const QString& name, QmpCallback cb = nullptr);
    void snapshotLoad(const QString& name, QmpCallback cb = nullptr);

signals:
    /// Emitted when QMP handshake completes successfully
    void ready();

    /// Emitted when connection is lost
    void disconnected();

    /// Emitted on connection or protocol error
    void error(const QString& message);

    /// Emitted for QMP async events (e.g., STOP, RESUME, SHUTDOWN)
    void event(const QString& name, const QJsonObject& data);

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onError(QLocalSocket::LocalSocketError err);

private:
    void processLine(const QByteArray& line);
    void sendRaw(const QJsonObject& obj);
    void processNextCommand();

    QLocalSocket* socket_ = nullptr;
    QByteArray buffer_;
    bool capabilities_sent_ = false;
    bool ready_ = false;

    struct PendingCommand {
        QJsonObject request;
        QmpCallback callback;
    };
    QQueue<PendingCommand> pending_;
    QmpCallback current_callback_;
};

} // namespace rex::qemu
```

- [ ] **Step 2: Write QMP client implementation**

Create `src/qemu/qmp_client.cpp`:

```cpp
#include "qmp_client.h"
#include <QJsonArray>
#include <cstdio>

namespace rex::qemu {

QmpClient::QmpClient(QObject* parent) : QObject(parent) {
    socket_ = new QLocalSocket(this);
    connect(socket_, &QLocalSocket::connected, this, &QmpClient::onConnected);
    connect(socket_, &QLocalSocket::readyRead, this, &QmpClient::onReadyRead);
    connect(socket_, &QLocalSocket::disconnected, this, &QmpClient::onDisconnected);
    connect(socket_, &QLocalSocket::errorOccurred, this, &QmpClient::onError);
}

QmpClient::~QmpClient() {
    disconnect();
}

void QmpClient::connectToSocket(const QString& path) {
    ready_ = false;
    capabilities_sent_ = false;
    buffer_.clear();
    socket_->connectToServer(path);
}

void QmpClient::connectToHost(const QString& host, quint16 port) {
    // For Windows TCP fallback — QLocalSocket doesn't do TCP,
    // so we'd need a QTcpSocket. For now, this is a placeholder.
    // TODO: Implement TCP path for Windows
    Q_UNUSED(host);
    Q_UNUSED(port);
    emit error("TCP QMP not yet implemented");
}

void QmpClient::disconnect() {
    if (socket_->state() != QLocalSocket::UnconnectedState) {
        socket_->disconnectFromServer();
    }
    ready_ = false;
}

bool QmpClient::isReady() const {
    return ready_;
}

void QmpClient::execute(const QString& command,
                         const QJsonObject& arguments,
                         QmpCallback callback) {
    QJsonObject req;
    req["execute"] = command;
    if (!arguments.isEmpty()) {
        req["arguments"] = arguments;
    }

    pending_.enqueue({req, callback});

    if (!current_callback_) {
        processNextCommand();
    }
}

void QmpClient::stop(QmpCallback cb) { execute("stop", {}, cb); }
void QmpClient::cont(QmpCallback cb) { execute("cont", {}, cb); }
void QmpClient::systemReset(QmpCallback cb) { execute("system_reset", {}, cb); }
void QmpClient::systemPowerdown(QmpCallback cb) { execute("system_powerdown", {}, cb); }
void QmpClient::quit(QmpCallback cb) { execute("quit", {}, cb); }

void QmpClient::queryStatus(QmpCallback cb) {
    execute("query-status", {}, cb);
}

void QmpClient::snapshotSave(const QString& name, QmpCallback cb) {
    QJsonObject args;
    args["job-id"] = QString("snap-save-%1").arg(name);
    args["tag"] = name;
    args["vmstate"] = name;
    args["devices"] = QJsonArray();
    execute("snapshot-save", args, cb);
}

void QmpClient::snapshotLoad(const QString& name, QmpCallback cb) {
    QJsonObject args;
    args["job-id"] = QString("snap-load-%1").arg(name);
    args["tag"] = name;
    args["vmstate"] = name;
    args["devices"] = QJsonArray();
    execute("snapshot-load", args, cb);
}

void QmpClient::onConnected() {
    fprintf(stderr, "qmp: connected\n");
}

void QmpClient::onReadyRead() {
    buffer_.append(socket_->readAll());

    // QMP sends one JSON object per line
    while (true) {
        int idx = buffer_.indexOf('\n');
        if (idx < 0) break;

        QByteArray line = buffer_.left(idx).trimmed();
        buffer_.remove(0, idx + 1);

        if (!line.isEmpty()) {
            processLine(line);
        }
    }
}

void QmpClient::onDisconnected() {
    ready_ = false;
    emit disconnected();
}

void QmpClient::onError(QLocalSocket::LocalSocketError err) {
    Q_UNUSED(err);
    emit error(socket_->errorString());
}

void QmpClient::processLine(const QByteArray& line) {
    QJsonParseError parseErr;
    auto doc = QJsonDocument::fromJson(line, &parseErr);
    if (doc.isNull()) {
        emit error(QString("QMP parse error: %1").arg(parseErr.errorString()));
        return;
    }

    QJsonObject obj = doc.object();

    // QMP greeting — send qmp_capabilities
    if (obj.contains("QMP")) {
        if (!capabilities_sent_) {
            QJsonObject caps;
            caps["execute"] = QString("qmp_capabilities");
            sendRaw(caps);
            capabilities_sent_ = true;
        }
        return;
    }

    // Capabilities response — we're ready
    if (obj.contains("return") && !ready_) {
        ready_ = true;
        emit ready();
        processNextCommand();
        return;
    }

    // Async event
    if (obj.contains("event")) {
        emit event(obj["event"].toString(),
                   obj.value("data").toObject());
        return;
    }

    // Command response
    if (current_callback_) {
        bool success = obj.contains("return");
        auto cb = current_callback_;
        current_callback_ = nullptr;
        cb(success, obj);
        processNextCommand();
    }
}

void QmpClient::sendRaw(const QJsonObject& obj) {
    QJsonDocument doc(obj);
    socket_->write(doc.toJson(QJsonDocument::Compact) + "\n");
    socket_->flush();
}

void QmpClient::processNextCommand() {
    if (pending_.isEmpty() || !ready_) return;

    auto cmd = pending_.dequeue();
    current_callback_ = cmd.callback;
    sendRaw(cmd.request);
}

} // namespace rex::qemu
```

- [ ] **Step 3: Write QMP client tests**

Create `tests/qemu_tests/test_qmp_client.cpp`:

```cpp
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTimer>
#include "../../src/qemu/qmp_client.h"

namespace {

class MockQmpServer : public QObject {
    Q_OBJECT
public:
    explicit MockQmpServer(const QString& name, QObject* parent = nullptr)
        : QObject(parent), server_(new QLocalServer(this)) {
        QLocalServer::removeServer(name);
        server_->listen(name);
        connect(server_, &QLocalServer::newConnection, this, [this]() {
            client_ = server_->nextPendingConnection();
            connect(client_, &QLocalSocket::readyRead, this, &MockQmpServer::onReadyRead);
            // Send QMP greeting
            QJsonObject greeting;
            QJsonObject qmp;
            QJsonObject version;
            version["major"] = 8; version["minor"] = 0; version["micro"] = 0;
            qmp["version"] = version;
            greeting["QMP"] = qmp;
            send(greeting);
        });
    }

    void send(const QJsonObject& obj) {
        if (client_) {
            client_->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
            client_->flush();
        }
    }

    QJsonObject lastReceived() const { return last_received_; }
    QList<QJsonObject> allReceived() const { return all_received_; }

signals:
    void commandReceived(const QJsonObject& cmd);

private slots:
    void onReadyRead() {
        buffer_.append(client_->readAll());
        while (true) {
            int idx = buffer_.indexOf('\n');
            if (idx < 0) break;
            QByteArray line = buffer_.left(idx).trimmed();
            buffer_.remove(0, idx + 1);
            if (!line.isEmpty()) {
                auto doc = QJsonDocument::fromJson(line);
                last_received_ = doc.object();
                all_received_.append(last_received_);
                emit commandReceived(last_received_);
            }
        }
    }

private:
    QLocalServer* server_;
    QLocalSocket* client_ = nullptr;
    QByteArray buffer_;
    QJsonObject last_received_;
    QList<QJsonObject> all_received_;
};

class QmpClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        int argc = 0;
        if (!QCoreApplication::instance()) {
            app_ = std::make_unique<QCoreApplication>(argc, nullptr);
        }
        socket_name_ = QString("rex-qmp-test-%1").arg(QCoreApplication::applicationPid());
    }

    void processEvents(int ms = 100) {
        QTimer timer;
        timer.setSingleShot(true);
        timer.start(ms);
        while (timer.isActive()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }

    std::unique_ptr<QCoreApplication> app_;
    QString socket_name_;
};

TEST_F(QmpClientTest, ConnectsAndNegotiatesCapabilities) {
    MockQmpServer server(socket_name_);
    rex::qemu::QmpClient client;

    QSignalSpy readySpy(&client, &rex::qemu::QmpClient::ready);

    // Server auto-responds to capabilities with return
    QObject::connect(&server, &MockQmpServer::commandReceived,
                     [&server](const QJsonObject& cmd) {
        if (cmd["execute"].toString() == "qmp_capabilities") {
            QJsonObject resp;
            resp["return"] = QJsonObject();
            server.send(resp);
        }
    });

    client.connectToSocket(socket_name_);
    processEvents(200);

    EXPECT_TRUE(client.isReady());
    EXPECT_EQ(readySpy.count(), 1);
}

TEST_F(QmpClientTest, ExecuteStopCommand) {
    MockQmpServer server(socket_name_);
    rex::qemu::QmpClient client;

    QObject::connect(&server, &MockQmpServer::commandReceived,
                     [&server](const QJsonObject& cmd) {
        QJsonObject resp;
        resp["return"] = QJsonObject();
        server.send(resp);
    });

    client.connectToSocket(socket_name_);
    processEvents(200);

    bool callback_called = false;
    bool callback_success = false;
    client.stop([&](bool success, const QJsonObject&) {
        callback_called = true;
        callback_success = success;
    });
    processEvents(200);

    EXPECT_TRUE(callback_called);
    EXPECT_TRUE(callback_success);

    // Verify the command sent was "stop"
    auto cmds = server.allReceived();
    bool found_stop = false;
    for (const auto& cmd : cmds) {
        if (cmd["execute"].toString() == "stop") found_stop = true;
    }
    EXPECT_TRUE(found_stop);
}

TEST_F(QmpClientTest, HandlesAsyncEvents) {
    MockQmpServer server(socket_name_);
    rex::qemu::QmpClient client;

    QObject::connect(&server, &MockQmpServer::commandReceived,
                     [&server](const QJsonObject& cmd) {
        QJsonObject resp;
        resp["return"] = QJsonObject();
        server.send(resp);
    });

    client.connectToSocket(socket_name_);
    processEvents(200);

    QSignalSpy eventSpy(&client, &rex::qemu::QmpClient::event);

    // Server sends an async event
    QJsonObject evt;
    evt["event"] = QString("STOP");
    evt["data"] = QJsonObject();
    server.send(evt);
    processEvents(200);

    EXPECT_EQ(eventSpy.count(), 1);
    EXPECT_EQ(eventSpy.at(0).at(0).toString(), "STOP");
}

TEST_F(QmpClientTest, QueuedCommands) {
    MockQmpServer server(socket_name_);
    rex::qemu::QmpClient client;

    QObject::connect(&server, &MockQmpServer::commandReceived,
                     [&server](const QJsonObject& cmd) {
        QJsonObject resp;
        resp["return"] = QJsonObject();
        server.send(resp);
    });

    client.connectToSocket(socket_name_);
    processEvents(200);

    int count = 0;
    client.stop([&](bool, const QJsonObject&) { count++; });
    client.cont([&](bool, const QJsonObject&) { count++; });
    client.queryStatus([&](bool, const QJsonObject&) { count++; });
    processEvents(500);

    EXPECT_EQ(count, 3);
}

} // namespace

#include "test_qmp_client.moc"
```

- [ ] **Step 4: Add rex_qemu to CMakeLists.txt**

Append to `CMakeLists.txt` after the dependency section:

```cmake
# --- QEMU process management ---
add_library(rex_qemu STATIC
    src/qemu/qmp_client.cpp
)
target_include_directories(rex_qemu PUBLIC src/qemu)
target_link_libraries(rex_qemu PUBLIC Qt6::Core Qt6::Network)
```

- [ ] **Step 5: Add QMP tests to tests/CMakeLists.txt**

```cmake
add_executable(test_qmp_client qemu_tests/test_qmp_client.cpp)
target_link_libraries(test_qmp_client PRIVATE
    rex_qemu GTest::gtest_main Qt6::Core Qt6::Network Qt6::Test
)
gtest_discover_tests(test_qmp_client)
```

- [ ] **Step 6: Create test directory**

```bash
mkdir -p tests/qemu_tests tests/spice_tests
```

- [ ] **Step 7: Build and run tests**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --target test_qmp_client && cd build && ctest -R test_qmp_client -V`
Expected: All 4 QMP tests pass

- [ ] **Step 8: Commit**

```bash
git add src/qemu/qmp_client.h src/qemu/qmp_client.cpp tests/qemu_tests/test_qmp_client.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(qemu): add QMP client with async command queue

JSON-RPC over QLocalSocket, capability negotiation,
queued command execution, async event forwarding."
```

---

## Task 3: QEMU Config Generator

**Files:**
- Create: `src/qemu/qemu_config.h`
- Create: `src/qemu/qemu_config.cpp`
- Create: `tests/qemu_tests/test_qemu_config.cpp`
- Modify: `CMakeLists.txt` (add source to rex_qemu)

- [ ] **Step 1: Write config header**

Create `src/qemu/qemu_config.h`:

```cpp
#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>

namespace rex::qemu {

struct QemuConfig {
    // VM hardware
    uint32_t vcpus = 4;
    uint32_t ram_mb = 4096;
    QString machine = "virt,gic-version=3";  // ARM64 default
    QString cpu = "cortex-a76";

    // Boot
    QString kernel_path;
    QString initrd_path;
    QString cmdline;
    QString system_image_path;

    // Display
    uint32_t display_width = 1080;
    uint32_t display_height = 1920;

    // QEMU paths
    QString qemu_binary;  // empty = auto-detect
    QString spice_socket_path;
    QString qmp_socket_path;

    // Network
    uint16_t adb_host_port = 5555;
    uint16_t adb_guest_port = 5555;

    // Platform
    enum class Accelerator { Auto, HVF, KVM, WHPX, TCG };
    Accelerator accelerator = Accelerator::Auto;

    // Frida
    uint16_t frida_host_port = 27042;
    uint16_t frida_guest_port = 27042;

    // Advanced
    QStringList extra_args;

    /// Generate the full QEMU command line
    QStringList toCommandLine() const;

    /// Auto-detect QEMU binary path
    static QString findQemuBinary();

    /// Auto-detect accelerator for current platform
    static Accelerator detectAccelerator();

    /// Generate unique socket paths for this instance
    void generateSocketPaths(const QString& instance_id);
};

} // namespace rex::qemu
```

- [ ] **Step 2: Write config implementation**

Create `src/qemu/qemu_config.cpp`:

```cpp
#include "qemu_config.h"
#include <QDir>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>

namespace rex::qemu {

QStringList QemuConfig::toCommandLine() const {
    QStringList args;

    // Binary
    QString binary = qemu_binary.isEmpty() ? findQemuBinary() : qemu_binary;
    args << binary;

    // Machine
    args << "-machine" << machine;

    // CPU
    args << "-cpu" << cpu;

    // SMP
    args << "-smp" << QString::number(vcpus);

    // RAM
    args << "-m" << QString::number(ram_mb);

    // Accelerator
    Accelerator accel = (accelerator == Accelerator::Auto)
                            ? detectAccelerator() : accelerator;
    switch (accel) {
        case Accelerator::HVF:  args << "-accel" << "hvf"; break;
        case Accelerator::KVM:  args << "-accel" << "kvm"; break;
        case Accelerator::WHPX: args << "-accel" << "whpx"; break;
        case Accelerator::TCG:  args << "-accel" << "tcg"; break;
        default: break;
    }

    // Disk
    if (!system_image_path.isEmpty()) {
        args << "-drive"
             << QString("file=%1,format=raw,if=virtio").arg(system_image_path);
    }

    // Kernel boot
    if (!kernel_path.isEmpty()) {
        args << "-kernel" << kernel_path;
    }
    if (!initrd_path.isEmpty()) {
        args << "-initrd" << initrd_path;
    }
    if (!cmdline.isEmpty()) {
        args << "-append" << cmdline;
    }

    // Display: none (we use SPICE)
    args << "-display" << "none";

    // SPICE
    if (!spice_socket_path.isEmpty()) {
#ifdef Q_OS_WIN
        args << "-spice"
             << QString("port=%1,disable-ticketing=on").arg(5930);
#else
        args << "-spice"
             << QString("unix=on,addr=%1,disable-ticketing=on")
                    .arg(spice_socket_path);
#endif
    }

    // QMP
    if (!qmp_socket_path.isEmpty()) {
#ifdef Q_OS_WIN
        args << "-qmp"
             << QString("tcp:127.0.0.1:4444,server=on,wait=off");
#else
        args << "-qmp"
             << QString("unix:%1,server=on,wait=off").arg(qmp_socket_path);
#endif
    }

    // Devices
    args << "-device" << "virtio-gpu-pci";
    args << "-device" << "virtio-keyboard-pci";
    args << "-device" << "virtio-tablet-pci";  // absolute positioning for touch
    args << "-device" << "intel-hda" << "-device" << "hda-duplex";

    // Network with port forwarding
    QString netdev = QString("user,id=net0,hostfwd=tcp::%1-:%2")
                         .arg(adb_host_port).arg(adb_guest_port);
    if (frida_host_port > 0) {
        netdev += QString(",hostfwd=tcp::%1-:%2")
                      .arg(frida_host_port).arg(frida_guest_port);
    }
    args << "-netdev" << netdev;
    args << "-device" << "virtio-net-pci,netdev=net0";

    // Serial for debug
    args << "-serial" << "mon:stdio";

    // Extra user-supplied args
    args << extra_args;

    return args;
}

QString QemuConfig::findQemuBinary() {
    // Try architecture-specific binary first
#if defined(Q_PROCESSOR_ARM_64)
    QString target = "qemu-system-aarch64";
#else
    QString target = "qemu-system-x86_64";
#endif

    QString path = QStandardPaths::findExecutable(target);
    if (!path.isEmpty()) return path;

    // Homebrew paths (macOS)
    QStringList extra_paths = {
        "/opt/homebrew/bin",
        "/usr/local/bin",
    };
    path = QStandardPaths::findExecutable(target, extra_paths);
    if (!path.isEmpty()) return path;

    return target; // fallback: hope it's in PATH
}

QemuConfig::Accelerator QemuConfig::detectAccelerator() {
#if defined(Q_OS_MACOS)
    return Accelerator::HVF;
#elif defined(Q_OS_LINUX)
    return Accelerator::KVM;
#elif defined(Q_OS_WIN)
    return Accelerator::WHPX;
#else
    return Accelerator::TCG;
#endif
}

void QemuConfig::generateSocketPaths(const QString& instance_id) {
    QString id = instance_id.isEmpty()
                     ? QUuid::createUuid().toString(QUuid::Id128).left(8)
                     : instance_id;
#ifdef Q_OS_WIN
    spice_socket_path = QString("rex-spice-%1").arg(id);
    qmp_socket_path = QString("rex-qmp-%1").arg(id);
#else
    QString tmp = QDir::tempPath();
    spice_socket_path = QString("%1/rex-spice-%2.sock").arg(tmp, id);
    qmp_socket_path = QString("%1/rex-qmp-%2.sock").arg(tmp, id);
#endif
}

} // namespace rex::qemu
```

- [ ] **Step 3: Write config tests**

Create `tests/qemu_tests/test_qemu_config.cpp`:

```cpp
#include <gtest/gtest.h>
#include "../../src/qemu/qemu_config.h"

namespace {

TEST(QemuConfigTest, DefaultCommandLineContainsRequiredArgs) {
    rex::qemu::QemuConfig config;
    config.spice_socket_path = "/tmp/test-spice.sock";
    config.qmp_socket_path = "/tmp/test-qmp.sock";

    auto args = config.toCommandLine();

    EXPECT_TRUE(args.contains("-machine"));
    EXPECT_TRUE(args.contains("-cpu"));
    EXPECT_TRUE(args.contains("-smp"));
    EXPECT_TRUE(args.contains("-m"));
    EXPECT_TRUE(args.contains("-display"));
    EXPECT_TRUE(args.contains("none"));
    EXPECT_TRUE(args.contains("-spice"));
    EXPECT_TRUE(args.contains("-qmp"));
}

TEST(QemuConfigTest, VcpuAndRamInArgs) {
    rex::qemu::QemuConfig config;
    config.vcpus = 8;
    config.ram_mb = 8192;
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";

    auto args = config.toCommandLine();

    int smp_idx = args.indexOf("-smp");
    ASSERT_GE(smp_idx, 0);
    EXPECT_EQ(args.at(smp_idx + 1), "8");

    int m_idx = args.indexOf("-m");
    ASSERT_GE(m_idx, 0);
    EXPECT_EQ(args.at(m_idx + 1), "8192");
}

TEST(QemuConfigTest, KernelBootArgs) {
    rex::qemu::QemuConfig config;
    config.kernel_path = "/path/to/Image";
    config.initrd_path = "/path/to/initrd.img";
    config.cmdline = "console=ttyAMA0";
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";

    auto args = config.toCommandLine();

    EXPECT_TRUE(args.contains("-kernel"));
    EXPECT_TRUE(args.contains("/path/to/Image"));
    EXPECT_TRUE(args.contains("-initrd"));
    EXPECT_TRUE(args.contains("/path/to/initrd.img"));
    EXPECT_TRUE(args.contains("-append"));
    EXPECT_TRUE(args.contains("console=ttyAMA0"));
}

TEST(QemuConfigTest, NoKernelOmitsBootArgs) {
    rex::qemu::QemuConfig config;
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";

    auto args = config.toCommandLine();

    EXPECT_FALSE(args.contains("-kernel"));
    EXPECT_FALSE(args.contains("-initrd"));
    EXPECT_FALSE(args.contains("-append"));
}

TEST(QemuConfigTest, DevicesPresent) {
    rex::qemu::QemuConfig config;
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";

    auto args = config.toCommandLine();

    EXPECT_TRUE(args.contains("virtio-gpu-pci"));
    EXPECT_TRUE(args.contains("virtio-keyboard-pci"));
    EXPECT_TRUE(args.contains("virtio-tablet-pci"));
    EXPECT_TRUE(args.contains("intel-hda"));
    EXPECT_TRUE(args.contains("hda-duplex"));
    EXPECT_TRUE(args.contains("virtio-net-pci,netdev=net0"));
}

TEST(QemuConfigTest, PortForwardingInNetdev) {
    rex::qemu::QemuConfig config;
    config.adb_host_port = 5555;
    config.adb_guest_port = 5555;
    config.frida_host_port = 27042;
    config.frida_guest_port = 27042;
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";

    auto args = config.toCommandLine();

    int idx = args.indexOf("-netdev");
    ASSERT_GE(idx, 0);
    QString netdev = args.at(idx + 1);
    EXPECT_TRUE(netdev.contains("hostfwd=tcp::5555-:5555"));
    EXPECT_TRUE(netdev.contains("hostfwd=tcp::27042-:27042"));
}

TEST(QemuConfigTest, ExtraArgsAppended) {
    rex::qemu::QemuConfig config;
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";
    config.extra_args = {"-monitor", "stdio"};

    auto args = config.toCommandLine();

    EXPECT_TRUE(args.contains("-monitor"));
    EXPECT_TRUE(args.contains("stdio"));
}

TEST(QemuConfigTest, GenerateSocketPathsUnique) {
    rex::qemu::QemuConfig a, b;
    a.generateSocketPaths("");
    b.generateSocketPaths("");

    EXPECT_NE(a.spice_socket_path, b.spice_socket_path);
    EXPECT_NE(a.qmp_socket_path, b.qmp_socket_path);
}

TEST(QemuConfigTest, GenerateSocketPathsWithId) {
    rex::qemu::QemuConfig config;
    config.generateSocketPaths("test123");

    EXPECT_TRUE(config.spice_socket_path.contains("test123"));
    EXPECT_TRUE(config.qmp_socket_path.contains("test123"));
}

TEST(QemuConfigTest, AcceleratorDetection) {
    auto accel = rex::qemu::QemuConfig::detectAccelerator();
#if defined(Q_OS_MACOS)
    EXPECT_EQ(accel, rex::qemu::QemuConfig::Accelerator::HVF);
#elif defined(Q_OS_LINUX)
    EXPECT_EQ(accel, rex::qemu::QemuConfig::Accelerator::KVM);
#elif defined(Q_OS_WIN)
    EXPECT_EQ(accel, rex::qemu::QemuConfig::Accelerator::WHPX);
#endif
}

} // namespace
```

- [ ] **Step 4: Add qemu_config.cpp to rex_qemu in CMakeLists.txt**

Add `src/qemu/qemu_config.cpp` to the `rex_qemu` library sources.

- [ ] **Step 5: Add config test to tests/CMakeLists.txt**

```cmake
add_executable(test_qemu_config qemu_tests/test_qemu_config.cpp)
target_link_libraries(test_qemu_config PRIVATE
    rex_qemu GTest::gtest_main Qt6::Core
)
gtest_discover_tests(test_qemu_config)
```

- [ ] **Step 6: Build and run tests**

Run: `cmake --build build --target test_qemu_config && cd build && ctest -R test_qemu_config -V`
Expected: All 10 config tests pass

- [ ] **Step 7: Commit**

```bash
git add src/qemu/qemu_config.h src/qemu/qemu_config.cpp tests/qemu_tests/test_qemu_config.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(qemu): add QEMU config generator with CLI arg builder

Platform-aware accelerator detection, socket path generation,
port forwarding, device selection, kernel boot args."
```

---

## Task 4: QEMU Process Manager

**Files:**
- Create: `src/qemu/qemu_process.h`
- Create: `src/qemu/qemu_process.cpp`
- Modify: `CMakeLists.txt` (add source to rex_qemu)

- [ ] **Step 1: Write process manager header**

Create `src/qemu/qemu_process.h`:

```cpp
#pragma once

#include "qemu_config.h"
#include "qmp_client.h"
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <memory>

namespace rex::qemu {

class QemuProcess : public QObject {
    Q_OBJECT

public:
    enum class State { Stopped, Starting, Running, Paused, Error };
    Q_ENUM(State)

    explicit QemuProcess(QObject* parent = nullptr);
    ~QemuProcess() override;

    /// Start QEMU with the given config
    void start(const QemuConfig& config);

    /// VM control (all async, emit stateChanged on completion)
    void pause();
    void resume();
    void reset();
    void poweroff();
    void kill();

    /// Snapshots
    void snapshotSave(const QString& name);
    void snapshotLoad(const QString& name);

    /// State
    State state() const { return state_; }
    bool isRunning() const { return state_ == State::Running; }
    QmpClient* qmp() { return qmp_.get(); }
    const QemuConfig& config() const { return config_; }

    /// SPICE connection info
    QString spiceSocketPath() const { return config_.spice_socket_path; }

signals:
    void stateChanged(State newState);
    void started();
    void stopped();
    void error(const QString& message);
    void qmpReady();

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError err);
    void onQmpReady();
    void onQmpEvent(const QString& name, const QJsonObject& data);
    void onHeartbeat();

private:
    void setState(State s);
    void cleanupSockets();

    QemuConfig config_;
    std::unique_ptr<QProcess> process_;
    std::unique_ptr<QmpClient> qmp_;
    QTimer* heartbeat_timer_ = nullptr;
    QTimer* qmp_connect_timer_ = nullptr;
    State state_ = State::Stopped;
    int qmp_connect_retries_ = 0;
    static constexpr int kMaxQmpRetries = 20;
    static constexpr int kQmpRetryIntervalMs = 250;
    static constexpr int kHeartbeatIntervalMs = 5000;
    static constexpr int kKillTimeoutMs = 5000;
};

} // namespace rex::qemu
```

- [ ] **Step 2: Write process manager implementation**

Create `src/qemu/qemu_process.cpp`:

```cpp
#include "qemu_process.h"
#include <QFile>
#include <cstdio>

namespace rex::qemu {

QemuProcess::QemuProcess(QObject* parent) : QObject(parent) {
    process_ = std::make_unique<QProcess>(this);
    qmp_ = std::make_unique<QmpClient>(this);

    connect(process_.get(), &QProcess::started,
            this, &QemuProcess::onProcessStarted);
    connect(process_.get(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &QemuProcess::onProcessFinished);
    connect(process_.get(), &QProcess::errorOccurred,
            this, &QemuProcess::onProcessError);

    connect(qmp_.get(), &QmpClient::ready, this, &QemuProcess::onQmpReady);
    connect(qmp_.get(), &QmpClient::event, this, &QemuProcess::onQmpEvent);
    connect(qmp_.get(), &QmpClient::disconnected, this, [this]() {
        if (state_ == State::Running || state_ == State::Paused) {
            emit error("QMP connection lost");
        }
    });

    heartbeat_timer_ = new QTimer(this);
    connect(heartbeat_timer_, &QTimer::timeout, this, &QemuProcess::onHeartbeat);

    qmp_connect_timer_ = new QTimer(this);
    qmp_connect_timer_->setSingleShot(true);
    connect(qmp_connect_timer_, &QTimer::timeout, this, [this]() {
        if (qmp_connect_retries_ < kMaxQmpRetries && state_ == State::Starting) {
            qmp_connect_retries_++;
            qmp_->connectToSocket(config_.qmp_socket_path);
            qmp_connect_timer_->start(kQmpRetryIntervalMs);
        } else if (state_ == State::Starting) {
            emit error("QMP connection failed after retries");
            kill();
        }
    });
}

QemuProcess::~QemuProcess() {
    kill();
}

void QemuProcess::start(const QemuConfig& config) {
    if (state_ != State::Stopped) {
        emit error("VM already running");
        return;
    }

    config_ = config;
    if (config_.spice_socket_path.isEmpty() || config_.qmp_socket_path.isEmpty()) {
        config_.generateSocketPaths("");
    }

    auto args = config_.toCommandLine();
    if (args.isEmpty()) {
        emit error("Empty QEMU command line");
        return;
    }

    QString binary = args.takeFirst();

    setState(State::Starting);
    fprintf(stderr, "qemu: starting %s\n", binary.toUtf8().constData());

    process_->setProcessChannelMode(QProcess::ForwardedChannels);
    process_->start(binary, args);
}

void QemuProcess::pause() {
    if (state_ != State::Running) return;
    qmp_->stop([this](bool ok, const QJsonObject&) {
        if (ok) setState(State::Paused);
    });
}

void QemuProcess::resume() {
    if (state_ != State::Paused) return;
    qmp_->cont([this](bool ok, const QJsonObject&) {
        if (ok) setState(State::Running);
    });
}

void QemuProcess::reset() {
    qmp_->systemReset();
}

void QemuProcess::poweroff() {
    qmp_->systemPowerdown();
}

void QemuProcess::kill() {
    heartbeat_timer_->stop();
    qmp_connect_timer_->stop();

    if (qmp_->isReady()) {
        qmp_->quit([this](bool, const QJsonObject&) {
            // If QEMU doesn't exit in time, force kill
            QTimer::singleShot(kKillTimeoutMs, this, [this]() {
                if (process_->state() != QProcess::NotRunning) {
                    fprintf(stderr, "qemu: force killing\n");
                    process_->kill();
                }
            });
        });
    } else if (process_->state() != QProcess::NotRunning) {
        process_->kill();
    }

    qmp_->disconnect();
}

void QemuProcess::snapshotSave(const QString& name) {
    qmp_->snapshotSave(name, [this, name](bool ok, const QJsonObject&) {
        if (!ok) emit error(QString("Snapshot save failed: %1").arg(name));
    });
}

void QemuProcess::snapshotLoad(const QString& name) {
    qmp_->snapshotLoad(name, [this, name](bool ok, const QJsonObject&) {
        if (!ok) emit error(QString("Snapshot load failed: %1").arg(name));
    });
}

void QemuProcess::onProcessStarted() {
    fprintf(stderr, "qemu: process started (PID %lld)\n", process_->processId());
    qmp_connect_retries_ = 0;
    qmp_connect_timer_->start(kQmpRetryIntervalMs);
}

void QemuProcess::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    heartbeat_timer_->stop();
    qmp_connect_timer_->stop();
    fprintf(stderr, "qemu: process exited (code=%d, status=%d)\n", exitCode, (int)status);

    cleanupSockets();
    setState(State::Stopped);
    emit stopped();

    if (status == QProcess::CrashExit) {
        emit error(QString("QEMU crashed (exit code %1)").arg(exitCode));
    }
}

void QemuProcess::onProcessError(QProcess::ProcessError err) {
    QString msg;
    switch (err) {
        case QProcess::FailedToStart: msg = "QEMU binary not found or not executable"; break;
        case QProcess::Crashed:       msg = "QEMU crashed"; break;
        case QProcess::Timedout:      msg = "QEMU timed out"; break;
        default:                      msg = "QEMU process error"; break;
    }
    setState(State::Error);
    emit error(msg);
}

void QemuProcess::onQmpReady() {
    qmp_connect_timer_->stop();
    fprintf(stderr, "qemu: QMP ready\n");
    setState(State::Running);
    heartbeat_timer_->start(kHeartbeatIntervalMs);
    emit qmpReady();
    emit started();
}

void QemuProcess::onQmpEvent(const QString& name, const QJsonObject& data) {
    Q_UNUSED(data);
    if (name == "STOP") {
        setState(State::Paused);
    } else if (name == "RESUME") {
        setState(State::Running);
    } else if (name == "SHUTDOWN" || name == "RESET") {
        fprintf(stderr, "qemu: guest %s\n", name.toUtf8().constData());
    }
}

void QemuProcess::onHeartbeat() {
    if (!qmp_->isReady()) return;
    qmp_->queryStatus([this](bool ok, const QJsonObject&) {
        if (!ok && state_ == State::Running) {
            emit error("QMP heartbeat failed — QEMU may be hung");
        }
    });
}

void QemuProcess::setState(State s) {
    if (state_ != s) {
        state_ = s;
        emit stateChanged(s);
    }
}

void QemuProcess::cleanupSockets() {
#ifndef Q_OS_WIN
    QFile::remove(config_.spice_socket_path);
    QFile::remove(config_.qmp_socket_path);
#endif
}

} // namespace rex::qemu
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `src/qemu/qemu_process.cpp` to `rex_qemu` library sources.

- [ ] **Step 4: Build**

Run: `cmake --build build`
Expected: compiles with 0 errors

- [ ] **Step 5: Commit**

```bash
git add src/qemu/qemu_process.h src/qemu/qemu_process.cpp CMakeLists.txt
git commit -m "feat(qemu): add QEMU process manager with lifecycle control

QProcess wrapper with QMP auto-connect retry, heartbeat monitoring,
error recovery, socket cleanup, and VM state machine."
```

---

## Task 5: SPICE Client (Display + Input)

**Files:**
- Create: `src/spice/spice_client.h`
- Create: `src/spice/spice_client.cpp`
- Create: `src/spice/spice_display.h`
- Create: `src/spice/spice_display.cpp`
- Create: `src/spice/spice_input.h`
- Create: `src/spice/spice_input.cpp`
- Modify: `CMakeLists.txt` (add rex_spice library)

- [ ] **Step 1: Write SPICE client header (session + GLib integration)**

Create `src/spice/spice_client.h`:

```cpp
#pragma once

#include <QObject>
#include <QTimer>
#include <QImage>
#include <glib.h>
#include <spice-client.h>

namespace rex::spice {

class SpiceDisplay;
class SpiceInput;

/// SPICE session manager with GLib event loop integration
class SpiceClient : public QObject {
    Q_OBJECT

public:
    explicit SpiceClient(QObject* parent = nullptr);
    ~SpiceClient() override;

    /// Connect to SPICE unix socket or TCP
    void connectToSocket(const QString& path);
    void connectToHost(const QString& host, int port);

    /// Disconnect
    void disconnect();

    bool isConnected() const { return connected_; }

    SpiceDisplay* display() { return display_; }
    SpiceInput* input() { return input_; }

signals:
    void connected();
    void disconnected();
    void error(const QString& message);

private:
    void setupGlibIntegration();
    static void onChannelNew(SpiceSession* session, SpiceChannel* channel, gpointer user_data);
    static void onChannelDestroy(SpiceSession* session, SpiceChannel* channel, gpointer user_data);

    SpiceSession* session_ = nullptr;
    GMainContext* glib_ctx_ = nullptr;
    QTimer* glib_timer_ = nullptr;
    SpiceDisplay* display_ = nullptr;
    SpiceInput* input_ = nullptr;
    bool connected_ = false;
};

} // namespace rex::spice
```

- [ ] **Step 2: Write SPICE display header**

Create `src/spice/spice_display.h`:

```cpp
#pragma once

#include <QObject>
#include <QImage>
#include <QMutex>
#include <spice-client.h>

namespace rex::spice {

/// Receives framebuffer data from SPICE display channel
class SpiceDisplay : public QObject {
    Q_OBJECT

public:
    explicit SpiceDisplay(QObject* parent = nullptr);
    ~SpiceDisplay() override;

    void attachChannel(SpiceDisplayChannel* channel);
    void detachChannel();

    /// Get current frame (thread-safe copy)
    QImage currentFrame() const;

    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return fps_; }

signals:
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

    // FPS tracking
    int frame_count_ = 0;
    double fps_ = 0.0;
    qint64 last_fps_time_ = 0;
};

} // namespace rex::spice
```

- [ ] **Step 3: Write SPICE input header**

Create `src/spice/spice_input.h`:

```cpp
#pragma once

#include <QObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <spice-client.h>

namespace rex::spice {

/// Sends keyboard and mouse input to SPICE inputs channel
class SpiceInput : public QObject {
    Q_OBJECT

public:
    explicit SpiceInput(QObject* parent = nullptr);
    ~SpiceInput() override;

    void attachChannel(SpiceInputsChannel* channel);
    void detachChannel();

    /// Send input events
    void sendKeyPress(int scancode);
    void sendKeyRelease(int scancode);
    void sendMouseMove(int x, int y);
    void sendMousePress(int button);
    void sendMouseRelease(int button);

    /// Convert Qt key to SPICE/X11 scancode
    static int qtKeyToScancode(int qtKey);

private:
    SpiceInputsChannel* channel_ = nullptr;
};

} // namespace rex::spice
```

- [ ] **Step 4: Write SPICE client implementation**

Create `src/spice/spice_client.cpp`:

```cpp
#include "spice_client.h"
#include "spice_display.h"
#include "spice_input.h"
#include <cstdio>

namespace rex::spice {

SpiceClient::SpiceClient(QObject* parent) : QObject(parent) {
    display_ = new SpiceDisplay(this);
    input_ = new SpiceInput(this);
    setupGlibIntegration();
}

SpiceClient::~SpiceClient() {
    disconnect();
    if (glib_ctx_) {
        g_main_context_unref(glib_ctx_);
    }
}

void SpiceClient::setupGlibIntegration() {
    glib_ctx_ = g_main_context_new();

    glib_timer_ = new QTimer(this);
    glib_timer_->setInterval(16); // ~60fps
    connect(glib_timer_, &QTimer::timeout, this, [this]() {
        while (g_main_context_iteration(glib_ctx_, FALSE)) {}
    });
    glib_timer_->start();
}

void SpiceClient::connectToSocket(const QString& path) {
    if (session_) disconnect();

    session_ = spice_session_new();
    g_object_set(session_,
                 "unix-path", path.toUtf8().constData(),
                 NULL);

    g_signal_connect(session_, "channel-new",
                     G_CALLBACK(onChannelNew), this);
    g_signal_connect(session_, "channel-destroy",
                     G_CALLBACK(onChannelDestroy), this);

    if (!spice_session_connect(session_)) {
        emit error("Failed to connect to SPICE socket");
        g_object_unref(session_);
        session_ = nullptr;
        return;
    }

    connected_ = true;
    emit connected();
    fprintf(stderr, "spice: connecting to %s\n", path.toUtf8().constData());
}

void SpiceClient::connectToHost(const QString& host, int port) {
    if (session_) disconnect();

    session_ = spice_session_new();
    g_object_set(session_,
                 "host", host.toUtf8().constData(),
                 "port", QString::number(port).toUtf8().constData(),
                 NULL);

    g_signal_connect(session_, "channel-new",
                     G_CALLBACK(onChannelNew), this);
    g_signal_connect(session_, "channel-destroy",
                     G_CALLBACK(onChannelDestroy), this);

    if (!spice_session_connect(session_)) {
        emit error("Failed to connect to SPICE host");
        g_object_unref(session_);
        session_ = nullptr;
        return;
    }

    connected_ = true;
    emit connected();
}

void SpiceClient::disconnect() {
    if (session_) {
        spice_session_disconnect(session_);
        g_object_unref(session_);
        session_ = nullptr;
    }
    connected_ = false;
    emit disconnected();
}

void SpiceClient::onChannelNew(SpiceSession*, SpiceChannel* channel, gpointer user_data) {
    auto* self = static_cast<SpiceClient*>(user_data);

    int type;
    g_object_get(channel, "channel-type", &type, NULL);

    if (type == SPICE_CHANNEL_DISPLAY) {
        fprintf(stderr, "spice: display channel connected\n");
        self->display_->attachChannel(SPICE_DISPLAY_CHANNEL(channel));
    } else if (type == SPICE_CHANNEL_INPUTS) {
        fprintf(stderr, "spice: inputs channel connected\n");
        self->input_->attachChannel(SPICE_INPUTS_CHANNEL(channel));
    }

    spice_channel_connect(channel);
}

void SpiceClient::onChannelDestroy(SpiceSession*, SpiceChannel* channel, gpointer user_data) {
    auto* self = static_cast<SpiceClient*>(user_data);

    int type;
    g_object_get(channel, "channel-type", &type, NULL);

    if (type == SPICE_CHANNEL_DISPLAY) {
        self->display_->detachChannel();
    } else if (type == SPICE_CHANNEL_INPUTS) {
        self->input_->detachChannel();
    }
}

} // namespace rex::spice
```

- [ ] **Step 5: Write SPICE display implementation**

Create `src/spice/spice_display.cpp`:

```cpp
#include "spice_display.h"
#include <QDateTime>
#include <cstdio>
#include <cstring>

namespace rex::spice {

SpiceDisplay::SpiceDisplay(QObject* parent) : QObject(parent) {}

SpiceDisplay::~SpiceDisplay() {
    detachChannel();
}

void SpiceDisplay::attachChannel(SpiceDisplayChannel* channel) {
    channel_ = channel;

    g_signal_connect(channel, "display-primary-create",
                     G_CALLBACK(onDisplayPrimaryCreate), this);
    g_signal_connect(channel, "display-primary-destroy",
                     G_CALLBACK(onDisplayPrimaryDestroy), this);
    g_signal_connect(channel, "display-invalidate",
                     G_CALLBACK(onDisplayInvalidate), this);
}

void SpiceDisplay::detachChannel() {
    if (channel_) {
        g_signal_handlers_disconnect_by_data(channel_, this);
        channel_ = nullptr;
    }
    primary_data_ = nullptr;
}

QImage SpiceDisplay::currentFrame() const {
    QMutexLocker lock(&mutex_);
    return frame_.copy();
}

void SpiceDisplay::onDisplayPrimaryCreate(SpiceChannel*,
    gint format, gint width, gint height, gint stride,
    gint, gpointer imgdata, gpointer user_data) {
    auto* self = static_cast<SpiceDisplay*>(user_data);
    Q_UNUSED(format);

    QMutexLocker lock(&self->mutex_);
    self->primary_data_ = static_cast<const uint8_t*>(imgdata);
    self->width_ = width;
    self->height_ = height;
    self->stride_ = stride;
    self->frame_ = QImage(self->primary_data_, width, height, stride,
                           QImage::Format_RGB32);

    fprintf(stderr, "spice: display created %dx%d\n", width, height);
    emit self->resolutionChanged(width, height);
}

void SpiceDisplay::onDisplayPrimaryDestroy(SpiceChannel*, gpointer user_data) {
    auto* self = static_cast<SpiceDisplay*>(user_data);
    QMutexLocker lock(&self->mutex_);
    self->primary_data_ = nullptr;
    self->frame_ = QImage();
}

void SpiceDisplay::onDisplayInvalidate(SpiceChannel*,
    gint, gint, gint, gint, gpointer user_data) {
    auto* self = static_cast<SpiceDisplay*>(user_data);

    // Update FPS counter
    self->frame_count_++;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (self->last_fps_time_ == 0) {
        self->last_fps_time_ = now;
    } else if (now - self->last_fps_time_ >= 1000) {
        self->fps_ = self->frame_count_ * 1000.0 / (now - self->last_fps_time_);
        self->frame_count_ = 0;
        self->last_fps_time_ = now;
    }

    // Notify Qt to repaint
    {
        QMutexLocker lock(&self->mutex_);
        if (self->primary_data_) {
            self->frame_ = QImage(self->primary_data_,
                                   self->width_, self->height_,
                                   self->stride_, QImage::Format_RGB32);
        }
    }
    emit self->frameReady();
}

} // namespace rex::spice
```

- [ ] **Step 6: Write SPICE input implementation**

Create `src/spice/spice_input.cpp`:

```cpp
#include "spice_input.h"
#include <cstdio>

namespace rex::spice {

SpiceInput::SpiceInput(QObject* parent) : QObject(parent) {}

SpiceInput::~SpiceInput() {
    detachChannel();
}

void SpiceInput::attachChannel(SpiceInputsChannel* channel) {
    channel_ = channel;
    fprintf(stderr, "spice: input channel attached\n");
}

void SpiceInput::detachChannel() {
    channel_ = nullptr;
}

void SpiceInput::sendKeyPress(int scancode) {
    if (!channel_) return;
    spice_inputs_key_press(channel_, scancode);
}

void SpiceInput::sendKeyRelease(int scancode) {
    if (!channel_) return;
    spice_inputs_key_release(channel_, scancode);
}

void SpiceInput::sendMouseMove(int x, int y) {
    if (!channel_) return;
    // Position uses absolute coordinates for tablet device
    spice_inputs_position(channel_, x, y, 0 /* display_id */,
                          SPICE_MOUSE_BUTTON_MASK_LEFT);
}

void SpiceInput::sendMousePress(int button) {
    if (!channel_) return;
    spice_inputs_button_press(channel_, button,
                               SPICE_MOUSE_BUTTON_MASK_LEFT);
}

void SpiceInput::sendMouseRelease(int button) {
    if (!channel_) return;
    spice_inputs_button_release(channel_, button, 0);
}

int SpiceInput::qtKeyToScancode(int qtKey) {
    // Common Qt key → X11 scancode mapping
    // This covers the essential keys; extend as needed
    switch (qtKey) {
        case Qt::Key_Escape:    return 1;
        case Qt::Key_1:         return 2;
        case Qt::Key_2:         return 3;
        case Qt::Key_3:         return 4;
        case Qt::Key_4:         return 5;
        case Qt::Key_5:         return 6;
        case Qt::Key_6:         return 7;
        case Qt::Key_7:         return 8;
        case Qt::Key_8:         return 9;
        case Qt::Key_9:         return 10;
        case Qt::Key_0:         return 11;
        case Qt::Key_Q:         return 16;
        case Qt::Key_W:         return 17;
        case Qt::Key_E:         return 18;
        case Qt::Key_R:         return 19;
        case Qt::Key_T:         return 20;
        case Qt::Key_Y:         return 21;
        case Qt::Key_A:         return 30;
        case Qt::Key_S:         return 31;
        case Qt::Key_D:         return 32;
        case Qt::Key_F:         return 33;
        case Qt::Key_Space:     return 57;
        case Qt::Key_Return:    return 28;
        case Qt::Key_Backspace: return 14;
        case Qt::Key_Tab:       return 15;
        case Qt::Key_Up:        return 103;
        case Qt::Key_Down:      return 108;
        case Qt::Key_Left:      return 105;
        case Qt::Key_Right:     return 106;
        case Qt::Key_Home:      return 102;
        case Qt::Key_End:       return 107;
        default:                return 0;
    }
}

} // namespace rex::spice
```

- [ ] **Step 7: Add rex_spice to CMakeLists.txt**

```cmake
# --- SPICE client ---
add_library(rex_spice STATIC
    src/spice/spice_client.cpp
    src/spice/spice_display.cpp
    src/spice/spice_input.cpp
)
target_include_directories(rex_spice PUBLIC src/spice)
target_include_directories(rex_spice PRIVATE ${SPICE_CLIENT_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS})
target_link_libraries(rex_spice PUBLIC Qt6::Core Qt6::Gui ${SPICE_CLIENT_LIBRARIES} ${GLIB_LIBRARIES})
```

- [ ] **Step 8: Install SPICE dependencies (prerequisite)**

```bash
# macOS
brew install spice-gtk glib

# Linux (Debian/Ubuntu)
sudo apt install libspice-client-glib-2.0-dev libglib2.0-dev
```

- [ ] **Step 9: Build**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build`
Expected: compiles and links against spice-client-glib

- [ ] **Step 10: Commit**

```bash
git add src/spice/
git commit -m "feat(spice): add SPICE client with display and input channels

GLib event loop integration via QTimer polling, framebuffer → QImage
pipeline, keyboard/mouse input forwarding, FPS tracking."
```

---

## Task 6: GUI Refactoring — MainWindow + DisplayWidget

**Files:**
- Modify: `src/gui/main.cpp`
- Modify: `src/gui/mainwindow.h`
- Modify: `src/gui/mainwindow.cpp`
- Modify: `src/gui/display_widget.h`
- Modify: `src/gui/display_widget.cpp`
- Modify: `src/gui/input_handler.h`
- Modify: `src/gui/input_handler.cpp`
- Modify: `CMakeLists.txt`

This is the largest task. The GUI files are fully rewritten to use QemuProcess + SpiceClient instead of direct VMM.

- [ ] **Step 1: Rewrite main.cpp**

Replace `src/gui/main.cpp` — remove all HAL/VMM/embedded kernel code. Launch QemuProcess from CLI args.

Key changes:
- Remove includes: `rex/vmm/vm.h`, `embedded_kernel.h`, `rex/hal/*`
- Add includes: `../qemu/qemu_process.h`, `../qemu/qemu_config.h`, `../spice/spice_client.h`
- `main()`: parse CLI → build `QemuConfig` → pass to `MainWindow`
- CLI options: `--kernel`, `--system-image`, `--cpus`, `--ram`, `--qemu-binary`, `--initrd`

- [ ] **Step 2: Rewrite mainwindow.h**

Replace VM* members with QemuProcess*, SpiceClient*. Add right sidebar QToolBar, status bar labels, action buttons per spec.

Key changes:
- Members: `QemuProcess* qemu_`, `SpiceClient* spice_`
- Right sidebar: `QToolBar* sidebar_` with action buttons
- Status bar: QLabels for VM state, SPICE, FPS, CPU, RAM
- Slots: rewired to QemuProcess signals

- [ ] **Step 3: Rewrite mainwindow.cpp**

Full implementation of new MainWindow:
- `createSidebar()` — build right toolbar with icon buttons, separators, tooltips with shortcuts
- `createStatusBar()` — VM state indicator (colored dot), SPICE, FPS, CPU, RAM, ADB
- `createMenus()` — File, VM, Tools, Help menus
- Signal connections: QemuProcess::stateChanged → update sidebar/status
- Button handlers: power → `qemu_->poweroff()`, pause → `qemu_->pause()`, etc.
- Screenshot: grab frame from `spice_->display()->currentFrame()`

- [ ] **Step 4: Rewrite display_widget.h/.cpp**

Remove framebuffer attachment, replace with SpiceDisplay sink:
- `setSpiceDisplay(SpiceDisplay*)` instead of `attachFramebuffer`
- `paintEvent`: draw `spice_display_->currentFrame()` with letterboxing
- Connect `SpiceDisplay::frameReady` → `update()`

- [ ] **Step 5: Rewrite input_handler.h/.cpp**

Route to SpiceInput instead of direct key injection:
- `setSpiceInput(SpiceInput*)`
- Key events → `SpiceInput::qtKeyToScancode()` → `sendKeyPress/Release`
- Mouse events → coordinate transform → `sendMouseMove/Press/Release`

- [ ] **Step 6: Update CMakeLists.txt — add GUI executable**

```cmake
add_executable(rexplayer
    src/gui/main.cpp
    src/gui/mainwindow.cpp
    src/gui/display_widget.cpp
    src/gui/input_handler.cpp
    src/gui/settings_dialog.cpp
    src/gui/keymap_editor.cpp
)
target_link_libraries(rexplayer PRIVATE
    rex_qemu rex_spice
    Qt6::Widgets Qt6::Gui Qt6::Core
)
target_include_directories(rexplayer PRIVATE src/gui)
```

- [ ] **Step 7: Build**

Run: `cmake --build build --target rexplayer`
Expected: compiles

- [ ] **Step 8: Commit**

```bash
git add src/gui/ CMakeLists.txt
git commit -m "feat(gui): rewire GUI to QEMU/SPICE backend

Replace direct VMM control with QemuProcess signals/slots.
Add right sidebar action buttons, status bar with VM state.
DisplayWidget renders SPICE frames, InputHandler routes to SPICE."
```

---

## Task 7: Settings Dialog — Modern Sidebar Style

**Files:**
- Modify: `src/gui/settings_dialog.h`
- Modify: `src/gui/settings_dialog.cpp`

- [ ] **Step 1: Rewrite settings_dialog.h**

Replace tab-based with sidebar navigation:
- `QListWidget* sidebar_` — category list
- `QStackedWidget* pages_` — settings pages
- 7 categories: General, Display, Performance, Network, Input, Frida, Advanced
- `RexConfig` struct expanded with QEMU fields

- [ ] **Step 2: Rewrite settings_dialog.cpp**

Full implementation with:
- Sidebar: 180px fixed, accent highlight on selected
- Search bar at top filtering across all categories
- Each page: QFormLayout with appropriate widgets
- General: theme (Dark/Light/Auto), QEMU path with file dialog, auto-start, update check
- Display: resolution presets (Phone 1080x1920 / Tablet 1600x2560 / Custom), DPI, FPS limit
- Performance: vCPU slider 1-16, RAM dropdown, accelerator combo, disk cache mode
- Network: port forwarding table (QTableWidget), DNS, proxy
- Input: keymap profile selector, mouse sensitivity slider, gamepad toggle
- Frida: server path, auto-start checkbox, port, script directory
- Advanced: extra CLI args textarea, log level, snapshot path
- Badge on settings that require VM restart

- [ ] **Step 3: Build and verify**

Run: `cmake --build build --target rexplayer`
Expected: compiles, settings dialog opens with sidebar layout

- [ ] **Step 4: Commit**

```bash
git add src/gui/settings_dialog.h src/gui/settings_dialog.cpp
git commit -m "feat(gui): modern sidebar settings with 7 categories

macOS-style sidebar navigation, search filter, QEMU config,
display presets, network port forwarding, Frida settings."
```

---

## Task 8: Keymap Editor — Game-Style Drag & Drop

**Files:**
- Modify: `src/gui/keymap_editor.h`
- Modify: `src/gui/keymap_editor.cpp`

- [ ] **Step 1: Rewrite keymap_editor.h**

Design:
- `KeymapWidget` base class with subclasses: `KeyWidget`, `DPadWidget`, `JoystickWidget`, `TapZoneWidget`, `SwipeWidget`, `AimWidget`, `ShootWidget`
- `KeymapOverlay` — transparent overlay widget on top of display
- `KeymapToolbox` — bottom panel with draggable widget templates
- `KeymapProfile` — profile save/load with app package name matching
- Edit mode toggle: show/hide overlay

- [ ] **Step 2: Rewrite keymap_editor.cpp**

Implementation:
- Widget drag & drop: QDrag from toolbox → drop on overlay → create widget at position
- Widget selection: click to select → show property popover (binding key, size, opacity)
- Widget deletion: drag off overlay area
- Live preview: key press highlights corresponding widget
- Profile management: JSON serialization, per-app profiles
- Built-in profiles: Default, FPS, MOBA (migrated from existing)

- [ ] **Step 3: Build and verify**

Run: `cmake --build build --target rexplayer`
Expected: compiles

- [ ] **Step 4: Commit**

```bash
git add src/gui/keymap_editor.h src/gui/keymap_editor.cpp
git commit -m "feat(gui): game-style drag-and-drop keymap editor

7 widget types (key, d-pad, joystick, tap zone, swipe, aim, shoot),
overlay editing on display, per-app profiles, JSON import/export."
```

---

## Task 9: Frida Panel — Script Editor + Console

**Files:**
- Create: `src/gui/frida_panel.h`
- Create: `src/gui/frida_panel.cpp`
- Modify: `src/gui/mainwindow.h` (add frida panel toggle)
- Modify: `src/gui/mainwindow.cpp` (wire frida panel)
- Modify: `CMakeLists.txt` (add frida_panel to rexplayer)

- [ ] **Step 1: Write frida_panel.h**

```cpp
#pragma once

#include <QWidget>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QComboBox>
#include <QPushButton>
#include <QSplitter>

namespace rex::gui {

/// Frida script editor and console panel
class FridaPanel : public QWidget {
    Q_OBJECT

public:
    explicit FridaPanel(QWidget* parent = nullptr);

    void setAdbPort(int port);

signals:
    void scriptStarted(const QString& path);
    void scriptStopped();

private slots:
    void onStart();
    void onStop();
    void onReload();
    void onNewScript();
    void onScriptSelected(QListWidgetItem* item);
    void refreshProcessList();

private:
    void setupUi();
    void loadScriptList();
    void appendConsole(const QString& text, const QString& color = "#b0b0b0");

    // Left panel
    QListWidget* script_list_ = nullptr;
    QPushButton* new_script_btn_ = nullptr;

    // Editor
    QPlainTextEdit* editor_ = nullptr;

    // Console
    QPlainTextEdit* console_ = nullptr;

    // Toolbar
    QComboBox* process_combo_ = nullptr;
    QPushButton* start_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    QPushButton* reload_btn_ = nullptr;

    QString current_script_path_;
    int adb_port_ = 5555;
};

} // namespace rex::gui
```

- [ ] **Step 2: Write frida_panel.cpp**

Implementation with:
- Layout: top toolbar (process combo + start/stop/reload), horizontal splitter (script list | editor), bottom console
- Script list: scan `~/.rexplayer/scripts/` for .js files
- Editor: QPlainTextEdit with monospace font, basic JS syntax highlighting via QSyntaxHighlighter
- Console: read-only, timestamp + color-coded output
- New script: template selection dialog (Hook Method, Trace Class, Dump Memory)
- Process list: shell out to `adb -s localhost:<port> shell ps` and parse

- [ ] **Step 3: Wire into MainWindow**

Add FridaPanel as a bottom dock widget in MainWindow, toggled by sidebar Frida button.

- [ ] **Step 4: Build**

Run: `cmake --build build --target rexplayer`
Expected: compiles

- [ ] **Step 5: Commit**

```bash
git add src/gui/frida_panel.h src/gui/frida_panel.cpp src/gui/mainwindow.h src/gui/mainwindow.cpp CMakeLists.txt
git commit -m "feat(gui): add Frida script editor panel with console

JS editor with syntax highlighting, process selector,
script templates, color-coded console output."
```

---

## Task 10: Update rex-config for QEMU Settings

**Files:**
- Modify: `middleware/rex-config/src/lib.rs`

- [ ] **Step 1: Update config struct**

Add QEMU-specific fields to `RexConfig`:

```rust
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct QemuConfig {
    #[serde(default = "default_qemu_binary")]
    pub binary_path: String,
    #[serde(default = "default_machine")]
    pub machine: String,
    #[serde(default = "default_cpu")]
    pub cpu: String,
    #[serde(default = "default_accelerator")]
    pub accelerator: String, // "auto", "hvf", "kvm", "whpx", "tcg"
    #[serde(default)]
    pub extra_args: Vec<String>,
}
```

Add `QemuConfig` to the top-level `RexConfig` struct.

- [ ] **Step 2: Add defaults and tests**

Default values: binary_path = "", machine = "virt,gic-version=3", cpu = "cortex-a76", accelerator = "auto".

Add tests for serialization/deserialization of new fields.

- [ ] **Step 3: Verify**

Run: `cd middleware && cargo test -p rex-config`
Expected: all tests pass

- [ ] **Step 4: Commit**

```bash
git add middleware/rex-config/
git commit -m "feat(config): add QEMU-specific config fields

binary path, machine type, CPU model, accelerator selection,
extra CLI args."
```

---

## Task 11: Update README & Documentation

**Files:**
- Modify: `README.md`
- Modify: `docs/ARCHITECTURE.md`

- [ ] **Step 1: Rewrite README.md**

Update to reflect QEMU-based architecture:
- New architecture diagram (QEMU subprocess model)
- Updated build prerequisites (QEMU, spice-gtk)
- Updated usage (--qemu-binary, --system-image now works)
- Remove references to custom VMM, HAL, device emulation
- Document new UI features (keymap editor, Frida panel, settings)

- [ ] **Step 2: Update ARCHITECTURE.md**

Update architecture diagram and component descriptions.

- [ ] **Step 3: Commit**

```bash
git add README.md docs/ARCHITECTURE.md
git commit -m "docs: update README and architecture for QEMU backend

Reflect migration from custom VMM to QEMU subprocess model.
Document SPICE display, QMP control, new UI features."
```

---

## Task 12: Integration Test

**Files:**
- No new files — manual verification

- [ ] **Step 1: Install QEMU**

```bash
# macOS
brew install qemu spice-gtk

# Linux
sudo apt install qemu-system-aarch64 libspice-client-glib-2.0-dev

# Verify
qemu-system-aarch64 --version
```

- [ ] **Step 2: Build full project**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

- [ ] **Step 3: Run all tests**

```bash
cd build && ctest --output-on-failure
cd ../middleware && cargo test --workspace
```

- [ ] **Step 4: Manual test — launch with test kernel**

```bash
./build/rexplayer --kernel /path/to/Image --ram 2048 --cpus 2
```

Verify: QEMU starts, SPICE connects, display shows guest output, sidebar buttons work, settings dialog opens.

- [ ] **Step 5: Final commit tag**

```bash
git tag v0.2.0 -m "RexPlayer v0.2.0 — QEMU backend"
```
