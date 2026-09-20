# WiFi 连接操作卡（Gemini-S1 / openvela）

> 实测环境：Gemini-S1 + 自编译固件（`NuttX 0.0.0 76354c6378`）
> 实测时间：2026-09-19　结果：**✅ 连通并 ping 通公网**

---

## 一句话结论

**板载 WiFi 开箱可用，不需要按丝印跳线。**

但有两个坑会让人误判成"硬件坏了"：
**`reconnect` 救固件加载，清 BSSID 救关联失败。**

---

## 标准操作流程

### 前置：确认板子在线

```bash
adb devices            # 应看到 1234  device
adb shell "ifconfig"   # 应看到 wlan0
```

### 第 1 步：扫描，确认目标 AP 在列表里

```bash
wapi scan wlan0
wapi scan_results wlan0
```

在输出里找到你的 SSID，记下 **BSSID** 与信号强度。

> 信号弱于 -80dBm 时关联会很不稳定，先把板子靠近路由器。

### 第 2 步：配置并连接

```bash
wapi disconnect wlan0
wapi essid wlan0 "<你的SSID>" 1
wapi psk   wlan0 "<你的密码>" 1 3        # 3 = WPA2-PSK
wapi ap    wlan0 00:00:00:00:00:00       # ★ 必须！清掉被钉死的 BSSID（见坑 2）
wapi power_save wlan0 off
wapi reconnect wlan0                     # ★ 必须！触发固件下载 + 关联（见坑 1）
```

### 第 3 步：等关联，看日志

```bash
# 等 10~15 秒
dmesg
```

**成功的样子**（关键几行）：

```
RTL871X: link to new AP
RTL871X: start auth to <目标AP的MAC>
RTL871X: auth success, start assoc
RTL871X: association success(res=2)
RTL871X: indicate_wx_custom_event WPA/WPA2 handshake done
```

### 第 4 步：DHCP 拿 IP

```bash
renew wlan0
ifconfig wlan0
```

**成功的样子**：

```
wlan0  HWaddr 84:fc:14:3d:5d:cb  at RUNNING
       inet addr:10.128.96.249  DRaddr:10.128.96.21  Mask:255.255.255.0
```

### 第 5 步：验证外网

```bash
ping -c 3 <网关IP>        # 先 ping 网关
ping -c 3 223.5.5.5       # 再 ping 公网 IP
ping -c 2 www.baidu.com   # 最后测 DNS
```

三条都通 = **完全联网** ✓

### 第 6 步（可选）：保存配置

```bash
wapi save_config wlan0
```

> ⚠️ 它会**顺手把当前 AP 的 BSSID 写进配置**。下次换网络记得先清（见坑 2）。

---

## 两个坑（按遇到概率排序）

### 坑 1：开机后 WiFi 是坏的 —— 敲 `reconnect` 就好

**症状**：`wapi scan` 返回空 / `wapi show` 不正常 / 驱动日志反复刷 `netif is DOWN`

**证据**（dmesg）：

```
[开机] rtl8723f_hal_init acquire FW from file:/resource/etc/wifi/FW_NIC_BCUT.bin
       download_fw: download firmware FAIL! status=0x27
       rtw_hal_init: hal__init fail
       -871x_drv - drv_open fail, bup=0

[reconnect 后]
       - download_firmware_8xxx HALMAC_RET_SUCCESS
       rtl8723f_hal_init Download Firmware from file success
```

**解法**：

```bash
wapi reconnect wlan0
```

**为什么**：`reconnect` 会**重走一遍驱动初始化**，这一遍通常能成功。

> 📌 芯片是 **D_CUT**，而 SDK 里只有 `FW_NIC_ACUT.bin` / `FW_NIC_BCUT.bin`。
> **实测结论：BCUT 固件完全能驱动 D_CUT 芯片**，不需要另找固件或联系厂商。

### 坑 2 ★：换网络后连不上 —— BSSID 被钉死了

**症状**：扫描明明扫到了目标 AP（甚至 -28dBm 就在旁边），但关联失败：

```
RTL871X: set ssid [你的SSID]
RTL871X: survey done event(11)                                                    ← 扫描是好的
RTL871X: rtw_select_and_join_from_scanned_queue: return _FAIL(candidate == NULL)  ← ★
RTL871X: try_to_join, but select scanning queue fail
RTL871X: indicate_wx_custom_event No Assoc Network After Scan Done
```

**原因**：`wapi save_config` 会把**上次成功关联的 AP 的 MAC** 写进
`/data/etc/wifi/wapi.conf` 的 `"bssid"` 字段。驱动只认这一个 MAC，
换网络后自然找不到。

```json
{"wlan0":{"mode":2,"auth":4,"cmode":8,"alg":3,
          "ssid":"玩会吧，别真学会了",
          "bssid":"b2:5a:a3:62:88:f0",     ← save_config 自动写进去的
          "psk":"<WIFI_PASSWORD_REDACTED>"}}
```

