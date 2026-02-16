"""CFS-Local daemon: watches KVBFS mount for session file changes,
triggers LLM inference via configurable backends, writes responses back.

Usage:
    # Local model (llamacpp backend, default):
    CFS_MODEL_PATH=/path/to/model.gguf CFS_MOUNT=/mnt/kvbfs python daemon.py

    # OpenAI-compatible API (openai backend):
    CFS_BACKEND=openai CFS_API_KEY=sk-xxx CFS_API_BASE=https://api.deepseek.com/v1 \
        CFS_API_MODEL=deepseek-chat CFS_MOUNT=/mnt/kvbfs python daemon.py
"""

import argparse
import glob
import logging
import os
import queue
import signal
import sys
import threading
import time

import inotify_simple

from config import Config

log = logging.getLogger("cfs")


# ---------------------------------------------------------------------------
# File protocol helpers
# ---------------------------------------------------------------------------

def needs_response(content: str) -> bool:
    """Return True if the file has an unanswered User: message."""
    lines = [line for line in content.splitlines() if line.strip()]
    if not lines:
        return False
    last_user_idx = -1
    last_assistant_idx = -1
    for i, line in enumerate(lines):
        if line.startswith("User:"):
            last_user_idx = i
        elif line.startswith("Assistant:"):
            last_assistant_idx = i
    return last_user_idx > last_assistant_idx


def build_prompt(content: str) -> str:
    """Build LLM prompt from conversation history."""
    return content.rstrip("\n") + "\nAssistant: "


def trim_to_context(content: str, max_chars: int) -> str:
    """Drop oldest turn pairs if content exceeds max_chars."""
    lines = content.splitlines(keepends=True)
    while len("".join(lines)) > max_chars and len(lines) > 2:
        lines = lines[2:]  # drop one User: + Assistant: pair
    return "".join(lines)


# ---------------------------------------------------------------------------
# Sentinel helpers
# ---------------------------------------------------------------------------

def sentinel_path(sessions_dir: str, session_id: str) -> str:
    return os.path.join(sessions_dir, f".generating.{session_id}")


def create_sentinel(sessions_dir: str, session_id: str):
    path = sentinel_path(sessions_dir, session_id)
    open(path, "w").close()


def remove_sentinel(sessions_dir: str, session_id: str):
    path = sentinel_path(sessions_dir, session_id)
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def cleanup_stale_sentinels(sessions_dir: str):
    """Clean up sentinel files left by a previous crashed daemon."""
    pattern = os.path.join(sessions_dir, ".generating.*")
    for spath in glob.glob(pattern):
        session_id = os.path.basename(spath).removeprefix(".generating.")
        session_file = os.path.join(sessions_dir, session_id)
        if os.path.isfile(session_file):
            with open(session_file, "a", encoding="utf-8") as f:
                f.write("Assistant: [Error: Daemon restarted during generation]\n")
        try:
            os.unlink(spath)
        except FileNotFoundError:
            pass
        log.warning("Cleaned stale sentinel for session '%s'", session_id)


# ---------------------------------------------------------------------------
# Generation worker
# ---------------------------------------------------------------------------

def process_session(backend, config: Config, session_path: str, session_id: str):
    """Read session file, run LLM inference, append response."""
    sessions_dir = config.sessions_dir

    with open(session_path, "r", encoding="utf-8") as f:
        content = f.read()

    if not needs_response(content):
        return

    create_sentinel(sessions_dir, session_id)
    try:
        prompt_content = trim_to_context(content, config.n_ctx * 3)
        prompt = build_prompt(prompt_content)

        response_text = backend.generate(
            prompt,
            max_tokens=config.max_tokens,
            temperature=config.temperature,
            stop=config.stop_tokens,
        )
    except Exception as e:
        response_text = f"[Error: {str(e)[:200]}]"
        log.error("Generation failed for '%s': %s", session_id, e)

    with open(session_path, "a", encoding="utf-8") as f:
        f.write(f"Assistant: {response_text}\n")

    remove_sentinel(sessions_dir, session_id)
    log.info("Generated response for session '%s'", session_id)


def worker_loop(backend, config: Config, gen_queue: queue.Queue,
                shutdown: threading.Event):
    """Worker thread: process generation tasks from queue."""
    while not shutdown.is_set():
        try:
            session_path, session_id = gen_queue.get(timeout=1.0)
        except queue.Empty:
            continue
        try:
            process_session(backend, config, session_path, session_id)
        except Exception as e:
            log.error("Unhandled error for '%s': %s", session_id, e)
        finally:
            gen_queue.task_done()


