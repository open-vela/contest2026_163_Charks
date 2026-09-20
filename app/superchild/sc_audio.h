/*
 * 板端音频：采集 + 播放 + 两个环形缓冲。
 *
 * 采样率固定 16kHz / 单声道 / 16bit
 * ---------------------------------
 * 这是 R4 实测可用的档位（驱动 + codec 组合下最稳）。
 * StepFun 那边是 24kHz，**重采样放在网关上做**，板子这一侧不碰 ——
 * R528 上做实时重采样既要 CPU 又要调试，而且重采样错了的现象
 * （变调/发闷）很难和"麦克风不好"区分开。
 *
 * 硬件层用 aw-alsa-lib
 * -------------------
 * `snd_vela_pcm_*` 与板上的 `arecord`/`aplay` 走的是同一条路
 * （vendor 的 hal/test/sound/arecord.c 就是它），所以行为可预期。
 * 另一条路（/dev/audio/pcm0c + read()）在 NuttX 里只是把调用
 * 透传给 lower-half，是否实现取决于底层驱动，风险更高。
 *
 * 线程模型
 * -------
 *     采集任务 : sc_audio_capture_once() → sc_audio_tx_push()
 *     网络任务 : sc_audio_tx_pop()      → 发帧
 *                sc_audio_rx_push()     ← 收到的音频
 *     播放任务 : sc_audio_rx_pop()      → sc_audio_play_once()
 *
 * 两端的环都用一把互斥锁保护。队列满时的策略**不同**，这是刻意的：
 *     tx（上行）满 → 丢**新**的。上行是"现在这一刻孩子在说什么"，
 *                    旧的比新的更有价值，丢新最多让 ASR 少听一点。
 *     rx（下行）满 → 丢**旧**的。下行如果堆着，播放就会越来越滞后，
 *                    宁可爆一小下也不能让延迟发散。
 */

#ifndef __SUPERCHILD_SC_AUDIO_H
#define __SUPERCHILD_SC_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SC_AUDIO_RATE   16000
#define SC_AUDIO_CH     1
#define SC_AUDIO_BITS   16

/* 采集一次读多少：20ms @16k = 320 帧 = 640 字节。
 * 20ms 是官方「实时对话开发指南」建议的上行分块粒度（20~30ms）。 */
#define SC_AUDIO_FRAME_BYTES   640

/* 环形缓冲容量 */
#define SC_TX_RING_BYTES  (SC_AUDIO_RATE * 2 * 3)   /* 3 秒 */
#define SC_RX_RING_BYTES  (SC_AUDIO_RATE * 2 * 6)   /* 6 秒 */

/*
 * 初始化：使能采集通路（sc_microute_enable）→ 打开采集与播放设备。
 *
 * @return 0 成功；负数 errno。
 *         采集失败**不算致命** —— 播放能用就先跑起来，
 *         孩子至少能听到问候；反之亦然。
 */
int  sc_audio_init(void);
void sc_audio_deinit(void);

bool sc_audio_capture_ok(void);
bool sc_audio_playback_ok(void);

/* 阻塞读一帧（20ms）。返回字节数，<0 表示错误。 */
int  sc_audio_capture_once(void *buf, size_t len);

/* 阻塞写。返回写入字节数，<0 表示错误。 */
int  sc_audio_play_once(const void *buf, size_t len);

/* 打断：丢弃全部待播音频。对应网关下发的 SC_T_AUDIO_FLUSH。 */
void sc_audio_rx_flush(void);

/* ---- 环形缓冲 ---- */
int  sc_audio_tx_push(const void *buf, size_t len);   /* 返回实际压入字节数 */
int  sc_audio_tx_pop(void *buf, size_t max);          /* 返回实际取出字节数 */
int  sc_audio_tx_used(void);

int  sc_audio_rx_push(const void *buf, size_t len);
int  sc_audio_rx_pop(void *buf, size_t max);
int  sc_audio_rx_used(void);

/*
 * 估算一段 16bit PCM 的"响度"（0-100），用于驱动嘴形与眼睛反应。
 *
 * 用峰值而不是 RMS：RMS 在安静段落会很低，嘴几乎不动，看起来像卡住；
 * 峰值跟音节的开合更同步，观感好得多。
 */
int  sc_audio_level(const void *buf, size_t len);

#endif /* __SUPERCHILD_SC_AUDIO_H */
