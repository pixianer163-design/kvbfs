"""LlamaCpp backend: wraps llama-cpp-python for local GGUF model inference."""

import logging

from backends.base import LLMBackend
from config import Config

log = logging.getLogger("cfs")


class LlamaCppBackend(LLMBackend):
    """Local LLM inference via llama-cpp-python."""

    def __init__(self, config: Config):
        from llama_cpp import Llama

        log.info("Loading model from '%s'...", config.model_path)
        self._llm = Llama(
            model_path=config.model_path,
            n_ctx=config.n_ctx,
            n_gpu_layers=config.n_gpu_layers,
            verbose=False,
        )
        log.info("Model loaded successfully")

    def generate(self, prompt: str, max_tokens: int, temperature: float,
                 stop: list[str]) -> str:
        output = self._llm(
            prompt,
            max_tokens=max_tokens,
            temperature=temperature,
            stop=stop,
        )
        return output["choices"][0]["text"].strip()
