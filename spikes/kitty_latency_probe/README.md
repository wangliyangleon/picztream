# Kitty 渲染延迟验证 Spike

一次性验证探针，非生产代码，回答 M0_PRD.md 风险清单里的两个悬而未决的问题：

1. Kitty 图像协议能否直接接收原始 JPEG 字节（绕过任何解码）？
2. 在 Ghostty + Tmux 窗格环境下，把一张全分辨率 JPEG 渲染出来的实际延迟是多少，相对 100ms 目标处于什么水平？

结论见 `results.md`，会被引用进 `docs/M0_Eng_Design.md` 的"渲染延迟验证结论"一节。

## 构建与运行

```
clang++ -std=c++20 -O2 -o probe probe.cpp \
  -framework CoreGraphics -framework ImageIO -framework CoreFoundation
./probe <jpeg1> [jpeg2 ...]
```

探针依赖 macOS 自带的 ImageIO/CoreGraphics 做 JPEG 解码（Apple Silicon 上有硬件加速，且零额外依赖），不引入 libjpeg-turbo 等第三方库。

人类可读的计时日志全部写到 stderr；stdout 只承载真正要发给终端的 Kitty 协议字节，指向真实终端时才有意义，不能与 stderr 混流。

## 已知局限

探针本次是通过 Claude Code 的 Bash 工具执行的：环境变量确认这个 shell 本身跑在 Ghostty + Tmux 里，但 Bash 工具的 stdin/stdout 经过管道转发，并未挂到真实的 pty 上（`test -t 0/1` 为假）。这意味着：

- 解码、文件写入等与 pty 无关的开销测量是可信的
- 真正经过 pty 的 write() 延迟、终端侧的 ACK 往返、以及最终的可视确认，本次都无法采集，需要在真实交互式 shell 里手动重跑 `./probe` 来补齐
