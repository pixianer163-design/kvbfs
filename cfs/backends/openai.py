"""OpenAI-compatible API backend for CFS.

Supports any API that implements the OpenAI /v1/chat/completions endpoint:
DeepSeek, OpenAI, vLLM, Ollama, etc.
"""

import json
import logging
import re
import urllib.request
import urllib.error

from backends.base import LLMBackend
from config import Config

log = logging.getLogger("cfs")


def _parse_conversation(prompt: str) -> list[dict]:
    """Convert CFS User:/Assistant: format into OpenAI messages array.

    The prompt ends with 'Assistant: ' (the generation cue), which we strip.
    """
    # Remove trailing "Assistant: " cue (with or without leading newline)
    text = re.sub(r"^Assistant:\s*$", "", prompt)
    text = re.sub(r"\nAssistant:\s*$", "", text)

    messages = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("User: "):
            messages.append({"role": "user", "content": line[len("User: "):]})
        elif line.startswith("Assistant: "):
            messages.append({"role": "assistant", "content": line[len("Assistant: "):]})
        else:
            # Treat unstructured lines as system context
            if messages and messages[-1]["role"] == "system":
                messages[-1]["content"] += "\n" + line
            else:
                messages.append({"role": "system", "content": line})

    return messages


class OpenAIBackend(LLMBackend):
    """OpenAI-compatible chat completions API backend."""

    def __init__(self, config: Config):
        self._api_key = config.api_key
        self._base_url = config.api_base.rstrip("/")
        self._model = config.api_model
        log.info("OpenAI backend: model=%s, base_url=%s", self._model, self._base_url)

    def generate(self, prompt: str, max_tokens: int, temperature: float,
                 stop: list[str]) -> str:
        messages = _parse_conversation(prompt)

        payload = {
            "model": self._model,
            "messages": messages,
            "max_tokens": max_tokens,
            "temperature": temperature,
            "stop": stop if stop else None,
        }

        url = f"{self._base_url}/chat/completions"
        data = json.dumps(payload).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {self._api_key}",
        }

        req = urllib.request.Request(url, data=data, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                result = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"API request failed ({e.code}): {body}") from e

        return result["choices"][0]["message"]["content"].strip()
