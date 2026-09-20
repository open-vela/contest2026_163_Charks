/*
 * 显示驱动实现。设计动机与背景见 sc_disp.h 文件头。
 */

#include "sc_disp.h"
#include "sc_port.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/lcd/lcd_dev.h>

/* 每像素字节数（RGB565） */
#define SC_DISP_BPP       2

/*
 * PARTIAL 缓冲的高度（行）。
 *
 * 官方建议 1/10 ~ 1/4 屏：
 *   · < 1/10 会明显掉性能（每次能画的面太小，flush 次数暴涨）
 *   · > 1/4 收益递减
 * 320×240 的 40 行 = 12,800 像素 = 25,600 字节 ≈ 17%，落在甜点区。
 */
#define SC_DISP_BUF_ROWS  40

/* flush 统计（每次 flush 累加，UI 循环每帧取一次并清零） */
static volatile int g_flush_calls;
static volatile int g_flush_rows;
static volatile uint32_t g_flush_us;    /* 真正花在刷屏上的微秒数 */

static int g_fd = -1;
static void *g_buf;

/*
 * 基准实验用：置 1 时 flush 变成空操作（只回 flush_ready，不真的写屏）。
 *
 * 这样就能把一帧拆成两个可分别测量的数：
 *   · flush 关掉 → 测出来的是**纯渲染**
 *   · flush 打开 → 渲染 + 刷屏
 * 两者相减 = 刷屏成本。
 *
 * 之前两轮我一直在"渲染刷屏"这一个数上推理，结果先怪屏、后怪 CPU，
 * 两次都错。原因就是这两件事混在一个数字里，凭它推不出结论。
 */
static volatile int g_flush_noop;

/* ---------------------------------------------------------------- 计时 */

static uint64_t us_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* ---------------------------------------------------------------- flush */

/*
 * LVGL 把渲染好的**脏区**交给我们，我们只把这一块推给屏。
 *
 * 这是整个改动的核心：原来 FULL 模式下每帧都推整屏 153,600 字节；
 * 现在只推真正变了的矩形。
 *
 * ⚠️ `lv_display_flush_ready()` 必须在**数据真的写完**之后调用。
 * 这里 ioctl 是同步的（lcd_dev.c 会把 240 行 putrun 全部跑完才返回），
 * 所以放在 ioctl 之后是安全的。
 * 若以后改成 DMA 异步，**必须**挪到传输完成中断里，否则会花屏。
 */
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    struct lcddev_area_s a;
    int w;
    int h;

    w = (int)(area->x2 - area->x1 + 1);
    h = (int)(area->y2 - area->y1 + 1);

    g_flush_calls++;
    g_flush_rows += h;

    if (g_flush_noop) {
        lv_display_flush_ready(disp);
        return;
    }

    if (g_fd >= 0 && w > 0 && h > 0) {
        uint64_t t0 = us_now();

        a.row_start = (fb_coord_t)area->y1;
        a.row_end   = (fb_coord_t)area->y2;
        a.col_start = (fb_coord_t)area->x1;
        a.col_end   = (fb_coord_t)area->x2;
        a.stride    = (fb_coord_t)(w * SC_DISP_BPP);
        a.data      = px_map;

        if (ioctl(g_fd, LCDDEVIO_PUTAREA, (unsigned long)&a) < 0) {
            /* 不要在这里刷屏打日志 —— flush 是热路径 */
            static int warned;
            if (warned++ < 3) {
                SC_ERR("disp: PUTAREA 失败 errno=%d（%d,%d %dx%d）\n",
                       errno, (int)area->x1, (int)area->y1, w, h);
            }
        }

        /*
         * 把**真正刷屏**的时间单独累计起来。
         *
         * 为什么要这个：`lv_timer_handler()` 里其实混着两件事 ——
         * LVGL 的**软件渲染**，以及我们这里的刷屏。两者在帧耗时上
         * 看起来一模一样，但修法完全不同：
         *   · 刷屏慢 → 屏/SPI 的问题（降行数、提时钟）
         *   · 渲染慢 → LVGL 的问题（关特效、减少重绘对象）
         * 之前就是因为混在一起测，才误判成"屏太慢"，白改了一轮。
         */
        g_flush_us += (uint32_t)(us_now() - t0);
    }

    lv_display_flush_ready(disp);
}

/* ---------------------------------------------------------------- 接管 */

