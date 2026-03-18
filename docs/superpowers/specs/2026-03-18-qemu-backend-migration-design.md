# RexPlayer QEMU Backend Migration Design

**Date:** 2026-03-18
**Status:** Approved
**Scope:** Replace custom VMM with QEMU subprocess + SPICE display

## Problem

RexPlayer's custom VMM (HAL + device emulation) cannot run Android guests.
The device emulation layer is incomplete — no functional GIC, virtio-gpu, virtio-input, etc.
Building all of these from scratch is impractical. QEMU already provides a complete, battle-tested machine model.

## Decision

Replace the custom VMM with QEMU as a subprocess. RexPlayer becomes a **frontend** that:
1. Manages QEMU process lifecycle
2. Receives guest display via SPICE (low-latency)
3. Sends input events via SPICE input channel
4. Controls VM state via QMP (QEMU Machine Protocol)
5. Integrates Frida, config management, and auto-update via Rust middleware

## Architecture

```
┌─────────────────────────────────────┐
│  Qt 6 GUI (RexPlayer)              │
│  SPICE Client + VM Controls        │
├─────────────────────────────────────┤
│  QEMU Process Manager              │
│  spawn, monitor, kill, config gen  │
├─────────────────────────────────────┤
│  SPICE Display Channel             │
│  framebuffer → QImage rendering    │
│  input events → SPICE channel      │
├──── Rust FFI (cxx) ─────────────────┤
│  rex-config  │ rex-frida │ rex-update│
├─────────────────────────────────────┤
│  qemu-system-{aarch64,x86_64}      │
│  (subprocess)                       │
│  SPICE socket + QMP socket         │
└─────────────────────────────────────┘
```

## Components

### 1. QEMU Process Manager (`src/qemu/`)

**Responsibility:** Launch, monitor, and terminate QEMU subprocess.

**Files:**
- `qemu_process.h/.cpp` — QProcess wrapper, QEMU binary discovery, command-line generation
- `qemu_config.h/.cpp` — Translate RexPlayer config (CPU, RAM, disk, network) into QEMU CLI args
- `qmp_client.h/.cpp` — QMP (JSON over socket) client for VM control

**QEMU Binary Discovery (priority order):**
1. User-configured path in TOML config
2. Bundled binary (packaging/qemu/)
3. System PATH lookup (`qemu-system-aarch64`, `qemu-system-x86_64`)

**QEMU CLI Generation Example:**
```
qemu-system-aarch64 \
  -machine virt,gic-version=3 \
  -cpu cortex-a76 \
  -smp 4 \
  -m 4096 \
  -drive file=system.img,format=raw,if=virtio \
  -kernel Image \
  -append "console=ttyAMA0 androidboot.hardware=rex" \
  -spice unix=on,addr=/tmp/rex-spice.sock,disable-ticketing=on \
  -qmp unix:/tmp/rex-qmp.sock,server=on,wait=off \
  -display none \
  -device virtio-gpu-pci \
  -device virtio-keyboard-pci \
  -device virtio-tablet-pci \
  -device intel-hda -device hda-duplex \
  -netdev user,id=net0,hostfwd=tcp::5555-:5555 \
  -device virtio-net-pci,netdev=net0 \
  -serial mon:stdio
```

**Process Lifecycle:**
- `start()` — spawn QProcess, connect QMP, connect SPICE
- `pause()` — QMP `stop`
- `resume()` — QMP `cont`
- `reset()` — QMP `system_reset`
- `poweroff()` — QMP `system_powerdown`
- `kill()` — QMP `quit`, then QProcess::kill() if needed (5s timeout)
- `snapshot_save(name)` — QMP `snapshot-save` (QEMU 8.0+)
- `snapshot_load(name)` — QMP `snapshot-load` (QEMU 8.0+)

**Error Recovery:**
- **Crash detection:** `QProcess::finished` signal with non-zero exit code or crash exit status
- **Hang detection:** QMP heartbeat via `query-status` every 5s; if no response within 10s, treat as hung
- **Kill timeout:** QMP `quit` → wait 5s → `QProcess::kill()` → wait 2s → `QProcess::terminate()`
- **Socket cleanup:** On abnormal exit, delete stale SPICE/QMP unix sockets (Linux/macOS)
- **User notification:** Emit signal to GUI with error code and stderr output for display in status bar / dialog
- **No auto-restart:** Crashed VM requires explicit user action to restart (prevents boot loops)

### 2. QMP Client (`src/qemu/qmp_client.h/.cpp`)

