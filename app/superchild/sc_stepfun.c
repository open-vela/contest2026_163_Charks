/*
 * StepFun Realtime 会话（板端）。设计取舍见 sc_stepfun.h。
 *
 * JSON 为什么用 cJSON 而不是手拼
 * -----------------------------
 * 上行要发 `instructions`（人格提示词，含中文、引号、换行）和
 * base64 音频。手拼 JSON 最危险的**不是引号**，而是：
 *
 *   - 提示词里出现一个未转义的 `"` → 整条 session.update 被服务端判为
 *     非法 JSON，报的是"无效的会话配置"，看起来像参数写错了
 *   - 中文如果按字节截断在 UTF-8 字符中间 → 同样是非法 JSON
 *
 * 这两种错都会让人去查参数而不是查转义。cJSON 用起来只多几行，
 * 但把这一整类问题消掉了。
 */

#include "sc_stepfun.h"
#include "sc_b64.h"
#include "sc_port.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SC_HOST_TEST
#  include "cJSON.h"
#else
#  include <netutils/cJSON.h>
#endif

/* ============================================================ 发送辅助 */

/* 把 cJSON 对象序列化并发出去。**负责释放 obj**，调用方不要再 free。 */
static int send_json(sc_stepfun_t *s, cJSON *obj)
{
    char *text;
    int rc;

    if (obj == NULL) {
        return -ENOMEM;
    }

    text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (text == NULL) {
        return -ENOMEM;
    }

    rc = sc_ws_send_text(s->ws, text, strlen(text));
    cJSON_free(text);

    if (rc != 0) {
        s->connected = 0;
    }
    return rc;
}

/* ============================================================ 初始化 */

int sc_stepfun_init(sc_stepfun_t *s, sc_ws_t *ws, const sc_stepfun_cbs_t *cbs)
{
    if (s == NULL || ws == NULL) {
        return -EINVAL;
    }

    memset(s, 0, sizeof(*s));
    s->ws = ws;
    if (cbs != NULL) {
        s->cbs = *cbs;
    }

    /* 两个方向的重采样器都要**跨块保留状态**。每次新建会产生
     * 块边界的周期性咔哒声，听起来像网络丢包，很难归因。 */
    if (sc_resamp_init(&s->up, 16000, 24000) != 0) {
        SC_ERR("stepfun: 上行重采样器初始化失败\n");
        return -EIO;
    }
    if (sc_resamp_init(&s->down, 24000, 16000) != 0) {
        SC_ERR("stepfun: 下行重采样器初始化失败\n");
        return -EIO;
    }

    s->opened_at = sc_now_ms();
    s->connected = 1;
    return 0;
}

/* ============================================================ session.update */

int sc_stepfun_session_update(sc_stepfun_t *s, const char *instructions,
                              const char *voice, int prefix_pad_ms,
                              int silence_ms, int energy_thresh)
{
    cJSON *root;
    cJSON *sess;
    cJSON *vad;
    int rc;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return -ENOMEM;
    }
    sess = cJSON_CreateObject();
    if (sess == NULL) {
        cJSON_Delete(root);
        return -ENOMEM;
    }

    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddItemToObject(root, "session", sess);

    {
        cJSON *mods = cJSON_CreateArray();
        cJSON_AddItemToArray(mods, cJSON_CreateString("text"));
        cJSON_AddItemToArray(mods, cJSON_CreateString("audio"));
        cJSON_AddItemToObject(sess, "modalities", mods);
    }

    if (instructions != NULL && instructions[0] != '\0') {
        cJSON_AddStringToObject(sess, "instructions", instructions);
    }

    /*
     * 音色必须显式设。
     * 服务端**默认是 `jingdiannvsheng`**（实测 session.created 里回来的就是它），
     * 不是我们想要的那个。不设的话会一直是默认音色，而现象只是
     * "声音不对" —— 很容易被当成"模型音色就这样"。
     */
    if (voice != NULL && voice[0] != '\0') {
        cJSON_AddStringToObject(sess, "voice", voice);
    }

    cJSON_AddStringToObject(sess, "input_audio_format", "pcm16");
    cJSON_AddStringToObject(sess, "output_audio_format", "pcm16");

    vad = cJSON_CreateObject();
    cJSON_AddStringToObject(vad, "type", "server_vad");
    cJSON_AddNumberToObject(vad, "prefix_padding_ms", prefix_pad_ms);
    cJSON_AddNumberToObject(vad, "silence_duration_ms", silence_ms);
    cJSON_AddNumberToObject(vad, "energy_awakeness_threshold", energy_thresh);
    cJSON_AddItemToObject(sess, "turn_detection", vad);

    rc = send_json(s, root);
    if (rc == 0) {
        SC_LOG("stepfun: 已发 session.update（voice=%s, 静音=%dms, 门限=%d）\n",
               voice != NULL ? voice : "(默认)", silence_ms, energy_thresh);
    }
    return rc;
}

