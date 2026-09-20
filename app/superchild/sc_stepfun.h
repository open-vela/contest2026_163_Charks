/*
 * StepFun Realtime 会话（板端）。
 *
 * 这份实现是 `server/superchild_gateway/stepfun.py` 的 C 移植 —— 那边
 * 已经在线路上验过，这里**逐事件对齐**，不重新发明。
 *
 * 线上实测事实（都写在网关代码里，抄过来免得两边不一致）
 * ======================================================
 *   端点    wss://api.stepfun.com/v1/realtime?model=<名>   ← model 走 query！
 *   鉴权    Authorization: Bearer <key>
 *   采样率  **24000 Hz 单声道 PCM16LE**（文档没写，六轮探针测出来的）
 *   音色    默认 `jingdiannvsheng`；要用 `linjiajiejie` 必须显式 session.update
 *   上行    必须**按 24k 发**。16k 原样发 = 孩子声音被放慢 1.5 倍并降调
 *   分块    ≤60ms 一小块
 *   上限    会话 30 分钟 → 提前 90 秒主动重连（中途被掐断孩子会直接感觉到）
 *
 * VAD 参数（两个都调过，别改回默认）
 * ================================
 *   silence_duration_ms          500 → **800**
 *       500ms 会在孩子句中的逗号停顿处收尾。实测出现过
 *       「小星星，一闪一闪亮晶晶」被切成「小星星。」
 *   energy_awakeness_threshold  2500 → **700**
 *       板子数字 MIC 标定后 RMS 约 −20dBFS，2500 的门限**孩子小声说话直接漏检**
 *
 * 三个非显然的设计决定
 * ==================
 *
 * 1. **重采样放在本模块内**，不是调用方。
 *    上行 16k→24k、下行 24k→16k，两个方向都必须**跨块保留滤波器状态**。
 *    如果每次调用新建一个重采样器，块边界会出现周期性的咔哒声 ——
 *    而且听起来像"网络丢包"，很难往重采样上想。
 *    放在这里，调用方只管递交/接收 16k 数据，格式转换只有一处。
 *
 * 2. **下行按 ~40ms 攒块再回调**，不是收到多少发多少。
 *    太小会让播放任务频繁唤醒；太大又加延迟。40ms @16k = 1280 字节。
 *    `response.done` 到达时必须**冲刷残余**，否则最后几个字的尾音会丢。
 *
 * 3. **打断要同时做两件事**：让服务端停止生成（response.cancel）
 *    **加** 让播放侧清空缓冲。只做前者的话，板子里已经缓冲的
 *    几百毫秒还会继续放出来 —— 表现是"被打断了但还在说"。
 *    所以本模块只负责前者，通过 `on_speech_started` 通知调用方做后者。
 */

#ifndef __SUPERCHILD_SC_STEPFUN_H
#define __SUPERCHILD_SC_STEPFUN_H

#include "sc_b64.h"
#include "sc_resamp.h"
#include "sc_ws.h"

#include <stdint.h>

/* 上行单块上限：官方建议 20~30ms，60ms 是硬上限。
 * 24k 单声道 16bit = 48 字节/ms → 60ms = 2880 字节 */
#define SC_STEPFUN_MAX_UP_MS      60
#define SC_STEPFUN_MAX_UP_BYTES   (SC_STEPFUN_MAX_UP_MS * 48)

/* 下行攒块阈值：40ms @16k */
#define SC_STEPFUN_DOWN_FLUSH     1280

/* 会话复用上限（留 90 秒余量） */
#define SC_STEPFUN_RECYCLE_MS     ((30 * 60 - 90) * 1000)

typedef struct {
    void (*on_audio)(void *ud, const int16_t *pcm, int nsamp);  /* 已转成 16k */
    void (*on_speech_started)(void *ud);   /* 调用方此时应清空播放缓冲（打断） */
    void (*on_speech_stopped)(void *ud);
    void (*on_response_started)(void *ud);
    void (*on_response_done)(void *ud);
    void (*on_user_text)(void *ud, const char *text);
    void (*on_reply_text)(void *ud, const char *text);
    void (*on_error)(void *ud, const char *msg);
    void *ud;
} sc_stepfun_cbs_t;

/*
 * 上行切片上限（输入样本数）。一次喂给重采样器这么多，
 * 对应的 24k 输出约 1536 个样本，装得进 ul_pcm24。
 * 分片是为了避免给重采样器一个巨大输入却只有小输出缓冲。
 */
#define SC_STEPFUN_UP_SLICE   1024

