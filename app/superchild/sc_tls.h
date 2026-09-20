/*
 * TLS 传输（mbedtls）。
 *
 * 这是方案 C（全部搬到板子上）的关键一环 —— 之前网关存在的三条理由里，
 * 有两条（TLS 证书链、系统时间）都在这里解决。
 *
 * 证书策略
 * --------
 * **只信任内嵌的 DigiCert Global Root G2**，不用系统的 CA bundle：
 *
 *   1. 树里根本没有 CA bundle（查过：全树找不到含 DigiCert 的 bundle 文件），
 *      所以"用现成的"这条路本来就不存在
 *   2. 只信一张根证书 = 攻击面只有这一张证书的私钥，
 *      比信任几百张根证书要小得多
 *   3. 服务端会送中间证书（实测链是 3 张：leaf → RapidSSL G1 → G2 根），
 *      所以 mbedtls 能自己拼出路径到我们内嵌的根
 *
 * 根证书的指纹（SHA-256）：
 *     CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:
 *     47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F
 * 有效期到 2038-01-15。**换证书必须重新核对指纹**，
 * 不要凭"能连上"就认为装对了。
 *
 * 系统时间
 * -------
 * 证书有效期校验要用真实时间。板子开机后由 rcS 里的 `ntpcstart` 同步，
 * 但**同步完成是异步的** —— 所以调用方必须在 open 之前等时间就绪
 * （见 sc_tls_wait_time()）。时间不对时的报错是
 * `X509 - Certificate validity failed`，极容易被误判成"证书过期了"，
 * 然后去查证书 —— 方向完全错了。
 */

#ifndef __SUPERCHILD_SC_TLS_H
#define __SUPERCHILD_SC_TLS_H

#include "sc_transport.h"

#include <stdbool.h>
#include <stdint.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

typedef struct {
    sc_transport_t base;               /* 必须是第一个成员 */

    int      fd;
    int      timeout_ms;
    int      inited;
    uint32_t verify_result;            /* mbedtls 校验结果，0 = 通过 */

    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         ca;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  entropy;

    char     hostname[128];            /* SNI + CN 校验用，必须常驻；
                                        * mbedtls 2.x 只存指针，3.x 会拷贝，
                                        * 存一份在结构体里对两个版本都安全 */
} sc_tls_t;

/*
 * 初始化（解析内嵌证书、建立 mbedtls 上下文）。幂等，可重复调用。
 * @return 0 成功
 */
int  sc_tls_init(sc_tls_t *t);

/* 释放。会先关闭连接。 */
void sc_tls_deinit(sc_tls_t *t);

/*
 * 设置 TLS 主机名（SNI + 证书 CN/SAN 校验）。
 *
 * **必须在 open 之前调用**，而且要用域名（如 "api.stepfun.com"）
 * 而不是 IP —— 用 IP 会同时导致 SNI 缺失和证书主体不匹配，
 * 报错是 `X509 - Certificate verification failed`，
 * 看起来像"证书不对"，其实是"传错参数"。
 *
 * 之所以把主机名和连接地址分开：板子上没有 DNS（或 DNS 不一定就绪），
 * 所以 TCP 这边连的是写死的 IP，而 TLS 这边必须用域名。
 * 这两件事**本来就不同**，接口上分开比塞在一个参数里更不容易错。
 *
 * @return 0 成功；-EINVAL 参数非法；-ENAMETOOLONG 超过 127 字节
 */
int  sc_tls_set_hostname(sc_tls_t *t, const char *host);

/* 上次认证失败的原因（人类可读），用于日志 */
const char *sc_tls_verify_text(const sc_tls_t *t);

/*
 * 系统时间是否已可用于证书校验。
 *
 * 判据是"年份 >= 2024"。为什么不判断"时间 != 0"：NuttX 的 ntpcstart
 * 是异步的，开机后一段时间里 time() 会返回 1970 或某个中间值；
 * 用年份判断更稳，也能顺带挡住"时间倒退"这类异常。
 */
bool sc_tls_time_ready(void);

/*
 * 阻塞等待时间就绪（带超时）。
 * @return true 就绪；false 超时
 */
bool sc_tls_wait_time(int timeout_ms);

#endif /* __SUPERCHILD_SC_TLS_H */
