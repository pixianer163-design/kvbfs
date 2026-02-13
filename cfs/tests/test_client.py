"""Integration tests for CFS Python SDK.

Uses a mock daemon thread that simulates LLM generation by watching
for User: lines and appending mock Assistant: responses.
"""

import os
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from client import Response, Session
from daemon import needs_response


class MockDaemon(threading.Thread):
    """Simulates CFS daemon: watches for User: lines, writes mock responses."""

    def __init__(self, sessions_dir: str):
        super().__init__(daemon=True)
        self.sessions_dir = sessions_dir
        self.stop_event = threading.Event()

    def run(self):
        while not self.stop_event.is_set():
            try:
                for name in os.listdir(self.sessions_dir):
                    if name.startswith("."):
                        continue
                    path = os.path.join(self.sessions_dir, name)
                    if not os.path.isfile(path):
                        continue

                    with open(path, "r", encoding="utf-8") as f:
                        content = f.read()

                    if needs_response(content):
                        sentinel = os.path.join(
                            self.sessions_dir, f".generating.{name}"
                        )
                        open(sentinel, "w").close()
                        time.sleep(0.05)  # simulate inference time
                        with open(path, "a", encoding="utf-8") as f:
                            f.write("Assistant: Mock response.\n")
                        try:
                            os.unlink(sentinel)
                        except FileNotFoundError:
                            pass
            except Exception:
                pass
            time.sleep(0.05)

    def stop(self):
        self.stop_event.set()


# ---------------------------------------------------------------------------
# Response model tests
# ---------------------------------------------------------------------------

class TestResponse:
    def test_content_parsing(self):
        r = Response(
            session_id="test",
            raw_content="User: hi\nAssistant: hello there\n",
        )
        assert r.content == "hello there"
        assert len(r.history) == 2
        assert r.is_error is False

    def test_error_detection(self):
        r = Response(
            session_id="test",
            raw_content="User: hi\nAssistant: [Error: model failed]\n",
        )
        assert r.is_error is True
        assert "model failed" in r.content

    def test_empty_content(self):
        r = Response(session_id="test", raw_content="")
        assert r.content == ""
        assert r.history == []

    def test_multi_turn_content(self):
        r = Response(
            session_id="test",
            raw_content="User: a\nAssistant: b\nUser: c\nAssistant: d\n",
        )
        assert r.content == "d"
        assert len(r.history) == 4


# ---------------------------------------------------------------------------
# Session tests with MockDaemon
# ---------------------------------------------------------------------------

class TestSession:
    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()
        self.daemon = MockDaemon(self.tmpdir)
        self.daemon.start()

    def teardown_method(self):
        self.daemon.stop()
        self.daemon.join(timeout=2.0)
        import shutil
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_send_basic(self):
        s = Session(session_id="test_send", sessions_dir=self.tmpdir)
        with s:
            r = s.send("Hello", timeout=5.0)
        assert r.content == "Mock response."
        assert len(r.history) == 2

    def test_read_raw(self):
        s = Session(session_id="test_read", sessions_dir=self.tmpdir)
        with s:
            s.send("Hello", timeout=5.0)
            raw = s.read()
        assert "User: Hello" in raw
        assert "Assistant: Mock response." in raw

    def test_multi_turn(self):
        s = Session(session_id="test_multi", sessions_dir=self.tmpdir)
        with s:
            r1 = s.send("First", timeout=5.0)
            assert len(r1.history) == 2
            r2 = s.send("Second", timeout=5.0)
            assert len(r2.history) == 4

    def test_session_from_file(self):
        s = Session(session_id="test_from_file", sessions_dir=self.tmpdir)
        with s:
            s.send("Hello", timeout=5.0)

        s2 = Session.from_file("test_from_file", sessions_dir=self.tmpdir)
        r = s2.read_response()
        assert len(r.history) == 2

    def test_delete(self):
        s = Session(session_id="test_delete", sessions_dir=self.tmpdir)
        with s:
            s.send("Hello", timeout=5.0)
        s.delete()
        assert not s.session_path.exists()

    def test_stream(self):
        s = Session(session_id="test_stream", sessions_dir=self.tmpdir)
        with s:
            chunks = list(s.stream("Tell me something", timeout=5.0))
        # Should have received at least one chunk
        combined = "".join(chunks)
        assert "Assistant:" in combined or "Mock response" in combined
