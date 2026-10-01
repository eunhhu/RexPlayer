# Local Android image preparation

`python3 runtime/provision.py inspect` reports local installation prerequisites.
`prepare-images` copies two exact user-supplied Waydroid images into a **new**
directory after checking their size and SHA-256. It exports fixed argument arrays
for administrative initialization and container start; it never runs them.

Example manifest (replace hashes and byte sizes with values from your trusted
image supplier, not from an unverified download):

```json
{
  "schema_version": 1,
  "architecture": "x86_64",
  "images": {
    "system.img": {"sha256": "<64 lowercase hex characters>", "size": 123},
    "vendor.img": {"sha256": "<64 lowercase hex characters>", "size": 456}
  }
}
```

```sh
python3 runtime/provision.py prepare-images \
  --source /path/to/trusted-images --manifest /path/to/images.json \
  --destination /path/to/new-prepared-images
```

Only native Linux x86_64/aarch64 is accepted. Existing Waydroid configuration,
WSL, mismatching architecture, symlink images, malformed/duplicate fields,
incomplete images, wrong hashes, and an existing output directory are rejected.
Copies occur before publishing the final directory. `READY` is the completion
marker; a crash during publication can leave an incomplete directory without
`READY`, which must be reviewed manually and is never reused automatically.
The chosen parent directory must be private to the user; concurrent adversarial
filesystem replacement is outside this unprivileged preview's trust boundary.

The plan is **not an installation result**. Hashes detect corruption relative to
the supplied manifest; they do not authenticate the image publisher or verify the
image architecture. Signed first-party images and a privileged isolated runtime
installer remain release requirements. This tool never enables repositories,
loads kernel modules, changes udev rules, edits WSL configuration, initializes
an existing Waydroid installation, starts networking, or deletes Android data.

After installing a trusted Waydroid package and reviewing the generated plan on
the intended machine, an administrator can initialize the staged images with the
upstream `waydroid init -i` command and start the container. Session start must run
as the desktop user. These steps need separate approval and real-host testing;
they were not executed by the cloud validation suite.

Upstream references checked 2026-10-01:
- [Waydroid command-line options](https://docs.waydro.id/usage/waydroid-command-line-options)
- [Official installation instructions](https://docs.waydro.id/usage/install-on-desktops)

The preparation tests exercise actual temporary-file copies, corruption,
symlinks, repeated invocations and preservation of pre-existing data. They do not
establish Android boot, signed-image trust, graphics/audio or safe provisioning.
