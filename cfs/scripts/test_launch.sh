#!/bin/bash
# Launch CFS daemon for testing. Run inside WSL.
set -e

source /tmp/cfs-venv/bin/activate

export CFS_MOUNT=/tmp/kvbfs
export CFS_MODEL_PATH=/mnt/d/project/kvbfs/cfs/models/qwen2.5-0.5b-instruct-q4_k_m.gguf

cd /mnt/d/project/kvbfs/cfs

# Kill any existing daemon
if [ -f /tmp/cfs-daemon.pid ]; then
    OLD_PID=$(cat /tmp/cfs-daemon.pid)
    kill "$OLD_PID" 2>/dev/null || true
    sleep 1
fi

# Start daemon
nohup python3 daemon.py > /tmp/cfs-daemon.log 2>&1 &
DAEMON_PID=$!
echo "$DAEMON_PID" > /tmp/cfs-daemon.pid
echo "CFS daemon started with PID $DAEMON_PID"
echo "Waiting for model to load..."
sleep 12

if ps -p "$DAEMON_PID" > /dev/null 2>&1; then
    echo "Daemon is running."
    tail -5 /tmp/cfs-daemon.log
else
    echo "Daemon failed!"
    cat /tmp/cfs-daemon.log
    exit 1
fi