/*
 * ⚠️ 曾经在这里"接管"显示驱动（换成 PARTIAL 局部刷新），**已撤销**。
 *
 * 2026-09-20 实测：接管之后**屏幕花掉/白屏**。原因是接管本身
 * 和系统的渲染装置打架 —— 本板 `CONFIG_LV_USE_NUTTX_LIBUV=y`，
 * 初始化时会另外起几个线程驱动同一个 display：
 *
 *     lv_nuttx_uv_fb_init:    lvgl fb loop start OK
 *     lv_nuttx_uv_input_init: lvgl input loop start OK
 *     _lv_nuttx_uv_vsync_init: lvgl vsync start OK
 *     lv_nuttx_uv_timer_init: uv_timer_start(..., 1, 1)   ← 每 1ms 调 lv_timer_handler()
 *
 * 我在 `lv_nuttx_init()` 之后把 display 的缓冲/回调换掉，那些线程
 * 手里还攥着旧缓冲 → 两边同时往屏上写，结果就是花屏/白屏。
 *
 * 而且这个接管**本来就多余**：静态表情模式下画面不变，就没有每帧重绘，
 * 系统默认的整屏刷新只在内容真的变化时发生一次 —— 完全够用。
 *
 * 所以这里保留函数（调用点不动），但**什么都不做**。
 * 要重新打开局部刷新，得先解决与 libuv 线程的互斥，不能只改这一处。
 */
int sc_disp_takeover(lv_display_t *disp)
{
    (void)disp;

    SC_LOG("disp: 保留系统默认显示驱动（不接管）。"
           "静态表情下无每帧重绘，够用；接管会与 libuv 线程抢屏。\n");
    return 0;
}

/* 未接管时没有 flush 统计可报 */
int sc_disp_is_owner(void)
{
    return g_fd >= 0;
}

