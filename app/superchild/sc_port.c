/*
 * sc_port.h 的实现。宿主与板端各一份，#ifdef 分开。
 */

#include "sc_port.h"

#ifdef SC_HOST_TEST

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int g_sc_verbose = 0;

uint32_t sc_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

int64_t sc_wall_time(void)
{
    return (int64_t)time(NULL);
}

int sc_random(void *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (f == NULL) {
        return -1;
    }
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

#else /* 板端 NuttX */

#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

uint32_t sc_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

int64_t sc_wall_time(void)
{
    return (int64_t)time(NULL);
}

int sc_random(void *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < len) {
            ssize_t n = read(fd, (char *)buf + got, len - got);
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
        }
        close(fd);
        if (got == len) {
            return 0;
        }
    }

    /*
     * 退化路径：NuttX 的 /dev/urandom 依赖 CONFIG_DEV_URANDOM。
     * 取不到时不能让调用方静默拿到全零 —— 全零的 Sec-WebSocket-Key
     * 是合法但可预测的，握手照样能成，问题只在安全审计里才暴露。
     * 所以这里混入时间戳并告警。
     */
    SC_WARN("random: /dev/urandom 不可用，退化为时间戳混合（安全强度不足）\n");

    uint64_t s = (uint64_t)sc_now_ms() * 6364136223846793005ULL + 1442695040888963407ULL;
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        p[i] = (uint8_t)(s >> 33);
    }
    return -1;
}

#endif /* SC_HOST_TEST */

void sc_memzero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) {
        *v++ = 0;
    }
}

int sc_timing_safe_eq(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    uint8_t diff = 0;

    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(x[i] ^ y[i]);
    }
    return diff == 0;
}
