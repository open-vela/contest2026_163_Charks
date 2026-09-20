/*
 * TLS 传输（mbedtls）。设计取舍见 sc_tls.h。
 *
 * 关于 mbedtls 版本
 * ----------------
 * 树内是 **3.4.0**（宿主测试也用树内源码编出的同一份，保证 API 一致）。
 * 代码只使用 2.28 / 3.x 共有的稳定 API，不碰版本相关的枚举与内部字段 ——
 * 因为 2.x 与 3.x 在 `mbedtls_ssl_conf_*` 上差异不小，
 * 用共有子集能省掉一整类"换了版本编不过"的麻烦。
 *
 * 关于错误映射
 * -----------
 * mbedtls 用 `MBEDTLS_ERR_SSL_WANT_READ/WANT_WRITE` 表示"暂时不能收发"，
 * 传输层约定用 `-EAGAIN`。这两个必须严格对应 —— 如果把手握期间的
 * WANT_READ 当成致命错误返回，就会表现为"偶发连接失败"，
 * 而实际只是那一次读超时了。
 */

#include "sc_tls.h"
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

/* ==========================================================================
 * 内嵌根证书：DigiCert Global Root G2
 *
 * 指纹（SHA-256）：
 *   CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:
 *   47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F
 * 有效期：2013-08-01 ~ 2038-01-15
 *
 * 这张证书是从 api.stepfun.com 的实际链里提取的**自签名根**
 * （用 subject==issuer 判定），不是从网上随便下的 —— 提取脚本
 * 会核对指纹，不一致就报错退出。
 * ========================================================================== */

static const char SC_ROOT_CA_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
"d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
"MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
"MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
"b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
"9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
"2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
"1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
"q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
"tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
"vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
"BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
"5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
"1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
"NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
"Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
"8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
"pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
"MrY=\n"
"-----END CERTIFICATE-----\n";

/* ========================================================================== */

static void tls_set_timeout(sc_transport_t *t, int ms);

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    sc_tls_t *t = (sc_tls_t *)ctx;
    ssize_t n;

    if (t->fd < 0) {
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    }

    n = send(t->fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0) {
        return (int)n;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    sc_tls_t *t = (sc_tls_t *)ctx;
    ssize_t n;

    if (t->fd < 0) {
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    }

    n = recv(t->fd, buf, len, 0);
    if (n > 0) {
        return (int)n;
    }
    if (n == 0) {
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int tls_open(sc_transport_t *t, const char *host, int port)
{
    sc_tls_t *tt = (sc_tls_t *)t->priv;
    struct sockaddr_in sa;
    int fd;
    int rc;
    int tries;

    if (!tt->inited) {
        return -EINVAL;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        SC_ERR("tls: 只支持点分十进制 IP（收到 \"%s\"）。"
               "TLS 的 SNI 仍会用下面的主机名。\n", host);
        return -EINVAL;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -errno;
    }
    {
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int e = errno;
        close(fd);
        SC_ERR("tls: TCP 连接失败 errno=%d (%s)\n", e, strerror(e));
        return -e;
    }

    tt->fd = fd;
    if (tt->timeout_ms > 0) {
        tls_set_timeout(t, tt->timeout_ms);
    }

    /* ---- TLS 会话重置（允许重连）---- */
    mbedtls_ssl_session_reset(&tt->ssl);

    /*
     * ⚠️ `mbedtls_ssl_conf_*` 只在 setup 之前设置一次（在 sc_tls_init 里），
     * 这里只做 per-connection 的两件事：绑定 hostname 和绑定 bio。
     *
     * hostname 同时承担两个作用：SNI（服务端据此选证书）和
     * 证书 CN/SAN 匹配。**传 IP 会匹配失败**，所以这里用的是
     * 调用方给的"主机名"，而 TCP 连接用的是 IP —— 两者可以不同，
     * 这正是 `open()` 的 host 参数在 TLS 实现里的额外含义。
     */
    rc = mbedtls_ssl_set_hostname(&tt->ssl, tt->hostname);
    if (rc != 0) {
        SC_ERR("tls: 设置 hostname 失败 -0x%04X\n", (unsigned)-rc);
        return -EIO;
    }
    mbedtls_ssl_set_bio(&tt->ssl, tt, bio_send, bio_recv, NULL);

    /* ---- 握手。WANT_READ/WANT_WRITE 要重试，不能当致命错误 ---- */
    for (tries = 0; tries < 200; tries++) {
        rc = mbedtls_ssl_handshake(&tt->ssl);

        if (rc == 0) {
            break;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;                    /* 超时或缓冲满，再来一次 */
        }

        SC_ERR("tls: 握手失败 -0x%04X\n", (unsigned)-rc);
        if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            tt->verify_result = mbedtls_ssl_get_verify_result(&tt->ssl);
            SC_ERR("tls: 证书校验失败：%s\n", sc_tls_verify_text(tt));
        }
        return -EPROTO;
    }

    if (tries >= 200) {
        SC_ERR("tls: 握手超时（%d 次 WANT_READ 后放弃）\n", tries);
        return -ETIMEDOUT;
    }

    /* ---- 握手完成，核对证书 ---- */
    tt->verify_result = mbedtls_ssl_get_verify_result(&tt->ssl);
    if (tt->verify_result != 0) {
        SC_ERR("tls: 证书校验未通过：%s\n", sc_tls_verify_text(tt));
        return -EPROTO;
    }

    SC_LOG("tls: 握手成功（%s:%d，%s / %s）\n",
           tt->hostname, port,
           mbedtls_ssl_get_version(&tt->ssl),
           mbedtls_ssl_get_ciphersuite(&tt->ssl));

    return 0;
}

static int tls_read(sc_transport_t *t, void *buf, size_t len)
{
    sc_tls_t *tt = (sc_tls_t *)t->priv;
    int rc;

    if (tt->fd < 0) {
        return -ENOTCONN;
    }

    rc = mbedtls_ssl_read(&tt->ssl, (unsigned char *)buf, len);
    if (rc > 0) {
        return rc;
    }
    if (rc == 0) {
        return 0;                          /* 对端发来 close_notify */
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return -EAGAIN;                    /* 读超时：本轮没数据 */
    }
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    }

    SC_WARN("tls: read 失败 -0x%04X\n", (unsigned)-rc);
    return -EIO;
}

