/*
 * 儿童安全（板端）—— 输入侧关键词判定 → 改舵提示。
 *
 * 移植自 `prototype/safety.py` 的 `INPUT_RULES`，类别一一对应：
 *
 *     危险行为 / 陌生人 / 隐私 / 医疗用药 / 负面评价 / 情绪困扰
 *
 * 为什么是"改舵"而不是"过滤"
 * ========================
 * 实时语音链路里**没法事后过滤回复文本** —— 音频是模型直接生成的，
 * 等拿到文本时声音已经发出来了。所以安全策略必须提前介入：
 *
 *     命中 → 不打断 → 插一条内部提示（"（小提示，请照做：…）"）
 *               → 触发一次新回复
 *
 * 实测能把话题拉回兜底话术（"这个要问爸爸妈妈哦。"）。
 * 系统提示词里也保留了完整红线（`persona/SOUL.md`），两层一起用。
 *
 * 后置校验（`check_output`）在实时链路上意义有限，但**仍然保留**用于
 * 事后评估提示词效果 —— 命中就记日志。
 *
 * ⚠️ 关键词表是**近似**的。真正的判据在系统提示词里；这里只是
 *    "把小概率事件变成大概率被兜住"。不要以为覆盖了全部情况。
 */

#ifndef __SUPERCHILD_SC_SAFETY_H
#define __SUPERCHILD_SC_SAFETY_H

typedef struct {
    int  hit;               /* 1 = 命中 */
    int  severity;          /* 1 = 改舵；2 = 改舵 + 安慰（情绪困扰类） */
    const char *category;   /* 类别名，进日志 */
    const char *steer;      /* 要插给模型的提示 */
} sc_safety_hit_t;

/*
 * 检查一句话。
 * @return 1 命中（out 已填）；0 未命中
 */
int sc_safety_check(const char *text, sc_safety_hit_t *out);

/*
 * 把文本规范化：去掉空白与标点。
 *
 * 为什么要先规范化：孩子说"玩 火"或者在词中间插入停顿，
 * 直接子串匹配会漏。Python 侧的 `normalize()` 做的是同一件事。
 *
 * @param src  UTF-8 输入
 * @param dst  输出缓冲
 * @param cap  容量
 * @return 写入的字节数
 */
int sc_safety_normalize(const char *src, char *dst, int cap);

#endif /* __SUPERCHILD_SC_SAFETY_H */
