# Read-only host capability inspector

This is the first runtime-controller building block, not a launcher, installer,
privileged preflight, or production-readiness gate. It inventories the current
Windows or Linux process environment without starting or provisioning anything.

## Run

Requires Python 3.10 or later; no third-party packages or administrator privileges
are needed. From the repository root:

```sh
python3 -B runtime/capability_inspector.py --pretty
python3 -B -m unittest discover -s runtime/tests -v
```

On native Windows with Python installed:

```powershell
py -3 -B runtime/capability_inspector.py --pretty
py -3 -B -m unittest discover -s runtime/tests -v
```

The report is JSON on standard output. `--pretty` only changes formatting. Exit
code `0` means the report was generated, even if prerequisites are missing or the
OS is unsupported. Invalid command-line arguments exit with code `2`. There is
no "ready" exit code. Redirecting stdout to a file is an explicit caller choice;
the inspector itself does not write a report or change configuration.

## Coverage and boundaries

Linux:

- OS, architecture, kernel release/version, and page size
- A clearly labelled WSL kernel-name heuristic
- Readable running-kernel configuration at `/proc/config.gz` or
  `/boot/config-<running-release>`; builtin/module/disabled/unknown are distinct
- Selected Binder, uinput, and DXG paths, checked with metadata only
- Advisory current-user read/write access for observed character devices;
  device access, ioctls, driver behavior, cgroup/LSM policy, and major/minor
  identity are **not** tested
- Binder and cgroup mounts in the current process's mount namespace
- Optional `docker`, `podman`, and `adb` executable discoverability without
  executing them or contacting their daemons/devices

Native Windows:

- OS version/build metadata and architecture, without declaring a supported
  Windows or CPU range
- `wsl.exe` discoverability in the process executable search path, without
  invoking it, enabling features, listing/starting distributions, or triggering
  installation
- Whether the current user's readable UTF-8 `.wslconfig` contains nonempty
  `kernel` or `kernelModules` overrides; values and paths are omitted
- An explicit unknown WSL runtime state. Run the Linux inspector separately
  inside an already-selected WSL environment to inspect that namespace. This
  tool never chooses or starts a distribution

Config reads are limited to 2 MiB after decompression. Missing, inaccessible,
malformed, oversized, non-UTF-8, symlinked, and non-regular config sources are
handled without inferring successful support. Special files are rejected before
opening, and opened descriptors are checked again. No child commands, device
probes, mounts, module loads, service changes, Docker/ADB connections, config
writes, or privilege escalation are performed.

The report omits usernames, hostnames, environment variables, executable paths,
WSL distribution names, and `.wslconfig` values. It still contains OS/kernel
information: review before sharing it publicly.

## Schema and status semantics

[`capability-report.schema.json`](./capability-report.schema.json) is the
versioned JSON Schema (Draft 2020-12). Consumers must check `schema_version`;
unknown schema versions must not be treated as successful compatibility checks.
Check identifiers are unique within a report and status counts must match the
checks. Those cross-field constraints are also tested in the stdlib test suite.

| Status | Meaning |
| --- | --- |
| `available` | The particular narrow observation succeeded; read its detail |
| `unavailable` | That exact path/tool was not observed, had the wrong type, or a kernel feature was explicitly disabled |
| `unknown` | This environment did not permit a trustworthy determination |
| `not_applicable` | The inspector does not cover this OS |

For example, a character device can be `available` while its advisory access
hint is false. A module can be configured without being loaded. `wsl.exe` can be
present while WSL is unusable. Alternate device paths and installations can
exist outside this inventory. Mount visibility is not proof of permission to
mount, delegate cgroups, or operate a container.

Every report independently contains:

- `summary.runtime_verification = "not_performed"`
- `summary.production_readiness = "not_established"`
- Nine runtime gates marked `not_tested`, including Android boot, application
  input, rendering, audio, security, lifecycle, performance, isolation, and
  installation/update/rollback

Historical proof evidence is not imported into this live host report. Even a
report with no missing observations does not demonstrate Android image
compatibility, rendered frames, application input, secure isolation, or safe
production provisioning. See the existing [roadmap](../docs/ROADMAP.md) and
[release gates](../docs/PACKAGING_AND_RELEASE_STRATEGY.md).

## Validation scope

Tests exercise Linux and Windows fixtures, missing tools, denied reads,
unsupported hosts, malformed/oversized/compressed configs, symlink/FIFO handling,
configuration redaction, advisory access, JSON structure, and CLI semantics.
Platform fixtures are not native Windows execution evidence. Native Windows,
WSL, and supported Linux hardware validation remain separate gates. Running this
suite does not perform Android or privileged proof tests.
