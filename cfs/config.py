"""CFS-Local shared configuration."""

import os
from dataclasses import dataclass, field


@dataclass
class Config:
    # Paths
    mount: str = ""
    sessions_subdir: str = "sessions"
    model_path: str = ""

    # Backend selection
    backend: str = "llamacpp"  # "llamacpp" or "openai"

    # LLM parameters (shared)
    n_ctx: int = 4096
    n_gpu_layers: int = 0
    max_tokens: int = 512
    temperature: float = 0.7
    stop_tokens: list = field(default_factory=lambda: ["User:"])

    # OpenAI-compatible API parameters
    api_key: str = ""
    api_base: str = ""
    api_model: str = ""

    # Daemon behavior
    poll_interval_ms: int = 1000

    @property
    def sessions_dir(self) -> str:
        return os.path.join(self.mount, self.sessions_subdir)

    def validate(self):
        if not self.mount:
            raise ValueError("CFS_MOUNT must be set")
        if self.backend == "llamacpp":
            if not self.model_path:
                raise ValueError("CFS_MODEL_PATH must be set for llamacpp backend")
            if not os.path.isfile(self.model_path):
                raise FileNotFoundError(f"Model not found: {self.model_path}")
        elif self.backend == "openai":
            if not self.api_key:
                raise ValueError("CFS_API_KEY must be set for openai backend")
            if not self.api_base:
                raise ValueError("CFS_API_BASE must be set for openai backend")
            if not self.api_model:
                raise ValueError("CFS_API_MODEL must be set for openai backend")
        else:
            raise ValueError(f"Unknown backend: {self.backend!r}")

    @classmethod
    def from_env(cls) -> "Config":
        return cls(
            mount=os.environ.get("CFS_MOUNT", "/mnt/kvbfs"),
            model_path=os.environ.get("CFS_MODEL_PATH", ""),
            backend=os.environ.get("CFS_BACKEND", "llamacpp"),
            n_ctx=int(os.environ.get("CFS_N_CTX", "4096")),
            n_gpu_layers=int(os.environ.get("CFS_N_GPU_LAYERS", "0")),
            max_tokens=int(os.environ.get("CFS_MAX_TOKENS", "512")),
            temperature=float(os.environ.get("CFS_TEMPERATURE", "0.7")),
            api_key=os.environ.get("CFS_API_KEY", ""),
            api_base=os.environ.get("CFS_API_BASE", ""),
            api_model=os.environ.get("CFS_API_MODEL", ""),
        )
