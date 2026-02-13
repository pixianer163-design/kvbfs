"""CFS-Local shared configuration."""

import os
from dataclasses import dataclass, field


@dataclass
class Config:
    # Paths
    mount: str = ""
    sessions_subdir: str = "sessions"
    model_path: str = ""

    # LLM parameters
    n_ctx: int = 4096
    n_gpu_layers: int = 0
    max_tokens: int = 512
    temperature: float = 0.7
    stop_tokens: list = field(default_factory=lambda: ["User:"])

    # Daemon behavior
    poll_interval_ms: int = 1000

    @property
    def sessions_dir(self) -> str:
        return os.path.join(self.mount, self.sessions_subdir)

    def validate(self):
        if not self.model_path:
            raise ValueError("CFS_MODEL_PATH must be set")
        if not os.path.isfile(self.model_path):
            raise FileNotFoundError(f"Model not found: {self.model_path}")
        if not self.mount:
            raise ValueError("CFS_MOUNT must be set")

    @classmethod
    def from_env(cls) -> "Config":
        return cls(
            mount=os.environ.get("CFS_MOUNT", "/mnt/kvbfs"),
            model_path=os.environ.get("CFS_MODEL_PATH", ""),
            n_ctx=int(os.environ.get("CFS_N_CTX", "4096")),
            n_gpu_layers=int(os.environ.get("CFS_N_GPU_LAYERS", "0")),
            max_tokens=int(os.environ.get("CFS_MAX_TOKENS", "512")),
            temperature=float(os.environ.get("CFS_TEMPERATURE", "0.7")),
        )
