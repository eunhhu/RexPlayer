# Offline host-release transactions

`release_manager.py` is an implemented **unsigned local preview**, requiring Python
3.10+ and Linux x86-64 or aarch64. It stages real files, runs an explicitly approved
local health command, activates with one atomic state-file replacement, and can
roll back to the retained previous host release. Windows and macOS fail closed;
Windows test fixtures do not constitute native installer support.

This installs the host application and included helper files only. It does not
install Android, a kernel, containers, services, device rules, or network policy.
There are no downloads, privilege escalation, system-setting writes, signatures,
TUF metadata, data migrations, automatic crash-loop rollback, or release
promotion. The production strategy in `docs/PACKAGING_AND_RELEASE_STRATEGY.md`
remains the target, not a statement that all its gates have passed.

## Build and install

From a source checkout, produce the release archive and its separate manifest:

```sh
python3 scripts/package_preview.py --output /tmp/rexplayer-0.1.0.tar.gz \
  --version 0.1.0-local --native-ui
```

The build creates `/tmp/rexplayer-0.1.0.tar.gz.manifest.json`. `--manifest-output`
can choose another new path. Without `--native-ui`, only the launcher entrypoint
is available. The archive includes this manager, the capability inspector,
provisioning-preparation tool, the integrated-player runbook, and a copyable
`keymaps/default.json` profile with its format documentation. Native GUI builds still
require the documented native toolchain and desktop libraries.

After reviewing and trusting the local code and manifest:

```sh
python3 runtime/release_manager.py --root "$HOME/.local/share/rexplayer/releases" \
  install --archive /tmp/rexplayer-0.1.0.tar.gz \
  --manifest /tmp/rexplayer-0.1.0.tar.gz.manifest.json \
  --allow-unsigned --allow-execution
python3 runtime/release_manager.py --root "$HOME/.local/share/rexplayer/releases" status
python3 runtime/release_manager.py --root "$HOME/.local/share/rexplayer/releases" \
  run --allow-execution launcher -- --help
python3 runtime/release_manager.py --root "$HOME/.local/share/rexplayer/releases" \
  run --allow-execution player
```

The manager can also be run as `python3 release_manager.py` from an already
reviewed/extracted preview. Do not blindly extract or execute an archive from an
unknown publisher to bootstrap trust. Obtain and inspect the manager through your
trusted source checkout. SHA-256 verifies consistency, **not publisher identity**;
an attacker who replaces both archive and manifest can replace the code.

`--allow-unsigned` explicitly accepts the local artifact's unauthenticated origin.
`--allow-execution` separately permits executing its healthcheck or entrypoint.
Neither flag makes the code safe, signed, sandboxed, or production-ready. Run only
local code you trust. The manager runs as the current user and should never be run
with `sudo`. Health checks inherit that user's environment and authority.

The generated manifest's health command is `bin/rex-launcher --help`, bounded to
10 seconds. This proves the bundled executable starts successfully on the current
host. It does **not** prove Android boot, rendering, audio, gameplay, device
permissions, distro/libc portability, or native GUI availability. Those remain
separate, substantive product gates.

## Stage, update, and rollback

`stage` verifies/extracts without running any bundled executable and leaves the
active release unchanged. It returns a complete content-derived release ID:

```sh
python3 runtime/release_manager.py --root /path/to/private-install stage \
  --archive /path/to/new.tar.gz --manifest /path/to/new.tar.gz.manifest.json \
  --allow-unsigned
python3 runtime/release_manager.py --root /path/to/private-install activate \
  '<release-id-from-stage>' --allow-execution
python3 runtime/release_manager.py --root /path/to/private-install rollback --allow-execution
```

Installing another release uses the same `install` command. Old releases remain
present. Reinstalling the exact manifest is idempotent, verifies its already-staged
files, and does not repeat health execution when it is already current. Distinct
builds with the same semantic version have different content-derived IDs. A
rollback verifies the previous payload and runs its health command before
switching; it exchanges current and previous pointers. A second explicit rollback
therefore returns to the other retained release. No cleanup/uninstall command
is implemented, and no user data is deleted.

`run` resolves only named manifest entrypoints from the active release, rechecks
all its file hashes and modes, and forwards arguments without invoking a shell.
It exports `REXPLAYER_USER_DATA` for applications that honor that location. This
does not redirect Waydroid's Android data or migrate an existing application's
configuration. Existing processes keep using their original version directory
when another release becomes current.

## Layout, journal, and compatibility

