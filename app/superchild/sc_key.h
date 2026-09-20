/*
 * API Key 的读取。
 *
 * 核心决定：**Key 不进固件。**
 * ==========================
 * 方案 C 把一切都搬到了板子上，唯一真正的代价就是"密钥在设备上"这件事。
 * 但"在设备上"不等于"在固件里" —— 这两者的风险差很远：
 *
 *     写进 CONFIG_xxx（固件里）  任何人拿到 .img，strings 一下就有了。
 *                                实测过：本项目的 WiFi 密码就是这么被
 *                                一行 strings 抓出来的
 *     放在可写分区的一个文件里   固件可以随便传、重刷、送人都不泄露；
 *                                密钥只在你在场时下发一次
 *
 * 板子的 `/data` 是 YAFFS 可写分区（`mount -t yaffs /dev/usrdata /data`），
 * 重启保留，只有整片重刷或恢复出厂才会丢。正好符合需要。
 *
 * 所以：开机从 `/data/etc/superchild/stepfun.key` 读，读不到就在屏幕上
 * 明确显示"缺少密钥"，并用串口把**怎么下发**打出来。
 *
 * 为什么不用环境变量：NuttX 的 nsh 环境变量在 `luncher_mini` 拉起的
 * 应用里继承不到（rcS 里设的变量不会传到 launcher 再往下）。
 * 文件是最可靠的一种。
 */

#ifndef __SUPERCHILD_SC_KEY_H
#define __SUPERCHILD_SC_KEY_H

/*
 * 读取 API Key。
 *
 * 会去掉首尾空白与换行（用 echo 重定向写入时几乎必然带一个换行，
 * 而带换行的 key 发出去会是鉴权失败，且报错完全看不出是换行导致的）。
 *
 * @param out  输出缓冲
 * @param cap  容量（建议 ≥ 128）
 * @return key 的字节数；<0 表示读取失败
 */
int sc_key_load(char *out, int cap);

/* Key 文件的路径（用于日志和下发脚本） */
const char *sc_key_path(void);

/*
 * 把 key 安全清零。用完就清，别留在内存里。
 * （用 volatile 写，防编译器把"写完就不再读"的 memset 优化掉）
 */
void sc_key_wipe(char *key, int cap);

#endif /* __SUPERCHILD_SC_KEY_H */
