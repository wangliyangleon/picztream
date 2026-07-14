"""子进程调用 pzt 子命令的唯一入口——agent 不链接任何 C++，只经这层跟
core 打交道。见 docs/M4_Eng_Design.md 一、二节"agent → cli → core 单向
依赖的唯一通道"。
"""
from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Callable, List, Optional, Tuple


class PztCommandError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


def default_pzt_bin() -> Path:
    env = os.environ.get("PZT_BIN")
    if env:
        return Path(env)
    # agent/pzt_client.py -> agent/ -> 仓库根 -> build_release/cli/pzt
    repo_root = Path(__file__).resolve().parent.parent
    return repo_root / "build_release" / "cli" / "pzt"


PztRunner = Callable[[List[str]], subprocess.CompletedProcess]


def _real_runner(argv: List[str]) -> subprocess.CompletedProcess:
    return subprocess.run(argv, capture_output=True, text=True)


def _parse_error(stderr: str) -> Tuple[str, str]:
    try:
        last_line = stderr.strip().splitlines()[-1]
        err = json.loads(last_line)
        return err.get("error", "unknown"), err.get("message", "")
    except (json.JSONDecodeError, IndexError):
        return "unknown", stderr.strip() or "pzt command failed with no stderr"


class PztClient:
    def __init__(self, pzt_bin: Optional[Path] = None, runner: Optional[PztRunner] = None) -> None:
        self.pzt_bin = Path(pzt_bin) if pzt_bin else default_pzt_bin()
        self._runner = runner or _real_runner

    def call(self, *args: str) -> dict:
        argv = [str(self.pzt_bin), *args, "--json"]
        proc = self._runner(argv)
        if proc.returncode != 0:
            code, message = _parse_error(proc.stderr)
            raise PztCommandError(code, message)
        return json.loads(proc.stdout.strip())