**Responsibility:** JSON-RPC over unix socket (Linux/macOS) or named pipe (Windows).

**Protocol:**
- Connect to QMP socket
- Read greeting, send `qmp_capabilities`
- Send commands as JSON: `{"execute": "stop"}`
- Parse responses and async events

**Implementation:**
- `QLocalSocket` (Qt) for cross-platform unix socket / named pipe
- Async command queue with callbacks
- Event listener for guest state changes

### 3. SPICE Client (`src/spice/`)

**Responsibility:** Receive display frames and send input events via SPICE protocol.

**Files:**
- `spice_client.h/.cpp` — Session management, channel callbacks
- `spice_display.h/.cpp` — Display channel → QImage conversion
- `spice_input.h/.cpp` — Qt input events → SPICE input channel

**Library:** `libspice-client-glib` (C library, GLib dependency only — no GTK)

**Connection:**
- Linux/macOS: unix socket `-spice unix=on,addr=<unique-path>,disable-ticketing=on`
- Windows: TCP localhost `-spice port=<dynamic>,disable-ticketing=on`
- Socket paths include instance UUID to avoid collision when running multiple instances

**Display Pipeline:**
```
QEMU virtio-gpu → SPICE server → socket → libspice-client-glib
  → SpiceDisplayChannel invalidate callback
  → copy pixel data to QImage
  → QWidget::update()
```

**Input Pipeline:**
```
Qt QKeyEvent/QMouseEvent/QTouchEvent
  → SpiceInputsChannel
  → SPICE server → QEMU → guest virtio-input
```

**Event Loop Integration (GLib ↔ Qt):**

`libspice-client-glib` requires a GLib main context for async dispatch. Integration strategy:
- Use a dedicated `QTimer` (16ms interval ≈ 60fps) calling `g_main_context_iteration(ctx, FALSE)` to pump GLib events from the Qt thread
- This avoids threading complexity while maintaining low latency
- The GLib context is created per-SpiceSession, not the global default, to avoid conflicts
- If frame rate needs exceed 60fps, the timer interval can be reduced or switched to `QSocketNotifier` polling GLib file descriptors directly

### 4. Qt GUI Refactoring (`src/gui/`)

**Changes from current:**
- `MainWindow` — replace VM direct control with QemuProcess signals/slots
- `DisplayWidget` — replace software renderer with SPICE display sink
- `InputHandler` — route events to SPICE input channel instead of vCPU inject
- `SettingsDialog` — QEMU path config, display options
- `KeymapEditor` — retained, adapt to send keymaps via SPICE input channel
- `main.cpp` — remove HAL/VMM initialization, launch QemuProcess

**New GUI features:**
- QEMU binary path configuration
- Snapshot manager (save/load/delete via QMP)
- Power menu: pause, resume, reset, poweroff
- Status bar: QEMU process state, SPICE connection state

### 5. Rust Middleware (retained crates)

**rex-config** — TOML config management
- Add QEMU-specific config fields (binary path, machine type, accelerator)
- Generate QEMU CLI args from config

**rex-frida** — Frida server lifecycle
- Inject Frida into QEMU guest via ADB over forwarded port
- No changes needed to core logic

**rex-update** — Auto-update
- Add QEMU binary update path (optional bundled QEMU)
- Self-update mechanism unchanged

### 6. Deleted Middleware

- `rex-ffi` — C++↔Rust FFI bridge for custom VMM dispatch (entirely obsolete)
- `rex-devices` — All virtio device backends (blk, net, gpu, input, console, balloon, snd, vsock, mmio) — QEMU provides all devices
- `rex-network` — QEMU SLIRP handles networking
- `rex-filesync` — SPICE shared folder or virtio-9p replaces this

**Post-migration `Cargo.toml` workspace members:**
```toml
[workspace]
members = ["rex-config", "rex-frida", "rex-update"]
```

## Platform-Specific Notes

### macOS
- QEMU uses HVF accelerator: `-accel hvf`
- SPICE via unix socket
- QEMU installed via Homebrew (`brew install qemu`) or bundled
- **`libspice-client-glib` on macOS:**
  - Install via Homebrew: `brew install spice-gtk` (pulls GLib as dependency; GTK is a build dep but not linked at runtime if only using `libspice-client-glib`)
  - ARM64 (Apple Silicon): confirmed available in Homebrew arm64 bottles
  - Alternative: build `spice-gtk` from source with `-Dgtk=false` Meson option to eliminate GTK dependency entirely
  - GLib event loop integration handled via QTimer polling (see Event Loop Integration section)

