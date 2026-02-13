"""CFS-Local Python SDK: Session-based interface to the CFS daemon.

All operations map to standard file I/O on the KVBFS mount point.
The SDK maintains no internal state — the file is the source of truth.

Usage:
    from client import Session

    with Session() as s:
        r = s.send("Hello")
        print(r.content)

        for chunk in s.stream("Tell me a joke"):
            print(chunk, end="", flush=True)
"""

import os
import time
import uuid
from pathlib import Path
from typing import Iterator, Optional

from pydantic import BaseModel, computed_field


class Response(BaseModel):
    """Structured response parsed from a CFS session file."""

    session_id: str
    raw_content: str

    @computed_field
    @property
    def history(self) -> list[str]:
        """All non-empty lines in the conversation file."""
        return [line for line in self.raw_content.splitlines() if line.strip()]

    @computed_field
    @property
    def content(self) -> str:
        """Content of the last Assistant: reply, without the prefix."""
        for line in reversed(self.history):
            if line.startswith("Assistant: "):
                return line[len("Assistant: "):]
        return ""

    @computed_field
    @property
    def is_error(self) -> bool:
        """Whether the last response is an error from the daemon."""
        return self.content.startswith("[Error:")


class Session:
    """A conversation session backed by a file on the KVBFS mount.

    Each method maps directly to file operations:
        Session()       -> creates/opens file
        send(msg)       -> write("User: msg\\n") + poll + read()
        read()          -> read() entire file
        stream(msg)     -> write() + loop read() yielding deltas
        close()         -> no-op (file preserved for cat/grep)
        delete()        -> unlink()
    """

    DEFAULT_MOUNT = "/mnt/kvbfs"
    SESSIONS_SUBDIR = "sessions"
    POLL_INTERVAL = 0.1
    POLL_TIMEOUT = 120.0

    def __init__(
        self,
        session_id: Optional[str] = None,
        mount: Optional[str] = None,
        sessions_dir: Optional[str] = None,
    ):
        self.session_id = session_id or str(uuid.uuid4())
        mount = mount or os.environ.get("CFS_MOUNT", self.DEFAULT_MOUNT)

        if sessions_dir:
            self._sessions_dir = Path(sessions_dir)
        else:
            self._sessions_dir = Path(mount) / self.SESSIONS_SUBDIR

        self.session_path = self._sessions_dir / self.session_id
        self.sentinel_path = self._sessions_dir / f".generating.{self.session_id}"

    # -- Context Manager --

    def __enter__(self) -> "Session":
        self._sessions_dir.mkdir(parents=True, exist_ok=True)
        self.session_path.touch(exist_ok=True)  # Maps to: open(O_CREAT)
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        pass  # File preserved for Unix tool inspection

    # -- Core Operations --

    def send(self, message: str, timeout: Optional[float] = None) -> Response:
        """Write user message, wait for LLM generation, return response.

        Maps to: write("User: <msg>\\n") -> poll sentinel -> read()
        """
        self._append_user_message(message)
        self._wait_for_generation(timeout or self.POLL_TIMEOUT)
        return self.read_response()

    def read(self) -> str:
        """Read full conversation content from session file.

        Maps to: open(path, 'r').read()
        """
        try:
            return self.session_path.read_text(encoding="utf-8")
        except FileNotFoundError:
            return ""

    def read_response(self) -> Response:
        """Read and parse conversation into a structured Response."""
        return Response(session_id=self.session_id, raw_content=self.read())

    def stream(self, message: str, timeout: Optional[float] = None) -> Iterator[str]:
        """Write message, then yield incremental text as the response grows.

        Maps to: write() -> loop read() detecting file size growth
        """
        self._append_user_message(message)
        last_size = self.session_path.stat().st_size
        deadline = time.monotonic() + (timeout or self.POLL_TIMEOUT)
        stable_count = 0

        while time.monotonic() < deadline:
            time.sleep(self.POLL_INTERVAL)
            try:
                current_size = self.session_path.stat().st_size
            except FileNotFoundError:
                break

            if current_size > last_size:
                with open(self.session_path, "r", encoding="utf-8") as f:
                    f.seek(last_size)
                    chunk = f.read(current_size - last_size)
                last_size = current_size
                stable_count = 0
                if chunk:
                    yield chunk
            else:
                stable_count += 1

            # Stop when sentinel is gone and file size has stabilized
            if not self.sentinel_path.exists() and stable_count >= 3:
                break

    def close(self) -> None:
        """No-op: file is preserved on KVBFS for inspection."""
        pass

    def delete(self) -> None:
        """Explicitly remove the session file from KVBFS.

        Maps to: unlink()
        """
        try:
            self.session_path.unlink()
        except FileNotFoundError:
            pass

    @classmethod
    def from_file(cls, session_id: str, **kwargs) -> "Session":
        """Open an existing session by ID."""
        s = cls(session_id=session_id, **kwargs)
        if not s.session_path.exists():
            raise FileNotFoundError(
                f"Session '{session_id}' not found at {s.session_path}"
            )
        return s

    # -- Private Helpers --

    def _append_user_message(self, message: str) -> None:
        """Append 'User: <message>\\n' and close the file.

        Maps to: open(O_APPEND|O_WRONLY) -> write() -> close()
        Closing the file triggers IN_CLOSE_WRITE in the daemon.
        """
        with open(self.session_path, "a", encoding="utf-8") as f:
            f.write(f"User: {message}\n")

    def _wait_for_generation(self, timeout: float) -> None:
        """Poll until the daemon completes generation.

        Watches for sentinel lifecycle (created then removed) OR file growth
        with no sentinel present (daemon processed faster than poll interval).
        """
        size_before = self.session_path.stat().st_size
        deadline = time.monotonic() + timeout
        started = False

        while time.monotonic() < deadline:
            time.sleep(self.POLL_INTERVAL)
            sentinel_exists = self.sentinel_path.exists()

            if sentinel_exists:
                started = True

            if not sentinel_exists:
                current_size = self.session_path.stat().st_size
                if current_size > size_before:
                    # File grew and no sentinel — generation complete
                    # (either we saw the sentinel before, or daemon was
                    # fast enough that sentinel appeared and vanished
                    # between polls)
                    return

        raise TimeoutError(
            f"Generation for session '{self.session_id}' "
            f"did not complete within {timeout}s"
        )
