/*
 * 裸 TCP 传输实现（sc_transport_t 的一个实现）。
 *
 * 用途有两个：
 *   1. **宿主机上单独测 WebSocket 层** —— 对着本机 Python 服务器跑，
 *      把"帧格式对不对"和"TLS 通不通"两个问题分开。
 *   2. 板端作为备用/降级路径（例如将来把网关放回家里）。
 *
 * 超时
 * ----
 * 用 SO_RCVTIMEO 实现读超时，超时时 recv 返回 EAGAIN → 映射成 -EAGAIN。
 * 调用方（sc_ws_recv_text）把它当作"本轮没消息"，**不丢解析状态**。
 *
 * SIGPIPE
 * -------
 * 对端关闭后 write 会触发 SIGPIPE，默认行为是**直接杀掉进程**。
 * 板子上表现为"跑着跑着突然重启"，而且没有日志 —— 极难查。
 * 所以 init 里统一忽略 SIGPIPE，由 send 的返回值来报错。
 */

#include "sc_transport.h"
#include "sc_port.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

/* 前置声明：tcp_open 里要用它（定义在下面） */
static void tcp_set_timeout(sc_transport_t *t, int ms);

static int tcp_open(sc_transport_t *t, const char *host, int port)
{
    sc_tcp_t *tt = (sc_tcp_t *)t->priv;
    struct sockaddr_in sa;
    int fd;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);

    /*
     * 先按点分十进制解析。超级本/板子上第一次连的是固定 IP，
     * 走这条路径**不需要 DNS**，也就不受"DNS 还没就绪"的影响
     * —— 开机后第一件事就是连服务器，这时 DNS 往往还没好。
     */
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        SC_ERR("tcp: 只支持点分十进制 IP（收到 \"%s\"）。"
               "域名请先在外部解析，或在板端实现 DNS。\n", host);
        return -EINVAL;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -errno;
    }

    /* 开了 Nagle 的话，小的 JSON 消息会被攒着不发 ——
     * 对话场景下表现为"偶发地慢半拍"，很难归因。关掉。 */
    {
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }

    tt->fd = fd;

    /*
     * 保留已设置的超时。
     *
     * 这里第一版写的是 `tt->timeout_ms = 0;`，它会**抹掉**
     * 调用方在 open 之前设好的超时 —— 而调用方很自然会写
     *     sc_transport_set_timeout(&t->base, 2000);
     *     sc_transport_open(&t->base, host, port);
     * 因为"先配参数再连接"是习惯写法。结果超时静默失效，
     * 之后 recv 一直阻塞，现象是"程序卡住不返回"。
     */
    if (tt->timeout_ms > 0) {
        tcp_set_timeout(t, tt->timeout_ms);
    }
    return 0;
}

static int tcp_read(sc_transport_t *t, void *buf, size_t len)
{
    sc_tcp_t *tt = (sc_tcp_t *)t->priv;
    ssize_t n;

    if (tt->fd < 0) {
        return -ENOTCONN;
    }

    n = recv(tt->fd, buf, len, 0);
    if (n > 0) {
        return (int)n;
    }
    if (n == 0) {
        SC_WARN("tcp: 对端关闭（recv 返回 0）\n");
        return 0;                          /* 对端正常关闭 */
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return -EAGAIN;                    /* 超时：本轮没数据 */
    }

    SC_WARN("tcp: recv 出错：errno=%d (%s)\n", errno, strerror(errno));
    /*
     * ⚠️ `-errno` 在 errno==0 时会变成 0，而 0 在传输层约定里表示
     * "对端正常关闭" —— 一个纯属意外的错误会被上层当成"连接断了"，
     * 然后静默走重连路径。这里的兜底是必要的，不是多此一举。
     */
    return errno != 0 ? -errno : -EIO;
}

static int tcp_write(sc_transport_t *t, const void *buf, size_t len)
{
    sc_tcp_t *tt = (sc_tcp_t *)t->priv;
    size_t sent = 0;

    if (tt->fd < 0) {
        return -ENOTCONN;
    }

    /* 大块要循环写：单次 send 不保证写完 */
    while (sent < len) {
        ssize_t n = send(tt->fd, (const char *)buf + sent, len - sent,
                         MSG_NOSIGNAL);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* 发送缓冲满：等一小会儿再试，不要当作错误返回 ——
             * 上层是大段 JSON，重发整段代价高得多 */
            struct timespec ts = { 0, 2 * 1000 * 1000 };   /* 2ms */
            nanosleep(&ts, NULL);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n == 0 ? -EIO : -errno;
    }

    return (int)sent;
}

static void tcp_close(sc_transport_t *t)
{
    sc_tcp_t *tt = (sc_tcp_t *)t->priv;

    if (tt->fd >= 0) {
        close(tt->fd);
        tt->fd = -1;
    }
}

static bool tcp_is_open(sc_transport_t *t)
{
    sc_tcp_t *tt = (sc_tcp_t *)t->priv;
    return tt->fd >= 0;
}

static void tcp_set_timeout(sc_transport_t *t, int ms)
{
    sc_tcp_t *tt = (sc_tcp_t *)t->priv;
    struct timeval tv;

    tt->timeout_ms = ms;
    if (tt->fd < 0) {
        return;
    }

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    (void)setsockopt(tt->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static const sc_transport_vtbl_t g_tcp_vtbl = {
    tcp_open, tcp_read, tcp_write, tcp_close, tcp_is_open, tcp_set_timeout
};

int sc_tcp_init(sc_tcp_t *t)
{
    if (t == NULL) {
        return -EINVAL;
    }

    memset(t, 0, sizeof(*t));
    t->fd = -1;
    t->base.vtbl = &g_tcp_vtbl;
    t->base.priv = t;

    /* 见文件头：不忽略 SIGPIPE 的话，对端一关，进程直接没 */
    signal(SIGPIPE, SIG_IGN);

    return 0;
}

void sc_tcp_deinit(sc_tcp_t *t)
{
    if (t != NULL) {
        tcp_close(&t->base);
    }
}
