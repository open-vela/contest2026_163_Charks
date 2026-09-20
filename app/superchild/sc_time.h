/*
 * 时间同步（板端）。
 *
 * 为什么不用系统自带的 NTP 客户端
 * ==============================
 * NuttX 有 ntpc，`rcS` 里也起了 `ntpcstart &`，但在这台板子上**完全拿不到样本**：
 *
 *   1. 配置项踩空：`CONFIG_NETUTILS_NTPCLIENT_SERVERIP` 的 Kconfig 是
 *      `depends on !LIBC_NETDB`。板子开了 `LIBC_NETDB`（有 DNS），
 *      所以这一项**根本不存在** —— 我往里写的 IP 被静默忽略了。
 *      实际生效的是 `CONFIG_NETUTILS_NTPCLIENT_SERVER`，
 *      默认值 `0.pool.ntp.org;1.pool.ntp.org;2.pool.ntp.org`，
 *      **在国内基本连不上或极慢**。
 *
 *   2. 更根本的：**网络封了出站 UDP 123**。手动指定阿里云 NTP
 *      （203.107.6.88）再试，`ntpcstatus` 依然是 0 个样本。
 *
 * 而实测 **TCP 完全可用**（TLS 握手成功过、ping 公网 0% 丢包）。
 * 所以这里改用两条**不依赖 UDP 123** 的路：
 *
 *   A. SNTP over UDP —— 保留，因为换个网络（比如家里）它可能就是通的；
 *      而且它比 HTTP 更精确、更快。
 *   B. **HTTP `Date:` 响应头 over TCP:80** —— 主路。
 *      任何 HTTP 响应都带 `Date:`，**哪怕是 302/404/门户劫持页**，
 *      所以端口 80 只要 TCP 能通就一定能拿到时间。
 *
 * 另外还有一层兜底：**把同步成功的时间存进 /data**，开机先读回来。
 * 证书有效期是月级的，所以哪怕存的时间差一天也完全够用 ——
 * 这样即使所有时间源都不可用，开机也能立刻工作（不必等同步）。
 *
 * 为什么非要这个时间
 * ================
 * TLS 证书校验需要真实时间。时间不对时的报错是
 * `X509 - The certificate validity starts in the future`，
 * 看起来**像证书本身有问题** —— 会让人去查证书、查服务器。
 * 实际原因只是"板子以为是 1970 年"。
 */

#ifndef __SUPERCHILD_SC_TIME_H
#define __SUPERCHILD_SC_TIME_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 判断时间是否已可用于证书校验。
 *
 * 判据是"年份 >= 2024"而不是"时间 != 0"：板子没有 RTC，
 * 开机后一段时间内 time() 会返回 1970 或者上次存下来的值，
 * 用年份判断更稳，也能挡住"时间倒退"这类异常。
 */
bool sc_time_ready(void);

/* 当前 Unix 时间戳（秒）。 */
uint32_t sc_time_now(void);

/*
 * 从 /data 读回上次同步成功的时间并设置系统时钟。
 * @return true 读到了有效值并已设置
 */
bool sc_time_restore_saved(void);

/* 把当前时间存进 /data（用于下次开机快速恢复）。 */
void sc_time_save(void);

/*
 * 尝试一次时间同步。**按顺序**试 SNTP 和 HTTP Date，任一成功即返回。
 *
 * @param timeout_ms 整轮的预算
 * @return true 同步成功且时钟已设置
 */
bool sc_time_sync_once(int timeout_ms);

/*
 * 循环同步直到成功或超时。
 *
 * @param timeout_ms 总超时
 * @return true 成功
 */
bool sc_time_sync(int timeout_ms);

/*
 * 把 HTTP 的 `Date:` 头值解析成 Unix 时间戳（0 = 解析失败）。
 *
 * 单独暴露是为了**能在宿主上直接测**：日期解析错一位会让时间差
 * 几天甚至几个月，而现象是"偶发的证书校验失败"，在现场极难定位 ——
 * 必须先把它单独钉死。
 */
uint32_t sc_time_parse_http_date(const char *s);

#endif /* __SUPERCHILD_SC_TIME_H */
