"""Require local receiver settings when flashing a Core2 with PlatformIO."""
from pathlib import Path
from SCons.Script import COMMAND_LINE_TARGETS, Exit, Import

Import("env")

if "upload" in COMMAND_LINE_TARGETS:
    header = Path(env.subst("$PROJECT_DIR")) / "include" / "settings.h"
    if not header.is_file():
        print("CARF: run python3 configure_logger.py in the repository root before flashing.")
        Exit(1)
    env.Append(CPPDEFINES=["CARF_VERIFY_UPLOAD"])
