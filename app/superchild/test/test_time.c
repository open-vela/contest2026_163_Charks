/*
 * HTTP Date 解析的自测。
 *
 * 为什么要单独测这个：日期解析错一位（月份名认错、闰年算错、
 * 时区忘了）会让系统时间差几天到几个月。而那个时间**看起来是合理的**，
 * 不会立刻出错 —— 要等到证书校验失败时才暴露，而报错是
 * "certificate validity starts in the future"，
 * 看起来像证书本身的问题。在现场极难定位。
 *
 * 所以这里用**已知正确答案**的日期逐个钉死，包括闰年和跨年边界。
 */

#include "sc_time.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int g_total = 0;

static void check_date(const char *s, uint32_t want)
{
    uint32_t got = sc_time_parse_http_date(s);

    g_total++;
    if (got == want) {
        printf("  [OK]   \"%s\"\n", s);
        printf("         → %u\n", (unsigned)got);
    } else {
        printf("  [FAIL] \"%s\"\n", s);
        printf("         期望 %u，实际 %u（差 %d 秒）\n",
               (unsigned)want, (unsigned)got,
               (int)((long)got - (long)want));
        g_fail++;
    }
}

static void check_bad(const char *s)
{
    uint32_t got = sc_time_parse_http_date(s);

    g_total++;
    if (got == 0) {
        printf("  [OK]   非法输入被拒：\"%s\"\n", s);
    } else {
        printf("  [FAIL] 非法输入居然解析成了 %u：\"%s\"\n",
               (unsigned)got, s);
        g_fail++;
    }
}

int main(void)
{
    printf("小满 · HTTP Date 解析自测\n");
    printf("=========================\n\n");

    printf("=== 标准格式 ===\n");
    /* 2026-09-20 20:30:00 UTC */
    check_date("Sun, 20 Sep 2026 20:30:00 GMT", 1789936200u);
    /* 2026-01-01 00:00:00 UTC —— 跨年边界 */
    check_date("Thu, 01 Jan 2026 00:00:00 GMT", 1767225600u);
    /* 2024-02-29 12:00:00 UTC —— 闰日 */
    check_date("Thu, 29 Feb 2024 12:00:00 GMT", 1709208000u);
    /* 2023-02-28 12:00:00 UTC —— 闰年前一天 */
    check_date("Tue, 28 Feb 2023 12:00:00 GMT", 1677585600u);
    /* 注：纪元起点（1970-01-01）刻意不测 —— 解析函数用 0 表示"失败"，
     * 两者不可区分。真实的 HTTP Date 也不会是 1970 年。 */

    printf("\n=== 容错：星期几缺失 / 大小写 / 多空格 ===\n");
    check_date("20 Sep 2026 20:30:00 GMT", 1789936200u);
    check_date("Sun,20 Sep 2026 20:30:00 GMT", 1789936200u);
    check_date("Sun, 20 SEP 2026 20:30:00 GMT", 1789936200u);
    check_date("Sunday, 20 Sep 2026 20:30:00 GMT", 1789936200u);

    printf("\n=== 边界 ===\n");
    /* 各月最大日 */
    check_date("Mon, 31 Dec 2035 23:59:59 GMT", 2082758399u);
    /* 世纪闰年 2000 是闰年 */
    check_date("Tue, 29 Feb 2000 00:00:00 GMT", 951782400u);

    printf("\n=== 非法输入必须被拒（宁可不同步，也不要设错时间）===\n");
    check_bad("");
    check_bad("not a date");
    check_bad("Sun, 20 Xxx 2026 20:30:00 GMT");   /* 月份名非法 */
    check_bad("Sun, 32 Sep 2026 20:30:00 GMT");   /* 日期越界 */
    check_bad("Sun, 20 Sep 1900 20:30:00 GMT");   /* 年份太早 */
    check_bad("Sun, 20 Sep 2026 25:30:00 GMT");   /* 小时越界 */

    printf("\n=========================\n");
    if (g_fail == 0) {
        printf("全部通过（%d 项）\n", g_total);
        printf("\n这一项钉死的意义：时间偏了的话，TLS 会以\n");
        printf("「certificate validity starts in the future」失败 ——\n");
        printf("看起来像证书有问题，实际是板子的时钟错了。\n");
        return 0;
    }
    printf("%d / %d 项失败\n", g_fail, g_total);
    return 1;
}