/* ============================================================ 上行 */

int sc_stepfun_send_audio(sc_stepfun_t *s, const int16_t *pcm16k, int n16k)
{
    int off = 0;

    if (s == NULL || pcm16k == NULL || n16k <= 0) {
        return 0;
    }
    if (!s->connected) {
        return -ENOTCONN;
    }

    /*
     * 输入按 SC_STEPFUN_UP_SLICE 切片，避免"大输入 + 小输出缓冲"
     * 导致重采样器返回 -1。板子上每次是 20ms（320 样本），
     * 一次就过；切片是为了容忍调用方给大块。
     */
    while (off < n16k) {
        int take = n16k - off;
        int n24;
        int i;

        if (take > SC_STEPFUN_UP_SLICE) {
            take = SC_STEPFUN_UP_SLICE;
        }

        n24 = sc_resamp_process(&s->up, pcm16k + off, take,
                                s->ul_pcm24,
                                (int)(sizeof(s->ul_pcm24) / sizeof(s->ul_pcm24[0])));
        if (n24 < 0) {
            SC_ERR("stepfun: 上行重采样失败（输出缓冲不足？）\n");
            return -EIO;
        }
        off += take;

        /* 按 ≤60ms 分块发送（官方建议 20~30ms，60ms 是硬上限） */
        for (i = 0; i < n24; i += SC_STEPFUN_MAX_UP_MS * 24) {
            int chunk = n24 - i;
            int bytes;
            cJSON *o;

            if (chunk > SC_STEPFUN_MAX_UP_MS * 24) {
                chunk = SC_STEPFUN_MAX_UP_MS * 24;
            }
            bytes = chunk * 2;

            if (sc_b64_encode(s->ul_pcm24 + i, (size_t)bytes,
                              s->b64, sizeof(s->b64)) < 0) {
                SC_ERR("stepfun: base64 失败（缓冲不足）\n");
                return -EIO;
            }

            o = cJSON_CreateObject();
            if (o == NULL) {
                return -ENOMEM;
            }
            cJSON_AddStringToObject(o, "type", "input_audio_buffer.append");
            cJSON_AddStringToObject(o, "audio", s->b64);

            if (send_json(s, o) != 0) {
                return -EIO;
            }
        }
    }

    return 0;
}

/* ============================================================ 让它说话 */

int sc_stepfun_say(sc_stepfun_t *s, const char *text)
{
    cJSON *root;
    cJSON *item;
    cJSON *content;
    char line[1024];
    int rc;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    /*
     * 包一层"请你原样念出"：直接发文本时模型会把它当**用户说的话**
     * 来回应，而不是照念。这是实测过的差别。
     */
    snprintf(line, sizeof(line),
             "请你原样、不加任何修改地念出下面这句话：%s", text);

    root = cJSON_CreateObject();
    item = cJSON_CreateObject();
    content = cJSON_CreateArray();
    if (root == NULL || item == NULL || content == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(item);
        cJSON_Delete(content);
        return -ENOMEM;
    }

    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON_AddStringToObject(item, "type", "message");
    cJSON_AddStringToObject(item, "role", "user");
    {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "type", "input_text");
        cJSON_AddStringToObject(c, "text", line);
        cJSON_AddItemToArray(content, c);
    }
    cJSON_AddItemToObject(item, "content", content);
    cJSON_AddItemToObject(root, "item", item);

    rc = send_json(s, root);
    if (rc != 0) {
        return rc;
    }

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "response.create");
    return send_json(s, root);
}

