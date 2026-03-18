# Plan Errata — QEMU Backend Migration

Corrections to the main plan based on plan review.

## Errata 1: Task 1 Step 6 — Wrong test directories

**Before:** `rm -rf tests/hal_tests tests/gpu_tests tests/device_tests tests/integration`
**After:** `rm -rf tests/hal_tests tests/gpu_tests tests/benchmarks`

## Errata 2: Task 5 — Install SPICE dependencies before building

Insert before the Build step in Task 5:

macOS: `brew install spice-gtk glib`
Linux: `sudo apt install libspice-client-glib-2.0-dev libglib2.0-dev`

CMakeLists.txt uses REQUIRED for spice-client-glib, so CMake will fail without it.

## Errata 3 (CRITICAL): Task 6 — Build order and missing code

### Problem

Task 6 includes settings_dialog.cpp and keymap_editor.cpp in the build target,
but those old files reference deleted headers (rex/vmm/vm.h, display.h). Build fails.

Task 6 also only has prose descriptions, not actual code.

### Fix

Task 6 must:

1. Provide full code for all GUI files
2. Write stub settings_dialog and keymap_editor that compile without old dependencies
3. Tasks 7 and 8 then replace the stubs with full implementations

### Task 6 revised step list (14 steps)

1. Write main.cpp (full code — see below)
2. Write mainwindow.h (full code)
3. Write mainwindow.cpp (full code — sidebar, menus, status bar, signal wiring)
4. Write display_widget.h (full code)
5. Write display_widget.cpp (full code — SPICE frame rendering with letterboxing)
6. Write input_handler.h (full code)
7. Write input_handler.cpp (full code — key/mouse to SPICE routing)
8. Write stub settings_dialog.h (empty QDialog subclass)
9. Write stub settings_dialog.cpp (just includes header)
10. Write stub keymap_editor.h (empty QWidget subclass)
11. Write stub keymap_editor.cpp (just includes header)
12. Update CMakeLists.txt (add rexplayer target)
13. Build
14. Commit

### Key code for main.cpp

- Remove all HAL/VMM/embedded_kernel includes
- Add qemu_process.h, qemu_config.h, spice_client.h includes
- Parse CLI args into QemuConfig
- Create QemuProcess and SpiceClient
- Connect qmpReady signal to SPICE connection
- Create MainWindow, set components, show, auto-start if kernel specified

### Key code for mainwindow.cpp

- createSidebar(): QToolBar with Qt::RightToolBarArea, vertical orientation
  - Dark styling (#2b2b2b bg, #b0b0b0 icons, hover white)
  - Button groups: Power | Vol+/Vol- | Home/Back/Recents | Rotate/Shot/GPS | Keymap/Frida/Settings
  - Each button has tooltip with shortcut
- createStatusBar_(): VM state dot + SPICE + FPS + CPU + RAM + ADB labels
- Signal wiring: QemuProcess::stateChanged -> onVmStateChanged
- Action handlers route to qemu_->pause/resume/reset/poweroff or spice_->input()->sendKeyPress

### Key code for display_widget.cpp

- setSpiceDisplay: connect SpiceDisplay::frameReady -> onFrameReady -> update()
- paintEvent: fill black, compute letterbox viewport, drawImage
- computeViewport: calculate aspect-ratio-preserving rectangle

### Key code for input_handler.cpp

- eventFilter: intercept KeyPress/KeyRelease/MousePress/MouseMove/MouseRelease
- Route keys via SpiceInput::qtKeyToScancode -> sendKeyPress/Release
- Route mouse via sendMouseMove/sendMousePress/sendMouseRelease

### Stub settings_dialog.h

Empty QDialog subclass with constructor setting title and size. No old dependencies.

### Stub keymap_editor.h

Empty QWidget subclass. No old dependencies.

## Errata 4: Spec inconsistency — cxx dependency

The spec lists cxx as Retained in Dependencies, but the plan correctly removes it.
The spec should be updated to remove cxx from Retained.

## Errata 5: Missing SPICE display test

The File Structure lists tests/spice_tests/test_spice_display.cpp but no task creates it.
This should be addressed as a follow-up task or added to Task 5.

## Errata 6: Task 9 — Missing CMake snippet

Task 9 modifies CMakeLists.txt but does not show the change explicitly.
Add src/gui/frida_panel.cpp to the add_executable(rexplayer ...) list.