static int tls_write(sc_transport_t *t, const void *buf, size_t len)
{
    sc_tls_t *tt = (sc_tls_t *)t->priv;
    size_t sent = 0;

    if (tt->fd < 0) {
        return -ENOTCONN;
    }

    while (sent < len) {
        int rc = mbedtls_ssl_write(&tt->ssl,
                                   (const unsigned char *)buf + sent,
                                   len - sent);
        if (rc > 0) {
            sent += (size_t)rc;
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            struct timespec ts = { 0, 2 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        SC_WARN("tls: write 失败 -0x%04X\n", (unsigned)-rc);
        return -EIO;
    }

    return (int)sent;
}

static void tls_close(sc_transport_t *t)
{
    sc_tls_t *tt = (sc_tls_t *)t->priv;

    if (tt->fd >= 0) {
        (void)mbedtls_ssl_close_notify(&tt->ssl);
        close(tt->fd);
        tt->fd = -1;
    }
}

static bool tls_is_open(sc_transport_t *t)
{
    sc_tls_t *tt = (sc_tls_t *)t->priv;
    return tt->fd >= 0;
}

static void tls_set_timeout(sc_transport_t *t, int ms)
{
    sc_tls_t *tt = (sc_tls_t *)t->priv;
    struct timeval tv;

    tt->timeout_ms = ms;
    if (tt->fd < 0) {
        return;
    }
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    (void)setsockopt(tt->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static const sc_transport_vtbl_t g_tls_vtbl = {
    tls_open, tls_read, tls_write, tls_close, tls_is_open, tls_set_timeout
};

/* ========================================================================== */

int sc_tls_init(sc_tls_t *t)
{
    int rc;
    static const char *pers = "superchild";

    if (t == NULL) {
        return -EINVAL;
    }
    if (t->inited) {
        return 0;
    }

    memset(t, 0, sizeof(*t));
    t->fd = -1;
    t->base.vtbl = &g_tls_vtbl;
    t->base.priv = t;

    signal(SIGPIPE, SIG_IGN);              /* 同 sc_tcp：别让断连杀进程 */

    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_entropy_init(&t->entropy);

    /* ---- 熵源 ---- */
    rc = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy,
                               (const unsigned char *)pers, strlen(pers));
    if (rc != 0) {
        SC_ERR("tls: 熵源初始化失败 -0x%04X\n", (unsigned)-rc);
        return -EIO;
    }

    /* ---- 内嵌根证书。注意长度要 +1 把结尾的 '\0' 一起给进去 ---- */
    rc = mbedtls_x509_crt_parse(&t->ca,
                                (const unsigned char *)SC_ROOT_CA_PEM,
                                sizeof(SC_ROOT_CA_PEM));
    if (rc != 0) {
        /*
         * 负数 -0x%04X 里的低位是"第几张证书解析失败"，
         * 正数才是失败张数。这里只看正数。
         */
        SC_ERR("tls: 根证书解析失败 -0x%04X（检查 PEM 是否被截断）\n",
               (unsigned)-rc);
        return -EIO;
    }

    /*
     * 把内嵌根证书的 SHA-256 指纹打出来。
     *
     * **这是唯一能证明"装进去的是对的那张证书"的手段**：
     * "能连上"证明不了 —— 如果误装了别的根证书，握手会在校验阶段
     * 失败（看起来像服务端有问题）；而如果校验被不小心关掉，
     * 就悄无声息地失去了保护，什么现象都没有。
     *
     * 启动时打一次，和 sc_tls.h 里记录的指纹对一眼即可。
     */
    {
        unsigned char fp[32];

        /* 用手写的数据而不是 mbedtls 的打印工具：
         * mbedtls 的 X509 打印在 2.x/3.x 之间换过名字，
         * 而 sha256 接口是稳定的。 */
        (void)mbedtls_sha256(t->ca.raw.p, t->ca.raw.len, fp, 0);
        SC_LOG("tls: 根证书指纹 SHA-256 = "
               "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:"
               "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:"
               "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:"
               "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
               fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6], fp[7],
               fp[8], fp[9], fp[10], fp[11], fp[12], fp[13], fp[14], fp[15],
               fp[16], fp[17], fp[18], fp[19], fp[20], fp[21], fp[22], fp[23],
               fp[24], fp[25], fp[26], fp[27], fp[28], fp[29], fp[30], fp[31]);
        SC_LOG("tls: 期望 = CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:"
               "47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F\n");
    }

    /* ---- SSL 配置 ---- */
    rc = mbedtls_ssl_config_defaults(&t->conf,
                                     MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        SC_ERR("tls: ssl_config_defaults 失败 -0x%04X\n", (unsigned)-rc);
        return -EIO;
    }

    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);

    /*
     * **必须 REQUIRED**。写成 OPTIONAL 的话，证书再离谱也能连上 ——
     * 那样 TLS 就只剩"防窃听"、没有"防冒充"，而中间人正是
     * 这里最需要防的东西。
     */
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);

    rc = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (rc != 0) {
        SC_ERR("tls: ssl_setup 失败 -0x%04X\n", (unsigned)-rc);
        return -EIO;
    }

    t->inited = 1;
    return 0;
}

