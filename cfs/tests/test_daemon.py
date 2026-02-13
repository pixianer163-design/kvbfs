"""Unit tests for CFS daemon protocol functions."""

import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from daemon import (
    build_prompt,
    cleanup_stale_sentinels,
    create_sentinel,
    needs_response,
    remove_sentinel,
    sentinel_path,
    trim_to_context,
)


# ---------------------------------------------------------------------------
# needs_response()
# ---------------------------------------------------------------------------

class TestNeedsResponse:
    def test_empty_content(self):
        assert needs_response("") is False

    def test_whitespace_only(self):
        assert needs_response("   \n\n  ") is False

    def test_single_user_message(self):
        assert needs_response("User: hello\n") is True

    def test_answered_single_turn(self):
        assert needs_response("User: hello\nAssistant: hi\n") is False

    def test_unanswered_second_turn(self):
        content = "User: hello\nAssistant: hi\nUser: bye\n"
        assert needs_response(content) is True

    def test_answered_multi_turn(self):
        content = "User: a\nAssistant: b\nUser: c\nAssistant: d\n"
        assert needs_response(content) is False

    def test_no_prefix(self):
        assert needs_response("random text\n") is False

    def test_assistant_without_user(self):
        assert needs_response("Assistant: orphan\n") is False


# ---------------------------------------------------------------------------
# build_prompt()
# ---------------------------------------------------------------------------

class TestBuildPrompt:
    def test_single_turn(self):
        prompt = build_prompt("User: hello\n")
        assert prompt == "User: hello\nAssistant: "

    def test_multi_turn(self):
        content = "User: a\nAssistant: b\nUser: c\n"
        prompt = build_prompt(content)
        assert prompt.endswith("\nAssistant: ")
        assert "User: a" in prompt
        assert "Assistant: b" in prompt
        assert "User: c" in prompt

    def test_trailing_newlines_stripped(self):
        prompt = build_prompt("User: x\n\n\n")
        assert prompt == "User: x\nAssistant: "


# ---------------------------------------------------------------------------
# trim_to_context()
# ---------------------------------------------------------------------------

class TestTrimToContext:
    def test_short_content_unchanged(self):
        content = "User: hi\nAssistant: hello\n"
        assert trim_to_context(content, 1000) == content

    def test_long_content_trimmed(self):
        content = ""
        for i in range(100):
            content += f"User: question {i}\nAssistant: answer {i}\n"
        trimmed = trim_to_context(content, 200)
        assert len(trimmed) <= 200
        # Last turn should be preserved
        assert "answer 99" in trimmed

    def test_minimum_two_lines(self):
        content = "User: very long message " + "x" * 500 + "\n"
        content += "Assistant: very long response " + "y" * 500 + "\n"
        trimmed = trim_to_context(content, 100)
        # Should keep at least the last two lines even if over limit
        lines = [l for l in trimmed.splitlines() if l.strip()]
        assert len(lines) >= 2


# ---------------------------------------------------------------------------
# Sentinel operations
# ---------------------------------------------------------------------------

class TestSentinel:
    def test_sentinel_path_format(self):
        path = sentinel_path("/mnt/kvbfs/sessions", "chat1")
        assert path == "/mnt/kvbfs/sessions/.generating.chat1"

    def test_create_and_remove(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            create_sentinel(tmpdir, "test_session")
            spath = sentinel_path(tmpdir, "test_session")
            assert os.path.isfile(spath)

            remove_sentinel(tmpdir, "test_session")
            assert not os.path.isfile(spath)

    def test_remove_nonexistent(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            # Should not raise
            remove_sentinel(tmpdir, "nonexistent")

    def test_cleanup_stale_sentinels(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            # Create a session file and a stale sentinel
            session_file = os.path.join(tmpdir, "stale_session")
            with open(session_file, "w") as f:
                f.write("User: hello\n")

            stale = os.path.join(tmpdir, ".generating.stale_session")
            open(stale, "w").close()

            cleanup_stale_sentinels(tmpdir)

            # Sentinel should be removed
            assert not os.path.isfile(stale)

            # Error message should be appended
            with open(session_file) as f:
                content = f.read()
            assert "Assistant: [Error: Daemon restarted" in content
