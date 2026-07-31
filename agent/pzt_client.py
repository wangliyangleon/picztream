"""子进程调用 pzt 子命令的唯一入口——agent 不链接任何 C++，只经这层跟
core 打交道。见 docs/history/M4_Eng_Design.md 一、二节"agent → cli → core 单向
依赖的唯一通道"。
"""
from __future__ import annotations

import json
import logging
import os
import shutil
import subprocess
import threading
from pathlib import Path
from typing import Callable, List, Optional, Tuple


_log = logging.getLogger("pzt.agent.client")


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


ProgressFn = Callable[[str, int, int], None]


def _parse_progress(line: str) -> Optional[Tuple[str, int, int]]:
    """把一行 stderr 解析成 (phase, done, total)，不是进度行就返回 None。

    宽进严出，全程不抛：stderr 上混着 dedup 的 F-08 调参明细这种纯文本行
    （`core/dedup/dedup.cpp:272` 对每一对比较都打一行），解析不出来是正常
    情况而不是异常。为自己的格式假设过期而报错，等于把自己的 bug 报成用
    户的错 —— 跟启动预检"模型清单解析不出来就闭嘴"同一条原则。

    schema 见 docs/Headless_Observability_Eng_Design.md 决策一。"""
    line = line.strip()
    if not line.startswith("{"):
        return None
    try:
        obj = json.loads(line)
    except (json.JSONDecodeError, ValueError):
        return None
    if not isinstance(obj, dict):
        return None
    payload = obj.get("progress")
    if not isinstance(payload, dict):
        return None
    done, total = payload.get("done"), payload.get("total")
    if not isinstance(done, int) or not isinstance(total, int):
        return None
    phase = payload.get("phase")
    return (phase if isinstance(phase, str) else "", done, total)


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
        # 布防点：worker 在可杀 stage（见 session.worker.KILLABLE_STAGES）
        # 即将运行前把
        # DriveJob 的 cancel_event 挂上来、返回后摘除——挂在实例上而不是
        # call() 参数，stages 内部的 client.call(...) 才能零改动吃到取消
        # 能力。worker 用自己专属的 client 实例，consumer 的只读查询走另
        # 一个实例，互不影响；单线程挂/摘，无并发写。
        self.cancel_event: Optional[threading.Event] = None
        # 同一个套路的第二个布防点（T-8）：stage 在调用前后挂上/摘掉，
        # 收到的是 (phase, done, total)。只有可取消路径（cancel_event 已
        # 布防）才会流式读 stderr —— 走 subprocess.run 的都是不需要进度的
        # 短命令。挂了也不影响正确性：进度是观测，丢了不改变结果。
        self.progress_sink: Optional[ProgressFn] = None
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
        """两个读取线程各排一条管道，主线程只轮询取消。

        这里曾经是轮询 communicate(timeout=)，选它的理由（"比 poll()+PIPE
        手工排水简单且不会管道死锁"）今天依然成立，但它不流式：超时时输
        出留在内部缓冲区、拿不到已到达的部分，只有子进程退出那一刻才一次
        性交出，进度会变成事后回放（T-8）。

        换成手工排水就要自己扛那个死锁：只盯一条管道读的话，另一条填满后
        子进程会阻塞在 write 上、父进程等在 wait 上，双方互等。所以两条各
        起一条线程、无条件读到 EOF，主线程一行输出都不读。

        用线程而不是 selectors：两条管道逻辑上没有交织，线程版就是两个循
        环；selectors 要手工处理半行缓冲、EOF 注销，还要跟取消轮询挤在同
        一个循环里。这条路径不是性能敏感的（分钟级任务里的两条线程）。"""
        popen = self._popen_factory(argv)
        out_chunks: List[str] = []
        err_chunks: List[str] = []
        readers = [
            threading.Thread(target=self._drain, args=(popen.stdout, out_chunks, None), daemon=True),
            threading.Thread(target=self._drain, args=(popen.stderr, err_chunks, self.progress_sink),
                              daemon=True),
        ]
        for reader in readers:
            reader.start()

        cancelled = False
        while True:
            try:
                popen.wait(timeout=self.poll_interval_seconds)
                break
            except subprocess.TimeoutExpired:
                if not self.cancel_event.is_set():
                    continue
                cancelled = True
                popen.terminate()
                try:
                    popen.wait(timeout=self.kill_grace_seconds)
                except subprocess.TimeoutExpired:
                    popen.kill()
                    popen.wait()
                break

        # 必须等排水线程收干净再返回：子进程已退出但管道里可能还有内容，
        # 提前返回会截断 stdout，_parse_error 也可能读不到最后那行错误。
        for reader in readers:
            reader.join()
        if cancelled:
            raise PztCancelledError(argv)
        return popen.returncode, "".join(out_chunks), "".join(err_chunks)

    @staticmethod
    def _drain(pipe, chunks: List[str], progress_sink: Optional[ProgressFn]) -> None:
        """无条件读到 EOF。**没人要进度也照读**——不读就是不排水，输出量
        大的命令会被管道背压卡死（dedup 的 F-08 明细行量级是 Σ C(簇大小,2)，
        几十个中等簇就能越过 64KB 管道缓冲区）。

        iter(readline, "") 而不是 `for line in pipe`：后者带 read-ahead 缓
        冲，会把已经到达的行攒着不交出来，流式就白做了。"""
        if pipe is None:
            return
        try:
            for line in iter(pipe.readline, ""):
                chunks.append(line)
                if progress_sink is None:
                    continue
                parsed = _parse_progress(line)
                if parsed is None:
                    continue
                try:
                    progress_sink(*parsed)
                except Exception:  # noqa: BLE001
                    # 下游炸了不能把这条线程带走：线程死了就不再排水，
                    # 子进程随后被背压卡死，一个播报 bug 升级成挂死。
                    _log.warning("[pzt] 进度回调抛异常，已忽略", exc_info=True)
        finally:
            try:
                pipe.close()
            except Exception:  # noqa: BLE001 管道已被 terminate 关掉是正常情况
                pass
