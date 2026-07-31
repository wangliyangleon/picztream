"""PztClient 的流式 stderr 读取（T-8 A.4）。

改造前 `_run_cancellable` 轮询 `communicate(timeout=)`，而 communicate
超时时输出留在内部缓冲区、拿不到已到达的部分，只有子进程退出那一刻才
一次性交出全部 —— 进度会变成事后回放。

改成两个读取线程之后，最容易写错的是"没人要进度时就不读 stderr"：那会
让输出量大的命令把管道填满、子进程阻塞在 write 上、父进程等在 wait 上，
双方互等。这个风险不是假设的，`core/dedup/dedup.cpp:272` 对候选簇内每一
对比较都无条件往 stderr 打一行明细。

这里的时序/死锁两条用真子进程跑（假 Popen 没有真管道，验不出背压），
解析规则那几条用假 Popen。
"""
from __future__ import annotations

import json
import subprocess
import sys
import threading

from pzt_client import PztClient

# 远超 macOS 管道缓冲区（64KB）：没有无条件排水的话这个子进程写不完。
_FLOOD_LINES = 5000
_FLOOD_LINE = "[pzt dedup] compare " + "x" * 80


def _script_client(script: str) -> PztClient:
    # call() 会在末尾追加 --json，落到 python 的 sys.argv[1]，无害。
    return PztClient(pzt_bin=sys.executable)


def _call_in_thread(client: PztClient, *args, timeout: float = 30.0):
    """在线程里跑并限时。直接调的话，一旦读取端漏了排水，测试会永久挂死
    而不是失败。daemon=True 保证挂死也不拖住 pytest 退出。"""
    box = {}

    def run():
        try:
            box["value"] = client.call(*args)
        except BaseException as e:  # noqa: BLE001 测试里要把异常带回主线程
            box["error"] = e

    t = threading.Thread(target=run, daemon=True)
    t.start()
    t.join(timeout=timeout)
    return t, box


def test_large_stderr_output_does_not_deadlock_without_a_progress_sink():
    # 没挂 sink 也必须排水。这是本次改造最容易写错的一条。
    script = (
        "import sys\n"
        f"for i in range({_FLOOD_LINES}):\n"
        f"    sys.stderr.write({_FLOOD_LINE!r} + '\\n')\n"
        "sys.stdout.write('{\"ok\": true}')\n"
    )
    client = _script_client(script)
    client.cancel_event = threading.Event()  # 布防才走可取消路径，但从不置位

    t, box = _call_in_thread(client, "-c", script)

    assert not t.is_alive(), "读取端没有无条件排水，子进程被管道背压卡死了"
    assert box.get("value") == {"ok": True}


def test_large_stdout_output_does_not_deadlock_either():
    # 反方向同理：只盯 stderr 读、不排 stdout 一样会互等。
    script = (
        "import sys, json\n"
        f"sys.stderr.write({_FLOOD_LINE!r} + '\\n')\n"
        "sys.stdout.write(json.dumps({'blob': 'y' * 200000}))\n"
    )
    client = _script_client(script)
    client.cancel_event = threading.Event()

    t, box = _call_in_thread(client, "-c", script)

    assert not t.is_alive(), "stdout 侧没排水，子进程被管道背压卡死了"
    assert len(box["value"]["blob"]) == 200000


def test_progress_arrives_while_the_process_is_still_running():
    # 这条就是 A.4 存在的理由：communicate 那版能跑完、能拿到全部输出，
    # 但进度全部堆到进程退出那一刻才到，等于事后回放。
    line = json.dumps({"progress": {"phase": "compare", "done": 1, "total": 9}})
    script = (
        "import sys, time\n"
        f"sys.stderr.write({line!r} + '\\n')\n"
        "sys.stderr.flush()\n"
        "time.sleep(5)\n"
        "sys.stdout.write('{\"ok\": true}')\n"
    )
    client = _script_client(script)
    client.cancel_event = threading.Event()
    seen = threading.Event()
    client.progress_sink = lambda phase, done, total: seen.set()

    t, _ = _call_in_thread(client, "-c", script)

    # 子进程要睡 5 秒才退出；2 秒内就收到进度，说明是边跑边读。
    assert seen.wait(timeout=2.0), "进度是子进程退出之后才拿到的，仍然是事后回放"
    t.join(timeout=10)


