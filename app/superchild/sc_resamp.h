/*
 * 定点重采样：16kHz ⇄ 24kHz。
 *
 * 为什么需要它
 * -----------
 * 板子的 codec 走 16kHz（实测最稳的档位），StepFun Realtime 用 24kHz。
 * 比值 3:2，两边都要转：
 *     上行  16k → 24k   （L=3, M=2）
 *     下行  24k → 16k   （L=2, M=3）
 *
 * 算法：有理数多相 FIR
 * -------------------
 * 统一在 **48kHz 中间域** 上做（16k×3 和 24k×2 都是 48k），
 * 两个方向共用同一个原型低通滤波器，截止 8kHz。
 *
 *     上行：×3 上采 → 滤 8k → 抽取 2 → 24k
 *     下行：×2 上采 → 滤 8k → 抽取 3 → 16k
 *
 * **截止频率为什么是 8kHz**：取 min(fs_in/2, fs_out/2)。
 * 两个方向算下来都是 8k —— 这不是巧合，是因为两个方向的
 * 「较窄那一边的奈奎斯特」都是 16k 系统的 8k。
 *
 * 下行那一侧必须滤干净，否则会混叠：24k 信号里 8k~12k 的内容
 * 在抽取到 16k 后会折回到 0~4k —— 那是**人声最关键的频段**，
 * 混叠进去的声音听起来像"沙哑 + 电子味"，而且很容易被误判成
 * "模型音色不好"或"喇叭破了"。所以原型滤波器取 96 抽头。
 *
 * （顺带记一笔：这是项目里第二次踩「音质问题其实是采样率问题」，
 *  第一次是判定 StepFun 输出是 24k 而不是 16k。）
 *
 * 定点
 * ----
 * 系数 Q15，**每个相位单独归一化到 1.0**。逐相位归一化而不是整体归一化，
 * 是为了消掉相位相关的直流纹波 —— 整体归一化时各相位直流增益会有
 * 微小差异，在语音上表现为一种很轻的"嗡嗡"声，很难查。
 */

#ifndef __SUPERCHILD_SC_RESAMP_H
#define __SUPERCHILD_SC_RESAMP_H

#include <stdint.h>

/* 原型滤波器长度。96 抽头 @48k → 过渡带约 2.75kHz */
#define SC_RESAMP_NTAPS   96
/* 历史缓冲。多留 8 个是给"跳读"留余量，实际按 per-sample 推进时
 * 偏移恒为 0（见 .c 里的推导），多留只是防御性写法。 */
#define SC_RESAMP_HIST    (SC_RESAMP_NTAPS + 8)

typedef struct {
    int      L;                       /* 上采样因子 */
    int      M;                       /* 下采样因子 */
    int      P;                       /* 每相位抽头数 = NTAPS / L */
    int      in_rate;
    int      out_rate;

    int32_t  coef[SC_RESAMP_NTAPS];   /* [frac * P + j]，Q15 */
    int16_t  hist[SC_RESAMP_HIST];
    int      wpos;
    int64_t  fed;                     /* 已喂入的输入样本数 */
    int64_t  k;                       /* 下一个待产出的输出样本序号 */
    int      ready;
} sc_resamp_t;

/*
 * 初始化。系数在这里现算（sin + Blackman 窗），一次约几百微秒。
 * @return 0 成功；<0 表示采样率比无法支持。
 */
int  sc_resamp_init(sc_resamp_t *r, int in_rate, int out_rate);

/* 清空历史与相位（断线重连后调用，避免把上一段的尾巴接进来）。 */
void sc_resamp_reset(sc_resamp_t *r);

/*
 * 处理一块输入。
 * @param in      输入样本（int16）
 * @param in_len  输入样本数
 * @param out     输出缓冲
 * @param out_cap 输出缓冲容量（样本数）
 * @return 产出的输出样本数；<0 表示输出缓冲不够
 *
 * 注意：这是**流式**接口，每次调用不必是整块。
 * 输出样本数不保证是 in_len*L/M 的整数 —— 累积误差由内部相位器吸收。
 */
int  sc_resamp_process(sc_resamp_t *r, const int16_t *in, int in_len,
                       int16_t *out, int out_cap);

/* 每产出 1 个输出样本平均需要多少输入样本（用于预估缓冲） */
int  sc_resamp_in_per_out(const sc_resamp_t *r);

#endif /* __SUPERCHILD_SC_RESAMP_H */