/*
 * 下行单次 base64 解码的容量（24k 样本数）。
 *
 * ⚠️ 这个数**不能照上行尺寸推**。实测服务端一次 `response.audio.delta`
 * 给 **10924 个 base64 字符** ≈ 8193 字节 ≈ **4096 个 24k 样本**
 * （约 170ms 音频）。第一版按上行尺寸（2112 样本）分配，
 * 结果每次解码都返回 -1，日志刷"base64 解不开" ——
 * 而听感上是"机器人不说话"，很容易往网络或鉴权上想。
 *
 * 取 16384 是留了 4 倍余量：万一服务端给更大的块（WS 单条上限 24KB
 * 对应的 base64 上限约 24000 字符 → 18000 字节 → 9000 样本）也够。
 */
#define SC_STEPFUN_DL_SAMPLES  16384

typedef struct {
    sc_ws_t          *ws;
    sc_stepfun_cbs_t  cbs;

    sc_resamp_t       up;              /* 16k → 24k */
    sc_resamp_t       down;            /* 24k → 16k */

    int      connected;
    int      speaking;                 /* 模型正在生成/说话 */
    int      errors;
    uint32_t opened_at;

    /*
     * 是否已收到 `session.created`。
     * 调用方必须**收到后立刻发 session.update** —— 服务端在
     * created 之后如果迟迟等不到配置，会主动关闭连接（实测），
     * 而现象是"刚连上就断"，看起来像网络问题。
     */
    int      got_created;
    int      got_updated;

    /* 下行攒块缓冲（样本数） */
    int16_t  downbuf[SC_STEPFUN_DOWN_FLUSH * 2];
    int      downbuf_len;

    /* ---- 工作缓冲。放结构体里而不是栈上：会话任务栈只有几 KB，
     *      这里加起来约 70KB，放栈上会直接踩爆。 ---- */
    char     rxmsg[SC_WS_MSG_MAX];                   /* 收一条事件 */
    char     b64[SC_B64_ENCLEN(SC_STEPFUN_MAX_UP_BYTES) + 8];
    int16_t  ul_pcm24[SC_STEPFUN_UP_SLICE * 2 + 64];  /* 上行重采样输出 */
    int16_t  dl_pcm24[SC_STEPFUN_DL_SAMPLES];         /* 下行 base64 解码结果 */
    int16_t  dl_pcm16[SC_STEPFUN_DL_SAMPLES];         /* 下行重采样输出 */
} sc_stepfun_t;

int  sc_stepfun_init(sc_stepfun_t *s, sc_ws_t *ws, const sc_stepfun_cbs_t *cbs);

/*
 * 发送 session.update。连接建立、收到 session.created 之后调用一次。
 *
 * @param instructions  人格提示词（可 NULL）
 * @param voice         音色名（如 "linjiajiejie"；NULL 表示不改，用服务端默认）
 * @param prefix_pad_ms 前缀补齐，默认 400
 * @param silence_ms    判"说完了"的静音时长，**用 800**（见文件头）
 * @param energy_thresh 能量门限，**用 700**（见文件头）
 * @return 0 成功
 */
int  sc_stepfun_session_update(sc_stepfun_t *s, const char *instructions,
                               const char *voice, int prefix_pad_ms,
                               int silence_ms, int energy_thresh);

/*
 * 送一块上行音频（16k，int16 样本）。内部会重采样到 24k、
 * 按 60ms 切块、base64、发 input_audio_buffer.append。
 * @return 0 成功；<0 错误
 */
int  sc_stepfun_send_audio(sc_stepfun_t *s, const int16_t *pcm16k, int n16k);

/*
 * 收事件并分发回调。**非阻塞**：没有消息时返回 0。
 * 建议在会话任务里每 10~20ms 调一次。
 * @return 0 正常（可能没消息）；<0 连接已断，调用方应重连
 */
int  sc_stepfun_poll(sc_stepfun_t *s, int max_msgs);

/* 让它念一句指定的话（开机问候、安全兜底话术）。 */
int  sc_stepfun_say(sc_stepfun_t *s, const char *text);

/*
 * 安全改舵：插一条内部提示再触发回复。
 *
 * 实时语音链路**没法事后过滤回复文本**（音频是模型直接生成的），
 * 所以安全策略必须提前介入。前缀用「（小提示：…）」包成旁白，
 * 而不是 system 角色 —— 实测 system 项在会话中途插入时模型不太理会。
 */
int  sc_stepfun_steer(sc_stepfun_t *s, const char *hint);

/* 取消当前回复（打断用）。没有进行中的回复时服务端会回 error，会被忽略。 */
int  sc_stepfun_cancel(sc_stepfun_t *s);

int  sc_stepfun_is_speaking(const sc_stepfun_t *s);
int  sc_stepfun_connected(const sc_stepfun_t *s);

/* 是否已接近 30 分钟会话上限，该主动重连了 */
int  sc_stepfun_needs_recycle(const sc_stepfun_t *s);

/* 已连接的毫秒数 */
uint32_t sc_stepfun_age_ms(const sc_stepfun_t *s);

#endif /* __SUPERCHILD_SC_STEPFUN_H */