def test_progress_sink_receives_phase_done_total():
    line = json.dumps({"progress": {"phase": "cluster", "done": 3, "total": 17}})
    script = (
        "import sys\n"
        f"sys.stderr.write({line!r} + '\\n')\n"
        "sys.stdout.write('{\"ok\": true}')\n"
    )
    client = _script_client(script)
    client.cancel_event = threading.Event()
    seen = []
    client.progress_sink = lambda phase, done, total: seen.append((phase, done, total))

    t, _ = _call_in_thread(client, "-c", script)
    t.join(timeout=10)

    assert seen == [("cluster", 3, 17)]


def test_error_object_is_still_the_last_stderr_line_after_progress_lines():
    # _parse_error 取 stderr 的最后一行。命令失败时错误对象一定是最后写
    # 的，前面垫多少进度行都不该影响它 —— 这正是选 stderr 当进度通道的
    # 依据之一，要有钉子守着。
    from pzt_client import PztCommandError

    progress = json.dumps({"progress": {"phase": "cluster", "done": 1, "total": 2}})
    err = json.dumps({"error": "dedup_failed", "message": "boom"})
    script = (
        "import sys\n"
        f"sys.stderr.write({progress!r} + '\\n')\n"
        f"sys.stderr.write({err!r} + '\\n')\n"
        "sys.exit(1)\n"
    )
    client = _script_client(script)
    client.cancel_event = threading.Event()

    t, box = _call_in_thread(client, "-c", script)
    t.join(timeout=10)

    assert isinstance(box.get("error"), PztCommandError)
    assert box["error"].code == "dedup_failed"


def test_unparseable_stderr_lines_are_ignored():
    # dedup 的 F-08 调参明细就是纯文本行，跟进度行混在同一条管道里。解析
    # 不出来是正常情况，不是异常：为自己的格式假设过期而报错，等于把自己
    # 的 bug 报成用户的错。
    good = json.dumps({"progress": {"phase": "cluster", "done": 1, "total": 1}})
    script = (
        "import sys\n"
        "sys.stderr.write('[pzt dedup] compare image_id=1 image_id=2 distance=0\\n')\n"
        "sys.stderr.write('{not json at all\\n')\n"
        "sys.stderr.write('{\"something\": \"else\"}\\n')\n"
        "sys.stderr.write('{\"progress\": \"not an object\"}\\n')\n"
        "sys.stderr.write('{\"progress\": {\"phase\": \"cluster\"}}\\n')\n"
        f"sys.stderr.write({good!r} + '\\n')\n"
        "sys.stdout.write('{\"ok\": true}')\n"
    )
    client = _script_client(script)
    client.cancel_event = threading.Event()
    seen = []
    client.progress_sink = lambda phase, done, total: seen.append((phase, done, total))

    t, box = _call_in_thread(client, "-c", script)
    t.join(timeout=10)

    assert box.get("value") == {"ok": True}
    assert seen == [("cluster", 1, 1)]


def test_a_raising_progress_sink_does_not_kill_the_call():
    # 进度是观测不是结果。下游回调炸了不该让整条命令失败 —— 更要紧的是
    # 别让读取线程死掉，那会退化成"不排水"进而卡死子进程。
    good = json.dumps({"progress": {"phase": "cluster", "done": 1, "total": 1}})
    script = (
        "import sys\n"
        f"sys.stderr.write({good!r} + '\\n')\n"
        f"for i in range({_FLOOD_LINES}):\n"
        f"    sys.stderr.write({_FLOOD_LINE!r} + '\\n')\n"
        "sys.stdout.write('{\"ok\": true}')\n"
    )
    client = _script_client(script)
    client.cancel_event = threading.Event()

    def boom(phase, done, total):
        raise RuntimeError("downstream blew up")

    client.progress_sink = boom

    t, box = _call_in_thread(client, "-c", script)

    assert not t.is_alive(), "回调抛异常打死了读取线程，子进程随后被背压卡死"
    assert box.get("value") == {"ok": True}


def test_unarmed_path_ignores_the_progress_sink():
    # cancel_event 未布防走 subprocess.run，那条路径上的命令都是不需要进
    # 度的短命令，不改。
    def fake_runner(argv):
        return subprocess.CompletedProcess(argv, 0, stdout='{"a": 1}',
                                            stderr=json.dumps({"progress": {"phase": "cluster",
                                                                             "done": 1, "total": 1}}))

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    seen = []
    client.progress_sink = lambda *a: seen.append(a)

    assert client.call("images", "proj-1") == {"a": 1}
    assert seen == []
