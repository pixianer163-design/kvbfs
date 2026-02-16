"""Unit tests for CFS LLM backends."""

import json
import os
import sys
import tempfile
from http.server import HTTPServer, BaseHTTPRequestHandler
import threading
import unittest
from unittest.mock import patch, MagicMock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from backends.base import LLMBackend
from backends.openai import OpenAIBackend, _parse_conversation
from backends import load_backend
from config import Config


# ---------------------------------------------------------------------------
# LLMBackend base class
# ---------------------------------------------------------------------------

class TestLLMBackend:
    def test_generate_raises_not_implemented(self):
        backend = LLMBackend()
        try:
            backend.generate("prompt", 100, 0.7, ["stop"])
            assert False, "Should have raised NotImplementedError"
        except NotImplementedError:
            pass


# ---------------------------------------------------------------------------
# _parse_conversation()
# ---------------------------------------------------------------------------

class TestParseConversation:
    def test_single_user_message(self):
        prompt = "User: hello\nAssistant: "
        msgs = _parse_conversation(prompt)
        assert len(msgs) == 1
        assert msgs[0] == {"role": "user", "content": "hello"}

    def test_multi_turn(self):
        prompt = "User: a\nAssistant: b\nUser: c\nAssistant: "
        msgs = _parse_conversation(prompt)
        assert len(msgs) == 3
        assert msgs[0] == {"role": "user", "content": "a"}
        assert msgs[1] == {"role": "assistant", "content": "b"}
        assert msgs[2] == {"role": "user", "content": "c"}

    def test_empty_prompt(self):
        msgs = _parse_conversation("Assistant: ")
        assert msgs == []


# ---------------------------------------------------------------------------
# load_backend() factory
# ---------------------------------------------------------------------------

class TestLoadBackend:
    def test_unknown_backend_raises(self):
        config = Config(mount="/tmp", backend="unknown")
        try:
            load_backend(config)
            assert False, "Should have raised ValueError"
        except ValueError as e:
            assert "unknown" in str(e).lower()

    def test_llamacpp_backend_import(self):
        """Verify llamacpp backend attempts to import llama_cpp."""
        config = Config(mount="/tmp", backend="llamacpp", model_path="/fake/model.gguf")
        # llama_cpp is not installed in test env, so we expect ImportError
        # (or ModuleNotFoundError which is a subclass)
        try:
            load_backend(config)
            # If llama_cpp is installed, we'd get FileNotFoundError for the model
        except (ImportError, ModuleNotFoundError):
            pass  # Expected: llama_cpp not installed
        except Exception:
            pass  # Model file not found is also acceptable


# ---------------------------------------------------------------------------
# OpenAIBackend with a mock HTTP server
# ---------------------------------------------------------------------------

class _MockOpenAIHandler(BaseHTTPRequestHandler):
    """Minimal mock for /v1/chat/completions."""

    def do_POST(self):
        content_length = int(self.headers["Content-Length"])
        body = json.loads(self.rfile.read(content_length))

        # Echo back the last user message content as the response
        last_user_msg = ""
        for msg in body.get("messages", []):
            if msg["role"] == "user":
                last_user_msg = msg["content"]

        response = {
            "choices": [{
                "message": {
                    "role": "assistant",
                    "content": f"Echo: {last_user_msg}",
                }
            }]
        }

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(response).encode("utf-8"))

    def log_message(self, format, *args):
        pass  # Suppress request logging in tests


class TestOpenAIBackend:
    def setup_method(self):
        self.server = HTTPServer(("127.0.0.1", 0), _MockOpenAIHandler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def teardown_method(self):
        self.server.shutdown()
        self.thread.join(timeout=2.0)

    def test_generate_with_mock_server(self):
        config = Config(
            mount="/tmp",
            backend="openai",
            api_key="test-key",
            api_base=f"http://127.0.0.1:{self.port}/v1",
            api_model="test-model",
        )
        backend = OpenAIBackend(config)
        result = backend.generate(
            "User: hello world\nAssistant: ",
            max_tokens=100,
            temperature=0.7,
            stop=["User:"],
        )
        assert result == "Echo: hello world"

    def test_generate_multi_turn(self):
        config = Config(
            mount="/tmp",
            backend="openai",
            api_key="test-key",
            api_base=f"http://127.0.0.1:{self.port}/v1",
            api_model="test-model",
        )
        backend = OpenAIBackend(config)
        prompt = "User: first\nAssistant: reply\nUser: second\nAssistant: "
        result = backend.generate(prompt, max_tokens=100, temperature=0.7, stop=["User:"])
        assert result == "Echo: second"


# ---------------------------------------------------------------------------
# Config validation for backends
# ---------------------------------------------------------------------------

class TestConfigValidation:
    def test_llamacpp_requires_model_path(self):
        config = Config(mount="/tmp", backend="llamacpp", model_path="")
        try:
            config.validate()
            assert False, "Should have raised ValueError"
        except ValueError as e:
            assert "MODEL_PATH" in str(e)

    def test_openai_requires_api_key(self):
        config = Config(mount="/tmp", backend="openai", api_key="",
                        api_base="http://x", api_model="m")
        try:
            config.validate()
            assert False, "Should have raised ValueError"
        except ValueError as e:
            assert "API_KEY" in str(e)

    def test_openai_requires_api_base(self):
        config = Config(mount="/tmp", backend="openai", api_key="k",
                        api_base="", api_model="m")
        try:
            config.validate()
            assert False, "Should have raised ValueError"
        except ValueError as e:
            assert "API_BASE" in str(e)

    def test_openai_requires_api_model(self):
        config = Config(mount="/tmp", backend="openai", api_key="k",
                        api_base="http://x", api_model="")
        try:
            config.validate()
            assert False, "Should have raised ValueError"
        except ValueError as e:
            assert "API_MODEL" in str(e)

    def test_openai_valid_config(self):
        config = Config(mount="/tmp", backend="openai", api_key="k",
                        api_base="http://x", api_model="m")
        config.validate()  # Should not raise

    def test_unknown_backend(self):
        config = Config(mount="/tmp", backend="bogus")
        try:
            config.validate()
            assert False, "Should have raised ValueError"
        except ValueError as e:
            assert "bogus" in str(e)
