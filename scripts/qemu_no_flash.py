import os
from pathlib import Path
import subprocess
import sys

from SCons.Script import COMMAND_LINE_TARGETS, Import


Import("env")


UNSAFE_TARGETS = {
    "upload",
    "uploadfs",
    "uploadfsota",
    "erase",
    "erase_upload",
    "download_fs",
}

if UNSAFE_TARGETS.intersection(COMMAND_LINE_TARGETS):
    executable = str(Path(sys.executable).resolve())
    refusal = (
        Path(env.subst("$PROJECT_DIR"))
        / "scripts"
        / "refuse_qemu_flash.py"
    )
    completed = subprocess.run([executable, str(refusal)], check=False)
    os._exit(completed.returncode)