### Linux
- QEMU uses KVM accelerator: `-accel kvm`
- SPICE via unix socket
- QEMU from system package manager

### Windows
- QEMU uses WHPX accelerator: `-accel whpx` (or TCG fallback)
- SPICE via TCP localhost (named pipes possible but less portable)
- QEMU from installer or bundled
- `libspice-client-glib` via vcpkg or pre-built

## Deleted Code

All of these directories are removed entirely:

- `src/hal/` — Hypervisor abstraction (KVM, HVF, WHPX direct calls)
- `src/vmm/` — Custom VM lifecycle, memory manager, boot, snapshot, optimizers
- `src/devices/` — Legacy device emulation (UART, i8042, RTC, PCI)
- `src/gpu/` — Software renderer, virgl/venus stubs
- `src/platform/` — OS abstraction, async I/O
- `middleware/rex-ffi/` — C++↔Rust FFI bridge (obsolete with QEMU)
- `middleware/rex-devices/` — All virtio device backends (QEMU replaces)
- `middleware/rex-network/`
- `middleware/rex-filesync/`

## Dependencies

### New
- `libspice-client-glib` — SPICE protocol client
- `glib-2.0` — Required by spice-client-glib

### Retained
- Qt 6 (Widgets, Gui, Core)
- Rust toolchain (for middleware crates)

### Removed
- Hypervisor.framework (macOS)
- Direct KVM/WHPX headers
- virglrenderer, Venus

## CMake Structure (new)

```cmake
# Core libraries
add_library(rex_qemu STATIC
    src/qemu/qemu_process.cpp
    src/qemu/qemu_config.cpp
    src/qemu/qmp_client.cpp
)

add_library(rex_spice STATIC
    src/spice/spice_client.cpp
    src/spice/spice_display.cpp
    src/spice/spice_input.cpp
)

# GUI executable
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
    PkgConfig::SPICE_CLIENT_GLIB
)
```

## UI/UX Design

### Overall Layout

```
┌──────────────────────────────────────────────────────┐
│  Menu Bar (File | VM | Tools | Help)                 │
├──────────────────────────────────────────────────────┤
│                                          │ ┌───────┐ │
│                                          │ │ Power │ │
│                                          │ ├───────┤ │
│                                          │ │ Vol+  │ │
│                                          │ ├───────┤ │
│       Android Display                    │ │ Vol-  │ │
│       (SPICE framebuffer)                │ ├───────┤ │
│                                          │ │ Home  │ │
│                                          │ ├───────┤ │
│                                          │ │ Back  │ │
│                                          │ ├───────┤ │
│                                          │ │Recent │ │
│                                          │ ├───────┤ │
│                                          │ │Rotate │ │
│                                          │ ├───────┤ │
│                                          │ │ Shot  │ │
│                                          │ ├───────┤ │
│                                          │ │ GPS   │ │
│                                          │ ├───────┤ │
│                                          │ │Keymap │ │
│                                          │ ├───────┤ │
│                                          │ │Frida  │ │
│                                          │ ├───────┤ │
│                                          │ │ Gear  │ │
│                                          │ └───────┘ │
├──────────────────────────────────────────────────────┤
│  Status Bar: VM state │ SPICE │ FPS │ CPU │ RAM     │
└──────────────────────────────────────────────────────┘
```

### Right Sidebar Action Buttons

Android Studio Emulator 스타일 세로 툴바. 디스플레이 오른쪽에 고정.

