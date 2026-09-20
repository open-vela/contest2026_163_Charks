/*
 * 极简 WebSocket 客户端（RFC 6455）。
 *
 * 为什么要自己写
 * -------------
 * 树里两个选择都不能用：
 *   - `apps/netutils/cwebsocket`：目录里**只有 patch，没有源码**
 *     （Makefile 引用 `cwebsocket/src/cwebsocket/client.c`，该路径不存在）
 *   - `apps/netutils/libwebsockets`：31MB 的移植包，为一个只用文本帧的
 *     场景引入太多不确定
 *
 * Realtime 的用法极窄：**只有带掩码的文本帧**（音频是 base64 塞在 JSON 里，
 * 不走二进制帧）。自己写 300 行，行为完全可控，而且不依赖 mbedtls ——
 * 这让 WS 层能对着本机 Python 服务器用裸 TCP 单独测，
 * 出错时范围只在帧格式，不会和 TLS 搅在一起。
 *
 * 实现范围（刻意留白，不假装支持）
 * ------------------------------
 *   ✓ 文本帧（opcode 1）+ 分片（continuation 0）
 *   ✓ 客户端掩码
 *   ✓ ping → 自动回 pong；pong → 忽略
 *   ✓ close 握手
 *   ✗ 二进制帧（用不到；收到就丢，不报错，免得一条无关帧打断整条会话）
 *   ✗ 扩展协商 —— 不发 Sec-WebSocket-Extensions，服务端就不会启用
 *
 * 握手会校验 `Sec-WebSocket-Accept`（SHA1(key+GUID)）。多写 10 行，
 * 但能在**中间有代理/拦截**时立刻发现 —— 否则表现为"连上了但收不到
 * 任何消息"，那是最难查的一种现象。
 */

#ifndef __SUPERCHILD_SC_WS_H
#define __SUPERCHILD_SC_WS_H

#include "sc_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 单条消息上限。
 *
 * Realtime 的音频增量是 base64 塞在 JSON 里的：服务端若一次给 16KB PCM，
 * base64 后是 21.9KB，加 JSON 外壳约 22.3KB。给 24KB 在"省内存"和
 * "不至于截断"之间取平衡。
 *
 * 超限**不静默截断**：静默截断会让音频断一小节，听感上像卡顿，
 * 很难归因。这里明确报 -EMSGSIZE 并丢弃整条。
 */
#define SC_WS_MSG_MAX   (24 * 1024)

/* 原始接收缓冲。比单条消息小得多也能工作：帧头一次、载荷分次。 */
#define SC_WS_RX_BUF    4096

typedef struct {
    sc_transport_t *t;

    uint8_t rx[SC_WS_RX_BUF];
    int     rx_len;                    /* rx 中有效字节数 */
    int     rx_off;                    /* 已消费偏移 */

    char    msg[SC_WS_MSG_MAX];        /* 分片重组缓冲 */
    int     msg_len;

    /* ---- 帧解析状态。**必须跨调用保持** ---- */

    int     st;                        /* 0 头2字节 / 1 扩展+掩码 / 2 载荷 */
    uint8_t hdr[14];
    int     hdr_need;
    int     hdr_got;
    int     fin;
    int     opcode;
    int     masked;
    uint8_t mask[4];
    int64_t paylen;
    int64_t pay_got;
    int     msg_overflow;              /* 本条消息超限 */

    uint8_t ctrl[125];                 /* ping 载荷（要回显，最大 125） */
    int     ctrl_len;

    bool    opened;
    bool    closed;
} sc_ws_t;

/*
 * 建立 WebSocket 连接（含 HTTP Upgrade 握手）。
 *
 * @param t      已连接好的传输层（TLS 或裸 TCP）
 * @param host   Host 头用的主机名
 * @param path   请求路径，例如 "/v1/realtime"
 * @param extra  额外请求头，可 NULL；**必须以 "\r\n" 结尾**，
 *               形如 "Authorization: Bearer xxx\r\n"
 * @return 0 成功；<0 错误（-EPROTO 表示握手响应非法）
 */
int  sc_ws_connect(sc_ws_t *ws, sc_transport_t *t, const char *host,
                   const char *path, const char *extra);

/*
 * 发送一条文本消息。
 * @return 0 成功；<0 错误
 */
int  sc_ws_send_text(sc_ws_t *ws, const char *text, size_t len);

/*
 * 取下一条完整的文本消息（内部自动处理 ping/pong 与分片）。
 *
 * 可重入：中途返回 -EAGAIN 后可以继续调用，不会破坏解析状态。
 *
 * @return >0 消息长度（out 已补 '\0'）；-EAGAIN 暂无消息；0 对端关闭；<0 错误
 */
int  sc_ws_recv_text(sc_ws_t *ws, char *out, size_t cap);

/* 发起关闭握手。不等待对端回应。 */
void sc_ws_close(sc_ws_t *ws);

/* 是否已建立过连接（不代表健康） */
bool sc_ws_opened(const sc_ws_t *ws);

#endif /* __SUPERCHILD_SC_WS_H */
