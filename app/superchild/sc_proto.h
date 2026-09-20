/*
 * 板子 ⇄ 网关 的线上协议（v1）—— 板端定义
 *
 * ⚠️ 本文件的每个数字都必须与 `server/superchild_gateway/protocol.py`
 *    逐字一致。改任何一边都必须同时改另一边；不一致时的现象是
 *    "能连上、能出声，但表情不动" 这类很难定位的问题。
 *
 * 帧格式（小端）：
 *     +--------+--------+------------------+-------------------+
 *     | type   | flags  | length (u16 LE)  | payload (length)  |
 *     +--------+--------+------------------+-------------------+
 *       1 字节    1 字节      2 字节             length 字节
 *
 * 板端只做两件事：读 4 字节头 → 读 length 字节。
 * 没有 TLS、没有 WebSocket、没有 JSON、没有 Base64 —— 这些全在网关上。
 */

#ifndef __SUPERCHILD_SC_PROTO_H
#define __SUPERCHILD_SC_PROTO_H

#include <stdint.h>

#define SC_PROTO_VERSION   1

/* ---------------------------------------------------------------- 帧类型 */

/* 板子 → 网关 */
#define SC_T_AUDIO_IN      0x01   /* payload = PCM16LE 16k mono */
#define SC_T_HELLO         0x02   /* <BB16s24s  版本 / 保留 / 设备ID / 固件串 */
#define SC_T_PING          0x03   /* <I token */
#define SC_T_EVENT         0x04   /* <B 事件码（1=按键，触发一次问候） */
#define SC_T_BYE           0x05

/* 网关 → 板子 */
#define SC_T_AUDIO_OUT     0x81   /* payload = PCM16LE 16k mono */
#define SC_T_STATE         0x82   /* <B 会话状态 */
#define SC_T_FACE          0x83   /* <BB 表情ID / 保持时长(×20ms，0=直到下一帧) */
#define SC_T_MOOD          0x84   /* <bbb 愉悦 / 唤醒 / 能量（各 -100..100） */
#define SC_T_TEXT          0x85   /* UTF-8，≤120 字节 */
#define SC_T_PONG          0x86   /* <I token */
#define SC_T_AUDIO_FLUSH   0x87   /* 打断：立刻清播放缓冲 */
#define SC_T_CONFIG        0x88   /* <BBBB 音量 / 麦增益 / 灯 / 保留 */
#define SC_T_LEVEL         0x89   /* <B 麦克风 RMS 0-100 */

/* ---------------------------------------------------------------- 状态 */

enum sc_state {
    SC_STATE_IDLE       = 0,
    SC_STATE_LISTENING  = 1,
    SC_STATE_THINKING   = 2,
    SC_STATE_SPEAKING   = 3,
    SC_STATE_CONNECTING = 4,
    SC_STATE_OFFLINE    = 5,   /* 与网关/StepFun 断开 —— 板子要显示"我掉线了"，
                                * 不要傻站着不动，孩子会以为坏了 */
};

/* ---------------------------------------------------------------- 表情 */

/*
 * 表情编号。**这是协议契约：只能往后加，不能改已发布的编号。**
 *
 * 分三组：
 *   0-5   会话状态（跟着 sc_state 走）
 *   6-11  语义回应
 *   12-23 本轮新增（情绪更细、更有戏）
 */
enum sc_face {
    SC_FACE_IDLE       = 0,
    SC_FACE_LISTENING  = 1,
    SC_FACE_THINKING   = 2,
    SC_FACE_SPEAKING   = 3,
    SC_FACE_HAPPY      = 4,
    SC_FACE_SLEEPY     = 5,

    SC_FACE_CURIOUS    = 6,
    SC_FACE_SURPRISED  = 7,
    SC_FACE_CONFUSED   = 8,
    SC_FACE_SHY        = 9,
    SC_FACE_PROUD      = 10,
    SC_FACE_SAD        = 11,

    SC_FACE_EXCITED    = 12,   /* 兴奋：星星眼 + 大张嘴 */
    SC_FACE_LAUGHING   = 13,   /* 大笑：眯眼 + 大嘴 + 抖动 */
    SC_FACE_WINKING    = 14,   /* 俏皮：单眼眨 */
    SC_FACE_SLEEPING   = 15,   /* 睡着：闭眼 + Zzz */
    SC_FACE_LOVE       = 16,   /* 喜欢：心心眼 + 飘心 */
    SC_FACE_SINGING    = 17,   /* 唱歌：音符 + 嘴开合 */
    SC_FACE_SCARED     = 18,   /* 怕怕：小眼 + 汗滴 + 抖 */
    SC_FACE_WAVING     = 19,   /* 打招呼：挥手 + 微笑 */
    SC_FACE_EATING     = 20,   /* 吃东西：咀嚼 + 鼓腮 */
    SC_FACE_BORED      = 21,   /* 无聊：半眼 + 走神 */
    SC_FACE_DIZZY      = 22,   /* 晕乎乎：螺旋眼 */
    SC_FACE_CELEBRATE  = 23,   /* 庆祝：撒花 + 眯眼笑 */

    SC_FACE_COUNT      = 24,
};

/* ---------------------------------------------------------------- 帧打包 */

/* 4 字节头。用结构体 + 逐字节写，避免 alignment 与大小端陷阱。 */
static inline void sc_frame_header(uint8_t *out, uint8_t type,
                                   uint8_t flags, uint16_t len)
{
    out[0] = type;
    out[1] = flags;
    out[2] = (uint8_t)(len & 0xFF);
    out[3] = (uint8_t)((len >> 8) & 0xFF);
}

/* HELLO 的 payload 布局：<BB16s24s = 42 字节 */
#define SC_HELLO_PAYLOAD_LEN  42
#define SC_DEV_ID_LEN         16
#define SC_FW_LEN             24

/* ---------------------------------------------------------------- 上限 */

/* 一帧最大 payload。音频帧只有 640 字节，留足余量给未来的文本帧。 */
#define SC_MAX_PAYLOAD        2048
/* 接收缓冲：容纳"头 + 最大 payload"再留一点，允许一次 read 里有多帧 */
#define SC_RX_BUF             6144

#endif /* __SUPERCHILD_SC_PROTO_H */