**디자인 원칙:**
- 28×28px 아이콘, 36×36px 버튼 히트 영역, 4px 간격
- 다크 배경 (#2b2b2b), 아이콘은 밝은 회색 (#b0b0b0), hover시 흰색
- 구분선으로 기능 그룹 분리
- 툴팁에 단축키 표시

**버튼 그룹:**

| 그룹 | 버튼 | 아이콘 | 단축키 | 동작 |
|------|------|--------|--------|------|
| **전원** | Power | ⏻ | `Ctrl+P` | QMP `system_powerdown` |
| **볼륨** | Volume Up | 🔊 | `Ctrl+↑` | SPICE key inject |
| | Volume Down | 🔉 | `Ctrl+↓` | SPICE key inject |
| **네비게이션** | Home | ⌂ | `Ctrl+H` | SPICE key inject |
| | Back | ← | `Ctrl+B` | SPICE key inject |
| | Recents | ☐ | `Ctrl+R` | SPICE key inject |
| **도구** | Rotate | ↻ | `Ctrl+Shift+R` | QEMU display rotation |
| | Screenshot | 📷 | `Ctrl+S` | SPICE display capture → 파일 저장 |
| | GPS | 📍 | — | GPS 좌표 입력 다이얼로그 |
| **고급** | Keymap | ⌨ | `Ctrl+K` | 키맵 에디터 패널 토글 |
| | Frida | 🔬 | `Ctrl+F` | Frida 패널 토글 |
| | Settings | ⚙ | `Ctrl+,` | 설정 창 열기 |

**상태 표현:**
- VM 꺼짐: Power 버튼 빨간색 링
- VM 일시정지: Power 버튼 노란색 링
- VM 실행 중: Power 버튼 초록색 링
- Frida 연결됨: Frida 버튼에 초록 점 인디케이터

### Keymap Editor (게임 특화 드래그&드롭)

디스플레이 위에 오버레이로 표시되는 키맵 편집 모드.

**활성화:** 사이드바 Keymap 버튼 클릭 → 편집 모드 진입

**편집 모드 UI:**
```
┌─────────────────────────────────────────┐
│ [Keymap Editor]        [Save] [Cancel]  │
├─────────────────────────────────────────┤
│                                         │
│  Android Display (반투명 오버레이)       │
│                                         │
│    ┌─────┐                              │
│    │  W  │  ← 드래그로 배치된 키        │
│    └─────┘                              │
│         ┌─────┐                         │
│    ┌────┤  S  ├────┐                    │
│    │ A  └─────┘ D  │                    │
│    └────┘     └────┘                    │
│                                         │
│              ┌──────────┐               │
│              │ Joystick │ ← 가상 조이   │
│              │    ◎     │   스틱 위젯   │
│              └──────────┘               │
│                                         │
├─────── Toolbox ─────────────────────────┤
│ [Key] [D-Pad] [Joystick] [Tap Zone]    │
│ [Swipe] [Aim] [Shoot]                  │
└─────────────────────────────────────────┘
```

**위젯 타입:**

| 위젯 | 설명 | 설정 |
|------|------|------|
| **Key** | 단일 키 → 화면 탭 | 크기, 투명도, 바인딩 키 |
| **D-Pad** | 4방향 키 → 화면 스와이프 | 크기, 데드존, WASD/방향키 |
| **Joystick** | 아날로그 스틱 에뮬레이션 | 크기, 감도, 바인딩 키 4개 |
| **Tap Zone** | 영역 클릭 → 화면 탭 | 크기, 위치, 바인딩 키 |
| **Swipe** | 키 누름 → 화면 스와이프 | 방향, 거리, 속도 |
| **Aim** | 마우스 이동 → 화면 드래그 | 감도, 영역 |
| **Shoot** | 마우스 클릭 → 화면 탭 | 위치, 바인딩 |

**프로파일:**
- 앱 패키지명 기반 자동 전환 (`com.example.game` → `game.keymap.json`)
- 프로파일 목록에서 선택/복제/삭제
- JSON 포맷으로 내보내기/가져오기

**동작:**
- Toolbox에서 위젯을 드래그하여 디스플레이 위에 드롭
- 배치된 위젯을 클릭하면 속성 편집 팝오버 (바인딩 키, 크기, 투명도)
- 위젯을 디스플레이 밖으로 드래그하면 삭제
- 실시간 미리보기: 편집 중에도 키 입력하면 해당 위젯이 하이라이트

### Settings Window (모던 사이드바 스타일)

macOS 시스템 설정 / VS Code 설정 스타일의 독립 윈도우.

**레이아웃:**
```
┌─────────────────────────────────────────────────┐
│  Settings                              [×]      │
├──────────┬──────────────────────────────────────┤
│          │                                      │
│ General  │  General Settings                    │
│ ──────── │                                      │
│ Display  │  Language    [English        ▾]      │
│ Perfor-  │  Theme       [Dark / Light / Auto]   │
│  mance   │  QEMU Path  [/usr/bin/qemu...] [📂] │
│ Network  │  Auto-start  [✓]                     │
│ Input    │  Check for updates [✓]               │
│ Frida    │                                      │
│ Advanced │                                      │
│          │                                      │
└──────────┴──────────────────────────────────────┘
```

**설정 카테고리:**

| 카테고리 | 항목 |
|----------|------|
| **General** | 언어, 테마(다크/라이트/시스템), QEMU 바이너리 경로, 자동 시작, 업데이트 체크 |
| **Display** | 해상도 프리셋 (Phone/Tablet/Custom), DPI, 프레임 제한(30/60/120), 스케일링 모드 |
| **Performance** | vCPU 수, RAM 크기, 가속기 선택 (HVF/KVM/WHPX/TCG), 디스크 캐시 모드 |
| **Network** | 포트 포워딩 테이블 (호스트→게스트), DNS 설정, 프록시 |
| **Input** | 키맵 프로파일 관리, 마우스 감도, 게임패드 바인딩 |
| **Frida** | Frida 서버 경로, 자동 시작, 포트, 스크립트 디렉토리 |
| **Advanced** | QEMU 추가 CLI 인자 (텍스트 필드), 로그 레벨, 스냅샷 저장 경로 |

**디자인:**
- 사이드바 너비 180px, 고정
- 선택된 카테고리는 accent color 배경 하이라이트
- 설정 변경 시 즉시 적용 (VM 재시작 필요한 항목은 배지 표시)
- 검색 바: 사이드바 상단에 설정 항목 필터링

### Frida Panel (스크립트 에디터 내장)

사이드바 Frida 버튼으로 토글되는 하단 패널 (터미널 스타일).

**레이아웃:**
```
┌─────────────────────────────────────────────────────┐
│ Frida                    [▾ Process] [Start] [Stop] │
├──────────┬──────────────────────────────────────────┤
│          │                                          │
│ Scripts  │  // hook-example.js                      │
│ ──────── │  Java.perform(function() {               │
│ hook.js  │    var Activity = Java.use(              │
│ dump.js  │      'android.app.Activity');            │
│ trace.js │    Activity.onCreate.implementation =    │
│          │      function() {                        │
│ [+ New]  │        console.log('onCreate called');   │
│          │        this.onCreate.apply(this, args);  │
│          │      };                                  │
│          │  });                                     │
│          │                                          │
├──────────┴──────────────────────────────────────────┤
│ Console Output                          [Clear] [⏎] │
│ > [2026-03-18 14:32:01] onCreate called             │
│ > [2026-03-18 14:32:01] Activity: MainActivity      │
│ > [2026-03-18 14:32:03] onCreate called             │
└─────────────────────────────────────────────────────┘
```

**기능:**

| 기능 | 설명 |
|------|------|
| **프로세스 선택** | 드롭다운으로 게스트 프로세스 목록 (Frida `enumerate_processes`) |
| **스크립트 에디터** | QScintilla 또는 QPlainTextEdit + JavaScript 신택스 하이라이팅 |
| **스크립트 목록** | 왼쪽 사이드에 저장된 스크립트 파일 목록, 클릭으로 전환 |
| **실행 제어** | Start (스크립트 주입), Stop (디태치), Reload (재주입) |
| **콘솔 출력** | `console.log` 출력 실시간 스트리밍, 타임스탬프, 컬러 코딩 |
| **자동 완성** | Frida API 기본 자동완성 (`Java.use`, `Interceptor.attach` 등) |
| **스크립트 템플릿** | New 버튼 클릭 시 템플릿 선택 (Hook Method, Trace Class, Dump Memory 등) |

**콘솔 컬러 코딩:**
- `console.log` → 흰색
- `console.warn` → 노란색
- `console.error` → 빨간색
- Frida 시스템 메시지 → 회색

### Status Bar

윈도우 하단 고정. 왼쪽→오른쪽 정보 배치.

```
[● Running] │ SPICE Connected │ 60 FPS │ CPU 23% │ RAM 2.1/4.0 GB │ ADB :5555
```

| 항목 | 소스 | 업데이트 주기 |
|------|------|---------------|
| VM State | QMP `query-status` | 실시간 (이벤트) |
| SPICE | 연결 상태 | 실시간 |
| FPS | SPICE 프레임 카운터 | 1초 |
| CPU/RAM | QMP `query-cpus` / host 모니터링 | 2초 |
| ADB | 포트 포워딩 상태 | 5초 |

## Testing Strategy

- **QMP Client**: Unit tests with mock socket (JSON request/response pairs)
- **QEMU Config**: Unit tests for CLI arg generation from various configs
- **SPICE Display**: Integration test with QEMU headless + frame capture
- **GUI**: Manual testing for now; consider Qt Test framework later
- **Rust crates**: Existing tests for rex-config, rex-frida, rex-update

## Migration Steps (high level)

1. Delete old code (HAL, VMM, devices, GPU, platform)
2. Delete unused Rust crates (rex-network, rex-filesync)
3. Implement QEMU process manager + QMP client
4. Implement SPICE client (display + input)
5. Refactor GUI to use new backends
6. Update CMakeLists.txt
7. Update rex-config for QEMU settings
8. Integration test: boot Android on all 3 platforms
9. Update README and docs