**解法**：

```bash
wapi ap wlan0 00:00:00:00:00:00     # 清零，恢复"自动选 AP"
wapi reconnect wlan0
```

或直接把 `/data/etc/wifi/wapi.conf` 里的 `"bssid"` 留空
（`start_wifi.sh` 里是 `if [ -n "$BSSID" ]`，留空会跳过 `wapi ap` 那一行）。

> ⚠️ **这个报错极具误导性。** 本次实测中它让人先后误判为：
>
> | 误判 | 验证方式 | 结果 |
> |---|---|---|
> | 中文 SSID 编码问题 | 换成纯 ASCII 的 `704` 再测 | ❌ **照样 NULL**，排除 |
> | 驱动 bug | 检查扫描/认证代码路径 | ❌ 都正常，排除 |
> | 固件版本不匹配（D_CUT vs BCUT） | 看固件下载日志 | ❌ 已成功加载，排除 |
> | **配置里残留的 BSSID** | `cat /data/etc/wifi/wapi.conf` | ✅ **就是这个** |
>
> **教训**：看到 `candidate == NULL` 且扫描正常时，**先去查配置里的 BSSID**。

---

## 排错速查表

| 现象 | 最可能的原因 | 先试这个 |
|---|---|---|
| `wapi scan` 返回空 | 驱动没初始化（坑 1） | `wapi reconnect wlan0` |
| 扫描有目标 AP，但 `candidate == NULL` | BSSID 被钉死（坑 2） | `wapi ap wlan0 00:00:00:00:00:00` |
| 关联成功但 IP 还是 `10.0.0.2` | 那是静态残留，DHCP 没跑 | `renew wlan0` |
| `renew` 报 `netlib_obtain_ipv4addr() failed` | 根本没关联上 | 先看 dmesg 里 `association` 那行 |
| 连得上但 ping 不通外网 | 路由器没连外网 / 需 Portal 认证 | 先 `ping <网关>` 确认局域网 |
| 目标 AP 是 `SYSU-SECURE` 之类 | **802.1X 企业认证，`wapi` 不支持** | 换 WPA2-PSK 的家用路由 / 热点 |

---

## 实测数据留档

**环境**（2026-09-19）：

| 项 | 值 |
|---|---|
| 板子 | Gemini-S1（R528），ADB serial `1234` |
| 固件 | 自编译 `NuttX 0.0.0 76354c6378 Sep 18 2026 22:01:45` |
| WiFi 模组 | RTL8723F（`CHIP_8723F_Normal_Chip_UMC_D_CUT_1T1R_RomVer(3)`），**SDIO** 接口 |
| 接口 MAC | `84:fc:14:3d:5d:cb` |

**连接结果**：

| 项 | 值 |
|---|---|
| SSID | `玩会吧，别真学会了`（2412MHz，**-28dBm**） |
| 加密 | WPA2-PSK |
| 获得 IP | `10.128.96.249` / 网关 `10.128.96.21` |
| ping 网关 | 10~11 ms |
| ping 223.5.5.5 | 42~75 ms |
| DNS 解析 | `www.baidu.com` → `111.45.11.5`，ping 33 ms |

**扫描能力**：一次扫到 12 个 AP，含 **2.4GHz 与 5GHz**（5765 / 5200 / 5805 MHz）→ **双频可用** ✓

---

## 已知遗留问题

1. **开机不会自动连上** —— 因为开机时固件下载会失败（坑 1），
   而 `start_wifi.sh` 里那次 `reconnect` 不一定能救回来。
   **建议**：改 `/etc/init.d/rcS` 在 WiFi 启动段后再加一次 `wapi reconnect`。
   （`/etc` 是 romfs 只读，改它需要重新编译固件。）
2. **`SYSU-SECURE` 校园网连不上** —— `wapi` 不支持 802.1X 企业认证。
3. **连通后 HTTPS 默认校验证书仍会失败** —— 因为板子**系统时间是 1970 年**
   （无 RTC，NTP 服务器写死为国外 `pool.ntp.org`，校园网封 UDP 123）。
   **先这样用**：`curl -k` 可以正常调用云端接口（实测打通百度 AI 开放平台）。
   **产品化前必修**：重编译固件换国内 NTP 服务器，恢复证书校验。
   **详见《探板记录表.md》§3.2。**

---

## 相关联的能力验证

WiFi 通了之后，**云端调用链路也一并验证过了**（R5 的前置）：

| 项 | 结果 |
|---|---|
| 纯 HTTP | ✅ 正常 |
| HTTPS + `curl -k` | ✅ **正常**（打通百度 AI 开放平台，服务器返回业务 JSON） |
| HTTPS 默认校验 | ❌ 失败（原因：系统时间 1970 年） |

详见《探板记录表.md》§3.2。
