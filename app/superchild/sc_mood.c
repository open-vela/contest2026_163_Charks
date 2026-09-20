/*
 * 心情状态机 —— 实现。设计动机见 sc_mood.h 文件头。
 */

#include "sc_mood.h"
#include "sc_proto.h"

#include <stdio.h>

#define CLAMP(v, lo, hi)  ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

static int clamp100(int v)
{
    return CLAMP(v, -100, 100);
}

void sc_mood_init(struct sc_mood *m)
{
    if (m == NULL) {
        return;
    }
    m->cur_v = m->tgt_v = SC_MOOD_BASE_VALENCE;
    m->cur_a = m->tgt_a = SC_MOOD_BASE_AROUSAL;
    m->cur_e = m->tgt_e = SC_MOOD_BASE_ENERGY;
    m->last_update_ms = 0;
    m->last_tick_ms = 0;
}

void sc_mood_set(struct sc_mood *m, int v, int a, int e)
{
    if (m == NULL) {
        return;
    }
    m->tgt_v = clamp100(v);
    m->tgt_a = clamp100(a);
    m->tgt_e = clamp100(e);
    m->last_update_ms = m->last_tick_ms;
}

/* 向目标靠拢一步。step 是"本周期允许走的最大点数"。 */
static int approach(int cur, int tgt, int step)
{
    int d = tgt - cur;
    if (d > step) {
        return cur + step;
    }
    if (d < -step) {
        return cur - step;
    }
    return tgt;
}

static int approach_base(int cur, int base, int step)
{
    /* 往基线漂，但**不越过** —— 否则会绕着基线来回跳 */
    if (cur > base) {
        return cur - step < base ? base : cur - step;
    }
    if (cur < base) {
        return cur + step > base ? base : cur + step;
    }
    return base;
}

void sc_mood_tick(struct sc_mood *m, uint32_t now_ms)
{
    uint32_t dt;
    int step;

    if (m == NULL) {
        return;
    }
    if (m->last_tick_ms == 0) {
        m->last_tick_ms = now_ms;
        m->last_update_ms = now_ms;
        return;
    }

    dt = now_ms - m->last_tick_ms;
    if (dt == 0) {
        return;
    }
    m->last_tick_ms = now_ms;

    /* ---- 链路超时：目标值自己往基线漂 ----------------------------------
     * 这里漂的是**目标**而不是当前值：当前值继续用同一套平滑去追目标，
     * 于是"掉线"表现为脸缓缓平静下来，而不是突然跳变。
     */
    if ((now_ms - m->last_update_ms) > SC_MOOD_TIMEOUT_MS) {
        /* 每 100ms 让目标将近 1 点：约 10 秒回到基线 */
        int n = (int)(dt / 100);
        if (n > 0) {
            m->tgt_v = approach_base(m->tgt_v, SC_MOOD_BASE_VALENCE, n);
            m->tgt_a = approach_base(m->tgt_a, SC_MOOD_BASE_AROUSAL, n);
            m->tgt_e = approach_base(m->tgt_e, SC_MOOD_BASE_ENERGY, n);
        }
    }

    /* ---- 平滑 ----------------------------------------------------------
     * step = 100 * dt / TAU。dt=20ms、TAU=400ms → 每周期最多走 5 点，
     * 走完 100 点的满量程约 0.4 秒。手感正好。
     */
    step = (int)((100 * dt) / SC_MOOD_SMOOTH_MS);
    if (step < 1) {
        step = 1;
    }

    m->cur_v = approach(m->cur_v, m->tgt_v, step);
    m->cur_a = approach(m->cur_a, m->tgt_a, step);
    m->cur_e = approach(m->cur_e, m->tgt_e, step);
}

/* ------------------------------------------------------------------------ */

int sc_mood_idle_face(const struct sc_mood *m, uint32_t idle_seconds)
{
    int v, a, e;

    if (m == NULL) {
        return SC_FACE_IDLE;
    }
    v = m->cur_v;
    a = m->cur_a;
    e = m->cur_e;

    /* ⚠️ 判定顺序很重要，而且**必须与网关侧 emotion.py 的
     *    FaceDirector.idle_face() 保持同一套规则**。
     *    两边不一致时会出现"网关认为该笑、板子显示困"的拉扯，
     *    表现为表情在两帧之间反复横跳。
     *
     * 先判"困/睡着"再判"兴奋"：刚聊完时 energy 高但 arousal 也可能低，
     * 反过来的话会立刻被判定成困。
     */
    if (idle_seconds > 120) {
        return SC_FACE_SLEEPING;
    }
    if (idle_seconds > 45 && e < 12) {
        return SC_FACE_SLEEPY;
    }

    if (e > 65 && v > 25) {
        return SC_FACE_EXCITED;
    }
    if (v > 55 && a > 15) {
        return SC_FACE_HAPPY;
    }
    if (v > 45) {
        return SC_FACE_PROUD;
    }
    if (v < -45) {
        return SC_FACE_SAD;
    }
    if (v < -15) {
        return SC_FACE_CONFUSED;
    }
    if (e < 10 && a < -30) {
        return SC_FACE_BORED;
    }
    return SC_FACE_IDLE;
}

uint32_t sc_mood_blink_period_ms(const struct sc_mood *m)
{
    /* 基线 4.2 秒；越活跃眨得越勤，最勤 1.8 秒。
     * 单靠这一项就能让"精神"和"困"看起来完全不同。 */
    int e = (m != NULL) ? m->cur_e : 0;
    uint32_t p = 4200 - (uint32_t)(e * 24);
    if (p < 1800) {
        p = 1800;
    }
    return p;
}

const char *sc_mood_describe(const struct sc_mood *m)
{
    static char buf[64];
    if (m == NULL) {
        return "(null)";
    }
    snprintf(buf, sizeof(buf), "v=%d a=%d e=%d", m->cur_v, m->cur_a, m->cur_e);
    return buf;
}
