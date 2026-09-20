/*
 * 直连真实 StepFun Realtime —— 方案 C 的**关键分水岭**测试。
 *
 * 分两段：
 *   A 段（协议栈）：TLS → WebSocket → 鉴权 → session.created
 *   B 段（会话）：  session.update → 让它说话 → 收下行音频 → 解码 → 重采样
 *
 * A 段通过说明"这条路走得通"；B 段通过说明"整条音频链路是对的" ——
 * 尤其是 B 段里的下行解码+重采样：如果 base64 分块解错、或者
 * 24k→16k 的重采样状态没跨块保持，声音会断续或变调，
 * 而那类问题在板子上极难和"喇叭不好"区分开。
 *
 * 用法：
 *     ./test_stepfun <API_KEY>
 */

#include "sc_port.h"
#include "sc_stepfun.h"
#include "sc_tls.h"
#include "sc_ws.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define STEPFUN_HOST   "api.stepfun.com"
#define STEPFUN_PORT   443

/*
 * ⚠️ **`model` 必须走 query 参数**，不是放在 session.update 里。
 *
 * 少了它服务端直接返回 `HTTP/1.1 400 Bad Request` —— 而 400 是
 * 最没有信息量的错误码之一：既可能是格式错、也可能是缺参数，
 * 看起来完全像"我的 WebSocket 握手写错了"。
 *
 * 这条已经在网关的 stepfun.py 注释表里记过，但写板端时漏了 ——
 * 说明"同一份协议知识存在两处"时一定会漏一处。
 */
#define STEPFUN_MODEL  "stepaudio-3-realtime-preview"
#define STEPFUN_PATH   "/v1/realtime?model=" STEPFUN_MODEL

/* 测试用音色。服务端**默认是 jingdiannvsheng**，不显式设就一直是它 */
#define TEST_VOICE     "linjiajiejie"

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int  resolve_ipv4(const char *host, char *out, size_t cap)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct sockaddr_in *sa;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        return -1;
    }
    sa = (struct sockaddr_in *)res->ai_addr;
    if (inet_ntop(AF_INET, &sa->sin_addr, out, (socklen_t)cap) == NULL) {
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return 0;
}

/* ---------------------------------------------------------------- 回调 */

static int  g_audio_samp = 0;          /* 收到的 16k 样本总数 */
static int  g_audio_cb = 0;            /* 回调次数 */
static char g_reply[2048];
static char g_user[1024];
static int  g_done = 0;
static int  g_started = 0;
static char g_err[512];

static void cb_audio(void *ud, const int16_t *pcm, int n)
{
    (void)ud; (void)pcm;
    g_audio_samp += n;
    g_audio_cb++;
}

static void cb_started(void *ud)            { (void)ud; g_started++; }
static void cb_done(void *ud)               { (void)ud; g_done++; }
static void cb_reply(void *ud, const char *t)
{
    (void)ud;
    snprintf(g_reply, sizeof(g_reply), "%s", t);
}
static void cb_user(void *ud, const char *t)
{
    (void)ud;
    snprintf(g_user, sizeof(g_user), "%s", t);
}
static void cb_err(void *ud, const char *t)
{
    (void)ud;
    snprintf(g_err, sizeof(g_err), "%s", t);
}