int sc_tls_set_hostname(sc_tls_t *t, const char *host)
{
    size_t n;

    if (t == NULL) {
        return -EINVAL;
    }
    if (host == NULL || host[0] == '\0') {
        /* 空主机名会导致 SNI 缺失 + CN 校验必然失败。
         * 与其让握手在几秒后失败，不如在这里立刻报错。 */
        return -EINVAL;
    }

    n = strlen(host);
    if (n >= sizeof(t->hostname)) {
        return -ENAMETOOLONG;
    }
    memcpy(t->hostname, host, n + 1);
    return 0;
}

void sc_tls_deinit(sc_tls_t *t)
{
    if (t == NULL) {
        return;
    }
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    sc_memzero(t, sizeof(*t));
}

const char *sc_tls_verify_text(const sc_tls_t *t)
{
    static char buf[256];

    if (t->verify_result == 0) {
        return "通过";
    }

    /* mbedtls_x509_crt_verify_info 会把结果逐条写成文本 */
    buf[0] = '\0';
    (void)mbedtls_x509_crt_verify_info(buf, sizeof(buf), "  ", t->verify_result);
    if (buf[0] == '\0') {
        snprintf(buf, sizeof(buf), "校验位图 0x%08X", (unsigned)t->verify_result);
    }
    return buf;
}

bool sc_tls_time_ready(void)
{
    int64_t now = sc_wall_time();

    /* 2024-01-01 = 1704067200。用年份而不是"非零"：
     * ntpcstart 是异步的，中间态会返回各种奇怪的值。 */
    return now >= 1704067200;
}

bool sc_tls_wait_time(int timeout_ms)
{
    uint32_t t0 = sc_now_ms();

    if (sc_tls_time_ready()) {
        return true;
    }

    SC_LOG("tls: 等待系统时间同步（ntpcstart）…\n");

    while ((int)(sc_now_ms() - t0) < timeout_ms) {
        struct timespec ts = { 0, 200 * 1000 * 1000 };   /* 200ms */

        if (sc_tls_time_ready()) {
            SC_LOG("tls: 时间已就绪（%ld）\n", (long)sc_wall_time());
            return true;
        }
        nanosleep(&ts, NULL);
    }

    SC_WARN("tls: 等待时间同步超时 —— 证书有效期校验会失败。"
            "检查 rcS 里的 ntpcstart 是否正常。\n");
    return false;
}
