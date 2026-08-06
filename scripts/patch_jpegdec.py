"""
PlatformIO pre-build script: apply CrossPoint's JPEGDEC patches via `git apply`.

The upstream JPEGDEC pin still has the wild-pointer + DC-write bugs in
JPEGDecodeMCU_P that surface when EIGHT_BIT_GRAYSCALE decodes a 3-component
progressive JPEG (each Y MCU drags two MCU_SKIP calls behind it for Cb/Cr).
The patches in `scripts/jpegdec_patches/` carry the fix; this script applies
each one against the libdep working tree.

Each patch's idempotency is decided by git itself:
  * `git apply --check --reverse` succeeds  -> already applied, skip
  * source already contains the equivalent fix -> already applied, skip
  * `git apply --check`            succeeds  -> apply
  * neither succeeds                          -> abort the build

Patches live in `scripts/jpegdec_patches/` as one-commit-per-fix files
(see the file headers for context). Applied in lexical order.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import subprocess
import sys


PATCH_DIR = os.path.join(env["PROJECT_DIR"], "scripts", "jpegdec_patches")  # noqa: F821


def patch_jpegdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    patches = _patch_files()
    jpeg_dir = os.path.join(libdeps_dir, env["PIOENV"], "JPEGDEC")
    if not os.path.isdir(os.path.join(jpeg_dir, ".git")):
        return
    for patch in patches:
        _apply_one(jpeg_dir, patch)


def _patch_files():
    if not os.path.isdir(PATCH_DIR):
        raise RuntimeError(
            "JPEGDEC patches missing -- aborting build (expected directory %s)"
            % PATCH_DIR
        )
    patches = sorted(
        os.path.join(PATCH_DIR, name)
        for name in os.listdir(PATCH_DIR)
        if name.endswith(".patch")
    )
    if not patches:
        raise RuntimeError(
            "JPEGDEC patches missing -- aborting build (no .patch files in %s)"
            % PATCH_DIR
        )
    return patches


def _apply_one(jpeg_dir, patch_path):
    name = os.path.basename(patch_path)
    if _git_apply_succeeds(jpeg_dir, patch_path, reverse=True):
        return
    if _patch_already_satisfied(jpeg_dir, name):
        return
    if not _git_apply_succeeds(jpeg_dir, patch_path, reverse=False):
        # Not applied, not appliable -- the libdep source has diverged from
        # what the patch expects. Don't write a half-patched file.
        result = subprocess.run(
            ["git", "apply", "--check", "-"],
            cwd=jpeg_dir,
            capture_output=True,
            input=_read_patch(patch_path),
        )
        sys.stderr.write(
            "ERROR: JPEGDEC patch %s does not apply cleanly:\n%s%s\n"
            % (
                name,
                result.stdout.decode("utf-8", errors="replace"),
                result.stderr.decode("utf-8", errors="replace"),
            )
        )
        raise SystemExit(1)
    subprocess.run(
        ["git", "apply", "-"],
        cwd=jpeg_dir,
        check=True,
        input=_read_patch(patch_path),
    )
    print("Applied JPEGDEC patch: %s" % name)


def _patch_already_satisfied(jpeg_dir, patch_name):
    jpeg_inl = os.path.join(jpeg_dir, "src", "jpeg.inl")
    if not os.path.isfile(jpeg_inl):
        return False
    with open(jpeg_inl, "r", encoding="utf-8") as f:
        content = f.read()
    normalized = " ".join(content.split())

    if patch_name.startswith("0001-"):
        return (
            "signed short *pMCU = (iMCU < 0) ? pJPEG->sMCUs : &pJPEG->sMCUs[iMCU & 0xffffff];"
            in normalized
        )

    if patch_name.startswith("0002-"):
        return (
            "if (iMCU >= 0) pMCU[0] |= iPositive;" in normalized
            and "if (iMCU >= 0) pMCU[0] = (short)*iDCPredictor;" in normalized
        )

    return False


def _git_apply_succeeds(jpeg_dir, patch_path, *, reverse):
    cmd = ["git", "apply", "--check"]
    if reverse:
        cmd.append("--reverse")
    cmd.append("-")
    return subprocess.run(
        cmd,
        cwd=jpeg_dir,
        capture_output=True,
        input=_read_patch(patch_path),
    ).returncode == 0


def _read_patch(patch_path):
    # Binary stdin prevents the Windows text layer from restoring CRLF after
    # normalization, so git receives the LF context recorded in the repository.
    with open(patch_path, "rb") as patch_file:
        return patch_file.read().replace(b"\r\n", b"\n")


patch_jpegdec(env)  # noqa: F821