void sc_disp_deinit(void)
{
    if (g_buf != NULL) {
        lv_free(g_buf);
        g_buf = NULL;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

/*
 * UI 循环每帧调一次：取走这一帧的 flush 统计。
 *
 * 这个数字直接回答"PARTIAL 到底省了多少"：
 *   FULL 模式下每帧恒定 1 次 flush / 240 行；
 *   PARTIAL 下应该是"若干次 flush / 远少于 240 行"。
 * 如果行数没有明显下降，说明脏区本来就很大 —— 那就得换思路。
 */
void sc_disp_take_flush_stat(int *calls, int *rows, uint32_t *us)
{
    if (!sc_disp_is_owner()) {
        /* 没接管显示：没有我们的 flush 回调，也就没有可报的数据。
         * 报 -1 让统计里能看出"这一版没接 flush 统计"。 */
        *calls = -1;
        *rows = -1;
        *us = 0;
        return;
    }

    *calls = g_flush_calls;
    *rows = g_flush_rows;
    *us = g_flush_us;
    g_flush_calls = 0;
    g_flush_rows = 0;
    g_flush_us = 0;
}

/* ------------------------------------------------- 渲染 vs 刷屏 的分离实验 */

/*
 * 强制整屏重绘 + 立刻同步刷新，用来测一帧的真实成本。
 *
 * 用 `lv_refr_now()` 而不是 `lv_timer_handler()`：后者受定时器和
 * "已被别的线程调用"的判断影响（本板 libuv 事件循环在用 1ms 周期
 * 并发调用它），测出来的数不可比。
 */
static uint64_t time_one_full_redraw(void)
{
    lv_display_t *disp = lv_display_get_default();
    uint64_t t0;
    uint64_t t1;

    if (disp == NULL) {
        return 0;
    }

    /* 让整屏都算脏，保证每次测的都是同样的工作量 */
    lv_obj_invalidate(lv_scr_act());

    t0 = us_now();
    lv_refr_now(disp);
    t1 = us_now();

    return t1 - t0;
}

void sc_disp_bench2(void)
{
    uint64_t with_flush = 0;
    uint64_t render_only = 0;
    int i;

    /* 预热一次，避开首次路径的额外开销（缓存、分支预测） */
    (void)time_one_full_redraw();

    for (i = 0; i < 5; i++) {
        with_flush += time_one_full_redraw();
    }

    g_flush_noop = 1;
    for (i = 0; i < 5; i++) {
        render_only += time_one_full_redraw();
    }
    g_flush_noop = 0;

    with_flush /= 5u;
    render_only /= 5u;

    SC_LOG("bench2: 整屏重绘 —— 渲染+刷屏 %u µs，**纯渲染** %u µs，"
           "推算刷屏 %u µs\n",
           (unsigned)with_flush, (unsigned)render_only,
           (unsigned)(with_flush > render_only ? with_flush - render_only : 0));

    /*
     * 判据：
     *   · 纯渲染就已经接近 1 秒 → LVGL 渲染本身有问题（特效/变换/字体），
     *     跟屏和 SPI 完全无关 —— 那就别再去动屏
     *   · 纯渲染很小、加上刷屏才变大 → 才是屏的问题
     */
    if (render_only > 300000u) {
        SC_LOG("bench2: 结论 —— **瓶颈在 LVGL 渲染**，不在屏。"
               "该查的对象是特效/变换/字体，不是 SPI。\n");
    } else if (with_flush > render_only * 3u) {
        SC_LOG("bench2: 结论 —— **瓶颈在刷屏**（屏/SPI）。\n");
    } else {
        SC_LOG("bench2: 结论 —— 渲染与刷屏都不是瓶颈，"
               "那 1 秒是在**别处**（并发/调度/日志）花的。\n");
    }
}

/* ---------------------------------------------------------------- 实测 */

/*
 * 实测 PUTAREA 的耗时随行数怎么变。
 *
 * **这是决定优化方向的实验**：
 *   · 若耗时 ≈ 行数 × 固定单价       → 纯带宽问题 → 提高 SPI 时钟
 *   · 若耗时 ≈ 固定值 + 极小的行增量 → 逐行开销问题 → 做整块 putarea
 * 两种结论的修法完全相反，猜错会白干很久。
 */
void sc_disp_bench(void)
{
    static const int test_rows[] = { 1, 8, 32, 128, 240 };
    static const int n_test = (int)(sizeof(test_rows) / sizeof(test_rows[0]));
    struct lcddev_area_s a;
    int fd;
    int i;

    fd = open("/dev/lcd0", O_RDWR);
    if (fd < 0) {
        SC_ERR("bench: 打不开 /dev/lcd0（errno=%d）\n", errno);
        return;
    }

    /*
     * 注：`LCDDEVIO_GETAREAALIGN` 可以问驱动对区域对齐的要求，
     * 但 `struct lcddev_area_align_s` 不在 lcd_dev.h 里（要另找头文件），
     * 而系统自带的 lv_nuttx_lcd.c 也从不查它 —— 说明不查是安全的。
     * 这里就不引用了，避免为了一个诊断去赌结构体布局。
     */

    /* ---- 逐档测 ---- */
    {
        static uint8_t buf[320 * 240 * SC_DISP_BPP];   /* 需要的最大缓冲 */
        uint64_t us_1row = 0;

        SC_LOG("bench: PUTAREA 耗时实测（屏 %u 字节/行）\n",
               (unsigned)(320 * SC_DISP_BPP));

        memset(buf, 0x5a, sizeof(buf));

        for (i = 0; i < n_test; i++) {
            int rows = test_rows[i];
            int rep;
            int reps;
            uint64_t t0;
            uint64_t t1;
            uint64_t per;

            /* 时间太短会淹没在测量误差里，小区域多跑几遍 */
            reps = (rows <= 8) ? 20 : ((rows <= 32) ? 5 : 1);

            t0 = us_now();
            for (rep = 0; rep < reps; rep++) {
                a.row_start = 0;
                a.row_end   = (fb_coord_t)(rows - 1);
                a.col_start = 0;
                a.col_end   = 319;
                a.stride    = (fb_coord_t)(320 * SC_DISP_BPP);
                a.data      = buf;

                if (ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)&a) < 0) {
                    SC_ERR("bench: %d 行时失败 errno=%d\n", rows, errno);
                    close(fd);
                    return;
                }
            }
            t1 = us_now();
            per = (t1 - t0) / (uint64_t)reps;

            if (i == 0) {
                us_1row = per;
            }

            /*
             * 打印四列，一眼能看出是哪种问题：
             *   µs/次   —— 该行数的总耗时
             *   µs/行   —— 单价（若基本恒定 = 带宽受限）
             *   推算带宽 —— 若远低于 SPI 时钟，说明有大量固定开销
             */
            SC_LOG("bench: %3d 行 → %6u µs（%5u µs/行，推算 %3u KB/s）\n",
                   rows, (unsigned)per,
                   (unsigned)(per / (uint64_t)rows),
                   (unsigned)(rows ? ((uint64_t)rows * 320u * SC_DISP_BPP
                                      * 1000000u / (per ? per : 1u)) / 1024u : 0u));
        }

        SC_LOG("bench: 第 1 行单独耗时 %u µs —— 这一项就是『每行固定开销』的"
               "下限（不含像素传输）\n", (unsigned)us_1row);
        SC_LOG("bench: 判据 —— 若各档 µs/行 基本一致，是『带宽』问题"
               "（去提 SPI 时钟）；若 1 行就已经很贵、之后每行只加一点点，"
               "是『逐行开销』问题（去做整块 putarea）\n");
    }

    close(fd);
}
