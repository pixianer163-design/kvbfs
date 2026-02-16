"""CFS LLM backend factory."""

from backends.base import LLMBackend
from config import Config


def load_backend(config: Config) -> LLMBackend:
    """Create and return the configured LLM backend."""
    if config.backend == "llamacpp":
        from backends.llamacpp import LlamaCppBackend
        return LlamaCppBackend(config)
    elif config.backend == "openai":
        from backends.openai import OpenAIBackend
        return OpenAIBackend(config)
    else:
        raise ValueError(f"Unknown backend: {config.backend!r}")