int sc_stepfun_steer(sc_stepfun_t *s, const char *hint)
{
    cJSON *root;
    cJSON *item;
    cJSON *content;
    char line[1024];
    int rc;

    if (hint == NULL || hint[0] == '\0') {
        return 0;
    }

    /*
     * 用「（小提示，请照做：…）」包成旁白，而不是 system 角色。
     * 实测 system 项在会话中途插入时模型不太理会。
     */
    snprintf(line, sizeof(line), "（小提示，请照做：%s）", hint);

    root = cJSON_CreateObject();
    item = cJSON_CreateObject();
    content = cJSON_CreateArray();
    if (root == NULL || item == NULL || content == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(item);
        cJSON_Delete(content);
        return -ENOMEM;
    }

    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON_AddStringToObject(item, "type", "message");
    cJSON_AddStringToObject(item, "role", "user");
    {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "type", "input_text");
        cJSON_AddStringToObject(c, "text", line);
        cJSON_AddItemToArray(content, c);
    }
    cJSON_AddItemToObject(item, "content", content);
    cJSON_AddItemToObject(root, "item", item);

    rc = send_json(s, root);
    if (rc != 0) {
        return rc;
    }
    SC_LOG("stepfun: 安全改舵 → %s\n", hint);

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "response.create");
    return send_json(s, root);
}

int sc_stepfun_cancel(sc_stepfun_t *s)
{
    cJSON *root;

    if (!s->connected) {
        return -ENOTCONN;
    }
    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "response.cancel");
    /* 没有进行中的回复时服务端会回 error，属于正常副作用，忽略即可 */
    return send_json(s, root);
}

/* ============================================================ 下行 */

/* 把攒够的下行样本投给调用方 */
static void down_flush(sc_stepfun_t *s)
{
    if (s->downbuf_len > 0 && s->cbs.on_audio != NULL) {
        s->cbs.on_audio(s->cbs.ud, s->downbuf, s->downbuf_len);
        s->downbuf_len = 0;
    }
}

static void down_push(sc_stepfun_t *s, const int16_t *pcm, int n)
{
    while (n > 0) {
        int space = (int)(sizeof(s->downbuf) / sizeof(s->downbuf[0])) - s->downbuf_len;
        int take;

        if (space <= 0) {
            down_flush(s);
            space = (int)(sizeof(s->downbuf) / sizeof(s->downbuf[0]));
        }
        take = n < space ? n : space;
        memcpy(s->downbuf + s->downbuf_len, pcm, (size_t)take * 2);
        s->downbuf_len += take;
        pcm += take;
        n -= take;

        /* 攒够 40ms 就投一次：太小会让播放任务频繁唤醒，太大又加延迟 */
        if (s->downbuf_len >= SC_STEPFUN_DOWN_FLUSH) {
            down_flush(s);
        }
    }
}

/* ============================================================ 事件分发 */

