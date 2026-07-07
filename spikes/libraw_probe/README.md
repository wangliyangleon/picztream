# LibRaw 性能 Spike(M2 Phase 0)

一次性验证探针,不是生产代码,回答 `docs/M2_PRD.md` 里没有实测数据支撑的几个问题:

1. "内嵌预览提取不触发 LibRaw 全量解码"这个 culling 路径的核心假设,实测耗时是否真的落在 100ms 延迟预算内(沿用 M0/M1 已定的标准)?
2. 内嵌预览的格式(`unpack_thumb`/`dcraw_make_mem_thumb` 返回的 `type`)实际是 JPEG 还是 BITMAP?决定预览提取要不要处理位图分支
3. 富士 X-Trans 阵列的全量解码耗时,是否真的比徕卡拜耳阵列明显更慢,慢多少
4. `pzt export` 遇到需要真正解码的 RAW 图片时,耗时量级是否需要加进度提示

结论见 `results.md`,会在 `docs/M2_Eng_Design.md` 里被引用。

## 构建与运行

```
clang++ -std=c++20 -O2 -fexperimental-library -o probe probe.cpp \
  -I$(brew --prefix libomp)/include \
  $(pkg-config --cflags --libs libraw) \
  -L$(brew --prefix libomp)/lib -lomp
./probe <raw1> [raw2 ...]
```

跟 `spikes/color_lut_probe` 一样需要 `-fexperimental-library`。额外需要显式补 `libomp` 的头文件/链接路径——`pkg-config --cflags libraw` 没有把这个传递依赖的 include 路径带出来(LibRaw 内部用 OpenMP 做部分运算并行化),Homebrew 装的 `libomp` 是 keg-only,不在默认搜索路径里。

依赖 Homebrew 装的 LibRaw 0.22.1(`brew install libraw pkg-config`),这次 spike 之前这台机器上还没装。

## 测试素材

用户本地真实拍摄的 RAW 文件,`~/Pictures/raw_test_files/`(不提交进仓库,不是仓库的一部分,只读引用):

| 文件 | 相机 | 格式 | 阵列类型 | 大小 |
|---|---|---|---|---|
| `L1000708.DNG` | 徕卡 Q3 | DNG | 拜耳(Bayer) | ~83MB |
| `L1000709.DNG` | 徕卡 Q3 | DNG | 拜耳(Bayer) | ~88MB |
| `DSCF5428.RAF` | 富士 X-T5 | RAF | X-Trans | ~87MB |
| `DSCF5429.RAF` | 富士 X-T5 | RAF | X-Trans | ~87MB |

用 `file` 命令确认过格式合法(徕卡 DNG 走 TIFF 容器 + JPEG 压缩、富士 RAF format version 0201)。这两台机身正好对应 `docs/Roadmap.md` 点名的两种去马赛克阵列类型,不需要额外找样本。

## 测的是什么

对每个文件测两组操作,各重复 5 次取最快一次(跟 `color_lut_probe` 同样的方法论):

- **内嵌预览提取**(culling 路径):`unpack_thumb()` + `dcraw_make_mem_thumb()`。每次都重新 `open_file`,避免前一次调用的内部状态影响计时。
- **全量解码**(processing/导出路径):`unpack()` + `dcraw_process()` + `dcraw_make_mem_image()`,`output_bps=8, use_camera_wb=1, output_color=1`(sRGB)——对应 `docs/M2_Eng_Design.md` 已确认的"LibRaw 内部完成白平衡+去马赛克+色彩矩阵+gamma,直接吐 8-bit sRGB"这个输出格式决策。

## 已知局限

跟 `color_lut_probe` 一样,这次纯粹是 CPU/IO 计算耗时,不涉及 pty/终端 I/O,不存在"非真实 tty 导致数据不可信"的局限。样本只有 2 台机身各 2 张,不是穷举所有相机型号的置信区间,但覆盖了 Roadmap 点名的两种阵列类型、且是用户实际会用到的真实器材,对这次要回答的架构问题(预览提取是否够快、全量解码量级、是否需要进度提示)已经够用——M2_PRD.md 本来就把"更多 RAW 格式支持"列为未来考虑,不是这次要验证的范围。
