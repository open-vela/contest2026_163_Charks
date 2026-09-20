/*
 * 板端音频 —— 实现。设计动机见 sc_audio.h 文件头。
 */

#include "sc_audio.h"
#include "sc_microute.h"

/* 标准头排在 vendor 的 ALSA 头之前 —— 理由见 sc_microute.c 顶部注释
 * （厂商头裸用 NULL 却没带 stddef.h）。 */
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <aw-alsa-lib/pcm.h>

#define TAG "sc_audio"

/* ---------------------------------------------------------------- 环形缓冲 */

struct ring {
    uint8_t *buf;
    size_t   cap;
    size_t   head;      /* 写指针 */
    size_t   tail;      /* 读指针 */
    size_t   used;
    bool     drop_new;  /* true: 满时丢新的（上行）；false: 满时丢最旧的（下行） */
    pthread_mutex_t lock;
};

static struct ring g_tx;
static struct ring g_rx;

static int ring_init(struct ring *r, size_t cap, bool drop_new)
{
    r->buf = malloc(cap);
    if (r->buf == NULL) {
        return -ENOMEM;
    }
    r->cap = cap;
    r->head = r->tail = r->used = 0;
    r->drop_new = drop_new;
    pthread_mutex_init(&r->lock, NULL);
    return 0;
}

static size_t ring_push(struct ring *r, const uint8_t *src, size_t len)
{
    size_t written = 0;

    pthread_mutex_lock(&r->lock);

    if (len > r->cap) {
        /* 单次就超过整个容量：只留最后 cap 字节 */
        src += (len - r->cap);
        len = r->cap;
    }

    if (r->used + len > r->cap) {
        if (r->drop_new) {
            len = r->cap - r->used;          /* 只塞得下这么多，其余丢掉 */
        } else {
            /* 丢最旧的，腾出位置 */
            size_t need = r->used + len - r->cap;
            r->tail = (r->tail + need) % r->cap;
            r->used -= need;
        }
    }

    for (size_t i = 0; i < len; i++) {
        r->buf[r->head] = src[i];
        r->head = (r->head + 1) % r->cap;
    }
    r->used += len;
    written = len;

    pthread_mutex_unlock(&r->lock);
    return written;
}

static size_t ring_pop(struct ring *r, uint8_t *dst, size_t max)
{
    size_t n;

    pthread_mutex_lock(&r->lock);
    n = (r->used < max) ? r->used : max;
    for (size_t i = 0; i < n; i++) {
        dst[i] = r->buf[r->tail];
        r->tail = (r->tail + 1) % r->cap;
    }
    r->used -= n;
    pthread_mutex_unlock(&r->lock);
    return n;
}

static size_t ring_used(struct ring *r)
{
    size_t n;
    pthread_mutex_lock(&r->lock);
    n = r->used;
    pthread_mutex_unlock(&r->lock);
    return n;
}

static void ring_clear(struct ring *r)
{
    pthread_mutex_lock(&r->lock);
    r->head = r->tail = r->used = 0;
    pthread_mutex_unlock(&r->lock);
}

/* ---------------------------------------------------------------- ALSA 句柄 */

static snd_pcm_t *g_cap;
static snd_pcm_t *g_play;
static int g_cap_hw_ch;      /* 硬件实际使用的通道数（可能被抬到 3） */
static uint8_t *g_scratch;   /* 多通道降混用 */
static size_t   g_scratch_sz;

/*
 * 配置 ALSA PCM。
 *
 * 关于通道数：板子的 codec 在"没有任何输入通路被使能"时会返回 -1 并要求
 * 1~3 通道，实测把请求抬到 3 通道能绕过去（这也是 ai_agent 的做法）。
 * 我们已经在 sc_microute_enable() 里使能了通路，正常应该 1 通道就够，
 * 但保留 3 通道回退，作为通路使能失败时的第二道保险。
 */
