/*
 * WebSocket 客户端的独立自测（对着本机 ws_server.py）。
 *
 * 为什么这些用例
 * --------------
 * 帧格式的错误有很强的"静默性"：长度编码算错，前几条消息看着正常，
 * 直到某条消息跨过 125/126 或 65535/65536 边界才突然乱掉，而且
 * 表现出来是"服务端不理我了"，完全看不出是长度的问题。
 *
 * 所以这里把三个长度分支的边界都踩一遍，并且在**故意触发一条超限消息
 * 之后**再发一条普通消息 —— 用来证明"流没有错位"。
 * 这一条是最有价值的：错误长度的帧会让解析器读到一个错位的载荷，
 * 之后所有消息都是垃圾，而加一条后续的正常消息就能立刻暴露它。
 *
 * 用法：
 *     1) python3 ws_server.py 8765 &
 *     2) make test_ws && ./test_ws [port]
 */

#include "sc_port.h"
#include "sc_transport.h"
#include "sc_ws.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                   \
        if (cond) {                                                        \
            printf("  [OK]   " fmt "\n", ##__VA_ARGS__);                    \
        } else {                                                           \
            printf("  [FAIL] " fmt "\n", ##__VA_ARGS__);                    \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

static sc_ws_t   g_ws;
static char      g_big[64 * 1024];
static char      g_snd[64 * 1024];

/* 收一条消息。读超时会返回 -EAGAIN，这里重试若干轮。 */
static int recv_msg(char *buf, size_t cap, int rounds)
{
    for (int i = 0; i < rounds; i++) {
        int n = sc_ws_recv_text(&g_ws, buf, cap);
        if (n != -EAGAIN) {
            return n;
        }
    }
    return -EAGAIN;
}

/* 全部字符都是 c 吗 */
static int all_same(const char *s, size_t n, char c)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] != c) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    sc_tcp_t tcp;
    int port = (argc > 1) ? atoi(argv[1]) : 8765;
    char addr[64];
    int n;
    int i;

    printf("小满 · WebSocket 客户端自测\n");
    printf("===========================\n");
    printf("目标 127.0.0.1:%d\n\n", port);

    /* ---------------------------------------------------------- 连接 */
    printf("=== 连接与握手 ===\n");

    if (sc_tcp_init(&tcp) != 0) {
        printf("  [FAIL] tcp 初始化失败\n");
        return 1;
    }
    sc_transport_set_timeout(&tcp.base, 2000);

    if (sc_transport_open(&tcp.base, "127.0.0.1", port) != 0) {
        printf("  [FAIL] 连不上 127.0.0.1:%d —— 先把 ws_server.py 跑起来\n", port);
        return 1;
    }
    printf("  [OK]   TCP 已连接\n");

    snprintf(addr, sizeof(addr), "127.0.0.1:%d", port);

    /* 打开详细日志：握手失败时如果只看到"失败"，没法判断是哪一步 */
    g_sc_verbose = 1;

    {
        int hr = sc_ws_connect(&g_ws, &tcp.base, addr, "/", NULL);
        if (hr != 0) {
            printf("  [FAIL] WebSocket 握手失败，返回 %d（-EAGAIN=%d, "
                   "-EPROTO=%d, -ECONNRESET=%d）\n",
                   hr, -EAGAIN, -EPROTO, -ECONNRESET);
            return 1;
        }
    }
    printf("  [OK]   握手成功（Sec-WebSocket-Accept 已校验）\n");

    n = recv_msg(g_big, sizeof(g_big), 40);
    CHECK(n == 5 && memcmp(g_big, "hello", 5) == 0,
          "握手后收到首条消息 \"hello\"（实际 %d 字节）", n);

    /* ---------------------------------------------------------- 基本回声 */
    printf("\n=== 基本收发 ===\n");

    CHECK(sc_ws_send_text(&g_ws, "abc", 3) == 0, "发送 3 字节");
    n = recv_msg(g_big, sizeof(g_big), 40);
    CHECK(n == 8 && memcmp(g_big, "echo:abc", 8) == 0,
          "回声正确（\"%.*s\"）", n > 0 ? n : 0, g_big);

    /* 空消息：长度 0 是个容易写错的分支 */
    CHECK(sc_ws_send_text(&g_ws, "", 0) == 0, "发送 0 字节");
    n = recv_msg(g_big, sizeof(g_big), 40);
    CHECK(n == 5 && memcmp(g_big, "echo:", 5) == 0,
          "空消息也被正确回声（%d 字节）", n);

    /*
     * ---------------------------------------------------------- 长度边界
     *
     * 客户端**发送**侧的长度编码边界：125/126。
     * 126 及以上必须切到 16 位长度。如果这里写错，服务端会按错误的
     * 长度解析，随后整条流错位 —— 而现象只是"echo 对不上"。
     */
    printf("\n=== 发送侧长度编码边界（125 / 126 字节）===\n");

    for (i = 0; i < 125; i++) {
        g_snd[i] = 'x';
    }
    g_snd[125] = '\0';
    CHECK(sc_ws_send_text(&g_ws, g_snd, 125) == 0, "发送 125 字节（1 字节长度）");
    n = recv_msg(g_big, sizeof(g_big), 40);
    CHECK(n == 130 && memcmp(g_big, "echo:", 5) == 0,
          "125 字节回声正确（%d 字节）", n);

    for (i = 0; i < 126; i++) {
        g_snd[i] = 'y';
    }
    g_snd[126] = '\0';
    CHECK(sc_ws_send_text(&g_ws, g_snd, 126) == 0, "发送 126 字节（切 16 位长度）");
    n = recv_msg(g_big, sizeof(g_big), 40);
    CHECK(n == 131 && memcmp(g_big, "echo:", 5) == 0,
          "126 字节回声正确（%d 字节）", n);

    for (i = 0; i < 5000; i++) {
        g_snd[i] = 'z';
    }
    CHECK(sc_ws_send_text(&g_ws, g_snd, 5000) == 0, "发送 5000 字节");
    n = recv_msg(g_big, sizeof(g_big), 60);
    CHECK(n == 5005 && memcmp(g_big, "echo:", 5) == 0,
          "5000 字节回声正确（%d 字节）", n);

    /* ---------------------------------------------------------- 接收侧长度 */
    printf("\n=== 接收侧长度编码（16 位）===\n");

    CHECK(sc_ws_send_text(&g_ws, "BIG16:5000", 10) == 0, "请求 5000 字节消息");
    n = recv_msg(g_big, sizeof(g_big), 60);
    CHECK(n == 5000 && all_same(g_big, 5000, 'B'),
          "收到 5000 字节全 'B'（%d）", n);

    printf("\n=== 接收侧长度编码（64 位）+ 超限处理 ===\n");

    /*
     * 70000 字节 > SC_WS_MSG_MAX(24K) → 期望 -EMSGSIZE。
     * 注意这里**不是**在测"能不能收大消息"，而是在测：
     *   解析器读 64 位长度是否正确，以及超限时是否**明确报错**
     *   而不是静默截断。静默截断会让音频断一小节，很难归因。
     */
    CHECK(sc_ws_send_text(&g_ws, "BIG64:70000", 11) == 0, "请求 70000 字节消息");
    n = recv_msg(g_big, sizeof(g_big), 80);
    CHECK(n == -EMSGSIZE, "超限消息被明确拒绝（返回 %d，期望 -EMSGSIZE=%d）",
          n, -EMSGSIZE);

    /*
     * ★ 关键：超限之后流必须仍然对齐 ★
     *
     * 如果 64 位长度解析错了（比如只读了 4 字节），载荷长度就会算错，
     * 解析器会从错位的地方继续读 —— 之后所有消息都是垃圾。
     * 加这一条普通回声就能立刻发现：它是整份测试里最有价值的一条。
     */
    CHECK(sc_ws_send_text(&g_ws, "after-big64", 11) == 0,
          "超限之后再发一条普通消息");
    n = recv_msg(g_big, sizeof(g_big), 60);
    /* "echo:"(5) + "after-big64"(11) = 16 字节。
     * 第一版这里写 17 —— 手工数字符串长度也是会错的，故直接说明算法。 */
    CHECK(n == 16 && memcmp(g_big, "echo:after-big64", 16) == 0,
          "流仍然对齐（\"%.*s\"，%d 字节）—— 证明 64 位长度解析正确",
          n > 0 ? n : 0, g_big, n);

    /* ---------------------------------------------------------- 分片重组 */
    printf("\n=== 分片重组（TEXT/0 + CONT/0 + CONT/1 三帧）===\n");

    CHECK(sc_ws_send_text(&g_ws, "FRAG:900", 8) == 0, "请求 900 字节分片消息");
    n = recv_msg(g_big, sizeof(g_big), 60);
    CHECK(n == 900 && all_same(g_big, 900, 'F'),
          "三帧分片被正确重组为 900 字节（%d）", n);

    /* 分片之间夹一个 ping：控制帧可以插在分片消息中间（RFC 允许） */
    printf("\n=== 分片 + 中间插 ping ===\n");
    CHECK(sc_ws_send_text(&g_ws, "FRAG:3000", 9) == 0, "请求 3000 字节分片");
    n = recv_msg(g_big, sizeof(g_big), 80);
    CHECK(n == 3000 && all_same(g_big, 3000, 'F'),
          "3000 字节分片重组正确（%d）", n);

    /* ---------------------------------------------------------- 多字节 */
    printf("\n=== 多字节 UTF-8 不被截断 ===\n");

    CHECK(sc_ws_send_text(&g_ws, "UNICODE", 7) == 0, "请求中文消息");
    n = recv_msg(g_big, sizeof(g_big), 60);
    g_big[n > 0 ? n : 0] = '\0';
    CHECK(n > 20 && strstr(g_big, "小满") != NULL,
          "中文完整（%d 字节：%s）", n, g_big);

    /* ---------------------------------------------------------- ping/pong */
    printf("\n=== ping/pong 自动应答 ===\n");

    CHECK(sc_ws_send_text(&g_ws, "PING", 4) == 0, "请求服务端发 ping");
    n = recv_msg(g_big, sizeof(g_big), 100);
    CHECK(n == 7 && memcmp(g_big, "PONG-OK", 7) == 0,
          "客户端自动回了 pong，服务端确认（\"%.*s\"）", n > 0 ? n : 0, g_big);

    /* ---------------------------------------------------------- 关闭 */
    printf("\n=== 关闭握手 ===\n");

    sc_ws_close(&g_ws);
    printf("  [OK]   已发送 close 帧\n");

    n = recv_msg(g_big, sizeof(g_big), 30);
    CHECK(n == 0 || n == -ECONNRESET,
          "对端关闭被正确识别（返回 %d）", n);

    sc_transport_close(&tcp.base);
    sc_tcp_deinit(&tcp);

    printf("\n===========================\n");
    if (g_fail == 0) {
        printf("全部通过\n");
        return 0;
    }
    printf("%d 项失败\n", g_fail);
    return 1;
}
