/*
 * 时间同步。设计与取舍见 sc_time.h。
 *
 * 两条路 + 一层兜底：
 *   A. SNTP over UDP/123     —— 精确、快，但可能被网络封
 *   B. HTTP Date over TCP/80 —— 主路。任何 HTTP 响应都带 Date，哪怕 302/404
 *   C. /data 里存上次的值    —— 开机先读回来，保证"所有时间源都不可用"时也能工作
 */

#include "sc_time.h"
#include "sc_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>      /* struct timeval（SO_RCVTIMEO 用） */

/* ---------------------------------------------------------------- 配置 */

/*
 * HTTP 取时间的目标。
 *
 * **刻意用连接服务器自己的 80 端口**：我们已知这台主机是可达的
 * （TLS 443 握手成功过），所以端口 80 大概率也通。
 * 而且**不在乎响应内容是什么** —— 301/404/门户劫持页都带 Date 头。
 */
#ifndef SC_HTTP_TIME_IP
#  define SC_HTTP_TIME_IP   "14.103.2.83"
#endif
#ifndef SC_HTTP_TIME_HOST
#  define SC_HTTP_TIME_HOST "api.stepfun.com"
#endif
#ifndef SC_HTTP_TIME_PORT
#  define SC_HTTP_TIME_PORT 80
#endif

/* SNTP 服务器（用 IP 避免依赖 DNS）。换成家里网络时这两个也能用。 */
static const char *const g_ntp[] = {
    "203.107.6.88",     /* ntp.aliyun.com */
    "120.25.115.20",    /* ntp1.aliyun.com */
};

/* 时间早于这个值就认为"没同步过"（2024-01-01 = 1704067200） */
#define SC_TIME_MIN_VALID  1704067200u

#define SC_TIME_SAVE_PATH  "/data/etc/superchild/time.saved"

/* NTP 纪元(1900) 到 Unix 纪元(1970) 的秒数 */
#define SC_NTP_EPOCH_DIFF  2208988800u

/* ---------------------------------------------------------------- 基础 */

static const char *time_str(void);

uint32_t sc_time_now(void)
{
    return (uint32_t)time(NULL);
}

bool sc_time_ready(void)
{
    return sc_time_now() >= SC_TIME_MIN_VALID;
}

static void sc_time_set(uint32_t t)
{
    struct timespec ts;

    ts.tv_sec = (time_t)t;
    ts.tv_nsec = 0;

    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        SC_ERR("time: clock_settime(%u) 失败 errno=%d\n", (unsigned)t, errno);
        return;
    }
    SC_LOG("time: 系统时间已同步 → %u（%s）\n", (unsigned)t, time_str());
}

