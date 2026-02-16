"""Abstract base class for CFS LLM backends."""


class LLMBackend:
    """Interface that all LLM backends must implement."""

    def generate(self, prompt: str, max_tokens: int, temperature: float,
                 stop: list[str]) -> str:
        """Generate text from a prompt. Returns the generated text string."""
        raise NotImplementedError
