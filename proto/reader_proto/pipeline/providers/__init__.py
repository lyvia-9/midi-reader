"""内置 provider 登记表。加新的 provider：写一个 Provider 子类，在这里登记。"""
from __future__ import annotations

from ..base import Provider
from ..config import Config
from .basic_pitch import BasicPitch
from .demucs import Demucs
from .fake import Fake
from .muscriptor import MuScriptor
from .sunoapi import SunoApi

BUILTIN: dict[str, type[Provider]] = {
    cls.manifest.id: cls for cls in (MuScriptor, Demucs, BasicPitch, SunoApi, Fake)
}


def create(pid: str, config: Config | None = None) -> Provider:
    if pid not in BUILTIN:
        raise KeyError(f"没有这个 provider：{pid}（可选：{', '.join(BUILTIN)}）")
    cfg = config.provider(pid) if config is not None else {}
    return BUILTIN[pid](cfg)
