"""按内容寻址的结果缓存：同一个 provider 版本 + 同样参数 + 同样输入，只跑一次。

分离和扒谱都很慢，远程 API 还要钱，所以每一步先查缓存。缓存条目是一个目录：
  <cache>/<key[:2]>/<key>/result.json   产物列表（路径相对本目录）
  <cache>/<key[:2]>/<key>/...           provider 写出的文件
跑的时候写在 <key>.partial/ 里，成功后改名，半截的结果不会被当成命中。
"""
from __future__ import annotations

import hashlib
import json
import shutil
from pathlib import Path
from typing import Any

from .base import Artifact, Provider, Task

RESULT = "result.json"


def cache_key(provider: Provider, task: Task) -> str:
    ident = {
        "provider": provider.manifest.id,
        "version": provider.manifest.version,
        "identity": provider.cache_identity(),
        "capability": task.capability.value,
        "params": task.params,
        "inputs": [a.fingerprint() for a in task.inputs],
    }
    blob = json.dumps(ident, sort_keys=True, ensure_ascii=False, separators=(",", ":"))
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()


class Cache:
    def __init__(self, root: Path | None):
        self.root = Path(root) if root is not None else None

    def _dir(self, key: str) -> Path:
        assert self.root is not None
        return self.root / key[:2] / key

    def get(self, key: str) -> list[Artifact] | None:
        if self.root is None:
            return None
        d = self._dir(key)
        f = d / RESULT
        if not f.exists():
            return None
        data = json.loads(f.read_text(encoding="utf-8"))
        arts = [Artifact.from_json(x, d) for x in data["artifacts"]]
        if any(a.path is not None and not a.path.exists() for a in arts):
            return None
        return arts

    def begin(self, key: str) -> Path:
        """给 provider 一个空的工作目录。没有缓存时用临时目录由调用方给。"""
        assert self.root is not None
        d = self._dir(key).with_name(key + ".partial")
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
        return d

    def commit(self, key: str, workdir: Path, artifacts: list[Artifact], info: dict[str, Any]) -> list[Artifact]:
        final = self._dir(key)
        (workdir / RESULT).write_text(json.dumps(
            {"artifacts": [a.to_json(workdir) for a in artifacts], **info},
            ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        if final.exists():
            shutil.rmtree(final)
        workdir.rename(final)
        return [Artifact.from_json(a.to_json(workdir), final) for a in artifacts]

    def abort(self, workdir: Path) -> None:
        shutil.rmtree(workdir, ignore_errors=True)