static int handle_event(sc_stepfun_t *s, const char *json)
{
    cJSON *root;
    cJSON *type_item;
    const char *type;

    root = cJSON_Parse(json);
    if (root == NULL) {
        /*
         * 解析失败**不致命**：丢掉这一条继续。
         * 一条看不懂的事件不该让整场对话断掉。
         */
        SC_WARN("stepfun: 事件不是合法 JSON（%d 字节），已丢弃\n",
                (int)strlen(json));
        return 0;
    }

    type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_item)) {
        cJSON_Delete(root);
        return 0;
    }
    type = type_item->valuestring;

    /* ---------------- 会话 ---------------- */
    if (strcmp(type, "session.created") == 0) {
        cJSON *sess = cJSON_GetObjectItemCaseSensitive(root, "session");
        cJSON *voice = sess != NULL ?
            cJSON_GetObjectItemCaseSensitive(sess, "voice") : NULL;
        SC_LOG("stepfun: session.created（服务端默认音色=%s）\n",
               (voice != NULL && cJSON_IsString(voice)) ? voice->valuestring : "?");
        s->got_created = 1;
        cJSON_Delete(root);
        return 0;
    }
    if (strcmp(type, "session.updated") == 0) {
        SC_LOG("stepfun: 会话配置已生效\n");
        s->got_updated = 1;
        cJSON_Delete(root);
        return 0;
    }

    /* ---------------- VAD ---------------- */
    if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
        if (s->speaking) {
            /*
             * **打断**：孩子插话了。
             *
             * 这里只做"让服务端停止生成"这一半；另一半
             * （清空板子上的播放缓冲）必须由调用方在
             * on_speech_started 里做 —— 否则已经缓冲的几百毫秒
             * 还会继续放出来，表现是"被打断了但还在说"。
             */
            SC_LOG("stepfun: 检测到插话 → 打断当前回复\n");
            (void)sc_stepfun_cancel(s);
        }
        if (s->cbs.on_speech_started != NULL) {
            s->cbs.on_speech_started(s->cbs.ud);
        }
        cJSON_Delete(root);
        return 0;
    }
    if (strcmp(type, "input_audio_buffer.speech_stopped") == 0) {
        if (s->cbs.on_speech_stopped != NULL) {
            s->cbs.on_speech_stopped(s->cbs.ud);
        }
        cJSON_Delete(root);
        return 0;
    }

    /* ---------------- 孩子说的话（ASR 结果）---------------- */
    if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        if (t != NULL && cJSON_IsString(t) && s->cbs.on_user_text != NULL) {
            SC_LOG("stepfun: 孩子说「%s」\n", t->valuestring);
            s->cbs.on_user_text(s->cbs.ud, t->valuestring);
        }
        cJSON_Delete(root);
        return 0;
    }

    /* ---------------- 模型回复 ---------------- */
    if (strcmp(type, "response.created") == 0) {
        s->speaking = 1;
        if (s->cbs.on_response_started != NULL) {
            s->cbs.on_response_started(s->cbs.ud);
        }
        cJSON_Delete(root);
        return 0;
    }

    if (strcmp(type, "response.audio.delta") == 0) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "delta");
        int n;

        if (d != NULL && cJSON_IsString(d)) {
            int raw_len = (int)strlen(d->valuestring);

            /*
             * 解出的字节数 = raw_len/4*3，24k 样本数 = 字节数/2。
             * 实测服务端一次给 10924 字符 → 4096 个样本 ⇒
             * dl_pcm24 必须按**下行**的尺寸开，不能照上行推。
             */
            n = sc_b64_decode(d->valuestring, (size_t)raw_len,
                              s->dl_pcm24, sizeof(s->dl_pcm24));
            if (n > 0) {
                int n24 = n / 2;

                if (n24 > 0) {
                    int n16 = sc_resamp_process(&s->down, s->dl_pcm24, n24,
                                                s->dl_pcm16,
                                                (int)(sizeof(s->dl_pcm16) /
                                                      sizeof(s->dl_pcm16[0])));
                    if (n16 > 0) {
                        down_push(s, s->dl_pcm16, n16);
                    } else if (n16 < 0) {
                        SC_ERR("stepfun: 下行重采样输出缓冲不足\n");
                    }
                }
            } else if (n < 0) {
                /*
                 * 解不开只有两种原因：base64 里有非法字符，
                 * 或者**输出缓冲不够**（sc_b64_decode 两种情况都返回 -1）。
                 * 所以把需要的大小一起打出来，免得下次又要猜。
                 */
                SC_WARN("stepfun: 音频增量 base64 解不开（%d 字符，"
                        "需要约 %d 字节，缓冲 %d 字节）\n",
                        raw_len, (int)((raw_len / 4) * 3),
                        (int)sizeof(s->dl_pcm24));
            }
        }
        cJSON_Delete(root);
        return 0;
    }

    if (strcmp(type, "response.audio_transcript.done") == 0) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        if (t != NULL && cJSON_IsString(t) && s->cbs.on_reply_text != NULL) {
            SC_LOG("stepfun: 机器人说「%s」\n", t->valuestring);
            s->cbs.on_reply_text(s->cbs.ud, t->valuestring);
        }
        cJSON_Delete(root);
        return 0;
    }

    if (strcmp(type, "response.audio_transcript.delta") == 0) {
        /* 只看最终文本，增量不进日志（会非常吵） */
        cJSON_Delete(root);
        return 0;
    }

    if (strcmp(type, "response.done") == 0) {
        /*
         * **必须冲刷残余**。剩下不足 40ms 的那点样本如果留着，
         * 最后几个字的尾音就丢了 —— 表现是机器人"最后一个字没说完"。
         */
        down_flush(s);
        s->speaking = 0;
        if (s->cbs.on_response_done != NULL) {
            s->cbs.on_response_done(s->cbs.ud);
        }
        cJSON_Delete(root);
        return 0;
    }

    /* ---------------- 错误 ---------------- */
    if (strcmp(type, "error") == 0) {
        cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON *msg = err != NULL ?
            cJSON_GetObjectItemCaseSensitive(err, "message") : NULL;
        cJSON *code = err != NULL ?
            cJSON_GetObjectItemCaseSensitive(err, "code") : NULL;
        const char *text = (msg != NULL && cJSON_IsString(msg)) ?
            msg->valuestring : "(无消息)";

        /*
         * `no active response` 是我们自己 cancel 的正常副作用
         * （打断时经常发生），不算错误，不必刷屏也不必上报。
         */
        if (strstr(text, "no active response") != NULL ||
            strstr(text, "cancel") != NULL) {
            SC_DBG("stepfun: cancel 回执：%s\n", text);
            cJSON_Delete(root);
            return 0;
        }

        s->errors++;
        SC_ERR("stepfun: 服务端报错 [%s] %s\n",
               (code != NULL && cJSON_IsString(code)) ? code->valuestring : "?",
               text);
        if (s->cbs.on_error != NULL) {
            s->cbs.on_error(s->cbs.ud, text);
        }
        cJSON_Delete(root);
        return 0;
    }

    SC_DBG("stepfun: 未处理事件 %s\n", type);
    cJSON_Delete(root);
    return 0;
}

