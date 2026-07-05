#!/usr/bin/env python3
"""One-off headless runner for the M0 PRD's 500+ image acceptance criterion
("browsing 500+ images has no perceptible stutter, verified with a simple
latency log"). Not part of ctest - this drives a real `pzt open` session
through a pty and inspects the exit-time key-to-render summary (added in
increment 6.4.7). See README.md for the full methodology and known
limitations (synthetic solid-color JPEGs decode faster than real photos, so
this catches scaling regressions across n=500, not exact per-decode cost).

Usage:
  python3 run_acceptance.py <pzt_binary> <project_name>

The project must already exist (run generate.cpp's output through
`pzt new <project_name> <dataset_dir>` first) and should have 500+ images.
Must be pointed at a RelWithDebInfo build, not the project's default
Debug+ASan build - ASan inflates decode time by roughly an order of
magnitude, confirmed earlier this session, and isn't representative of real
user-perceived latency.
"""
import os
import pty
import fcntl
import termios
import struct
import sys
import time
import select
import re


def run(pzt_binary, project_name):
    master_fd, slave_fd = pty.openpty()
    # 必须带非零的像素尺寸,否则 get_terminal_size() 会判定无效、悄悄退回
    # 一个不真实的窄默认宽度(cli/term/screen.h,这次已经踩过的坑)。
    fcntl.ioctl(slave_fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 960, 640))

    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.close(master_fd)
        os.dup2(slave_fd, 0)
        os.dup2(slave_fd, 1)
        os.dup2(slave_fd, 2)
        os.execvp(pzt_binary, [pzt_binary, "open", project_name])
        os._exit(1)

    os.close(slave_fd)
    time.sleep(0.5)

    output = b""

    def drain(duration):
        nonlocal output
        deadline = time.time() + duration
        while time.time() < deadline:
            r, _, _ = select.select([master_fd], [], [], 0.1)
            if r:
                try:
                    chunk = os.read(master_fd, 65536)
                    if not chunk:
                        break
                    output += chunk
                except OSError:
                    break

    # 一长串 l 走完整个项目,中间每 50 张左右插几次 h 往回走(压一下预取
    # 窗口反向驱逐/重新填充路径),每 100 张左右按一次 x(最简单的单键
    # 打标签路径),中途在第 250 张附近切一次 g+0 筛选再 g+g 清除(这是
    # 唯一有固有冷解码成本的操作,顺带确认 filter_by_tag 那行日志)。
    step = 0
    target_steps = 520  # 略多于 500,覆盖整个项目再多走几步
    while step < target_steps:
        os.write(master_fd, b"l")
        step += 1
        if step % 50 == 0:
            os.write(master_fd, b"h")
            step += 1
        if step % 100 == 0:
            os.write(master_fd, b"x")
        if step == 250:
            os.write(master_fd, b"g0")
            time.sleep(0.3)
            os.write(master_fd, b"gg")
        # 10ms 间隔比任何真人按键都快得多,也远超单线程解码吞吐量(约
        # 50-70ms/张),会让预取窗口永远追不上、每一步都接近冷解码——这测的
        # 是"导航速度超过解码吞吐量"这种压力场景,不是 PRD 真正关心的"正常
        # 速度浏览是否流畅"。改成 120ms,更接近真实快速翻片的节奏,给预取
        # 线程留出跟上的空间。
        drain(0.12)

    os.write(master_fd, b"q")
    drain(1.0)

    text = output.decode("utf-8", errors="replace")
    return text


def parse_summary(text):
    m = re.search(
        r"key-to-render summary: n=(\d+) avg=([\d.]+)ms p95=([\d.]+)ms max=([\d.]+)ms",
        text,
    )
    if not m:
        return None
    return {
        "n": int(m.group(1)),
        "avg": float(m.group(2)),
        "p95": float(m.group(3)),
        "max": float(m.group(4)),
    }


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <pzt_binary> <project_name>", file=sys.stderr)
        sys.exit(1)

    text = run(sys.argv[1], sys.argv[2])
    summary = parse_summary(text)
    if summary is None:
        print("FAIL: 没有看到 key-to-render summary,进程可能崩溃或卡死", file=sys.stderr)
        sys.exit(1)

    print(f"n={summary['n']} avg={summary['avg']:.2f}ms p95={summary['p95']:.2f}ms "
          f"max={summary['max']:.2f}ms")

    # 参考阈值,不是 PRD 本身的硬性数字——PRD 原文是主观的"无可感知卡顿",
    # 这几个数字是给无头验证用的代理判据。
    #
    # max 的阈值特意放宽到 600ms:实测(见 README.md)确认整个会话里
    # key-to-render 的最大值只出现一次,就是打开项目看到第一张图那一帧
    # ——预取缓存是空的,第一张图不可能被预先解码过,这是任何交互式看图
    # 工具都躲不掉的、一次性的冷启动成本,不是反复出现的问题,不应该跟
    # "导航过程中散落出现的高峰"用同一个严格阈值衡量。真正体现"浏览过程
    # 流不流畅"的是 avg/p95——这两个已经排除了单次冷启动的影响。
    ok = summary["avg"] < 50.0 and summary["p95"] < 100.0 and summary["max"] < 600.0
    if ok:
        print("PASS(参考阈值内,仍需真机主观确认)")
    else:
        print("超出参考阈值,需要人工核查(筛选切换、会话第一帧这类孤立的一次性"
              "高峰是预期内的,但普通 l/h 走动里散落出现类似高峰说明预取窗口"
              "跟不上)")
    sys.exit(0 if ok else 1)