static int cfg_pcm(snd_pcm_t *pcm, unsigned rate, unsigned channels)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_uframes_t period = rate / 50;     /* 20ms */
    snd_pcm_uframes_t buffer = period * 4;
    int ret;

    if (period == 0) {
        period = 320;
    }

    snd_pcm_hw_params_alloca(&hw);
    ret = snd_vela_pcm_hw_params_any(pcm, hw);
    if (ret < 0) {
        return ret;
    }
    ret = snd_vela_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        return ret;
    }
    ret = snd_vela_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (ret < 0) {
        return ret;
    }
    ret = snd_vela_pcm_hw_params_set_channels(pcm, hw, channels);
    if (ret < 0) {
        return ret;
    }
    ret = snd_vela_pcm_hw_params_set_rate(pcm, hw, rate, 0);
    if (ret < 0) {
        return ret;
    }
    ret = snd_vela_pcm_hw_params_set_period_size(pcm, hw, period, 0);
    if (ret < 0) {
        return ret;
    }
    ret = snd_vela_pcm_hw_params_set_buffer_size(pcm, hw, buffer);
    if (ret < 0) {
        return ret;
    }
    ret = snd_vela_pcm_hw_params(pcm, hw);
    if (ret < 0) {
        return ret;
    }

    snd_pcm_sw_params_alloca(&sw);
    ret = snd_vela_pcm_sw_params_current(pcm, sw);
    if (ret < 0) {
        return ret;
    }
    snd_vela_pcm_sw_params_set_start_threshold(pcm, sw, 1);
    snd_vela_pcm_sw_params_set_stop_threshold(pcm, sw, buffer);
    snd_vela_pcm_sw_params_set_avail_min(pcm, sw, period);
    return snd_vela_pcm_sw_params(pcm, sw);
}

static int open_capture(void)
{
    const unsigned tries[2] = { SC_AUDIO_CH, 3 };
    int last = -ENODEV;

    for (int i = 0; i < 2; i++) {
        snd_pcm_t *pcm = NULL;
        int ret = snd_vela_pcm_open(&pcm, "default",
                                    SND_VELA_PCM_STREAM_CAPTURE, 0);
        if (ret < 0) {
            last = ret;
            continue;
        }
        ret = cfg_pcm(pcm, SC_AUDIO_RATE, tries[i]);
        if (ret < 0) {
            snd_vela_pcm_close(pcm);
            last = ret;
            syslog(LOG_WARNING, "[%s] capture cfg %uch failed: %d\n",
                   TAG, tries[i], ret);
            continue;
        }
        g_cap = pcm;
        g_cap_hw_ch = tries[i];
        syslog(LOG_INFO, "[%s] capture opened: %uHz %uch (hw) %ubit\n",
               TAG, SC_AUDIO_RATE, tries[i], SC_AUDIO_BITS);
        return 0;
    }
    syslog(LOG_ERR, "[%s] capture open failed: %d\n", TAG, last);
    return last;
}

static int open_playback(void)
{
    snd_pcm_t *pcm = NULL;
    int ret = snd_vela_pcm_open(&pcm, "default", SND_VELA_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] playback open failed: %d\n", TAG, ret);
        return ret;
    }
    ret = cfg_pcm(pcm, SC_AUDIO_RATE, SC_AUDIO_CH);
    if (ret < 0) {
        snd_vela_pcm_close(pcm);
        syslog(LOG_ERR, "[%s] playback cfg failed: %d\n", TAG, ret);
        return ret;
    }
    g_play = pcm;
    snd_vela_pcm_prepare(g_play);
    syslog(LOG_INFO, "[%s] playback opened: %uHz %uch %ubit\n",
           TAG, SC_AUDIO_RATE, SC_AUDIO_CH, SC_AUDIO_BITS);
    return 0;
}

/* ---------------------------------------------------------------- 对外 */

int sc_audio_init(void)
{
    int rc = 0;

    /* 顺序不能反：必须先使能通路，否则采集的 hw_params 一定失败 */
    if (sc_microute_enable() != 0) {
        syslog(LOG_WARNING, "[%s] mic route 未完全就绪 —— 采集可能报"
               "'capture only support 1~3 channel'\n", TAG);
    }

    if (ring_init(&g_tx, SC_TX_RING_BYTES, true) != 0) {
        return -ENOMEM;
    }
    if (ring_init(&g_rx, SC_RX_RING_BYTES, false) != 0) {
        return -ENOMEM;
    }

    g_scratch_sz = SC_AUDIO_FRAME_BYTES * 4;
    g_scratch = malloc(g_scratch_sz);
    if (g_scratch == NULL) {
        return -ENOMEM;
    }

    if (open_capture() != 0) {
        rc = -EIO;          /* 不致命：至少还能播问候 */
    } else {
        snd_vela_pcm_prepare(g_cap);
    }
    if (open_playback() != 0) {
        rc = -EIO;
    }
    return rc;
}