```text
<root>/installation.json      owned-root marker
<root>/.lock                  persistent advisory-lock inode
<root>/state.json             atomic current/previous/generation record
<root>/journal.json           pending operation, present only during a transaction
<root>/versions/<id>/manifest.json
<root>/versions/<id>/payload/  manifest-verified files
<root>/staging/<transaction>/ incomplete installation, never active
<root>/user-data/             preserved on install, activation, and rollback
```

The root must belong to the current user, must not be writable by other users,
and cannot contain symlink path components. A nonempty unrelated directory is
not adopted. An OS advisory lock prevents cooperating installers from modifying
the same root concurrently and is automatically released when a process exits.
The lock file is never removed, avoiding a lock-inode replacement race.

Files and transition records are flushed with `fsync`; staging is renamed on the
same filesystem. Current and previous pointers change together in a single
`os.replace`, never by separately editing two links. Use a local filesystem with
normal Linux atomic-rename and fsync semantics; network filesystems and sudden
storage loss are not validated. Installed versions are immutable by convention,
not a security boundary against the same user or root; integrity is rechecked
before activation, rollback, status, and launch. No version garbage collection
can invalidate a running application's files.

Only manifest and user-data schema 1 are supported. Other data schemas are
refused, so unsupported migrations cannot silently make a rollback incompatible.
The manager itself never opens, rewrites, migrates, or deletes user-data files.
Code that you approve and launch has your user's permissions and may modify data;
backups and application-level migration compatibility remain your responsibility.

Before every mutating operation, a pending journal is reconciled with the atomic
state. Incomplete staging is removed only from its exact recorded transaction
directory. A fully renamed release is retained. A switch without a committed
pointer is aborted; a health-approved committed switch is retained. Recovery
never guesses through contradictory records and never runs code automatically.
Inspect recovery explicitly with:

```sh
python3 runtime/release_manager.py --root /path/to/private-install recover
```

A failed or timed-out healthcheck leaves the previous pointer unchanged. A normal
healthcheck completion or timeout cleans up its POSIX process group before the
group leader is reaped. This is not a sandbox; deliberately daemonizing programs
can escape a process group. An abruptly killed manager can leave a health process
running, so do not use health commands with long-running side effects. Recovery
protects the activation pointer but does not reconstruct process ownership after
an arbitrary host crash.

## Manifest and extraction rules

The separate schema-v1 JSON manifest covers the final archive's SHA-256, exact
compressed size, total unpacked size, every file's SHA-256 and size, executable
mode, platform, entrypoints, healthcheck, and user-data schema. It has no circular
self-hash because it is not inside the archive it covers. Unknown fields, duplicate
JSON keys, unsupported schema/platform/channel, invalid limits, and ambiguous paths
are rejected. The manager does not accept a `signature` field or pretend to verify
one.

Extraction uses explicit bounded reads and exclusively created regular files.
There is no `extractall`. Absolute paths, traversal, dot segments, case collisions,
backslashes, drive names, control characters, duplicate/missing/extra files,
file/directory conflicts, symlinks, hardlinks, devices, FIFOs, sparse members,
and setuid/setgid/sticky modes are refused. PAX/GNU metadata-extension headers
are refused before their bodies are processed, preventing unbounded metadata
allocation. Directory tar entries are intentionally unsupported; directories are derived from validated regular-file paths. The
manager verifies and extracts through the same archive descriptor, then rechecks
the installed tree. Limits: 4 GiB archive, 8 GiB unpacked payload, 20,000 files,
8 MiB JSON, and health timeouts of 1–120 seconds.

To create a sidecar for an existing compatible local archive without running it:

```sh
python3 runtime/release_manager.py manifest --archive /path/to/host.tar.gz \
  --output /path/to/host.manifest.json --version 0.2.0-local
```

Add `--native-ui` when `bin/rex-player` exists. This command defaults to the
`bin/rex-launcher --help` smoke check. Programmatic `create_manifest` supports an
explicit alternative health argv/timeout; executing it still needs consent.

## Verification

```sh
python3 -B -m unittest discover -s runtime/tests -p test_release_manager.py -v
python3 -B -m unittest discover -s tests -p test_packaging.py -v
```

Tests use actual temporary directories and local fixture shell executables. They
cover staging, launching, upgrades, retained rollback, data preservation, repeated
operations, hashes/sizes, bad archives, symlinks, lock contention from another
process, timeout cleanup, post-health integrity, and actual subprocess death
before/after durable staging and atomic activation. No downloaded payload is run.
Platform-independent validation tests run on Windows; POSIX transaction tests
explicitly skip there. Hardware/Android/runtime tests remain separate.
