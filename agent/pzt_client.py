"""子进程调用 pzt 子命令的唯一入口——agent 不链接任何 C++，只经这层跟
core 打交道。见 docs/M4_Eng_Design.md 一、二节"agent → cli → core 单向
依赖的唯一通道"。
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import threading
from pathlib import Path
from typing import Callable, List, Optional, Tuple


class PztCommandError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class PztCancelledError(Exception):
    """用户取消导致的子进程终止。故意不继承 PztCommandError：stages 的
    `except PztCommandError` 会把命令失败降级成 StageOutput(ok=False)，
    而取消必须穿透 stage.run 和 driver.advance 直达 worker 的推进循环，
    走 CANCELLED 收尾而不是 FAILED（见 docs/W2026-07-15_AgentRuntime_
    Eng_Design.md 第六节）。"""

    def __init__(self, argv: List[str]) -> None:
        super().__init__(f"cancelled: {' '.join(argv)}")
        self.argv = argv


def default_pzt_bin() -> Path:
    # 解析顺序:显式 PZT_BIN > 仓库内构建物 > PATH 上的 pzt > 仓库路径兜底。
    # dev 在仓库里用自己刚构建的 build_release/cli/pzt;brew 装的 agent 没有
    # 仓库,回落到 PATH 上 brew 装的 pzt(pzt-agent formula depends_on pzt)。
    env = os.environ.get("PZT_BIN")
    if env:
        return Path(env)
    # agent/pzt_client.py -> agent/ -> 仓库根 -> build_release/cli/pzt
    repo_local = Path(__file__).resolve().parent.parent / "build_release" / "cli" / "pzt"
    if repo_local.exists():
        return repo_local
    found = shutil.which("pzt")
    if found:
        return Path(found)
    return repo_local  # 都没有:回落到约定路径,让后续报错信息清晰


PztRunner = Callable[[List[str]], subprocess.CompletedProcess]


def _real_runner(argv: List[str]) -> subprocess.CompletedProcess:
    return subprocess.run(argv, capture_output=True, text=True)


def _real_popen_factory(argv: List[str]) -> subprocess.Popen:
    return subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def _parse_error(stderr: str) -> Tuple[str, str]:
    try:
        last_line = stderr.strip().splitlines()[-1]
        err = json.loads(last_line)
        return err.get("error", "unknown"), err.get("message", "")
    except (json.JSONDecodeError, IndexError):
        return "unknown", stderr.strip() or "pzt command failed with no stderr"


class PztClient:
    def __init__(self, pzt_bin: Optional[Path] = None, runner: Optional[PztRunner] = None,
                 popen_factory: Optional[Callable[[List[str]], subprocess.Popen]] = None) -> None:
        self.pzt_bin = Path(pzt_bin) if pzt_bin else default_pzt_bin()
        self._runner = runner or _real_runner
        self._popen_factory = popen_factory or _real_popen_factory
        # 布防点：worker 在可杀 stage（Evaluate/Dedup）即将 advance 前把
        # DriveJob 的 cancel_event 挂上来、返回后摘除——挂在实例上而不是
        # call() 参数，stages 内部的 client.call(...) 才能零改动吃到取消
        # 能力。worker 用自己专属的 client 实例，consumer 的只读查询走另
        # 一个实例，互不影响；单线程挂/摘，无并发写。
        self.cancel_event: Optional[threading.Event] = None
        self.kill_grace_seconds = 2.0
        self.poll_interval_seconds = 0.1

    def call(self, *args: str) -> dict:
        argv = [str(self.pzt_bin), *args, "--json"]
        if self.cancel_event is None:
            proc = self._runner(argv)
            returncode, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
        else:
            returncode, stdout, stderr = self._run_cancellable(argv)
        if returncode != 0:
            code, message = _parse_error(stderr)
            raise PztCommandError(code, message)
        return json.loads(stdout.strip())

    def _run_cancellable(self, argv: List[str]) -> Tuple[int, str, str]:
        popen = self._popen_factory(argv)
        while True:
            try:
                # communicate(timeout=) 超时重试不丢输出（subprocess 文档
                # 保证），比 poll()+PIPE 手工排水简单且不会管道死锁。
                stdout, stderr = popen.communicate(timeout=self.poll_interval_seconds)
                return popen.returncode, stdout, stderr
            except subprocess.TimeoutExpired:
                if not self.cancel_event.is_set():
                    continue
                popen.terminate()
                try:
                    popen.communicate(timeout=self.kill_grace_seconds)
                except subprocess.TimeoutExpired:
                    popen.kill()
                    popen.communicate()
                raise PztCancelledError(argv)