# ---------------------------------------------------------------------------
# inotify event loop
# ---------------------------------------------------------------------------

def handle_event(event, sessions_dir: str, gen_queue: queue.Queue):
    """Handle a single inotify event."""
    filename = event.name
    if not filename or filename.startswith("."):
        return
    session_path = os.path.join(sessions_dir, filename)
    if not os.path.isfile(session_path):
        return
    log.debug("Event on '%s', enqueuing for processing", filename)
    gen_queue.put((session_path, filename))


def event_loop(inotify_fd, sessions_dir: str, gen_queue: queue.Queue,
               shutdown: threading.Event, poll_ms: int):
    """Main thread: read inotify events until shutdown."""
    while not shutdown.is_set():
        events = inotify_fd.read(timeout=poll_ms)
        for event in events:
            handle_event(event, sessions_dir, gen_queue)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def wait_for_mount(mount: str, timeout: float = 30.0):
    """Wait until mount point is available."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.isdir(mount):
            return
        log.info("Waiting for mount at '%s'...", mount)
        time.sleep(2.0)
    raise RuntimeError(f"Mount point '{mount}' not available after {timeout}s")


def load_backend(config: Config):
    """Create and return the configured LLM backend."""
    from backends import load_backend as _load_backend
    return _load_backend(config)


def parse_args() -> Config:
    """Parse CLI args, falling back to environment variables."""
    parser = argparse.ArgumentParser(description="CFS-Local daemon")
    parser.add_argument("--model", help="Path to GGUF model file")
    parser.add_argument("--mount", help="KVBFS mount point")
    parser.add_argument("--n-ctx", type=int, help="Context window size")
    parser.add_argument("--n-gpu-layers", type=int, help="GPU layers to offload")
    parser.add_argument("--max-tokens", type=int, help="Max generation tokens")
    parser.add_argument("--temperature", type=float, help="Sampling temperature")
    parser.add_argument("--backend", choices=["llamacpp", "openai"],
                        help="LLM backend to use")
    parser.add_argument("--api-key", help="API key for OpenAI-compatible backend")
    parser.add_argument("--api-base", help="API base URL for OpenAI-compatible backend")
    parser.add_argument("--api-model", help="Model name for OpenAI-compatible backend")
    args = parser.parse_args()

    config = Config.from_env()
    if args.model:
        config.model_path = args.model
    if args.mount:
        config.mount = args.mount
    if args.n_ctx is not None:
        config.n_ctx = args.n_ctx
    if args.n_gpu_layers is not None:
        config.n_gpu_layers = args.n_gpu_layers
    if args.max_tokens is not None:
        config.max_tokens = args.max_tokens
    if args.temperature is not None:
        config.temperature = args.temperature
    if args.backend:
        config.backend = args.backend
    if args.api_key:
        config.api_key = args.api_key
    if args.api_base:
        config.api_base = args.api_base
    if args.api_model:
        config.api_model = args.api_model
    return config


def main():
    logging.basicConfig(
        level=logging.INFO,
        format="[%(asctime)s] [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    config = parse_args()
    config.validate()

    # Wait for KVBFS mount
    wait_for_mount(config.mount)

    # Create sessions directory
    sessions_dir = config.sessions_dir
    os.makedirs(sessions_dir, exist_ok=True)
    log.info("Sessions directory: %s", sessions_dir)

    # Cleanup stale sentinels from previous crash
    cleanup_stale_sentinels(sessions_dir)

    # Load LLM backend
    backend = load_backend(config)

    # Setup inotify
    inotify_fd = inotify_simple.INotify()
    flags = inotify_simple.flags.CLOSE_WRITE | inotify_simple.flags.MOVED_TO
    inotify_fd.add_watch(sessions_dir, flags)
    log.info("Watching '%s' for session file changes", sessions_dir)

    # Shutdown coordination
    shutdown = threading.Event()
    gen_queue = queue.Queue()

    def on_signal(sig, _frame):
        log.info("Signal %d received, shutting down...", sig)
        shutdown.set()

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    # Start worker thread
    worker = threading.Thread(
        target=worker_loop,
        args=(backend, config, gen_queue, shutdown),
        daemon=True,
    )
    worker.start()

    # Main event loop
    try:
        event_loop(inotify_fd, sessions_dir, gen_queue, shutdown,
                   config.poll_interval_ms)
    finally:
        shutdown.set()
        gen_queue.join()
        log.info("CFS daemon exited cleanly")


if __name__ == "__main__":
    main()
