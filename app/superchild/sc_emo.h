/*
 * 情绪引擎（板端）—— 文本 → 表情 + 心情增量。
 *
 * 这是 `server/superchild_gateway/emotion.py` 里 `_EMO_RULES` 的 C 移植。
 * 方案 C 之后板子自己拿到 ASR 文本和回复文本，所以规则表也必须搬过来 ——
 * **两边的规则必须一致**，否则会出现"网关（如果有）认为该笑、
 * 板子显示困"的拉扯，表现为表情在两帧之间反复横跳。
 *
 * 规则表的几条经验（原文抄自 Python 侧，都是踩过的）
 * ================================================
 *   · **用短语不用单字**。用 "玩火" 而不是 "火"，否则"开火车"被误伤。
 *   · 一个词可能同时命中多条，所以表是**有序**的，先匹配先返回。
 *   · 情绪类规则不能太贪：孩子说"我不喜欢你"是情绪表达不是攻击，
 *     该给"委屈/害羞"而不是"生气"。
 *   · `excited` 必须排在 `praise` **前面**："哇塞太厉害了"两条都命中，
 *     但孩子是在"惊叹"不是在"夸机器人"，给兴奋脸才对。
 *     反过来放会得到一张莫名其妙的得意脸。
 *
 * 权重
 * ----
 *   孩子说的话     权重 1.0，另外 +8 能量 / +3 唤醒（"有人跟我说话"本身就有能量）
 *   机器人自己的话 权重 0.6（只反映它"表达出来"的情绪，不代表孩子的感受）
 */

#ifndef __SUPERCHILD_SC_EMO_H
#define __SUPERCHILD_SC_EMO_H

#include <stdint.h>

typedef struct {
    int  face;          /* 要闪的表情（enum sc_face）；-1 表示"不闪" */
    int  hold_ms;       /* 停留时长 */
    int  dv;            /* 愉悦度增量 */
    int  da;            /* 唤醒度增量 */
    int  de;            /* 能量增量 */
    char label[32];     /* 命中的标签，进日志 */
} sc_emo_result_t;

void sc_emo_init(void);

/*
 * 分析一句话。
 *
 * 同一秒内重复触发同一张脸会被**去抖**（face 置 -1，心情照常更新）——
 * 连续说"好玩好玩好玩"时表情不停重置会很怪。
 *
 * @param text      UTF-8
 * @param child     1 = 孩子说的（权重 1.0 + 参与能量），0 = 机器人回复（权重 0.6）
 * @param out       结果
 * @return 1 命中了某条规则；0 没命中
 */
int  sc_emo_analyze(const char *text, int child, sc_emo_result_t *out);

#endif /* __SUPERCHILD_SC_EMO_H */
