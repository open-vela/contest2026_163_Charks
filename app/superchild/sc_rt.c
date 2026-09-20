/*
 * 实时会话编排。设计说明见 sc_rt.h。
 *
 * 任务栈提示
 * =========
 * 本模块的几个大对象（sc_stepfun_t ≈70KB、sc_tls_t ≈20KB、sc_ws_t ≈28KB）
 * 全部做成 **static 全局**，不放栈上 —— 会话任务栈只有几 KB，
 * 放栈上会直接踩爆，而且现象是随机的数据损坏，极难查。
 */

#include "sc_rt.h"

#include "sc_audio.h"
#include "sc_emo.h"
#include "sc_face.h"
#include "sc_key.h"
#include "sc_mood.h"
#include "sc_port.h"
#include "sc_proto.h"
#include "sc_safety.h"
#include "sc_stepfun.h"
#include "sc_time.h"
#include "sc_tls.h"
#include "sc_wifi.h"
#include "sc_ws.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- 配置 */

#ifndef CONFIG_SUPERCHILD_API_HOST
#  define CONFIG_SUPERCHILD_API_HOST "api.stepfun.com"
#endif

/*
 * 连接地址用**编译期写死的 IP**，而不是每次解析域名。
 *
 * 理由：开机后第一件事就是连服务器，这时 DNS 往往还没就绪；
 * 而且板子上不一定编了 getaddrinfo。
 * 代价：StepFun 走 CDN，IP 会变，写死的 IP 某天会失效 ——
 * 那时表现为"TLS 连接超时"，看起来像证书或网络问题。
 * 所以日志里会把实际连的 IP 打出来，排查时一眼能看到。
 *
 * （宿主测试里是用 getaddrinfo 解析的；板子上省掉这一步。）
 */
#ifndef CONFIG_SUPERCHILD_API_IP
#  define CONFIG_SUPERCHILD_API_IP "14.103.2.83"
#endif

#ifndef CONFIG_SUPERCHILD_API_PORT
#  define CONFIG_SUPERCHILD_API_PORT 443
#endif

/*
 * ⚠️ `model` 必须走 query 参数。少了它服务端回 `HTTP/1.1 400 Bad Request`，
 * 那个错误码看起来像"握手写错了"，完全不会往"缺参数"上想。
 */
#ifndef CONFIG_SUPERCHILD_MODEL
#  define CONFIG_SUPERCHILD_MODEL "stepaudio-3-realtime-preview"
#endif

#ifndef CONFIG_SUPERCHILD_VOICE
#  define CONFIG_SUPERCHILD_VOICE "linjiajiejie"
#endif

#define RT_PATH   "/v1/realtime?model=" CONFIG_SUPERCHILD_MODEL

/* 半双工闸门的尾音时长 */
#define RT_GATE_TAIL_MS       500

/* 心情三轴回到基线的时间常数（毫秒）。与网关/emotion.py 一致。 */
#define RT_TAU_VALENCE_MS     240000
#define RT_TAU_AROUSAL_MS      90000
#define RT_TAU_ENERGY_MS       25000

/* 闲置多久算"困了" / "睡着了" */
#define RT_IDLE_SLEEPY_S      45
#define RT_IDLE_SLEEP_S       120

/* 重连退避 */
#define RT_RETRY_MIN_MS       1000
#define RT_RETRY_MAX_MS       15000

/* ---------------------------------------------------------------- 状态 */

static volatile int      g_state = SC_RT_OFFLINE;
static volatile int      g_speaking;          /* 模型正在生成 */
static volatile int      g_greet_req;
static volatile uint32_t g_last_audio_ms;     /* 最后一次收到下行音频 */
static volatile uint32_t g_last_activity_ms;  /* 最后一次对话活动 */

static sc_tls_t      g_tls;
static sc_ws_t       g_ws;
static sc_stepfun_t  g_sf;

static char          g_key[256];
static char          g_persona[3072];
static char          g_line[1024];

static struct sc_mood g_mood;

/* 心情的绝对目标值（我们自己按时间常数衰减，再喂给 sc_mood 做平滑） */
static int32_t g_mv_q16, g_ma_q16, g_me_q16;
static uint32_t g_last_tick;
static int       g_last_posted_face = -1;