void sc_audio_deinit(void)
{
    if (g_cap != NULL) {
        snd_vela_pcm_drop(g_cap);
        snd_vela_pcm_close(g_cap);
        g_cap = NULL;
    }
    if (g_play != NULL) {
        snd_vela_pcm_drop(g_play);
        snd_vela_pcm_close(g_play);
        g_play = NULL;
    }
    free(g_scratch);
    g_scratch = NULL;
}

bool sc_audio_capture_ok(void)  { return g_cap != NULL; }
bool sc_audio_playback_ok(void) { return g_play != NULL; }

int sc_audio_capture_once(void *buf, size_t len)
{
    snd_pcm_uframes_t want;
    int nframes;

    if (g_cap == NULL || buf == NULL || len < 2) {
        return -ENODEV;
    }
    want = len / 2;                       /* 单声道：1 帧 = 2 字节 */
    if (g_cap_hw_ch == 1) {
        nframes = (int)snd_vela_pcm_readi(g_cap, buf, want);
    } else {
        /* 硬件是多通道，读进来再降混成单声道 */
        snd_pcm_uframes_t hw_want = want;
        if (hw_want * (unsigned)g_cap_hw_ch * 2 > g_scratch_sz) {
            hw_want = g_scratch_sz / ((unsigned)g_cap_hw_ch * 2);
        }
        nframes = (int)snd_vela_pcm_readi(g_cap, g_scratch, hw_want);
        if (nframes > 0) {
            int16_t *out = buf;
            const int16_t *in = (const int16_t *)g_scratch;
            for (int i = 0; i < nframes; i++) {
                int32_t acc = 0;
                for (int c = 0; c < g_cap_hw_ch; c++) {
                    acc += in[i * g_cap_hw_ch + c];
                }
                out[i] = (int16_t)(acc / g_cap_hw_ch);
            }
        }
    }

    if (nframes == -EPIPE) {
        snd_vela_pcm_prepare(g_cap);
        return -EAGAIN;
    }
    if (nframes < 0) {
        return nframes;
    }
    return nframes * 2;
}

int sc_audio_play_once(const void *buf, size_t len)
{
    size_t done = 0;
    const uint8_t *p = buf;

    if (g_play == NULL || buf == NULL || len < 2) {
        return -ENODEV;
    }

    while (done < len) {
        ssize_t n = snd_vela_pcm_writei(g_play, p + done, (len - done) / 2);
        if (n == -EAGAIN) {
            usleep(5000);
            continue;
        }
        if (n == -EPIPE) {
            snd_vela_pcm_prepare(g_play);
            continue;
        }
        if (n < 0) {
            return (done > 0) ? (int)done : (int)n;
        }
        done += (size_t)n * 2;
    }
    return (int)done;
}

void sc_audio_rx_flush(void)
{
    ring_clear(&g_rx);
    /* 已经进了 codec 队列的那部分冲不掉，但至少不再往里塞了。
     * 打断时"少听后半句"比"继续听完整句反过来"要好得多。 */
}

int sc_audio_tx_push(const void *buf, size_t len)
{
    return (int)ring_push(&g_tx, buf, len);
}

int sc_audio_tx_pop(void *buf, size_t max)
{
    return (int)ring_pop(&g_tx, buf, max);
}

int sc_audio_tx_used(void) { return (int)ring_used(&g_tx); }

int sc_audio_rx_push(const void *buf, size_t len)
{
    return (int)ring_push(&g_rx, buf, len);
}

int sc_audio_rx_pop(void *buf, size_t max)
{
    return (int)ring_pop(&g_rx, buf, max);
}

int sc_audio_rx_used(void) { return (int)ring_used(&g_rx); }

int sc_audio_level(const void *buf, size_t len)
{
    const int16_t *s = buf;
    size_t n = len / 2;
    int peak = 0;

    for (size_t i = 0; i < n; i++) {
        int v = s[i];
        if (v < 0) {
            v = -v;
        }
        if (v > peak) {
            peak = v;
        }
    }
    /* 32767 对应 100。除以 328 让满量程落在 100 附近。 */
    return peak / 328;
}