int sc_stepfun_poll(sc_stepfun_t *s, int max_msgs)
{
    int i;

    if (s == NULL || !s->connected) {
        return -ENOTCONN;
    }

    for (i = 0; i < max_msgs; i++) {
        int n = sc_ws_recv_text(s->ws, s->rxmsg, sizeof(s->rxmsg));

        if (n == -EAGAIN) {
            return 0;                      /* 本轮没消息，正常 */
        }
        if (n == -EMSGSIZE) {
            SC_WARN("stepfun: 单条事件超过 %d 字节，已丢弃\n",
                    (int)sizeof(s->rxmsg));
            continue;
        }
        if (n <= 0) {
            SC_WARN("stepfun: 连接已断（recv 返回 %d）\n", n);
            s->connected = 0;
            return -ECONNRESET;
        }

        (void)handle_event(s, s->rxmsg);
    }

    return 0;
}

/* ============================================================ 状态 */

int sc_stepfun_is_speaking(const sc_stepfun_t *s)
{
    return s != NULL && s->speaking;
}

int sc_stepfun_connected(const sc_stepfun_t *s)
{
    return s != NULL && s->connected;
}

uint32_t sc_stepfun_age_ms(const sc_stepfun_t *s)
{
    return s == NULL ? 0 : (sc_now_ms() - s->opened_at);
}

int sc_stepfun_needs_recycle(const sc_stepfun_t *s)
{
    /*
     * 会话硬上限 30 分钟。**必须主动提前重连** —— 被服务端掐断时
     * 孩子正说着话，那种断线是能直接感觉到的；主动重连则发生在
     * 空闲时刻，几乎无感。留 90 秒余量。
     */
    return sc_stepfun_age_ms(s) >= SC_STEPFUN_RECYCLE_MS;
}