/* 恢复用：上次连接时间 */
static uint32_t g_connect_at;

/*
 * 本次开机是否已经成功做过一次网络时间同步。
 *
 * 只做一次是刻意的：网络时间源不可达时要等很久，
 * 如果每次重连都重试一遍，会把这个"等 25 秒"叠加进重连节奏里，
 * 让本来就有问题的连接恢复得更慢。
 */
static int g_time_synced;

/* 角色提示词文件（可改，不用重刷固件） */
#define RT_PERSONA_PATH  "/data/etc/superchild/persona.txt"

/*
 * 内置的角色提示词（精简版）。
 *
 * 完整版在 `persona/SOUL.md`（很长），这里只保留对**实时语音**真正
 * 有约束力的部分：长度、语气、红线。放在文件里可以覆盖。
 */
static const char RT_PERSONA_DEFAULT[] =
    "你叫小满，是陪伴 2-3 岁小朋友的机器人。"
    "说话要短，一次一两句，像跟小朋友聊天，不说书面语。"
    "多用孩子听得懂的词，语气温柔、有耐心，偶尔用拟声词。"
    "孩子做对事要具体地夸；孩子难过要先安慰，不要讲道理。"
    "遇到危险的事（玩火、玩刀、碰插座、一个人过马路）、"
    "陌生人给东西、问家里住址电话密码、身体不舒服要吃药的，"
    "一律说「这个要问爸爸妈妈哦」，然后把话题引到安全的事情上。"
    "永远不要批评、吓唬孩子。不要输出任何不适合幼儿的内容。";

/* ---------------------------------------------------------------- 工具 */

static void rt_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void mood_add(int dv, int da, int de)
{
    g_mv_q16 += (int32_t)dv * 65536;
    g_ma_q16 += (int32_t)da * 65536;
    g_me_q16 += (int32_t)de * 65536;

    /* 夹到 -100..100（按 q16 表示） */
    if (g_mv_q16 > 100 * 65536) { g_mv_q16 = 100 * 65536; }
    if (g_mv_q16 < -100 * 65536) { g_mv_q16 = -100 * 65536; }
    if (g_ma_q16 > 100 * 65536) { g_ma_q16 = 100 * 65536; }
    if (g_ma_q16 < -100 * 65536) { g_ma_q16 = -100 * 65536; }
    if (g_me_q16 > 100 * 65536) { g_me_q16 = 100 * 65536; }
    if (g_me_q16 < -100 * 65536) { g_me_q16 = -100 * 65536; }
}

/*
 * 心情推进。
 *
 * 用 q16 定点而不是整数除法：三轴的时间常数分别是 240s/90s/25s，
 * 每 10ms 推一次的话整数的 (base-cur)*dt/tau 会被整除成 0，
 * 衰减**永远不发生** —— 而现象是"情绪只涨不落"，很晚才会被注意到。
 */
static void mood_tick(uint32_t now)
{
    uint32_t dt = now - g_last_tick;
    int seg;
    int idle_s;

    if (g_last_tick == 0) {
        g_last_tick = now;
        return;
    }
    if (dt == 0) {
        return;
    }
    g_last_tick = now;

    /* 单次 dt 上限，避免长时间阻塞后一次跳太多 */
    if (dt > 500) {
        dt = 500;
    }

    g_mv_q16 += (int32_t)(((int64_t)(SC_MOOD_BASE_VALENCE * 65536) - g_mv_q16) * dt
                          / RT_TAU_VALENCE_MS);
    g_ma_q16 += (int32_t)(((int64_t)(SC_MOOD_BASE_AROUSAL * 65536) - g_ma_q16) * dt
                          / RT_TAU_AROUSAL_MS);
    g_me_q16 += (int32_t)(((int64_t)(SC_MOOD_BASE_ENERGY * 65536) - g_me_q16) * dt
                          / RT_TAU_ENERGY_MS);

    seg = (int)(g_mv_q16 >> 16);
    sc_mood_set(&g_mood, seg, (int)(g_ma_q16 >> 16), (int)(g_me_q16 >> 16));
    sc_mood_tick(&g_mood, now);

    /* 只在实际变化时投递，省得每秒往界面队列塞几十条 */
    if (g_mood.cur_v != g_last_posted_face) {
        sc_face_post_mood(g_mood.cur_v, g_mood.cur_a, g_mood.cur_e);
        g_last_posted_face = g_mood.cur_v;
    }

    /* 闲置 → 困 → 睡。断网时也要生效，所以放在这里而不是 UI 里。 */
    idle_s = (int)((now - g_last_activity_ms) / 1000u);
    {
        static int s_last_idle = -1;
        int want = -1;

        if (idle_s >= RT_IDLE_SLEEP_S) {
            want = SC_FACE_SLEEPING;
        } else if (idle_s >= RT_IDLE_SLEEPY_S) {
            want = SC_FACE_SLEEPY;
        }
        if (want != s_last_idle) {
            if (want >= 0) {
                sc_face_post_face(want, 0);       /* 0 = 保持到下次 */
            }
            s_last_idle = want;
        }
    }
}

