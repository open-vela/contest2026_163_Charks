/*
 * 极简 WebSocket 客户端。设计取舍见 sc_ws.h。
 *
 * 解析器是**可重入的状态机**，不是"阻塞读到一整帧"。
 *
 * 为什么要可重入：读超时按块返回 -EAGAIN，若解析到一半（比如帧头只读了
 * 2 字节）就返回，下次调用必须能**从断点继续**，否则会把帧头当载荷读，
 * 此后整条流全乱 —— 而现象只是"后面收不到消息"，很难查。
 * 把状态存进 struct 是唯一不容易错的做法。
 *
 *
 * ⚠️ 一个必须写下来的坑：msg_len 的**清零时机**
 * ------------------------------------------------
 * 载荷是在「状态 2」里追加进 msg 的，所以新文本消息的 msg_len 必须在
 * **进入状态 2 之前**清零 —— 也就是在帧头解析完的那一刻。
 *
 * 我第一版把清零放在了"帧收完之后"，那样数据已经被追加进去了，
 * 再清零等于把刚收的消息扔掉。而且它**不一定表现为报错**：
 * 分片消息会看起来只有最后一片，短消息会看起来全丢。
 * 所以清零放在 ws_frame_begin() 里（帧头解析完、状态 2 之前）。
 */

#include "sc_ws.h"
#include "sc_b64.h"
#include "sc_port.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define WS_GUID     "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_OP_CONT  0x0
#define WS_OP_TEXT  0x1
#define WS_OP_BIN   0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING  0x9
#define WS_OP_PONG  0xA

/* ============================================================ SHA-1
 *
 * 只用于校验 Sec-WebSocket-Accept。自己写 60 行，好处是 sc_ws.c
 * **不依赖 mbedtls** —— 于是 WS 层能对着本机 Python 服务器用裸 TCP
 * 单独测，出问题时范围只在帧格式。
 */

typedef struct {
    uint32_t h[5];
    uint64_t bitlen;
    uint8_t  buf[64];
    int      n;
} sha1_t;

static uint32_t s1_rol(uint32_t v, int b)
{
    return (v << b) | (v >> (32 - b));
}

static void s1_init(sha1_t *c)
{
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->bitlen = 0;
    c->n = 0;
}

