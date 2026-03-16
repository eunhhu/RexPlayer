# Contributing to RexPlayer

Thank you for your interest in contributing to RexPlayer! This document covers the development workflow, coding standards, and testing practices.

## Getting Started

1. Fork and clone the repository
2. Set up your development environment (see [README.md](../README.md#building))
3. Create a feature branch from `main`
4. Make your changes with tests
5. Submit a pull request

## Development Workflow

```
1. Create branch:    git checkout -b feature/my-feature
2. Implement:        Write tests first (TDD), then implementation
3. Verify:           cargo test --workspace && ctest --test-dir build
4. Commit:           git commit -m "feat(scope): description"
5. Push + PR:        git push -u origin feature/my-feature
```

## Commit Messages

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <description>

Types: feat, fix, docs, style, refactor, test, chore
Scopes: hal, vmm, devices, gpu, gui, network, frida, config, ci
```

Examples:
```
feat(hal): add TSC scaling support for KVM
fix(devices): handle zero-length virtio-blk reads
test(gpu): add scanout rotation tests
docs(api): document virtio-mmio register layout
```

## Code Organization

### C++ (src/)

| Directory | Purpose | Style |
|-----------|---------|-------|
| `src/hal/` | Hypervisor abstraction | Virtual interfaces, platform-guarded |
| `src/vmm/` | VM lifecycle, memory, boot | Core logic, performance-critical |
| `src/devices/` | Legacy device emulation | Simple I/O handlers |
| `src/gpu/` | GPU rendering pipeline | Renderer interface + implementations |
| `src/platform/` | OS abstraction | Platform-guarded implementations |
| `src/gui/` | Qt 6 UI | Qt conventions (Q_OBJECT, signals/slots) |

### Rust (middleware/)

| Crate | Purpose | Dependencies |
|-------|---------|-------------|
| `rex-devices` | Virtio device backends | vm-memory, virtio-queue |
| `rex-ffi` | C++/Rust FFI bridge | cxx |
| `rex-config` | Configuration (TOML) | serde, toml |
| `rex-frida` | Frida management | tokio |
| `rex-network` | NAT, DHCP, DNS | — |
| `rex-filesync` | File synchronization | — |
| `rex-update` | Self-update | serde |

## Coding Standards

### C++

- **Standard**: C++20
- **Naming**: `snake_case` for functions/variables, `PascalCase` for classes, `UPPER_CASE` for constants
- **Headers**: `#pragma once`, include what you use
- **Memory**: RAII, `std::unique_ptr` for ownership, raw pointers for non-owning references
- **Errors**: `std::expected<T, Error>` (C++23 via `HalResult<T>`, `RendererResult<T>`)
- **Threading**: `std::mutex` + `std::lock_guard`, `std::atomic` for flags
- **Platform code**: Guard with `#ifdef __linux__`, `#ifdef __APPLE__`, `#ifdef _WIN32`

### Rust

- **Edition**: 2021
- **Naming**: Standard Rust conventions (`snake_case`, `PascalCase`, `SCREAMING_SNAKE_CASE`)
- **Errors**: `thiserror` for library errors, `anyhow` for application errors
- **Serialization**: `serde` with `#[derive(Serialize, Deserialize)]`
- **Async**: `tokio` runtime (not used in hot paths)
- **Testing**: `#[cfg(test)] mod tests` in each module
- **Formatting**: `cargo fmt` (enforced in CI)
- **Linting**: `cargo clippy -- -D warnings` (enforced in CI)

## Testing

### Test Structure

```
tests/
├── hal_tests/           # HAL type tests + KVM integration
├── gpu_tests/           # Software renderer tests
└── benchmarks/          # Performance measurement framework

middleware/
├── rex-devices/src/     # 150 tests (inline #[cfg(test)])
├── rex-network/src/     # 28 tests
├── rex-frida/src/       # 10 tests
├── rex-config/src/      # 5 tests
├── rex-filesync/src/    # 7 tests
└── rex-update/src/      # 8 tests
```

### Writing Tests

**Rust** — Add tests in the same file:
```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_feature() {
        // Arrange
        let mut device = VirtioBlk::new(config).unwrap();
        // Act
        let result = device.process_request(0, 0, &mut buf);
        // Assert
        assert_eq!(result.unwrap(), 0);
    }
}
```

**C++** — Add tests in `tests/` using GoogleTest:
```cpp
#include <gtest/gtest.h>
#include "component.h"

TEST(Component, BasicOperation) {
    Component c;
    auto result = c.do_something();
    EXPECT_TRUE(result.has_value());
}
```

### Running Tests

```bash
# All Rust tests
cargo test --workspace

# Specific crate
cargo test -p rex-devices

# C++ tests
ctest --test-dir build --output-on-failure

# With verbose output
cargo test --workspace -- --nocapture
```

## Adding a New Virtio Device

1. Create `middleware/rex-devices/src/virtio_mydevice.rs`
2. Implement the `VirtioDevice` trait:
   ```rust
   impl VirtioDevice for VirtioMyDevice {
       fn device_type(&self) -> VirtioDeviceType { ... }
       fn features(&self) -> u64 { ... }
       fn activate(&mut self) -> DeviceResult<()> { ... }
       fn reset(&mut self) { ... }
       fn process_queue(&mut self, queue_index: u16) -> DeviceResult<()> { ... }
   }
   ```
3. Add `pub mod virtio_mydevice;` to `lib.rs`
4. Write tests (minimum 5)
5. Update the MMIO device registration in `rex-ffi`

## Adding a New Hypervisor Backend

1. Create `src/hal/mybackend/mybackend_hypervisor.h` and `.cpp`
2. Implement `IHypervisor`, `IVcpu`, and `IMemoryManager`
3. Add to `hypervisor_factory.cpp` with appropriate `#ifdef`
4. Add CMake option `REX_ENABLE_MYBACKEND` and conditional sources
5. Add CI job for the target platform

## Pull Request Guidelines

- Each PR should address a single concern
- Include tests for new functionality
- All CI checks must pass (build, test, clippy, fmt)
- Update documentation if adding user-facing features
- Keep commits atomic and well-described

## Reporting Issues

Use [GitHub Issues](https://github.com/rexplayer/rexplayer/issues) with:
- Platform and version information
- Steps to reproduce
- Expected vs actual behavior
- Relevant log output (UART console, VM state)