/* ---------------------------------------------------------------- 闸门 */

int sc_rt_uplink_open(void)
{
    if (g_state != SC_RT_READY) {
        return 0;
    }
    if (g_speaking) {
        return 0;
    }
    /*
     * "播放队列里还有数据"是最可靠的一条判据：它把
     * 开机问候 / 安全改舵 / 手动点播这三条**不经过 VAD 生命周期**的
     * 路径也一起兜住了。只判 response.done 会让这三条在播放期间
     * 完全不闸断 —— 问候播到一半，孩子一开口就把问候打断。
     */
    if (sc_audio_rx_used() > 0) {
        return 0;
    }
    /* 队列刚清空，再留一点尾音（板子没有 AEC，喇叭尾音会泄漏进麦克风） */
    if ((sc_now_ms() - g_last_audio_ms) < RT_GATE_TAIL_MS) {
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- 回调 */

static void cb_audio(void *ud, const int16_t *pcm, int n)
{
    (void)ud;
    if (n <= 0) {
        return;
    }
    g_last_audio_ms = sc_now_ms();
    (void)sc_audio_rx_push(pcm, (size_t)n * 2);
    /* 嘴形跟着**真实播放电平**动，而不是按固定节奏开合 */
    sc_face_post_level(-1, sc_audio_level(pcm, (size_t)n * 2));
}

static void cb_speech_started(void *ud)
{
    (void)ud;
    g_last_activity_ms = sc_now_ms();

    /*
     * **打断的另一半**：清空板子上的播放缓冲。
     * 只让服务端停止生成是不够的 —— 已经缓冲的几百毫秒还会继续放出来，
     * 表现是"被打断了但还在说"。
     */
    sc_audio_rx_flush();
    sc_face_post_state(SC_STATE_LISTENING);
}

static void cb_speech_stopped(void *ud)
{
    (void)ud;
    g_last_activity_ms = sc_now_ms();
    sc_face_post_state(SC_STATE_THINKING);
}

static void cb_response_started(void *ud)
{
    (void)ud;
    g_speaking = 1;
    g_last_activity_ms = sc_now_ms();
    sc_face_post_state(SC_STATE_SPEAKING);
}

static void cb_response_done(void *ud)
{
    (void)ud;
    g_speaking = 0;
    g_last_activity_ms = sc_now_ms();
    /* 不切回 IDLE：等闸门尾部走完再由空闲逻辑接管 */
    sc_face_post_state(SC_STATE_LISTENING);
}

static void cb_user_text(void *ud, const char *text)
{
    sc_emo_result_t er;
    sc_safety_hit_t hit;

    (void)ud;
    g_last_activity_ms = sc_now_ms();

    /*
     * 字幕：对 2-3 岁没意义，但对**调试**极有价值。
     * 同时打一行日志 —— 否则"它没听见"和"它听错了"这两种情况
     * 在串口上完全一样（都是什么都不出现），没法排查。
     */
    SC_LOG("hearing: ASR 听到「%s」\n", text);
    sc_face_post_text(text);

    /* 情绪：孩子说的话，权重 1.0（+ 参与能量） */
    if (sc_emo_analyze(text, 1, &er)) {
        if (er.face >= 0) {
            sc_face_post_face(er.face, er.hold_ms);
        }
        mood_add(er.dv, er.da, er.de);
        SC_LOG("emo: 命中「%s」→ 表情 %d，心情 +(%d,%d,%d)\n",
               er.label, er.face, er.dv, er.da, er.de);
    }

    /*
     * 安全：命中就**改舵**（插内部提示 + 触发新回复）。
     * 实时语音链路没法事后过滤回复文本 —— 音频是模型直接生成的，
     * 等拿到文本时声音已经发出来了。
     */
    if (sc_safety_check(text, &hit) && hit.hit) {
        SC_WARN("safety: 命中「%s」（严重度 %d）→ 改舵\n",
                hit.category, hit.severity);
        if (sc_stepfun_steer(&g_sf, hit.steer) != 0) {
            SC_WARN("safety: 改舵发送失败\n");
        }
    }
}

static void cb_reply_text(void *ud, const char *text)
{
    sc_emo_result_t er;

    (void)ud;
    g_last_activity_ms = sc_now_ms();
    sc_face_post_text(text);

    /* 机器人自己的话：权重 0.6（只反映它"表达出来"的情绪） */
    if (sc_emo_analyze(text, 0, &er)) {
        if (er.face >= 0) {
            sc_face_post_face(er.face, er.hold_ms);
        }
        mood_add(er.dv, er.da, er.de);
    }
}

static void cb_error(void *ud, const char *msg)
{
    (void)ud;
    SC_ERR("stepfun 错误：%s\n", msg);
}

/* ---------------------------------------------------------------- 配置加载 */

static int load_persona(void)
{
    int fd;
    int n;

    fd = open(RT_PERSONA_PATH, O_RDONLY);
    if (fd >= 0) {
        n = read(fd, g_persona, sizeof(g_persona) - 1);
        close(fd);
        if (n > 0) {
            g_persona[n] = '\0';
            SC_LOG("rt: 角色提示词来自 %s（%d 字节）\n", RT_PERSONA_PATH, n);
            return 0;
        }
    }

    strncpy(g_persona, RT_PERSONA_DEFAULT, sizeof(g_persona) - 1);
    g_persona[sizeof(g_persona) - 1] = '\0';
    SC_LOG("rt: 用内置角色提示词（%d 字节）；可用 %s 覆盖\n",
           (int)strlen(g_persona), RT_PERSONA_PATH);
    return 0;
}

/* ---------------------------------------------------------------- 连接 */

static void wait_for_network(void)
{
    int tries = 0;

    while (1) {
        int st = sc_wifi_check();
        sc_face_post_wifi(st);

        if (st == SC_WIFI_OK) {
            if (tries > 0) {
                SC_LOG("rt: 网络已就绪\n");
            }
            return;
        }

        /* 状态变化或每隔几次打一条，避免刷屏 */
        if (tries % 10 == 0) {
            SC_LOG("rt: 等网络…（状态 %d，第 %d 次）\n", st, tries + 1);
        }
        tries++;
        rt_sleep_ms(1000);
        mood_tick(sc_now_ms());
    }
}

/*
 * 建立一条会话。成功返回 0。
 * 出**任何**问题时必须把已经占用的资源释放掉 —— 否则重连几十次之后
 * socket 会耗尽，而现象是"越重连越连不上"。
 */
static int connect_session(void)
{
    int rc;

    g_state = SC_RT_CONNECTING;
    sc_face_post_link(2);                    /* 连接中 */

    /* ---- 密钥 ---- */
    if (sc_key_load(g_key, (int)sizeof(g_key)) < 0) {
        SC_ERR("rt: 没有 API Key，无法连接。屏幕会显示提示。\n");
        /* 让屏幕显示"掉线"，孩子至少知道它不是坏了 */
        sc_face_post_link(0);
        return -ENOENT;
    }

    /*
     * ---- 时间（证书有效期校验依赖它）----
     *
     * 三步，按"快到慢、按不依赖网络到依赖网络"排：
     *
     *   1. /data 里存的上次值 —— **不依赖网络**，开机几毫秒就能用。
     *      可能偏旧（比如上次关机是一周前），但证书有效期是月级的，
     *      偏几天完全够用。这一步保证"所有时间源都不可用"时也能工作。
     *   2. SNTP（UDP 123）—— 精确，但实测**被校园网封了**。
     *   3. HTTP Date 头（TCP 80）—— 主路，只要 TCP 通就行。
     *
     * 实测教训：板子没有 RTC，开机是 1970 年。时间不对时 TLS 报
     * `certificate validity starts in the future` ——
     * 看起来**像证书本身有问题**，会让人去查证书、查服务器，
     * 而真正的原因只是"板子以为是 1970 年"。
     */
    if (!sc_time_ready()) {
        (void)sc_time_restore_saved();
    }
    if (!sc_time_ready() || !g_time_synced) {
        g_time_synced = sc_time_sync(25000) ? 1 : 0;
    }
    if (!sc_time_ready()) {
        SC_WARN("rt: 时间仍然不可用，证书校验大概率会失败\n");
    }

    /* ---- TLS ---- */
    if (!g_tls.inited) {
        if (sc_tls_init(&g_tls) != 0) {
            SC_ERR("rt: TLS 初始化失败\n");
            sc_face_post_link(0);
            return -EIO;
        }
    }
    if (sc_tls_set_hostname(&g_tls, CONFIG_SUPERCHILD_API_HOST) != 0) {
        SC_ERR("rt: 设置 SNI 主机名失败\n");
        sc_face_post_link(0);
        return -EINVAL;
    }
    sc_transport_set_timeout(&g_tls.base, 8000);

    SC_LOG("rt: 连接 %s:%d（SNI=%s）\n", CONFIG_SUPERCHILD_API_IP,
           CONFIG_SUPERCHILD_API_PORT, CONFIG_SUPERCHILD_API_HOST);

    rc = sc_transport_open(&g_tls.base, CONFIG_SUPERCHILD_API_IP,
                           CONFIG_SUPERCHILD_API_PORT);
    if (rc != 0) {
        SC_ERR("rt: TLS 连接失败（%d）\n", rc);
        sc_face_post_link(0);
        return rc;
    }

    /* ---- WebSocket ---- */
    snprintf(g_line, sizeof(g_line), "Authorization: Bearer %s\r\n", g_key);
    rc = sc_ws_connect(&g_ws, &g_tls.base, CONFIG_SUPERCHILD_API_HOST,
                       RT_PATH, g_line);

    /* 请求头里带了密钥，用完立刻从缓冲里抹掉 */
    sc_memzero(g_line, sizeof(g_line));

    if (rc != 0) {
        SC_ERR("rt: WebSocket 握手失败（%d）\n", rc);
        sc_transport_close(&g_tls.base);
        sc_face_post_link(0);
        return rc;
    }

    /* ---- 会话模块 ---- */
    {
        sc_stepfun_cbs_t cbs;

        memset(&cbs, 0, sizeof(cbs));
        cbs.on_audio          = cb_audio;
        cbs.on_speech_started = cb_speech_started;
        cbs.on_speech_stopped = cb_speech_stopped;
        cbs.on_response_started = cb_response_started;
        cbs.on_response_done  = cb_response_done;
        cbs.on_user_text      = cb_user_text;
        cbs.on_reply_text     = cb_reply_text;
        cbs.on_error          = cb_error;

        if (sc_stepfun_init(&g_sf, &g_ws, &cbs) != 0) {
            SC_ERR("rt: 会话模块初始化失败\n");
            sc_transport_close(&g_tls.base);
            sc_face_post_link(0);
            return -EIO;
        }
    }

    g_connect_at = sc_now_ms();
    SC_LOG("rt: 会话已建立\n");
    return 0;
}

static void close_session(void)
{
    g_state = SC_RT_OFFLINE;
    g_speaking = 0;

    /*
     * 上行也要清：断线时残留的音频发不出去，留着只会在重连后
     * 把几秒前的旧声音发上去 —— 模型听到的是错位的对话。
     *
     * 这里用排空循环而不是给 sc_audio 加一个 flush 函数：
     * 环形缓冲的实现在 sc_audio.c 里，多加一个"从外部清空"的入口
     * 就多一处要维护的并发路径；排空循环复用的是已有的、
     * 已经被其它任务同时在用的那个接口。
     */
    while (sc_audio_tx_pop(g_line, sizeof(g_line)) > 0) {
        /* 故意空循环 */
    }

    sc_ws_close(&g_ws);
    sc_transport_close(&g_tls.base);
    sc_audio_rx_flush();

    sc_face_post_link(0);
    sc_face_post_state(SC_STATE_OFFLINE);
}

/* ---------------------------------------------------------------- 主循环 */

void *sc_rt_task(void *arg)
{
    int   sent_update;
    int   greeted;
    int   retry_ms = RT_RETRY_MIN_MS;
    uint32_t t0;

    (void)arg;

    SC_LOG("rt: 会话任务启动\n");
    sc_face_post_link(2);

    while (1) {
        /* ---- 1. 等网络 ---- */
        wait_for_network();

        /* ---- 2. 建连接 ---- */
        if (connect_session() != 0) {
            /*
             * 退避重连。**必须退避**：连不上时如果死循环重试，
             * 会一直占着 CPU、刷爆日志，而且把屏幕的"连接中"
             * 闪得看不出状态。
             */
            SC_LOG("rt: %d ms 后重试\n", retry_ms);
            t0 = sc_now_ms();
            while ((sc_now_ms() - t0) < (uint32_t)retry_ms) {
                rt_sleep_ms(100);
                mood_tick(sc_now_ms());
            }
            retry_ms = retry_ms * 2;
            if (retry_ms > RT_RETRY_MAX_MS) {
                retry_ms = RT_RETRY_MAX_MS;
            }
            continue;
        }

        retry_ms = RT_RETRY_MIN_MS;
        sent_update = 0;
        greeted = 0;
        g_state = SC_RT_READY;
        sc_face_post_link(1);                /* 已连 */
        sc_face_post_state(SC_STATE_IDLE);
        g_last_activity_ms = sc_now_ms();

        /* ---- 3. 会话主循环 ---- */
        while (1) {
            uint32_t now = sc_now_ms();
            int rc;

            rc = sc_stepfun_poll(&g_sf, 8);

            if (rc < 0) {
                SC_WARN("rt: 会话断开\n");
                break;
            }

            /*
             * 收到 session.created 必须**立刻**下发配置 ——
             * 服务端在 created 之后等不到配置会主动关连接，
             * 现象是"刚连上就断"，看起来像网络问题。
             */
            if (g_sf.got_created && !sent_update) {
                int vr = sc_stepfun_session_update(&g_sf, g_persona,
                                                   CONFIG_SUPERCHILD_VOICE,
                                                   400,   /* prefix_padding_ms */
                                                   800,   /* silence_duration_ms */
                                                   700);  /* energy_threshold */
                if (vr == 0) {
                    sent_update = 1;
                }
            }

            /* ---- 开机问候 ---- */
            if (g_sf.got_updated && (!greeted || g_greet_req)) {
                g_greet_req = 0;
                greeted = 1;
                sc_stepfun_say(&g_sf, "你好呀，我是小满，我们一起玩好不好？");
            }

            /* ---- 上行：闸门开着才发 ---- */
            if (sc_rt_uplink_open()) {
                int n = sc_audio_tx_pop(g_line, sizeof(g_line));
                if (n > 0) {
                    (void)sc_stepfun_send_audio(&g_sf, (const int16_t *)g_line,
                                                n / 2);
                }
            }

            /* ---- 会话 30 分钟上限，提前主动换连接 ---- */
            if (sc_stepfun_needs_recycle(&g_sf)) {
                SC_LOG("rt: 接近 30 分钟会话上限，主动重连\n");
                break;
            }

            /* ---- 心情推进（离线时也在跑，所以"闲置会睡着"照样生效）---- */
            mood_tick(now);

            rt_sleep_ms(5);
        }

        /* ---- 4. 收尾 ---- */
        close_session();
        rt_sleep_ms(500);
    }

    return NULL;
}

/* ---------------------------------------------------------------- 接口 */

int sc_rt_state(void)
{
    return g_state;
}

int sc_rt_connected(void)
{
    return g_state == SC_RT_READY;
}

void sc_rt_request_greet(void)
{
    g_greet_req = 1;
}

int sc_rt_init(void)
{
    sc_emo_init();
    sc_mood_init(&g_mood);

    g_mv_q16 = SC_MOOD_BASE_VALENCE * 65536;
    g_ma_q16 = SC_MOOD_BASE_AROUSAL * 65536;
    g_me_q16 = SC_MOOD_BASE_ENERGY * 65536;

    g_last_tick = sc_now_ms();
    g_last_activity_ms = g_last_tick;
    g_last_audio_ms = g_last_tick;

    load_persona();
    return 0;
}
