"""PztClient 的开销行解析（票 10）。

跟进度共用同一条 stderr 带外通道、同一条读取线程，区别只在 key：
`{"cost": {...}}` 是"这一趟 AI 要花多少"，`{"progress": {...}}` 是"跑到
哪了"。分成两个 sink 而不是一个带 kind 的，是因为下游对两者的处置完全
不同 - 进度节流后原地编辑一条消息，开销必须**立刻**发一条新消息并带上
可取消入口，晚一分钟就失去意义。

宽进严出这条规矩照抄进度：解析不出来的行是正常情况（stderr 上还混着
dedup 的 F-08 调参明细），不抛异常、不打日志噪声。
"""
from __future__ import annotations

import json
import subprocess
import threading

from pzt_client import PztClient

from tests.test_pzt_client_progress import _call_in_thread, _script_client


def test_cost_sink_receives_comparisons_and_evaluations():
    line = json.dumps({"cost": {"comparisons": 18, "evaluations": 6}})
    script = (
        "import sys\n"
        f"sys.stderr.write({line!r} + '\\n')\n"
        "sys.stdout.write('{\"ok\": true}')\n"
    )
    client = _script_client(script)
    client.cancel_event = threading.Event()
    seen = []
    client.cost_sink = lambda comparisons, evaluations: seen.append((comparisons, evaluations))

    t, box = _call_in_thread(client, "-c", script)
    t.join(timeout=10)

    assert box.get("value") == {"ok": True}
    assert seen == [(18, 6)]


def test_cost_arrives_while_the_process_is_still_running():
    """开销行的全部价值就在于**早**：它报的是接下来几分钟要花的钱，等子
    进程退出才交出来的话，用户是在钱花完之后才被告知的。"""
    line = json.dumps({"cost": {"comparisons": 4, "evaluations": 0}})
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
    client.cost_sink = lambda comparisons, evaluations: seen.set()

    t, _ = _call_in_thread(client, "-c", script)

    assert seen.wait(timeout=2.0), "开销是子进程退出之后才拿到的，用户已经等完了整趟"
    t.join(timeout=10)


def test_progress_and_cost_sinks_do_not_cross_talk():
    """两种消息在同一条管道上交错。各认各的 key - 进度行喂进 cost_sink
    会让用户收到一条凭空的"要跑 3 次"，反过来会把开销当成进度吞掉。"""
    progress = json.dumps({"progress": {"phase": "compare", "done": 3, "total": 9}})
    cost = json.dumps({"cost": {"comparisons": 9, "evaluations": 2}})
    script = (
        "import sys\n"
        f"sys.stderr.write({cost!r} + '\\n')\n"
        f"sys.stderr.write({progress!r} + '\\n')\n"
        "sys.stdout.write('{\"ok\": true}')\n"
    )
    client = _script_client(script)
    client.cancel_event = threading.Event()
    costs, progresses = [], []
    client.cost_sink = lambda c, e: costs.append((c, e))
    client.progress_sink = lambda phase, done, total: progresses.append((phase, done, total))

    t, _ = _call_in_thread(client, "-c", script)
    t.join(timeout=10)

    assert costs == [(9, 2)]
    assert progresses == [("compare", 3, 9)]


def test_malformed_cost_lines_are_ignored():
    good = json.dumps({"cost": {"comparisons": 1, "evaluations": 1}})
    script = (
        "import sys\n"
        "sys.stderr.write('[pzt dedup] compare image_id=1 image_id=2\\n')\n"
        "sys.stderr.write('{\"cost\": \"not an object\"}\\n')\n"
        "sys.stderr.write('{\"cost\": {\"comparisons\": 1}}\\n')\n"
        "sys.stderr.write('{\"cost\": {\"comparisons\": \"x\", \"evaluations\": 1}}\\n')\n"
        f"sys.stderr.write({good!r} + '\\n')\n"
        "sys.stdout.write('{\"ok\": true}')\n"
    )
    client = _script_client(script)
    client.cancel_event = threading.Event()
    seen = []
    client.cost_sink = lambda c, e: seen.append((c, e))

    t, box = _call_in_thread(client, "-c", script)
    t.join(timeout=10)

    assert box.get("value") == {"ok": True}
    assert seen == [(1, 1)]


def test_a_raising_cost_sink_does_not_kill_the_call():
    """同 progress_sink：回调炸了不能带走读取线程，否则不排水 -> 背压卡死
    子进程，一个播报 bug 升级成挂死。"""
    cost = json.dumps({"cost": {"comparisons": 1, "evaluations": 0}})
    from tests.test_pzt_client_progress import _FLOOD_LINE, _FLOOD_LINES

    script = (
        "import sys\n"
        f"sys.stderr.write({cost!r} + '\\n')\n"
        f"for i in range({_FLOOD_LINES}):\n"
        f"    sys.stderr.write({_FLOOD_LINE!r} + '\\n')\n"
        "sys.stdout.write('{\"ok\": true}')\n"
    )
    client = _script_client(script)
    client.cancel_event = threading.Event()

    def boom(comparisons, evaluations):
        raise RuntimeError("downstream blew up")

    client.cost_sink = boom

    t, box = _call_in_thread(client, "-c", script)

    assert not t.is_alive(), "回调抛异常打死了读取线程，子进程随后被背压卡死"
    assert box.get("value") == {"ok": True}


def test_unarmed_path_ignores_the_cost_sink():
    # 同进度：cancel_event 未布防的短命令走 subprocess.run，不流式读。
    def fake_runner(argv):
        return subprocess.CompletedProcess(
            argv, 0, stdout='{"a": 1}',
            stderr=json.dumps({"cost": {"comparisons": 1, "evaluations": 1}}))

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    seen = []
    client.cost_sink = lambda *a: seen.append(a)

    assert client.call("images", "proj-1") == {"a": 1}
    assert seen == []