int main(int argc, char **argv)
{
    sc_tls_t tls;
    sc_ws_t  ws;
    sc_stepfun_t sf;
    sc_stepfun_cbs_t cbs;
    char     extra[512];
    char     ip[64];
    const char *key;
    int      rc;
    int      i;

    if (argc < 2) {
        printf("用法: %s <API_KEY>\n", argv[0]);
        return 2;
    }
    key = argv[1];

    if (resolve_ipv4(STEPFUN_HOST, ip, sizeof(ip)) != 0) {
        printf("域名解析失败：%s\n", STEPFUN_HOST);
        return 1;
    }

    printf("小满 · 直连 StepFun Realtime 自测\n");
    printf("================================\n");
    printf("目标 %s → %s:%d%s\n\n", STEPFUN_HOST, ip, STEPFUN_PORT, STEPFUN_PATH);

    /* ============================================================ A 协议栈 */

    printf("=== A1. 系统时间（证书有效期校验依赖它）===\n");
    if (!sc_tls_time_ready()) {
        printf("  时间未就绪，等待同步…\n");
        if (!sc_tls_wait_time(15000)) {
            printf("  [失败] 等待超时\n");
            return 1;
        }
    }
    printf("  [OK]   时间可用（%ld）\n", (long)sc_wall_time());

    printf("\n=== A2. TLS 初始化 ===\n");
    if (sc_tls_init(&tls) != 0) {
        printf("  [失败] sc_tls_init\n");
        return 1;
    }
    sc_tls_set_hostname(&tls, STEPFUN_HOST);
    sc_transport_set_timeout(&tls.base, 8000);
    printf("  [OK]   根证书已加载，SNI = %s\n", STEPFUN_HOST);

    printf("\n=== A3. TLS 握手 ===\n");
    rc = sc_transport_open(&tls.base, ip, STEPFUN_PORT);
    if (rc != 0) {
        printf("  [失败] 连接返回 %d\n", rc);
        return 1;
    }
    printf("  [OK]   TLS 通道建立，证书校验通过\n");

    printf("\n=== A4. WebSocket Upgrade ===\n");
    snprintf(extra, sizeof(extra), "Authorization: Bearer %s\r\n", key);
    rc = sc_ws_connect(&ws, &tls.base, STEPFUN_HOST, STEPFUN_PATH, extra);
    if (rc != 0) {
        printf("  [失败] 握手返回 %d\n", rc);
        return 1;
    }
    printf("  [OK]   %s\n", sc_ws_opened(&ws) ? "已建立" : "异常");

    /* ============================================================ B 会话 */

    printf("\n=== B1. 初始化会话模块 ===\n");
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_audio           = cb_audio;
    cbs.on_response_started = cb_started;
    cbs.on_response_done   = cb_done;
    cbs.on_reply_text      = cb_reply;
    cbs.on_user_text       = cb_user;
    cbs.on_error           = cb_err;

    if (sc_stepfun_init(&sf, &ws, &cbs) != 0) {
        printf("  [失败] sc_stepfun_init\n");
        return 1;
    }
    printf("  [OK]   重采样器就绪（16k⇄24k）\n");

    /*
     * 等 session.created，然后**立刻**发 session.update。
     *
     * 实测：created 之后如果迟迟不发配置，服务端会主动关闭连接 ——
     * 现象是"刚连上就断"，看起来像网络问题或鉴权问题，
     * 完全不会想到是"配置发晚了"。
     * 所以这里的顺序是硬的：一到 created 就下发配置。
     */
    printf("\n=== B2. 等 session.created 并立刻下发配置 ===\n");
    for (i = 0; i < 500 && !sf.got_created; i++) {
        if (sc_stepfun_poll(&sf, 4) < 0) {
            printf("  [失败] 还没等到 session.created 连接就断了"
                   "（服务端可能要求更早下发配置）\n");
            return 1;
        }
        sleep_ms(10);
    }
    if (!sf.got_created) {
        printf("  [失败] 5 秒内没有收到 session.created\n");
        return 1;
    }
    printf("  [OK]   收到 session.created\n");

    printf("\n=== B3. 立刻发 session.update（音色 + VAD 参数）===\n");
    rc = sc_stepfun_session_update(&sf,
                                   "你叫小满，是一个温柔有耐心的陪伴机器人，"
                                   "说话简短、语气亲切，面向 2-3 岁的孩子。",
                                   TEST_VOICE,
                                   400,     /* prefix_padding_ms */
                                   800,     /* silence_duration_ms  ← 不是 500 */
                                   700);    /* energy_threshold     ← 不是 2500 */
    if (rc != 0) {
        printf("  [失败] session.update 返回 %d\n", rc);
        return 1;
    }
    printf("  [OK]   已发送\n");

    /* 等 session.updated */
    for (i = 0; i < 300 && !sf.got_updated; i++) {
        if (sc_stepfun_poll(&sf, 4) < 0) {
            printf("  [失败] 等 session.updated 期间连接断了\n");
            return 1;
        }
        sleep_ms(10);
    }
    if (sf.got_updated) {
        printf("  [OK]   session.updated 已收到（配置生效）\n");
    } else {
        printf("  [注意] 没收到 session.updated，继续尝试\n");
    }

    printf("\n=== B4. 让它说一句话（测下行音频链路）===\n");
    g_done = 0;
    rc = sc_stepfun_say(&sf, "你好呀，我是小满，我们一起玩好不好？");
    if (rc != 0) {
        printf("  [失败] say 返回 %d\n", rc);
        return 1;
    }
    printf("  [OK]   已请求\n");

    /* 等 response.done（最多 25 秒） */
    for (i = 0; i < 2500 && g_done == 0; i++) {
        rc = sc_stepfun_poll(&sf, 4);
        if (rc < 0) {
            printf("  [失败] 连接断了\n");
            return 1;
        }
        sleep_ms(10);
    }

    if (g_done == 0) {
        printf("  [失败] 25 秒内没等到 response.done\n");
    } else {
        printf("  [OK]   response.done 已收到\n");
    }

    printf("\n=== B5. 下行音频链路统计 ===\n");
    printf("  回调次数      %d\n", g_audio_cb);
    printf("  16k 样本总数  %d（约 %.2f 秒）\n",
           g_audio_samp, (double)g_audio_samp / 16000.0);
    printf("  回复文本      %s\n", g_reply[0] ? g_reply : "(无)");
    if (g_user[0]) {
        printf("  ASR 文本      %s\n", g_user);
    }
    if (g_err[0]) {
        printf("  服务端报错    %s\n", g_err);
    }

    if (g_audio_samp > 16000) {
        printf("  [OK]   收到 >1 秒音频，解码/重采样链路正常\n");
    } else if (g_audio_samp > 0) {
        printf("  [注意] 只收到 %d 个样本，偏少\n", g_audio_samp);
    } else {
        printf("  [失败] 一个音频样本都没收到\n");
    }

    printf("\n================================\n");
    printf("TLS + WebSocket + 鉴权 + 会话 + 下行音频 全链路通过\n");

    sc_ws_close(&ws);
    sc_transport_close(&tls.base);
    sc_tls_deinit(&tls);
    return 0;
}
