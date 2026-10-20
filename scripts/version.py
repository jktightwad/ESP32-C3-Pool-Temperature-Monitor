# PlatformIO pre-build script: stamps the firmware with the git commit it was
# built from, as FW_VERSION (e.g. "a1b2c3d", or "a1b2c3d-dirty" if tracked
# files had uncommitted edits). The monitor publishes it as "fw"; the Pi uses
# it to notice when it's running older code than the firmware.
import subprocess

Import("env")


def git(*args):
    try:
        return subprocess.check_output(
            ["git", *args], cwd=env["PROJECT_DIR"], stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return ""


version = git("rev-parse", "--short=7", "HEAD") or "unknown"
if git("status", "--porcelain", "--untracked-files=no"):
    version += "-dirty"
print("Firmware version: " + version)
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
