#!/bin/bash
# Stop CFS daemon and unmount KVBFS.

set -e

PID_FILE="/tmp/cfs-local.pids"

if [ ! -f "$PID_FILE" ]; then
    echo "No PID file found at $PID_FILE. Is CFS-Local running?"
    exit 1
fi

source "$PID_FILE"

# Stop CFS daemon
if kill -0 "$CFS_PID" 2>/dev/null; then
    echo "Stopping CFS daemon (PID: $CFS_PID)..."
    kill -TERM "$CFS_PID"
    wait "$CFS_PID" 2>/dev/null || true
    echo "CFS daemon stopped."
else
    echo "CFS daemon (PID: $CFS_PID) is not running."
fi

# Unmount KVBFS
if mountpoint -q "$MOUNTPOINT" 2>/dev/null; then
    echo "Unmounting KVBFS at $MOUNTPOINT..."
    fusermount -u "$MOUNTPOINT"
    echo "KVBFS unmounted."
else
    echo "KVBFS at $MOUNTPOINT is not mounted."
    # Kill KVBFS process if still running
    if kill -0 "$KVBFS_PID" 2>/dev/null; then
        kill -TERM "$KVBFS_PID"
        wait "$KVBFS_PID" 2>/dev/null || true
    fi
fi

rm -f "$PID_FILE"
echo "CFS-Local stopped."
