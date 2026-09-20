/*
 * 传输层抽象。
 *
 * 目的：让 WebSocket 层**不知道**自己跑在裸 TCP 上还是 TLS 上。
 *
 * 这个抽象不是为了"架构好看"，而是为了**能分开调试**：
 *
 *   1. 先用裸 TCP 对着本机一个 Python WS 服务器测帧解析 ——
 *      握手、掩码、分片、ping/pong 如果写错，在这里就能抓出来，
 *      而且错误信息明确（就是帧格式不对）。
 *   2. 再接上 TLS 对着真实 StepFun 测 —— 这时只剩"证书/SNI/时间"
 *      这一类问题。
 *
 * 不分层的做法是：一个函数里同时有 socket、TLS、HTTP 握手、帧解析。
 * 出错时只能看到"连不上"，得靠猜是哪一层。这个项目在
 * 「板子能不能直接连 StepFun」这件事上已经因为"多个问题搅在一起"
 * 绕过一次弯路，所以这一层是刻意加的。
 *
 * 约定
 * ----
 *   read()   >0 实际字节数；0 对端正常关闭；-EAGAIN 暂时无数据（超时）；<0 其它错误
 *   write()  >0 实际写入；<0 错误
 *
 * 超时语义由各实现负责（TCP 用 SO_RCVTIMEO；TLS 把 EAGAIN 映射成
 * MBEDTLS_ERR_SSL_WANT_READ 再还原为 -EAGAIN）。
 */

#ifndef __SUPERCHILD_SC_TRANSPORT_H
#define __SUPERCHILD_SC_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct sc_transport sc_transport_t;

typedef struct {
    /*
     * 建立连接。host 对 TLS 实现有额外含义（SNI + 证书 CN 校验）；
     * 对裸 TCP 就是目标地址。
     * @return 0 成功，<0 负数 errno
     */
    int  (*open)(sc_transport_t *t, const char *host, int port);

    int  (*read)(sc_transport_t *t, void *buf, size_t len);
    int  (*write)(sc_transport_t *t, const void *buf, size_t len);
    void (*close)(sc_transport_t *t);

    /* 底层 socket 是否还开着（只看是否已关闭，不判断健康度） */
    bool (*is_open)(sc_transport_t *t);

    /* 设置读超时（毫秒）。0 = 阻塞。不支持的实现可忽略。 */
    void (*set_timeout)(sc_transport_t *t, int ms);
} sc_transport_vtbl_t;

struct sc_transport {
    const sc_transport_vtbl_t *vtbl;
    void                      *priv;
};

/* 简写，避免调用点写成 ws->t->vtbl->read(ws->t, ...) 这种噪音 */
static inline int sc_transport_open(sc_transport_t *t, const char *host, int port)
{
    return t->vtbl->open(t, host, port);
}

static inline int sc_transport_read(sc_transport_t *t, void *buf, size_t len)
{
    return t->vtbl->read(t, buf, len);
}

static inline int sc_transport_write(sc_transport_t *t, const void *buf, size_t len)
{
    return t->vtbl->write(t, buf, len);
}

static inline void sc_transport_close(sc_transport_t *t)
{
    t->vtbl->close(t);
}

static inline bool sc_transport_is_open(sc_transport_t *t)
{
    return t->vtbl->is_open(t);
}

static inline void sc_transport_set_timeout(sc_transport_t *t, int ms)
{
    t->vtbl->set_timeout(t, ms);
}

/* ---------- 裸 TCP 实现（宿主机测试用；板端也可用）---------- */

typedef struct {
    sc_transport_t base;
    int            fd;
    int            timeout_ms;
} sc_tcp_t;

int  sc_tcp_init(sc_tcp_t *t);
void sc_tcp_deinit(sc_tcp_t *t);

#endif /* __SUPERCHILD_SC_TRANSPORT_H */
