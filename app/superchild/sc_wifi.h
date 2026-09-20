/*
 * WiFi 联网守卫。
 *
 * 为什么需要这个文件（"开机即对话"缺的就是这一环）
 * =============================================
 *
 * 探察结论（2026-09-20，逐文件核对源码树得出）：
 *
 * 1. `rcS.nsh` 里**确实有**开机自动联网：
 *        if [ -f /data/etc/wifi/wapi.conf ]; then
 *            sh /etc/wifi/start_wifi.sh &
 *        fi
 *
 * 2. 但这个链条**两处都断**：
 *
 *    a) **那个文件根本不存在**。打包素材 `src/etctmp/etc/wifi/` 里
 *       只有 `start_wifi.sh`，没有 `wapi.conf`。所以 `-f` 判断为假，
 *       `start_wifi.sh` 一次都不会执行。
 *
 *    b) 就算文件补上了，`start_wifi.sh` 也跑不动：它用
 *       `SSID=$(grep -oP ...)` 提取配置 —— 而**板上没有 grep**，
 *       也没有 `$(...)` 命令替换（nsh 不支持）。结果 SSID/PSK 全是空，
 *       脚本走到 `wapi reconnect` 时是在重连一个没配过的网卡。
 *
 * 3. 官方真正可用的机制是 **netinit 的编译期配置**：
 *        apps/netutils/netinit/netinit_associate.c:
 *            load = wapi_load_config(ifname, NULL, &conf);
 *            if (!load) {                       // 读不到 wapi.conf 才用这里
 *                conf.ssid       = CONFIG_NETINIT_WAPI_SSID;
 *                conf.passphrase = CONFIG_NETINIT_WAPI_PASSPHRASE;
 *                conf.bssid      = NULL;        // ← 注意：不带 BSSID
 *            }
 *            if (conf.ssidlen > 0) wpa_driver_wext_associate(&conf);
 *       **不带 BSSID** 这一点很关键 —— 带了就会被钉死在某个 AP 上，
 *       换个网络必然连不上（这是项目文档里记过的坑，见
 *       `wapi.conf` 里那个写死的 `be:c1:a6:ff:d6:5e`）。
 *
 * 所以本文件做两件事：
 *
 *   · **可见性**：定期检查 `wlan0` 有没有 IP，把结果投给界面。
 *     孩子/家长一眼能看出"是没联网"还是"是它不理我" ——
 *     这两件事的排查方向完全不同。
 *
 *   · **兜底重试**：连不上时周期性尝试重连。netinit 只在开机那一刻
 *     关联一次，如果那时路由器还没起来（很常见：板子和路由器同时上电），
 *     就再也不会重试了。
 */

#ifndef __SUPERCHILD_SC_WIFI_H
#define __SUPERCHILD_SC_WIFI_H

#include <stdbool.h>

/* WiFi 状态。 */
enum sc_wifi_state {
    SC_WIFI_UNKNOWN = 0,   /* 还没查过 */
    SC_WIFI_OK,            /* 有 IP **且链路已建立**，可用 */
    SC_WIFI_NO_IP,         /* 网卡起来了但没拿到 IP（多半是密码错/信号弱） */
    SC_WIFI_NO_DEV,        /* 连 wlan0 都没有（驱动没加载） */

    /*
     * ⚠️ 后面加的这一档是**上板实测后才补的**，原因值得记：
     *
     * 板子的 netinit 配了**静态 IP**（CONFIG_NETINIT_IPADDR=10.0.0.2），
     * 于是**不管有没有关联上 AP，网卡永远"有 IP"**。
     * 原来只按"有没有非零 IP"判断，就一直返回 SC_WIFI_OK：
     *   · WiFi 守卫认为网络没问题 → 从不重试
     *   · sc_rt 认为网络就绪 → 直接连服务器 → ENETUNREACH(101)
     * 而屏幕显示"连接中"，完全看不出是 WiFi 没连上。
     *
     * 所以现在**同时**要求 IFF_RUNNING —— 那个标志只有在链路层
     * 真正建立（WiFi 关联成功）之后才会置位。
     *
     * 放在枚举**末尾**是刻意的：界面的判定是
     * `wifi_state != 0 && != 1`，并且用 `== 3` 区分"没有 wlan0"，
     * 所以新增值只要不是 3，就会落到"WiFi 未连"那一支 —— 语义正好。
     * 插在中间会改掉 NO_IP/NO_DEV 的数值，把界面判断弄错。
     */
    SC_WIFI_NO_LINK,
};

/*
 * 检查一次 wlan0 是否已获取 IP。用标准 socket ioctl，不依赖 wapi 库。
 * @return enum sc_wifi_state
 */
int sc_wifi_check(void);

/* 当前状态（上次检查的结果） */
int sc_wifi_state(void);

/*
 * 联网守卫任务主体。用 pthread_create 起一个独立任务跑它。
 *
 * 行为：最多尝试 SC_WIFI_MAX_TRIES 次，每次间隔递增（前几次快、
 *       后面慢下来），期间把状态投给界面。成功即返回。
 *       即使最终失败也返回 —— **不阻塞其它任务**，
 *       界面照样显示，孩子至少能看到一张会动的脸。
 */
void *sc_wifi_guard_task(void *arg);

#endif /* __SUPERCHILD_SC_WIFI_H */
