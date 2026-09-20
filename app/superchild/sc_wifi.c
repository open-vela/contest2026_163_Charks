/*
 * WiFi 联网守卫 —— 实现。设计动机见 sc_wifi.h 文件头。
 *
 * 为什么用 ioctl(SIOCGIFADDR) 而不是 wapi_get_ip()
 * -----------------------------------------------
 * `wapi_get_ip()` 在 `apps/wireless/wapi/src/` 里，但那个目录是编成
 * **可执行程序** `wapi` 的（`PROGNAME = wapi`），不是库 ——
 * 别的应用链接不到它。要用就得把那些 .c 再编一份进来，代价不值得。
 *
 * 而 `ioctl(SIOCGIFADDR)` 是标准 socket 接口，`ifconfig` 用的就是它，
 * 板子上肯定能用，且零额外依赖。
 *
 * 为什么重连用 system("wapi ...") 而不是直接调 API
 * ----------------------------------------------
 * 同上：wapi 是个独立程序。`system()` 让 nsh 去跑它，是这里最省事
 * 也最少出错的做法。**若 system() 不可用**（内核没编 nsh 的 system 支持），
 * 这里会静默跳过重连，只做状态显示 —— 不致命，因为主路径是
 * netinit 的编译期配置（见 sc_wifi.h）。
 */

#include "sc_wifi.h"
#include "sc_face.h"    /* 把 WiFi 状态投给界面角标 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#define TAG "sc_wifi"

#define SC_WIFI_IF          "wlan0"
#define SC_WIFI_MAX_TRIES   12          /* 只用于日志节流的分母 */
#define SC_WIFI_FIRST_WAIT  3
#define SC_WIFI_MAX_WAIT    12

/*
 * 已联网时的巡查间隔。
 *
 * ⚠️ 这个守卫**永不退出**，这一点是上板实测之后改的：
 *
 * 原来试 12 次（约 1 分钟）就 return 了。而这块板子上，
 * 真正的 DHCP 成功要等到开机几分钟之后 —— 因为 netinit 触发关联后
 * **不等链路建立就去跑 DHCP**（`netinit_associate.c` 里
 * `wpa_driver_wext_associate()` 之后直接 return），而 Realtek 驱动是
 * 异步关联的；那一轮 DHCP 必然失败。等驱动真正就绪时，
 * 守卫早就退出了，于是**永远不会有人再补一次 DHCP** ——
 * 现象是"WiFi 看着连上了，但一直没 IP，一直到关机"。
 *
 * 改成永不放弃之后，迟到的重试会自动补上。
 * 顺带也覆盖了"连上之后又掉线"的场景（netinit 只关联一次）。
 */
#define SC_WIFI_OK_POLL_S   15

static volatile int g_state = SC_WIFI_UNKNOWN;

int sc_wifi_state(void)
{
    return g_state;
}

/* ------------------------------------------------------------------------ */

int sc_wifi_check(void)
{
    struct ifreq ifr;
    struct sockaddr_in *sin;
    int fd;
    int rc;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        g_state = SC_WIFI_NO_DEV;
        return g_state;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, SC_WIFI_IF, IFNAMSIZ - 1);

    rc = ioctl(fd, SIOCGIFADDR, (unsigned long)&ifr);
    if (rc < 0) {
        /* 区分两种情况：网卡在但没 IP，还是连网卡都没有。
         * 这两种的排查方向完全不同（前者看密码/信号，后者看驱动）。 */
        struct ifreq q;
        memset(&q, 0, sizeof(q));
        strncpy(q.ifr_name, SC_WIFI_IF, IFNAMSIZ - 1);
        if (ioctl(fd, SIOCGIFFLAGS, (unsigned long)&q) < 0) {
            g_state = SC_WIFI_NO_DEV;
        } else {
            g_state = SC_WIFI_NO_IP;
        }
        close(fd);
        return g_state;
    }

    sin = (struct sockaddr_in *)&ifr.ifr_addr;

    /*
     * ⚠️ 光看"有没有非零 IP"**不够** —— 这条在板子上实测被坑过一次。
     *
     * 板子的 netinit 配了静态 IP（CONFIG_NETINIT_IPADDR=10.0.0.2），
     * 于是**不管有没有关联上 AP，网卡永远"有 IP"**。
     * 结果这个函数一直返回 SC_WIFI_OK：
     *   · WiFi 守卫认为网络没问题 → 从不重试
     *   · sc_rt 也认为网络就绪 → 直接去连服务器 → ENETUNREACH(101)
     * 而屏幕显示"连接中"，完全看不出是 WiFi 没连上。
     *
     * 所以必须**同时**要求 IFF_RUNNING —— 那个标志只有在链路层
     * 真正建立（WiFi 关联成功）之后才会置位。
     */
    if (sin->sin_addr.s_addr == 0 || sin->sin_addr.s_addr == INADDR_ANY) {
        g_state = SC_WIFI_NO_IP;
    } else {
        struct ifreq q;
        int running = 0;

        memset(&q, 0, sizeof(q));
        strncpy(q.ifr_name, SC_WIFI_IF, IFNAMSIZ - 1);
        if (ioctl(fd, SIOCGIFFLAGS, (unsigned long)&q) == 0) {
            running = (q.ifr_flags & IFF_RUNNING) != 0;
        }
        g_state = running ? SC_WIFI_OK : SC_WIFI_NO_LINK;
    }

    close(fd);
    return g_state;
}