static void s1_block(sha1_t *c, const uint8_t *p)
{
    uint32_t w[80];
    uint32_t a, b, cc, d, e;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = s1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k, t;

        if (i < 20)      { f = (b & cc) | ((~b) & d);         k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ cc ^ d;                    k = 0xCA62C1D6u; }

        t = s1_rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = s1_rol(b, 30); b = a; a = t;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void s1_update(sha1_t *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    c->bitlen += (uint64_t)len * 8u;

    while (len > 0) {
        int space = 64 - c->n;
        int take = (len < (size_t)space) ? (int)len : space;

        memcpy(c->buf + c->n, p, (size_t)take);
        c->n += take;
        p += take;
        len -= (size_t)take;

        if (c->n == 64) {
            s1_block(c, c->buf);
            c->n = 0;
        }
    }
}

static void s1_final(sha1_t *c, uint8_t out[20])
{
    uint64_t bl = c->bitlen;
    uint8_t pad = 0x80;
    uint8_t z = 0x00;
    uint8_t lenb[8];

    s1_update(c, &pad, 1);
    while (c->n != 56) {
        s1_update(c, &z, 1);
    }
    for (int i = 0; i < 8; i++) {
        lenb[i] = (uint8_t)(bl >> (56 - i * 8));
    }
    s1_update(c, lenb, 8);

    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

/* ============================================================ 底层 IO */

/*
 * 保证 rx 里至少可读 1 字节。
 * @return >0 可用字节数；-EAGAIN 超时；0 对端关闭；<0 错误
 */
static int ws_fill(sc_ws_t *ws)
{
    int avail = ws->rx_len - ws->rx_off;
    int got;

    if (avail > 0) {
        return avail;
    }

    ws->rx_len = 0;
    ws->rx_off = 0;

    got = sc_transport_read(ws->t, ws->rx, SC_WS_RX_BUF);
    if (got > 0) {
        ws->rx_len = got;
    }
    return got;
}

static int ws_avail(const sc_ws_t *ws)
{
    return ws->rx_len - ws->rx_off;
}

/* ============================================================ 帧状态机 */

static int ws_send_frame(sc_ws_t *ws, int opcode, const uint8_t *payload,
                         size_t len);

/* 准备解析下一帧 */
static void ws_frame_begin(sc_ws_t *ws)
{
    ws->st = 0;
    ws->hdr_got = 0;
    ws->hdr_need = 2;
    ws->paylen = 0;
    ws->pay_got = 0;
}

/*
 * 推进状态机（不阻塞）。
 * @return 1 完成一条文本消息（在 ws->msg，长度 ws->msg_len）
 *         0 需要更多数据
 *         <0 错误
 */
static int ws_step(sc_ws_t *ws)
{
    /* ---- 帧头（状态 0 → 1）---- */
    while (ws->st <= 1) {
        int avail = ws_avail(ws);
        int take;

        if (avail <= 0) {
            return 0;
        }

        take = ws->hdr_need - ws->hdr_got;
        if (take > avail) {
            take = avail;
        }
        memcpy(ws->hdr + ws->hdr_got, ws->rx + ws->rx_off, (size_t)take);
        ws->hdr_got += take;
        ws->rx_off += take;

        if (ws->st == 0 && ws->hdr_got >= 2) {
            int len7;

            ws->fin    = (ws->hdr[0] & 0x80) ? 1 : 0;
            ws->opcode = ws->hdr[0] & 0x0F;
            ws->masked = (ws->hdr[1] & 0x80) ? 1 : 0;
            len7       = ws->hdr[1] & 0x7F;

            ws->hdr_need = 2;
            if (len7 == 126) {
                ws->hdr_need += 2;
            } else if (len7 == 127) {
                ws->hdr_need += 8;
            }
            if (ws->masked) {
                ws->hdr_need += 4;
            }
            ws->st = 1;
        }

        if (ws->st != 1 || ws->hdr_got < ws->hdr_need) {
            continue;
        }

        /* ---- 扩展长度 + 掩码 ---- */
        {
            int len7 = ws->hdr[1] & 0x7F;
            int off = 2;

            if (len7 == 126) {
                ws->paylen = ((int64_t)ws->hdr[off] << 8) | ws->hdr[off + 1];
                off += 2;
            } else if (len7 == 127) {
                ws->paylen = 0;
                for (int i = 0; i < 8; i++) {
                    ws->paylen = (ws->paylen << 8) | ws->hdr[off + i];
                }
                off += 8;
            } else {
                ws->paylen = len7;
            }

            if (ws->masked) {
                memcpy(ws->mask, ws->hdr + off, 4);
            }
        }

        /*
         * 控制帧不能分片，载荷必须 ≤125（RFC 6455 §5.5）。
         * 违反说明对端实现有问题 —— 报错而不是硬着头皮解，
         * 否则会读出一个长度荒谬的载荷把缓冲撑爆。
         */
        if (ws->opcode == WS_OP_PING || ws->opcode == WS_OP_PONG ||
            ws->opcode == WS_OP_CLOSE) {
            if (ws->paylen > 125 || !ws->fin) {
                SC_ERR("ws: 控制帧非法（op=%d len=%d fin=%d）\n",
                       ws->opcode, (int)ws->paylen, ws->fin);
                return -EPROTO;
            }
        }

        /*
         * ★ 清零时机就在这里 ★
         *
         * 新文本消息必须**在追加载荷之前**把 msg_len 归零。
         * 放在"帧收完之后"清是错的（数据已被追加，清掉等于丢消息），
         * 而且不报错、只是消息变短或全丢 —— 极难归因。
         * 分片续帧（CONT）**不能**清零，否则前面收的分片全丢。
         */
        if (ws->opcode == WS_OP_TEXT) {
            ws->msg_len = 0;
            ws->msg_overflow = 0;
        }
        if (ws->opcode == WS_OP_PING) {
            ws->ctrl_len = 0;
        }

        ws->pay_got = 0;
        ws->st = 2;
    }

    /* ---- 载荷（状态 2）---- */
    while (ws->pay_got < ws->paylen) {
        int avail = ws_avail(ws);
        int64_t remain = ws->paylen - ws->pay_got;
        int take;

        if (avail <= 0) {
            return 0;
        }

        take = (int)(remain < (int64_t)avail ? remain : (int64_t)avail);

        /* 服务端→客户端按 RFC 不带掩码；带了也照样解，不报错 */
        if (ws->masked) {
            for (int i = 0; i < take; i++) {
                ws->rx[ws->rx_off + i] ^= ws->mask[(ws->pay_got + i) & 3];
            }
        }

        if (ws->opcode == WS_OP_TEXT || ws->opcode == WS_OP_CONT) {
            if (ws->msg_len + take > SC_WS_MSG_MAX) {
                if (!ws->msg_overflow) {
                    SC_ERR("ws: 单条消息超过 %d 字节，本条丢弃\n", SC_WS_MSG_MAX);
                    ws->msg_overflow = 1;
                }
            } else {
                memcpy(ws->msg + ws->msg_len, ws->rx + ws->rx_off, (size_t)take);
                ws->msg_len += take;
            }
        } else if (ws->opcode == WS_OP_PING) {
            memcpy(ws->ctrl + ws->ctrl_len, ws->rx + ws->rx_off, (size_t)take);
            ws->ctrl_len += take;
        }
        /* binary / pong / close 的载荷直接丢 */

        ws->rx_off += take;
        ws->pay_got += take;
    }

    /* ---- 本帧收完 ---- */
    {
        int op = ws->opcode;
        int fin = ws->fin;
        int64_t plen = ws->paylen;

        ws_frame_begin(ws);

        switch (op) {
        case WS_OP_CONT:
            if (fin) {
                if (ws->msg_overflow) {
                    ws->msg_overflow = 0;
                    ws->msg_len = 0;
                    return -EMSGSIZE;
                }
                return 1;
            }
            return 0;

        case WS_OP_TEXT:
            if (fin) {
                if (ws->msg_overflow) {
                    ws->msg_overflow = 0;
                    ws->msg_len = 0;
                    return -EMSGSIZE;
                }
                return 1;
            }
            return 0;

        case WS_OP_CLOSE:
            SC_LOG("ws: 收到 close 帧\n");
            (void)ws_send_frame(ws, WS_OP_CLOSE, NULL, 0);
            ws->closed = true;
            return -ECONNRESET;

        case WS_OP_PING:
            /* 必须回 pong，否则服务端可能主动断开 */
            (void)ws_send_frame(ws, WS_OP_PONG, ws->ctrl, (size_t)ws->ctrl_len);
            ws->ctrl_len = 0;
            return 0;

        case WS_OP_PONG:
            return 0;

        case WS_OP_BIN:
            /* Realtime 不用二进制帧。收全丢弃、不报错 ——
             * 报错会让一条无关的帧打断整条会话。 */
            SC_WARN("ws: 收到未预期的二进制帧（%d 字节），已丢弃\n", (int)plen);
            return 0;

        default:
            SC_ERR("ws: 未知 opcode 0x%X\n", op);
            return -EPROTO;
        }
    }
}

/* ============================================================ 发送帧 */

static int ws_send_frame(sc_ws_t *ws, int opcode, const uint8_t *payload,
                         size_t len)
{
    uint8_t hdr[14];
    uint8_t mask[4];
    int hlen = 0;

    hdr[0] = (uint8_t)(0x80 | (opcode & 0x0F));      /* FIN=1, 不分片发送 */

    if (len < 126) {
        hdr[1] = (uint8_t)(0x80 | len);
        hlen = 2;
    } else if (len <= 0xFFFF) {
        hdr[1] = (uint8_t)(0x80 | 126);
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    } else {
        hdr[1] = (uint8_t)(0x80 | 127);
        for (int i = 0; i < 8; i++) {
            hdr[2 + i] = (uint8_t)(((uint64_t)len >> (56 - i * 8)) & 0xFF);
        }
        hlen = 10;
    }

    /* 客户端→服务端**必须**掩码（RFC 6455 §5.3）。
     * 不掩码有的服务端直接断连，而且原因很难看出来。 */
    (void)sc_random(mask, sizeof(mask));
    memcpy(hdr + hlen, mask, 4);
    hlen += 4;

    if (sc_transport_write(ws->t, hdr, (size_t)hlen) != hlen) {
        return -EIO;
    }

    if (len > 0 && payload != NULL) {
        /*
         * 分块掩码后发送，避免为大消息再开一份同样大的缓冲。
         * 掩码下标必须按**整条消息的偏移**算，不能每块从 0 开始。
         */
        uint8_t tmp[1024];
        size_t off = 0;

        while (off < len) {
            size_t chunk = len - off;

            if (chunk > sizeof(tmp)) {
                chunk = sizeof(tmp);
            }
            for (size_t i = 0; i < chunk; i++) {
                tmp[i] = payload[off + i] ^ mask[(off + i) & 3];
            }
            if (sc_transport_write(ws->t, tmp, chunk) != (int)chunk) {
                return -EIO;
            }
            off += chunk;
        }
    }

    return 0;
}

/* ============================================================ 握手 */

/* 在 buf 里找 "\r\n\r\n"，返回其结束位置（即 payload 起点），找不到返回 -1 */
static int find_hdr_end(const char *buf, int len)
{
    for (int i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i + 4;
        }
    }
    return -1;
}

/*
 * 大小写不敏感的定长比较。
 *
 * 不用 libc 的 strncasecmp：NuttX 的那份受配置项影响，不是所有
 * 配置都编进去；HTTP 头名是大小写不敏感的（RFC 7230），
 * 靠 strncmp 严格比会在某些服务端上莫名失败。
 */
static int ci_prefix_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

/*
 * 从响应头里取某个 header 的值（名字大小写不敏感），写入 out。
 * @return 0 找到；-1 没找到
 */
static int get_header(const char *hdr, const char *name, char *out, size_t cap)
{
    size_t nlen = strlen(name);
    const char *p = hdr;

    while (*p != '\0') {
        const char *eol = strstr(p, "\r\n");
        int linelen = eol != NULL ? (int)(eol - p) : (int)strlen(p);

        if (linelen > (int)nlen && p[nlen] == ':' &&
            ci_prefix_eq(p, name, nlen)) {
            const char *v = p + nlen;
            size_t vl;

            while (*v == ' ' || *v == ':') {
                v++;
            }
            vl = (size_t)(linelen - (int)(v - p));
            if (vl >= cap) {
                vl = cap - 1;
            }
            memcpy(out, v, vl);
            out[vl] = '\0';
            return 0;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 2;
    }
    return -1;
}

int sc_ws_connect(sc_ws_t *ws, sc_transport_t *t, const char *host,
                  const char *path, const char *extra)
{
    char req[1024];
    /*
     * ⚠️ 这几个缓冲的大小不能手算着"差不多够"。
     * SC_B64_ENCLEN(n) = ceil(n/3)*4 + 1：
     *     16 字节 → 25 字节（Key）
     *     20 字节 → 29 字节（Accept）
     * 第一版把 key 写成 char[24]，就差 1 个字节，sc_b64_encode 直接返回 -1，
     * 整个 connect 返回 -EIO —— 而 -EIO 看起来像"网络写失败"，
     * 完全不会往"缓冲小了一格"上想。所以这里留足余量。
     */
    char key[32];
    char expected[40];
    char got[64];
    char rbuf[2048];
    uint8_t raw[16];
    uint8_t digest[20];
    int reqlen;
    int total = 0;
    int hdr_end = -1;
    int n;
    sha1_t sha;

    if (ws == NULL || t == NULL || host == NULL) {
        return -EINVAL;
    }

    memset(ws, 0, sizeof(*ws));
    ws->t = t;

    /* ---- Sec-WebSocket-Key：16 字节随机 + base64 ---- */
    if (sc_random(raw, sizeof(raw)) != 0) {
        return -EIO;
    }
    if (sc_b64_encode(raw, sizeof(raw), key, sizeof(key)) < 0) {
        return -EIO;
    }

    /* ---- 期望的 Accept = base64(SHA1(key + GUID)) ---- */
    s1_init(&sha);
    s1_update(&sha, key, strlen(key));
    s1_update(&sha, WS_GUID, strlen(WS_GUID));
    s1_final(&sha, digest);
    if (sc_b64_encode(digest, sizeof(digest), expected, sizeof(expected)) < 0) {
        return -EIO;
    }

    /* ---- 请求 ---- */
    reqlen = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: %s\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "%s"
                      "\r\n",
                      path, host, key, extra != NULL ? extra : "");
    if (reqlen <= 0 || reqlen >= (int)sizeof(req)) {
        return -EINVAL;
    }

    SC_DBG("ws: 发送握手请求\n");
    if (sc_transport_write(t, req, (size_t)reqlen) != reqlen) {
        return -EIO;
    }

    /* ---- 读响应头。可能要分几次才到 ---- */
    {
        int rounds = 0;

        while (total < (int)sizeof(rbuf) - 1) {
            n = sc_transport_read(t, rbuf + total,
                                  sizeof(rbuf) - 1 - (size_t)total);

            if (n == -EAGAIN) {
                /*
                 * ⚠️ 握手**不能**返回 -EAGAIN 让调用方重试。
                 *
                 * 请求已经发出去了；调用方重试会把同一份 Upgrade 请求
                 * 再发一遍，服务端会看到两条 HTTP 请求头，
                 * 行为未定义 —— 而且现象是"偶尔握手失败"，
                 * 很难往"重试把请求发了两次"上想。
                 *
                 * 所以在内部重试到超时为止，返回 -ETIMEDOUT
                 * 才是明确的语义。
                 */
                if (++rounds > 30) {
                    SC_ERR("ws: 握手响应超时（%d 次读超时后放弃）\n", rounds);
                    return -ETIMEDOUT;
                }
                continue;
            }
            if (n <= 0) {
                SC_ERR("ws: 握手期间连接断开（已读 %d 字节）\n", total);
                return n == 0 ? -ECONNRESET : n;
            }
            total += n;
            rbuf[total] = '\0';
            hdr_end = find_hdr_end(rbuf, total);
            if (hdr_end >= 0) {
                break;
            }
        }
    }

    if (hdr_end < 0) {
        SC_ERR("ws: 握手响应头超过 %d 字节仍未结束\n", total);
        return -EPROTO;
    }

    /* ---- 状态行必须是 101 ---- */
    if (strncmp(rbuf, "HTTP/1.1 101", 12) != 0 &&
        strncmp(rbuf, "HTTP/1.0 101", 12) != 0) {
        /*
         * 把状态行原样打出来。StepFun 鉴权失败时返回的是 401/403，
         * 而且响应体里通常有原因 —— 只看"连不上"是查不出来的。
         */
        char line[160];
        int i = 0;
        while (i < (int)sizeof(line) - 1 && rbuf[i] != '\r' && rbuf[i] != '\0') {
            line[i] = rbuf[i];
            i++;
        }
        line[i] = '\0';
        SC_ERR("ws: 握手被拒绝：%s\n", line);
        return -EPROTO;
    }

    /* ---- 校验 Sec-WebSocket-Accept ---- */
    rbuf[hdr_end] = '\0';              /* 截到 header 区（含结尾空行）*/
    if (get_header(rbuf, "Sec-WebSocket-Accept", got, sizeof(got)) != 0) {
        SC_ERR("ws: 响应里没有 Sec-WebSocket-Accept\n");
        return -EPROTO;
    }
    if (strcmp(got, expected) != 0) {
        SC_ERR("ws: Sec-WebSocket-Accept 不匹配（期望 %s，收到 %s）\n",
               expected, got);
        return -EPROTO;
    }

    /* ---- 响应头之后可能已经跟着帧数据，搬进 rx ---- */
    {
        int leftover = total - hdr_end;
        if (leftover > 0) {
            if (leftover > SC_WS_RX_BUF) {
                leftover = SC_WS_RX_BUF;
            }
            memcpy(ws->rx, rbuf + hdr_end, (size_t)leftover);
            ws->rx_len = leftover;
            ws->rx_off = 0;
        }
    }

    ws_frame_begin(ws);
    ws->opened = true;
    ws->closed = false;
    ws->msg_len = 0;

    SC_LOG("ws: 握手成功（%s%s）\n", host, path);
    return 0;
}

int sc_ws_send_text(sc_ws_t *ws, const char *text, size_t len)
{
    if (ws == NULL || !ws->opened || ws->closed) {
        return -ENOTCONN;
    }
    return ws_send_frame(ws, WS_OP_TEXT, (const uint8_t *)text, len);
}

int sc_ws_recv_text(sc_ws_t *ws, char *out, size_t cap)
{
    if (ws == NULL || out == NULL || !ws->opened) {
        return -ENOTCONN;
    }
    if (ws->closed) {
        return 0;
    }

    for (;;) {
        int r;
        int avail = ws_avail(ws);

        if (avail <= 0) {
            int got = ws_fill(ws);

            if (got == -EAGAIN) {
                return -EAGAIN;        /* 本轮没消息，调用方稍后再来 */
            }
            if (got == 0) {
                SC_LOG("ws: 对端关闭连接\n");
                ws->closed = true;
                return 0;
            }
            if (got < 0) {
                return got;
            }
        }

        r = ws_step(ws);
        if (r < 0) {
            return r;
        }
        if (r == 1) {
            int mlen = ws->msg_len;

            if ((size_t)mlen + 1 > cap) {
                ws->msg_len = 0;
                return -EMSGSIZE;
            }
            memcpy(out, ws->msg, (size_t)mlen);
            out[mlen] = '\0';
            ws->msg_len = 0;
            return mlen;
        }
        /* r == 0：还需要更多数据，继续循环 */
    }
}

void sc_ws_close(sc_ws_t *ws)
{
    if (ws == NULL || !ws->opened) {
        return;
    }
    if (!ws->closed) {
        (void)ws_send_frame(ws, WS_OP_CLOSE, NULL, 0);
        ws->closed = true;
    }
}

bool sc_ws_opened(const sc_ws_t *ws)
{
    return ws != NULL && ws->opened && !ws->closed;
}
