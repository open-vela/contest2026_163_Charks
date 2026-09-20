/*
 * 可移植性薄层。
 *
 * 目标：sc_b64 / sc_resamp / sc_ws / sc_tls / sc_stepfun **同一份源码**
 * 既能编进 openvela 固件，也能在宿主 Linux 上原生编译。
 *
 * 为什么值得这么做：TLS + WebSocket + JSON 这一层如果只能在板子上调试，
 * 每次改一行都要「交叉编译 → 打包 → 烧录 → 上电」十几分钟，而且出错时
 * 只能看串口。在宿主上能直接连真实 StepFun 验证，把「协议栈对不对」和
 * 「板子环境对不对」两个问题**分开**，否则它们搅在一起没法定位。
 *
 * 宿主编译时定义 SC_HOST_TEST；板端不定义。
 */

#ifndef __SUPERCHILD_SC_PORT_H
#define __SUPERCHILD_SC_PORT_H

/*
 * ⚠️ 这一行必须在**任何**系统头文件之前。
 *
 * 严格 -std=c11 下 glibc 默认不暴露 clock_gettime / CLOCK_MONOTONIC
 * 这类 POSIX 符号，而特性宏只有在处理第一个系统头文件之前定义才生效。
 * 这个文件是所有板端模块的第一个 include，所以放这里最稳妥
 * （放在 .c 里会失效 —— 因为 .h 已经把 <stdio.h> 引进去了）。
 */
#ifdef SC_HOST_TEST
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef SC_HOST_TEST

#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>

#  define SC_LOG(fmt, ...)   fprintf(stderr, "[sc] " fmt "\n", ##__VA_ARGS__)
#  define SC_DBG(fmt, ...)   do { if (g_sc_verbose) fprintf(stderr, "[sc] " fmt "\n", ##__VA_ARGS__); } while (0)

extern int g_sc_verbose;

#else /* 板端（NuttX） */

#  include <nuttx/config.h>
#  include <string.h>
#  include <syslog.h>

#  define SC_LOG(fmt, ...)   syslog(LOG_INFO,    "[sc] " fmt, ##__VA_ARGS__)
#  define SC_ERR(fmt, ...)   syslog(LOG_ERR,     "[sc] " fmt, ##__VA_ARGS__)
#  define SC_WARN(fmt, ...)  syslog(LOG_WARNING, "[sc] " fmt, ##__VA_ARGS__)

#  ifdef CONFIG_DEBUG_INFO
#    define SC_DBG(fmt, ...) syslog(LOG_DEBUG, "[sc] " fmt, ##__VA_ARGS__)
#  else
#    define SC_DBG(fmt, ...) do { } while (0)
#  endif

#endif /* SC_HOST_TEST */

#ifndef SC_ERR
#  define SC_ERR(fmt, ...)   SC_LOG("[错误] " fmt, ##__VA_ARGS__)
#endif
#ifndef SC_WARN
#  define SC_WARN(fmt, ...)  SC_LOG("[注意] " fmt, ##__VA_ARGS__)
#endif
#ifndef SC_INFO
#  define SC_INFO(fmt, ...)  SC_LOG(fmt, ##__VA_ARGS__)
#endif

/* 毫秒时间戳（单调钟）。板端与宿主都有 clock_gettime。 */
uint32_t sc_now_ms(void);

/* 当前 Unix 时间（秒）。TLS 证书有效期校验要用；NuttX 上由 ntpcstart 提供。
 * 若返回 <= 0 表示时间未同步。 */
int64_t  sc_wall_time(void);

/* 随机字节（给 WebSocket 的 Sec-WebSocket-Key 和 mbedtls 熵源兜底用）。
 * 优先 /dev/urandom，取不到时退化为时间戳混合，**并在日志里告警**。 */
int      sc_random(void *buf, size_t len);

/*
 * 安全清零。为什么不用 memset：编译器会把「写完就不再读」的 memset
 * 优化掉，密钥就留在栈上了。用 volatile 指针阻断这个优化。
 */
void     sc_memzero(void *p, size_t n);

/* 校验比较：不因位置不同而提前返回，避免时间侧信道。 */
int      sc_timing_safe_eq(const void *a, const void *b, size_t n);

#endif /* __SUPERCHILD_SC_PORT_H */