/* ------------------------------------------------------------------------ */

/*
 * 一次重连尝试。
 *
 * 命令序列参考厂商的 `start_wifi.sh`，但**去掉了它那两个致命依赖**：
 *   · 不用 grep（板上没有）
 *   · 不用 $(...)（nsh 不支持）
 *   · **不设 BSSID** —— 设了就钉死某个 AP，换网络必死
 *
 * `wapi reconnect` 会按已保存的配置重新关联；`renew` 是重新 DHCP。
 * 两者都可能失败（比如本来就没关联过），失败不致命 —— 计一次，
 * 下一轮再来。
 */
static void wifi_retry_once(void)
{
    int rc;

    /* system() 在某些内核配置下不可用。不可用就只做状态显示，
     * 不折腾 —— 主路径是 netinit 的编译期配置。 */
    rc = system("wapi reconnect " SC_WIFI_IF);
    if (rc != 0) {
        syslog(LOG_INFO, "[%s] wapi reconnect 返回 %d（正常，可能本来就没关联）\n",
               TAG, rc);
    }

    rc = system("renew " SC_WIFI_IF);
    if (rc != 0) {
        syslog(LOG_INFO, "[%s] renew 返回 %d\n", TAG, rc);
    }
}

void *sc_wifi_guard_task(void *arg)
{
    int wait_s = SC_WIFI_FIRST_WAIT;
    int announced_ok = 0;
    int i;

    (void)arg;

    /*
     * 这里**不做 `system(NULL)` 探测**：NuttX 对 `system(NULL)` 的
     * 返回值语义不一定与 glibc 一致，拿它当判据反而会误判成"不可用"
     * 而白白放弃重连。改成"直接试一次，失败就只在日志里说一声" ——
     * 重连本身失败是无害的，主路径是 netinit 的编译期配置。
     *
     * ⚠️ **永不退出**（原来 12 次就 return，见 SC_WIFI_OK_POLL_S 的注释）。
     */
    for (i = 0; ; i++) {
        int st = sc_wifi_check();

        /* 状态上屏：孩子/家长一眼能看出"没联网"还是"它不理我" */
        sc_face_post_wifi(st);

        if (st == SC_WIFI_OK) {
            if (!announced_ok) {
                syslog(LOG_INFO, "[%s] wlan0 已就绪（第 %d 次检查）\n",
                       TAG, i + 1);
                announced_ok = 1;
            }
            /* 掉线后要能重新快速重试，所以把间隔重置回最小值 */
            wait_s = SC_WIFI_FIRST_WAIT;
            sleep(SC_WIFI_OK_POLL_S);
            continue;
        }

        announced_ok = 0;

        if (i == 0) {
            syslog(LOG_WARNING, "[%s] wlan0 还没有 IP（state=%d），开始重试。"
                   "若长时间失败请检查：\n"
                   "  · WiFi 是否配好（CONFIG_NETINIT_WAPI_SSID / PASSPHRASE）\n"
                   "  · /data/etc/wifi/wapi.conf 是否残留（它会覆盖编译期配置，"
                   "且可能写死了别的 AP 的 BSSID）\n"
                   "  · 路由器是否已起来（板子与路由器同时上电时很常见）\n",
                   TAG, st);
        }

        /* 前几次快、后面慢：开机那几秒链路可能还在建立，
         * 但也没必要一直每 3 秒敲一次。 */
        sleep((unsigned)wait_s);
        wifi_retry_once();

        wait_s = (wait_s < SC_WIFI_MAX_WAIT) ? wait_s + 2 : SC_WIFI_MAX_WAIT;

        /* 每约 5 分钟报一次，避免刷屏但又能看出"还在努力" */
        if ((i + 1) % 30 == 0) {
            syslog(LOG_WARNING, "[%s] 仍未联网：已重试 %d 次（state=%d），继续\n",
                   TAG, i + 1, g_state);
        }
    }

    /* 不可达：上面的循环是无限的 */
    return NULL;
}
