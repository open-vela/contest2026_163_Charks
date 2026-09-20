/*
 * 心情状态机（板端）
 *
 * 为什么板子上还要有一份
 * ----------------------
 * 网关上已经有一份完整的心情模型（`server/.../emotion.py`），它知道
 * 对话内容、能判语义。板子这份不是替代品，它解决两个只有板子才知道的问题：
 *
 *   1. **平滑**。网关只在心情变化 ≥5 个点时才发一帧（省得刷屏）。
 *      直接用收到的值会让脸"跳"。板子把当前值以固定时间常数
 *      滑向目标值，视觉上就是连续变化的。
 *
 *   2. **断链降级**。网关掉线时不会再有心跳般的心情帧。
 *      如果板子只会"照着收到的值画"，掉线后表情会永远冻在最后一帧上 ——
 *      孩子看到一张僵住的脸，比看到"困了"更吓人。
 *      所以超过 SC_MOOD_TIMEOUT_MS 没收到更新，目标值就自己往
 *      中性基线漂，同时把状态切到 OFFLINE。
 *
 * 数值全部用整数（-100..100），不用浮点：
 * R528 上的构建配置不保证开硬件浮点，整数运算没有这个不确定性。
 */

#ifndef __SUPERCHILD_SC_MOOD_H
#define __SUPERCHILD_SC_MOOD_H

#include <stdint.h>
#include <stdbool.h>

/* 基线：陪伴机器人默认是"温和愉快、比较安静"的 */
#define SC_MOOD_BASE_VALENCE   20
#define SC_MOOD_BASE_AROUSAL  (-10)
#define SC_MOOD_BASE_ENERGY     0

/* 多久没收到网关的心情帧就认为链路不可信 */
#define SC_MOOD_TIMEOUT_MS     15000

/* 平滑时间常数（毫秒）。越小越跟手，越大越"肉"。
 * 取 400ms：快到来话能感觉到，慢到不会一跳一跳。 */
#define SC_MOOD_SMOOTH_MS      400

struct sc_mood {
    int cur_v, cur_a, cur_e;    /* 当前显示值（平滑后） */
    int tgt_v, tgt_a, tgt_e;    /* 目标值（来自网关） */
    uint32_t last_update_ms;    /* 上次收到网关更新的时刻 */
    uint32_t last_tick_ms;
};

void sc_mood_init(struct sc_mood *m);

/*
 * 收到网关的心情帧。值域 -100..100，超出会被夹紧。
 * 这是**绝对目标值**，不是增量。
 */
void sc_mood_set(struct sc_mood *m, int v, int a, int e);

/* 周期性推进（建议 20ms 一次）。内部会做平滑与超时衰减。 */
void sc_mood_tick(struct sc_mood *m, uint32_t now_ms);

/*
 * 当前心情下、"不说话时"该显示哪张脸。
 *
 * idle_seconds 是"多久没有对话活动"。它会覆盖心情：
 * 长时间没人理 → 犯困 → 睡着。这是陪伴机器人最讨喜的一档，
 * 也是孩子最容易理解的一档。
 *
 * 返回值是 enum sc_face（见 sc_proto.h）。
 */
int sc_mood_idle_face(const struct sc_mood *m, uint32_t idle_seconds);

/* 眨眼频率：心情越活跃眨得越勤（纯观感，但很能拉开"精神/困倦"的差距） */
uint32_t sc_mood_blink_period_ms(const struct sc_mood *m);

const char *sc_mood_describe(const struct sc_mood *m);

#endif /* __SUPERCHILD_SC_MOOD_H */
