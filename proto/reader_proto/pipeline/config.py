"""用户配置：各 provider 的命令、模型、API key，以及缓存目录。

位置（按顺序找第一个）：
  1. 环境变量 MIDI_READER_CONFIG 指向的文件
  2. Windows：%APPDATA%\\midi-reader\\providers.json；其他：~/.config/midi-reader/providers.json

API key 只放这里或环境变量，不进 sidecar、不进 run.json、不进仓库。
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Any

ENV_CONFIG = "MIDI_READER_CONFIG"
ENV_CACHE = "MIDI_READER_CACHE"

# 环境变量覆盖：provider id → {配置字段: 环境变量名}
ENV_OVERRIDES = {"sunoapi": {"apiKey": "SUNOAPI_KEY"}}


def config_path() -> Path:
    env = os.environ.get(ENV_CONFIG)
    if env:
        return Path(env)
    if sys.platform == "win32" and os.environ.get("APPDATA"):
        return Path(os.environ["APPDATA"]) / "midi-reader" / "providers.json"
    return Path.home() / ".config" / "midi-reader" / "providers.json"


def default_cache_dir() -> Path:
    env = os.environ.get(ENV_CACHE)
    if env:
        return Path(env)
    if sys.platform == "win32" and os.environ.get("LOCALAPPDATA"):
        return Path(os.environ["LOCALAPPDATA"]) / "midi-reader" / "cache" / "pipeline"
    return Path.home() / ".cache" / "midi-reader" / "pipeline"


class Config:
    def __init__(self, data: dict[str, Any] | None = None, path: Path | None = None):
        self.data = data or {}
        self.path = path

    @staticmethod
    def load(path: Path | None = None) -> "Config":
        p = path or config_path()
        data = json.loads(p.read_text(encoding="utf-8")) if p.exists() else {}
        return Config(data, p)

    def provider(self, pid: str) -> dict[str, Any]:
        cfg = dict(self.data.get("providers", {}).get(pid, {}))
        for key, env in ENV_OVERRIDES.get(pid, {}).items():
            if os.environ.get(env):
                cfg[key] = os.environ[env]
        return cfg

    @property
    def cache_dir(self) -> Path:
        c = self.data.get("cacheDir")
        return Path(c) if c else default_cache_dir()
