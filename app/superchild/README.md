# superchild —— 板端应用

开机直接进入表情界面，通过裸 TCP 连到网关与 StepFun Realtime 做实时语音对话。

设计动机、实测依据见仓库根目录 `架构-实时语音v3.md`。
操作步骤见 `操作卡-实时对话.md`。

---

## 文件

| 文件 | 作用 | 备注 |
|---|---|---|
| `sc_proto.h` | 线上协议 | ⚠️ 与 `server/superchild_gateway/protocol.py` **必须逐字一致** |
| `sc_mood.h/.c` | 心情三轴 + 平滑 + 断链回落 | 纯逻辑，可用主机编译器单测 |
| `sc_face.h/.c` | 24 表情 + 10 维 + 动画 + 装饰 + 跨任务投递队列 | **唯一碰 LVGL 的文件** |
| `sc_audio.h/.c` | ALSA 采集/播放 + 环形缓冲 + 电平 | aw-alsa-lib |
| `sc_microute.h/.c` | 采集通路使能（R528 必踩的坑） | 移植自 `board/baidu_voice/mic_route.c` |
| `sc_net.h/.c` | TCP 长连接 + 帧解析 + 重连 | |
| `sc_main.c` | 入口 + 4 个任务 | |
| `Kconfig` / `Make.defs` / `Makefile` | 构建注册 | 被集成到 `packages/demos/superchild/` |

---

## 四个任务与线程规则

```
main / UI 任务    LVGL 初始化 + 表情 + 60ms 一帧    ← 唯一能调 LVGL 的任务
sc_net 任务       TCP 收发 / 重连                   ← 改界面走 sc_face_post_*
sc_cap 任务       麦克风 → 上行队列
sc_play 任务      下行队列 → 喇叭
```

**LVGL 不是线程安全的。** 除了 UI 任务，任何任务都不能直接调
`sc_face_set*`，必须走 `sc_face_post_*`（互斥锁保护的环形队列，
UI 任务在自己的 tick 里消费）。这条规矩一旦破了，症状是随机花屏/崩溃，
而且**取决于两个任务恰好撞在同一帧**，极难复现。

音频电平（驱动嘴形）是例外：它是两个 `volatile int`，直接覆盖，不进队列 ——
每秒更新几十次，塞队列只会把队列挤爆，而且它不需要保序。

---

## 音频

- **采样率固定 16kHz / 单声道 / 16bit。** R4 实测这个档位最稳。
- StepFun 那边是 24kHz，**重采样在网关上做**。板子侧不碰。
- 通路使能必须在打开采集设备**之前**做，见 `sc_microute.h` 的长注释
  （那条 `capture only support 1~3 channel` 报错与声道数毫无关系）。
- 增益目标：ADC 数字增益 205（厂商默认 160 低约 20dB）。

## 表情

`sc_face.c` 里的 `g_looks[]` 表是**唯一需要改视觉的地方**，24 行 × 17 列。
表头意思见 `sc_face.h` 的 `struct sc_look`。

表情编号是**协议契约**，只能往后加，不能改已发布的编号
（`sc_proto.h` 的 `enum sc_face` 与 `server/protocol.py` 的 `Face` 一一对应）。

优先级：**瞬时表情 > 会话状态 > 心情**。判定在 `decide_face()` 里，
心情到脸的映射在 `sc_mood.c` 里 —— ⚠️ 后者必须与网关的
`emotion.py::FaceDirector.idle_face()` **保持同一套规则**，否则会出现
"网关认为该笑、板子显示困"的拉扯，表现为表情在两帧之间反复横跳。

---

## 主机端单测（不用板子）

`sc_mood.c` 是纯逻辑，可以直接在 PC 上编：

```bash
gcc -I. -c sc_mood.c -o /tmp/sc_mood.o          # 只验证能编过
```

⚠️ `sc_face.c` / `sc_net.c` / `sc_audio.c` 依赖 LVGL / NuttX / aw-alsa-lib
头文件，**没法在主机上编**，只能靠真机交叉编译验证。

---

## 集成与构建

```bash
# 在源码树所在机器上
cd /root/build
OPENVELA_SRC=/root/openvela bash scripts/integrate_superchild.sh   # 集成 + 自启挂接
nohup bash scripts/ecs_build.sh > /root/fw_build.log 2>&1 &        # 编译 + 打包
```

### 两个已经踩过的坑（改代码前先看）

1) **不要往厂商板级文件里加 `#include`。**
   `integrate_superchild.sh` 挂自启时只声明函数原型、不 include 任何头。
   实测加了 `#include <syslog.h>` 会让 `r528_appinit.c` 编译失败，报错却是
   ```
   nuttx/include/sys/select.h:87: 'OPEN_MAX' undeclared
   nuttx/include/nuttx/wdog.h:339: 'CLOCK_MAX' undeclared
   ```
   报错点全在 NuttX 自己的头里，跟真因毫无字面关系。
   原因：`<limits.h>` 里的 `OPEN_MAX`/`CLOCK_MAX` 依赖 `<nuttx/config.h>`
   与其它头的先后关系，多插一个 `<syslog.h>` 会让 limits.h 在被正确配置前先读一遍。
   **结论：在厂商文件里改代码，不要碰 include。**

2) **`.c` 文件传到 Linux 之前必须去掉 CRLF。**
   Windows 上编辑过的文件带 `\r`，编译能过，但行尾会进到字符串字面量里，
   日志和字幕会多出莫名其妙的东西。
   ```bash
   sed -i 's/\r$//' /root/build/board/superchild/*
   ```

---

## 开源许可

本目录下的 `sc_microute.c` 移植自本仓库 `board/baidu_voice/mic_route.c`
（同一作者、同一项目）。其余文件为本项目原创。
