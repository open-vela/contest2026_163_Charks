/*
 * R528 采集通路使能 —— 实现。设计动机见 sc_microute.h 文件头。
 *
 * 从 board/baidu_voice/mic_route.c 移植过来，去掉对 ai_agent 的
 * agent_compat.h 依赖，改成自包含。表里的值与实测记录一致，未改动。
 */

#include "sc_microute.h"

/*
 * ⚠️ 标准头必须**排在 vendor 的 ALSA 头之前**。
 *
 * 厂商头 `sound/pcm_common.h` 里裸用了 `NULL`，但自己没 include <stddef.h>
 * （实测报 "error: 'NULL' undeclared ... note: 'NULL' is defined in
 * header '<stddef.h>'"）。谁先把它引进来，谁就得先把 stddef.h 带上。
 *
 * 上一版（board/baidu_voice/mic_route.c）没踩到这个坑，是因为它先
 * include 了 agent_compat.h —— 那个头顺带把 stddef.h 带进来了。
 * 移植过来去掉那层依赖之后，坑就露出来了。
 */
#include <errno.h>
#include <stddef.h>
#include <syslog.h>

#include <aw-alsa-lib/control.h>

#define TAG "microute"

/* soundcard -l 实测：card 0 的 card_name 就是 "audiocodec" */
#define MIC_CARD "audiocodec"

#ifndef OK
#  define OK 0
#endif

struct route_ctl {
    const char  *name;
    unsigned int on_value;
};

/*
 * 板上是 2 路数字 MIC，所以开 MIC1 + MIC2。
 * ADC3 相关通路不用，保持关闭 —— 多开一条没接麦克风的通路，
 * 只会把静音通道混进来，对 ASR 有害无益。
 *
 * "ADC1_2 digital volume switch" 也必须打开，否则通路虽然使能了，
 * 但拿到的数据全是零（实测确认）。
 */
static const struct route_ctl g_routes[] = {
    { "MIC1 input switch",            1 },
    { "MIC2 input switch",            1 },
    { "ADC1_2 digital volume switch", 1 },
};

#define ROUTE_COUNT (sizeof(g_routes) / sizeof(g_routes[0]))

/*
 * 增益。语义与开关不同：**只升不降** —— 当前值已经不低于目标值就保持不动，
 * 避免把别处特意调高的设置压回去。
 *
 * 标定依据（2026-09-18 自编译固件上实测，板载数字 MIC、约 30cm、正常音量）：
 *     ADC 数字增益  160（厂商默认） → 峰值  540 (-35.7 dBFS)  太轻
 *                   205（本表目标） → 峰值 32768 ( 0 dBFS)    可用
 *                   255（满值）     → 削顶 2.39%
 * 160~255 之间只有约 36dB 可调范围，工作窗口并不宽。
 */
static const struct route_ctl g_gains[] = {
    { "MIC1 gain volume", 31 },     /* 模拟增益，实测出厂已是满值，显式写一遍兜底 */
    { "MIC2 gain volume", 31 },
    { "ADC1 digital volume", 205 },
    { "ADC2 digital volume", 205 },
};

#define GAIN_COUNT (sizeof(g_gains) / sizeof(g_gains[0]))

int sc_microute_query(const char *name, unsigned long *value)
{
    snd_ctl_info_t info;

    if (name == NULL || value == NULL) {
        return -EINVAL;
    }
    if (snd_ctl_get(MIC_CARD, name, &info) != 0) {
        return -ENOENT;
    }
    *value = info.value;
    return OK;
}

int sc_microute_apply_gain(void)
{
    size_t i;
    unsigned failures = 0;

    for (i = 0; i < GAIN_COUNT; i++) {
        const struct route_ctl *g = &g_gains[i];
        snd_ctl_info_t info;

        if (snd_ctl_get(MIC_CARD, g->name, &info) != 0) {
            syslog(LOG_ERR, "[%s] gain '%s' not found on card '%s'\n",
                   TAG, g->name, MIC_CARD);
            failures++;
            continue;
        }

        if (info.value >= g->on_value) {
            syslog(LOG_INFO, "[%s] '%s' already at %lu (>= %u), left as is\n",
                   TAG, g->name, info.value, g->on_value);
            continue;
        }

        if (snd_ctl_set(MIC_CARD, g->name, g->on_value) != 0) {
            syslog(LOG_ERR, "[%s] failed to raise '%s' from %lu to %u\n",
                   TAG, g->name, info.value, g->on_value);
            failures++;
            continue;
        }

        /* 回读确认 —— snd_ctl_set 返回成功不等于生效 */
        if (snd_ctl_get(MIC_CARD, g->name, &info) != 0 ||
            info.value < g->on_value) {
            syslog(LOG_ERR, "[%s] '%s' did not take effect (now %lu, want >= %u)\n",
                   TAG, g->name, info.value, g->on_value);
            failures++;
            continue;
        }
        syslog(LOG_INFO, "[%s] '%s' raised to %lu\n", TAG, g->name, info.value);
    }

    if (failures != 0) {
        /* 与开关不同，增益失败**不会**让采集报错，只会让声音偏轻 ——
         * 这种问题很容易被误判成"麦克风不行"或"ASR 不准"，必须显式告警。 */
        syslog(LOG_ERR, "[%s] %u/%u gain controls failed; capture works but "
               "level may be ~20dB low, hurting ASR\n",
               TAG, failures, (unsigned)GAIN_COUNT);
        return -EIO;
    }
    return OK;
}

int sc_microute_enable(void)
{
    size_t i;
    unsigned failures = 0;

    for (i = 0; i < ROUTE_COUNT; i++) {
        const struct route_ctl *r = &g_routes[i];
        snd_ctl_info_t info;

        if (snd_ctl_get(MIC_CARD, r->name, &info) != 0) {
            syslog(LOG_ERR, "[%s] control '%s' not found on card '%s'\n",
                   TAG, r->name, MIC_CARD);
            failures++;
            continue;
        }

        if (info.value == r->on_value) {
            syslog(LOG_INFO, "[%s] '%s' already on\n", TAG, r->name);
            continue;
        }

        if (snd_ctl_set(MIC_CARD, r->name, r->on_value) != 0) {
            syslog(LOG_ERR, "[%s] failed to enable '%s' (was %lu)\n",
                   TAG, r->name, info.value);
            failures++;
            continue;
        }

        if (snd_ctl_get(MIC_CARD, r->name, &info) != 0 ||
            info.value != r->on_value) {
            syslog(LOG_ERR, "[%s] '%s' did not take effect (now %lu, want %u)\n",
                   TAG, r->name, info.value, r->on_value);
            failures++;
            continue;
        }
        syslog(LOG_INFO, "[%s] '%s' enabled\n", TAG, r->name);
    }

    if (sc_microute_apply_gain() != OK) {
        failures++;
    }

    if (failures != 0) {
        syslog(LOG_ERR, "[%s] %u/%u routes failed; capture will likely fail "
               "with 'capture only support 1~3 channel'\n",
               TAG, failures, (unsigned)ROUTE_COUNT);
        return -EIO;
    }
    syslog(LOG_INFO, "[%s] capture route ready\n", TAG);
    return OK;
}
