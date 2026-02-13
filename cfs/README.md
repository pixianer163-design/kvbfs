# CFS-Local: Computational File System

A Python daemon that turns KVBFS files into LLM chat sessions. Write a message to a file, get an AI response appended back — all persisted on KVBFS.

## Architecture

```
User / SDK / Shell (echo, cat)
       | file I/O
       v
  KVBFS mount (/mnt/kvbfs/)          CFS daemon (daemon.py)
  - persists session files            - watches via inotify
  - standard POSIX semantics          - runs llama-cpp inference
  - RocksDB backend                   - writes responses back
```

## Quick Start

### 1. Build and mount KVBFS

```bash
cd /path/to/kvbfs
mkdir -p build && cd build
cmake .. -DKVBFS_BACKEND=rocksdb && make

export KVBFS_DB_PATH=/tmp/kvbfs_data
mkdir -p /mnt/kvbfs
./kvbfs /mnt/kvbfs -f &
```

### 2. Install CFS dependencies

```bash
cd cfs
pip install -r requirements.txt
```

### 3. Start the CFS daemon

```bash
CFS_MODEL_PATH=/path/to/model.gguf CFS_MOUNT=/mnt/kvbfs python daemon.py
```

Or use the start script:

```bash
./scripts/start.sh /path/to/model.gguf /mnt/kvbfs
```

### 4. Chat via shell

```bash
# First message (creates session file)
echo "User: Hello" > /mnt/kvbfs/sessions/chat1

# Wait for response
while [ -f /mnt/kvbfs/sessions/.generating.chat1 ]; do sleep 0.1; done
cat /mnt/kvbfs/sessions/chat1

# Continue the conversation (MUST use >> to append)
echo "User: Tell me a joke" >> /mnt/kvbfs/sessions/chat1
while [ -f /mnt/kvbfs/sessions/.generating.chat1 ]; do sleep 0.1; done
cat /mnt/kvbfs/sessions/chat1
```

### 5. Chat via Python SDK

```python
from client import Session

with Session(mount="/mnt/kvbfs") as s:
    # Blocking call
    r = s.send("Hello, who are you?")
    print(r.content)

    # Streaming
    for chunk in s.stream("Tell me a short story"):
        print(chunk, end="", flush=True)
    print()

    # Read full history
    print(s.read())
```

## Configuration

All options can be set via environment variables or CLI flags:

| Env Variable | CLI Flag | Default | Description |
|---|---|---|---|
| `CFS_MOUNT` | `--mount` | `/mnt/kvbfs` | KVBFS mount point |
| `CFS_MODEL_PATH` | `--model` | (required) | Path to GGUF model |
| `CFS_MAX_TOKENS` | `--max-tokens` | `512` | Max generation tokens |
| `CFS_TEMPERATURE` | `--temperature` | `0.7` | Sampling temperature |
| `CFS_N_CTX` | `--n-ctx` | `4096` | Context window size |
| `CFS_N_GPU_LAYERS` | `--n-gpu-layers` | `0` | GPU layers to offload |

## File Protocol

Session files live at `/mnt/kvbfs/sessions/<session_id>`:

```
User: first message
Assistant: first response
User: second message
Assistant: second response
```

**Important**: Use `>` for the first message (creates file), `>>` for subsequent messages (appends). Using `>` on an existing session will erase history.

The daemon signals generation status via sentinel files:
- `.generating.<session_id>` exists while generating
- Removed when generation completes

## Testing

```bash
# Unit tests (no KVBFS or model needed)
cd cfs && python -m pytest tests/test_daemon.py -v

# Integration tests with mock daemon (no model needed)
python -m pytest tests/test_client.py -v

# E2E test (requires KVBFS + daemon running)
CFS_MOUNT=/mnt/kvbfs ./tests/test_e2e.sh
```