/* 把当前时间格式化成可读串（静态缓冲，仅供日志用，非线程安全） */
static const char *time_str(void)
{
    static char buf[40];
    time_t t = (time_t)sc_time_now();
    struct tm tmv;

    if (gmtime_r(&t, &tmv) == NULL) {
        return "?";
    }
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

/* ---------------------------------------------------------------- 持久化 */

bool sc_time_restore_saved(void)
{
    int fd;
    char buf[32];
    int n;
    unsigned long v;

    fd = open(SC_TIME_SAVE_PATH, O_RDONLY);
    if (fd < 0) {
        SC_LOG("time: 没有已保存的时间（首次开机？）\n");
        return false;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';

    v = strtoul(buf, NULL, 10);
    if ((uint32_t)v < SC_TIME_MIN_VALID) {
        SC_WARN("time: 保存的时间 %lu 明显无效，忽略\n", v);
        return false;
    }

    /*
     * 直接把时钟设成上次的值。**可能偏旧**（比如上次关机是一周前），
     * 但证书有效期是月级的，偏几天完全够用 ——
     * 这样"所有时间源都不可用"时也能立刻开始工作，
     * 而不是卡在 1970 上反复报"证书还没生效"。
     */
    sc_time_set((uint32_t)v);
    SC_LOG("time: 已从 %s 恢复时间（可能偏旧，稍后会用网络校正）\n",
           SC_TIME_SAVE_PATH);
    return true;
}

void sc_time_save(void)
{
    uint32_t now = sc_time_now();
    char buf[32];
    int fd;
    int n;

    if (now < SC_TIME_MIN_VALID) {
        return;
    }

    fd = open(SC_TIME_SAVE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        SC_WARN("time: 存不进 %s（errno=%d），下次开机就只能靠网络同步了\n",
                SC_TIME_SAVE_PATH, errno);
        return;
    }
    n = snprintf(buf, sizeof(buf), "%u\n", (unsigned)now);
    if (write(fd, buf, (size_t)n) != n) {
        SC_WARN("time: 写入 %s 不完整\n", SC_TIME_SAVE_PATH);
    }
    close(fd);
}

/* ---------------------------------------------------------------- SNTP */

/*
 * 一次 SNTP 查询。
 *
 * 报文就是 48 字节：首字节 0x1B = LI(0) | VN(3) | Mode(3, client)，
 * 其余全 0。服务端回的 48 字节里，**发送时间戳在偏移 40..43**
 * （大端，自 1900 起的秒数）。
 *
 * 直接单播给已知 IP —— 不用广播，也就不依赖 ARP 是否正常。
 */
static bool sntp_once(const char *ip, int port, int timeout_ms, uint32_t *out)
{
    struct sockaddr_in sa;
    struct timeval tv;
    unsigned char pkt[48];
    uint32_t secs;
    int fd;
    int n;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (tv.tv_sec == 0 && tv.tv_usec == 0) {
        tv.tv_usec = 500 * 1000;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        close(fd);
        return false;
    }

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;

    if (sendto(fd, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return false;
    }

    n = (int)recv(fd, pkt, sizeof(pkt), 0);
    close(fd);

    if (n < 48) {
        return false;
    }

    secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
           ((uint32_t)pkt[42] << 8)  | (uint32_t)pkt[43];

    if (secs < SC_NTP_EPOCH_DIFF) {
        return false;
    }
    *out = secs - SC_NTP_EPOCH_DIFF;
    return true;
}

/* ---------------------------------------------------------- HTTP Date */

static const char *const g_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/*
 * 前 3 个字符的大小写不敏感比较。
 *
 * 不用 libc 的 strncasecmp：NuttX 那份受配置项影响，不是所有配置
 * 都编进去（和 HTTP 头名比较是同一个理由）。
 */
static int ci_eq3(const char *a, const char *b)
{
    int i;

    for (i = 0; i < 3; i++) {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb + 32);
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

/* 年/月/日 → 自 1970-01-01 起的天数（Howard Hinnant 的 days_from_civil） */
static int32_t days_from_civil(int y, int m, int d)
{
    int32_t era;
    unsigned yoe;
    unsigned doy;
    unsigned doe;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

/*
 * 解析 HTTP 的 Date 头：`Sun, 20 Sep 2026 20:30:00 GMT`
 *
 * 单独暴露出来是为了**能在宿主上直接测** —— 日期解析错一位
 * 会让时间差几天甚至几个月，而现象是偶发的"证书校验失败"，
 * 在现场极难定位。
 *
 * 宽容处理：星期几、逗号、GMT 后缀都可缺；月份名大小写不敏感。
 * HTTP 的 Date 一定是 GMT，所以不需要时区换算。
 */
uint32_t sc_time_parse_http_date(const char *s)
{
    char mon_name[4];
    int day = 0;
    int mon = -1;
    int year = 0;
    int hh = 0;
    int mm = 0;
    int ss = 0;
    const char *p;
    int i;

    if (s == NULL || s[0] == '\0') {
        return 0;
    }

    /* 跳过开头的星期几（"Sun, " 或 "Sunday, "） */
    p = s;
    while (*p != '\0' && *p != ',' && !(*p >= '0' && *p <= '9')) {
        p++;
    }
    if (*p == ',') {
        p++;
    }
    while (*p == ' ') {
        p++;
    }

    mon_name[0] = '\0';
    if (sscanf(p, "%d %3s %d %d:%d:%d",
               &day, mon_name, &year, &hh, &mm, &ss) != 6) {
        return 0;
    }
    mon_name[3] = '\0';

    for (i = 0; i < 12; i++) {
        if (ci_eq3(mon_name, g_months[i])) {
            mon = i;
            break;
        }
    }

    /* 边界检查：宁可不设时间，也不要设一个错得离谱的值 ——
     * 错的时间会让证书校验以**看起来像证书问题**的方式失败。 */
    if (mon < 0) {
        return 0;
    }
    if (year < 1970 || year > 2100) {
        return 0;
    }
    if (day < 1 || day > 31) {
        return 0;
    }
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) {
        return 0;
    }

    {
        int64_t t = (int64_t)days_from_civil(year, mon + 1, day) * 86400
                    + hh * 3600 + mm * 60 + ss;

        if (t < 0) {
            return 0;
        }
        return (uint32_t)t;
    }
}

static bool http_date_once(const char *ip, int port, const char *host,
                           int timeout_ms, uint32_t *out)
{
    struct sockaddr_in sa;
    struct timeval tv;
    char req[256];
    static char rbuf[2048];
    const char *hdr;
    int fd;
    int n;
    int total = 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (tv.tv_sec == 0 && tv.tv_usec == 0) {
        tv.tv_usec = 500 * 1000;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        close(fd);
        return false;
    }

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return false;
    }

    /*
     * HTTP/1.0 —— **不要 keep-alive**。
     * 我们只要响应头里的 Date，响应体是死重量。
     */
    n = snprintf(req, sizeof(req),
                 "GET / HTTP/1.0\r\n"
                 "Host: %s\r\n"
                 "User-Agent: superchild\r\n"
                 "Accept: */*\r\n"
                 "\r\n", host);

    if (send(fd, req, (size_t)n, 0) != n) {
        close(fd);
        return false;
    }

    while (total < (int)sizeof(rbuf) - 1) {
        n = (int)recv(fd, rbuf + total, sizeof(rbuf) - 1 - (size_t)total, 0);
        if (n <= 0) {
            break;
        }
        total += n;
        rbuf[total] = '\0';
        if (strstr(rbuf, "\r\n\r\n") != NULL) {
            break;
        }
    }
    close(fd);

    if (total <= 0) {
        return false;
    }
    rbuf[total] = '\0';

    hdr = strstr(rbuf, "Date:");
    if (hdr == NULL) {
        hdr = strstr(rbuf, "date:");
    }
    if (hdr == NULL) {
        SC_WARN("time: 响应里没有 Date 头（前 60 字节：%.60s）\n", rbuf);
        return false;
    }
    hdr += 5;
    while (*hdr == ' ') {
        hdr++;
    }

    {
        uint32_t t = sc_time_parse_http_date(hdr);

        if (t == 0) {
            SC_WARN("time: Date 头解析失败：「%.40s」\n", hdr);
            return false;
        }
        *out = t;
        return true;
    }
}

/* ---------------------------------------------------------------- 编排 */

bool sc_time_sync_once(int timeout_ms)
{
    uint32_t t = 0;
    int i;

    /* --- A. SNTP（快，但可能被网络封）--- */
    for (i = 0; i < (int)(sizeof(g_ntp) / sizeof(g_ntp[0])); i++) {
        if (sntp_once(g_ntp[i], 123, timeout_ms / 4, &t)) {
            SC_LOG("time: SNTP 同步成功（%s）\n", g_ntp[i]);
            sc_time_set(t);
            sc_time_save();
            return true;
        }
    }

    /* --- B. HTTP Date（主路：只要 TCP 通就行）--- */
    if (http_date_once(SC_HTTP_TIME_IP, SC_HTTP_TIME_PORT,
                       SC_HTTP_TIME_HOST, timeout_ms / 2, &t)) {
        SC_LOG("time: 由 HTTP Date 头同步成功（%s:%d）\n",
               SC_HTTP_TIME_IP, SC_HTTP_TIME_PORT);
        sc_time_set(t);
        sc_time_save();
        return true;
    }

    SC_WARN("time: HTTP Date 也失败（%s:%d 的 80 端口不可达？）\n",
            SC_HTTP_TIME_IP, SC_HTTP_TIME_PORT);
    return false;
}

bool sc_time_sync(int timeout_ms)
{
    uint32_t t0 = sc_now_ms();
    int round = 0;

    if (sc_time_ready()) {
        return true;
    }

    while ((int)(sc_now_ms() - t0) < timeout_ms) {
        struct timespec ts;

        round++;
        SC_LOG("time: 第 %d 轮同步…\n", round);

        if (sc_time_sync_once(3000)) {
            return true;
        }

        ts.tv_sec = 1;
        ts.tv_nsec = 0;
        nanosleep(&ts, NULL);
    }

    SC_ERR("time: %d ms 内没能同步上。TLS 证书校验会失败 —— "
           "报错是 certificate validity starts in the future，"
           "看起来像证书有问题，实际是板子以为是 1970 年。\n", timeout_ms);
    return false;
}
