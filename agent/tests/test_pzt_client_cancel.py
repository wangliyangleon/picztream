"""PztClient 可取消调用路径（docs/W2026-07-15_AgentRuntime_Eng_Design.md
第六节）：worker 把 DriveJob 的 cancel_event 挂到自己 client 实例上，
call() 感知置位后 terminate 子进程、宽限后 kill、抛 PztCancelledError。
PztCancelledError 故意不是 PztCommandError 的子类——stages 的
`except PztCommandError` 不能吞掉它，它必须穿透 stage.run 和
driver.advance 直达 worker 的推进循环。
"""
from __future__ import annotations

import subprocess
import threading

import pytest

from pzt_client import PztCancelledError, PztClient, PztCommandError


class FakePopen:
    """可控 Popen 替身：finish_after 次 communicate(timeout=...) 超时后
    才结束；terminate/kill 各自记录调用并（可配置地）让进程结束。"""

    def __init__(self, argv, stdout='{"ok": true}', returncode=0,
                 finish_after=0, stubborn=False):
        self.argv = argv
        self._stdout = stdout
        self._returncode_when_done = returncode
        self._timeouts_left = finish_after
        self._stubborn = stubborn  # terminate 后仍不退出，逼出 kill()
        self.terminated = False
        self.killed = False
        self.returncode = None

    def communicate(self, timeout=None):
        if self.killed or (self.terminated and not self._stubborn):
            self.returncode = -15
            return "", ""
        if self._timeouts_left > 0:
            self._timeouts_left -= 1
            raise subprocess.TimeoutExpired(self.argv, timeout)
        if self.terminated and self._stubborn:
            raise subprocess.TimeoutExpired(self.argv, timeout)
        self.returncode = self._returncode_when_done
        return self._stdout, ""

    def terminate(self):
        self.terminated = True

    def kill(self):
        self.killed = True


def _client(fake_popen_holder: dict, **popen_kwargs) -> PztClient:
    def factory(argv):
        fake_popen_holder["popen"] = FakePopen(argv, **popen_kwargs)
        return fake_popen_holder["popen"]

    client = PztClient(pzt_bin="/fake/pzt", popen_factory=factory)
    return client


def test_armed_but_never_cancelled_call_returns_parsed_stdout():
    holder = {}
    client = _client(holder, stdout='{"submitted": 3}')
    client.cancel_event = threading.Event()  # 布防但从不置位

    result = client.call("eval", "proj-1")

    assert result == {"submitted": 3}
    assert holder["popen"].argv == ["/fake/pzt", "eval", "proj-1", "--json"]
    assert not holder["popen"].terminated


def test_cancel_terminates_subprocess_and_raises_cancelled():
    holder = {}
    client = _client(holder, finish_after=5)
    client.cancel_event = threading.Event()
    client.cancel_event.set()

    with pytest.raises(PztCancelledError):
        client.call("eval", "proj-1")

    assert holder["popen"].terminated
    assert not holder["popen"].killed


def test_stubborn_subprocess_gets_killed_after_grace():
    holder = {}
    client = _client(holder, finish_after=5, stubborn=True)
    client.kill_grace_seconds = 0.01  # 测试不真等 2 秒
    client.cancel_event = threading.Event()
    client.cancel_event.set()

    with pytest.raises(PztCancelledError):
        client.call("eval", "proj-1")

    assert holder["popen"].terminated
    assert holder["popen"].killed


def test_cancelled_error_is_not_a_command_error():
    # worker 靠这个类型区分"用户取消"和"命令失败"，stages 的
    # except PztCommandError 也靠这个不把取消吞成 StageOutput(ok=False)。
    assert not issubclass(PztCancelledError, PztCommandError)


def test_armed_call_with_nonzero_exit_still_raises_command_error():
    def factory(argv):
        return FakePopen(argv, stdout="", returncode=1)

    client = PztClient(pzt_bin="/fake/pzt", popen_factory=factory)
    client.cancel_event = threading.Event()

    with pytest.raises(PztCommandError):
        client.call("eval", "missing-proj")


def test_unarmed_call_never_touches_popen_factory():
    def factory(argv):
        raise AssertionError("cancel_event 未布防时不该走 Popen 路径")

    def fake_runner(argv):
        return subprocess.CompletedProcess(argv, 0, stdout='{"a": 1}', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner, popen_factory=factory)

    assert client.call("images", "proj-1") == {"a": 1}
