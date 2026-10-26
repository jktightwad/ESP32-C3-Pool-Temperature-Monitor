# PlatformIO pre-build script: stamps the firmware with the git commit it was
# built from, as FW_VERSION (e.g. "a1b2c3d"). Builds with uncommitted edits to
# tracked files get "-dirty.<YYMMDDHHMM>" appended, so two different
# uncommitted builds never share a version (OTA compares versions to decide
# whether to update). The monitor publishes it as "fw"; the Pi uses it to
# notice when it's running older code than the firmware.
import subprocess
import time

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
    version += "-dirty." + time.strftime("%y%m%d%H%M")
print("Firmware version: " + version)
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
# For scripts/ota_upload.py, which tells the Pi which version it's sending.
env.Replace(FW_VERSION_STR=version)
