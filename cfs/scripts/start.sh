#!/bin/bash
# Start KVBFS filesystem and CFS daemon together.
#
# Usage:
#   ./start.sh /path/to/model.gguf [mountpoint] [db_path]
#
# Environment variables (optional):
#   CFS_N_GPU_LAYERS  - GPU layers to offload (default: 0)
#   CFS_MAX_TOKENS    - Max generation tokens (default: 512)
#   CFS_TEMPERATURE   - Sampling temperature (default: 0.7)

set -e

MODEL_PATH="${1:?Usage: $0 <model_path> [mountpoint] [db_path]}"
MOUNTPOINT="${2:-/mnt/kvbfs}"
DB_PATH="${3:-/tmp/kvbfs_data}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CFS_DIR="$(dirname "$SCRIPT_DIR")"
KVBFS_DIR="$(dirname "$CFS_DIR")"

# Find KVBFS binary
KVBFS_BIN=""
for candidate in "$KVBFS_DIR/build/kvbfs" "$KVBFS_DIR/build-rocksdb/kvbfs"; do
    if [ -x "$candidate" ]; then
        KVBFS_BIN="$candidate"
        break
    fi
done

if [ -z "$KVBFS_BIN" ]; then
    echo "Error: kvbfs binary not found. Build it first with cmake." >&2
    exit 1
fi

# Create directories
mkdir -p "$MOUNTPOINT"
mkdir -p "$DB_PATH"

# Start KVBFS
echo "Starting KVBFS at $MOUNTPOINT (db: $DB_PATH)..."
export KVBFS_DB_PATH="$DB_PATH"
"$KVBFS_BIN" "$MOUNTPOINT" -f &
KVBFS_PID=$!
echo "KVBFS PID: $KVBFS_PID"

# Wait for mount to be ready
sleep 1
if ! mountpoint -q "$MOUNTPOINT" 2>/dev/null; then
    echo "Waiting for KVBFS mount..."
    for i in $(seq 1 10); do
        sleep 1
        mountpoint -q "$MOUNTPOINT" 2>/dev/null && break
    done
fi

# Start CFS daemon
echo "Starting CFS daemon (model: $MODEL_PATH)..."
export CFS_MOUNT="$MOUNTPOINT"
export CFS_MODEL_PATH="$MODEL_PATH"
python3 "$CFS_DIR/daemon.py" &
CFS_PID=$!
echo "CFS daemon PID: $CFS_PID"

# Save PIDs for stop script
PID_FILE="/tmp/cfs-local.pids"
echo "KVBFS_PID=$KVBFS_PID" > "$PID_FILE"
echo "CFS_PID=$CFS_PID" >> "$PID_FILE"
echo "MOUNTPOINT=$MOUNTPOINT" >> "$PID_FILE"

echo ""
echo "CFS-Local is running."
echo "  Sessions dir: $MOUNTPOINT/sessions/"
echo "  PID file: $PID_FILE"
echo ""
echo "Quick test:"
echo "  echo 'User: Hello' > $MOUNTPOINT/sessions/test"
echo "  sleep 3 && cat $MOUNTPOINT/sessions/test"
echo ""
echo "To stop: $(dirname "$0")/stop.sh"

wait
