#!/bin/bash
# Pulls the latest code from GitHub and, if anything under pi/ changed,
# restarts the pool service (re-installing any changed systemd unit files).
# Run as root by pool-monitor-update.timer; git itself runs as the repo owner.
set -euo pipefail

REPO="${REPO:-/home/pi/ESP32-C3-Pool-Temperature-Monitor}"
REPO_USER="${REPO_USER:-pi}"
UNIT_DIR="${UNIT_DIR:-/etc/systemd/system}"

as_user() { runuser -u "$REPO_USER" -- "$@"; }

cd "$REPO"
as_user git fetch --quiet origin main
OLD=$(as_user git rev-parse HEAD)
NEW=$(as_user git rev-parse origin/main)
if [ "$OLD" = "$NEW" ]; then
  exit 0
fi

# --ff-only: never merge or overwrite local edits; fail loudly instead.
as_user git merge --ff-only --quiet origin/main
echo "Updated ${OLD:0:7} -> ${NEW:0:7}"

CHANGED=$(as_user git diff --name-only "$OLD" "$NEW" -- pi/)
if [ -z "$CHANGED" ]; then
  echo "No changes under pi/; nothing to restart"
  exit 0
fi
echo "Changed: $(echo "$CHANGED" | tr '\n' ' ')"

RELOAD=""
for unit in pool-monitor.service pool-temp-mdns.service \
            pool-monitor-update.service pool-monitor-update.timer; do
  if echo "$CHANGED" | grep -qx "pi/$unit"; then
    cp "pi/$unit" "$UNIT_DIR/"
    RELOAD=1
  fi
done
if [ -n "$RELOAD" ]; then
  systemctl daemon-reload
fi
if echo "$CHANGED" | grep -qx "pi/pool-temp-mdns.service"; then
  systemctl restart pool-temp-mdns
fi
systemctl restart pool-monitor
echo "Restarted pool-monitor"
