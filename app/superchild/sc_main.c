/*
 * 小满 —— 应用入口（方案 C：全部跑在板子上）。
 *
 * 目标形态：**开机直接进 face 界面，直接能说话**。
 * 不需要电脑、不需要网关、不需要 adb、不需要敲命令。
 *
 * 与上一版的区别
 * =============
 * 上一版板子只跑"裸 TCP + 4 字节头"，TLS / WebSocket / JSON / 采样率
 * 转换 / 情绪判定 / 安全策略全在 ECS 上的网关里。这一版**全部下沉到板子**：
 *
 *     走掉的：  sc_net.c（板子⇄网关的私有协议）
 *     新来的：  sc_tls.c      TLS（内嵌 DigiCert G2 根证书）
 *               sc_ws.c       WebSocket 客户端
 *               sc_stepfun.c  会话协议（session.update / append / delta）
 *               sc_emo.c      情绪判定（emotion.py 的 C 移植）
 *               sc_safety.c   儿童安全关键词（safety.py 的 C 移植）
 *               sc_key.c      API Key（**从 /data 读，不进固件**）
 *               sc_rt.c       编排：闸门 / 音频泵 / 重连 / 界面投递
 *
 * 任务划分（4 个任务 + 主任务）
 * ============================
 *   main / UI 任务（本函数）
 *       LVGL 初始化 + 表情界面 + 每 60ms 画一帧。
 *       **唯一允许调 LVGL 的任务**（LVGL 不是线程安全的）。
 *
 *   sc_rt 任务
 *       TLS + WebSocket + 会话 + 闸门 + 心情推进。
 *       所有对界面的修改都走 sc_face_post_*（线程安全的投递队列）。
 *
 *   sc_cap 任务
 *       麦克风 → **先问闸门** → sc_audio_tx_push() → 由 sc_rt 发出去
 *
 *   sc_play 任务
 *       收到的音频 → sc_audio_play_once() → 喇叭
 *
 * 为什么采集和播放要拆成两个任务：两者都是阻塞 IO。合成一个的话，
 * "正在播"和"正在录"会互相卡住，表现出来就是机器人一说话就听不见孩子。
 *
 * 为什么闸门放在**入队前**（而不是发送前）：如果先把音频推进环形队列、
 * 发送时再判断，闸门打开后会把**几秒前的旧声音**发上去 ——
 * 模型听到的是错位的对话，表现为"答非所问"，比丢一点音频糟糕得多。
 */

#include <nuttx/config.h>

#include "sc_audio.h"
#include "sc_disp.h"
#include "sc_face.h"
#include "sc_proto.h"
#include "sc_rt.h"
#include "sc_wifi.h"

#include <lvgl/lvgl.h>

#include <debug.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
#  include <uv.h>
#endif

/* ---------------------------------------------------------------- 配置 */

#ifndef CONFIG_SUPERCHILD_UI_TICK_MS
#  define CONFIG_SUPERCHILD_UI_TICK_MS 60
#endif

#ifndef CONFIG_SUPERCHILD_RT_STACKSIZE
#  define CONFIG_SUPERCHILD_RT_STACKSIZE 20480
#endif

#define TAG "superchild"

static volatile int g_running = 1;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ==========================================================================
 * 任务：采集
 *
 * 麦克风 → （闸门）→ 上行队列。
 *
 * ⚠️ 闸门判断必须在**入队前**。见文件头。
 *
 * 采到之后顺手算一个响度投给界面，让孩子说话时机器人的眼睛有反应 ——
 * 这是"它在听我"最直接的证据，比任何文字都有效。
 * **电平不受闸门影响**：即使闸门关着，眼睛该有反应还是要有，
 * 否则孩子会以为"它没听见我"。
 * ========================================================================== */

static void *cap_task(void *arg)
{
    uint8_t frame[SC_AUDIO_FRAME_BYTES];
    uint32_t last_log = 0;
    uint32_t last_stat;
    int peak = 0;
    uint32_t pushed = 0;
    uint32_t gated = 0;

    (void)arg;

    if (!sc_audio_capture_ok()) {
        syslog(LOG_ERR, "[%s] 没有可用采集设备，采集任务退出\n", TAG);
        return NULL;
    }

    last_stat = now_ms();

    while (g_running) {
        int n = sc_audio_capture_once(frame, sizeof(frame));

        if (n == -EAGAIN) {
            continue;
        }
        if (n <= 0) {
            if (now_ms() - last_log > 5000) {
                syslog(LOG_WARNING, "[%s] 采集失败 %d\n", TAG, n);
                last_log = now_ms();
            }
            usleep(50000);
            continue;
        }

        /* 电平永远更新（"它在听我"） */
        {
            int lv = sc_audio_level(frame, (size_t)n);

            if (lv > peak) {
                peak = lv;
            }
            sc_face_post_level(lv, -1);
        }

        /* 闸门：正在播/刚播完就**直接丢掉**，不入队 */
        if (sc_rt_uplink_open()) {
            sc_audio_tx_push(frame, (size_t)n);
            pushed++;
        } else {
            gated++;
        }

        /*
         * 每 2 秒报一次"麦克风实况"。**别删** —— 这是回答
         * "它到底听不听得见"唯一的硬证据，而且只靠听得见/听不见
         * 是分不出下面这几种情况的（它们的表象完全一样：说了没反应）：
         *
         *   · 峰值 ≈ 0        → 采集通路没打开 / 麦克风没接 / 增益为 0
         *   · 峰值 大于 0 但很小（<10）→ 有声音但太轻，服务端 VAD 门限（700）
         *                                根本触发不了 → 要从增益或门限入手
         *   · 闸门一直"关丢弃" → 上行根本没送出去（半双工闸门卡住）
         *   · 峰值正常、也一直在推  → 板子这一侧没问题，
         *                             该去查服务端 VAD 门限或音频格式标签
         *
         * 四种结论对应的修法完全不同，所以必须把这几个数分开打出来。
         */
        if ((uint32_t)(now_ms() - last_stat) >= 2000u) {
            SC_LOG("mic: 近2s 峰值 %d；闸门开推送 %u 块 / 闸门关丢弃 %u 块\n",
                   peak, (unsigned)pushed, (unsigned)gated);
            peak = 0;
            pushed = 0;
            gated = 0;
            last_stat = now_ms();
        }
    }
    return NULL;
}

/* ==========================================================================
 * 任务：播放
 *
 * 下行队列 → 喇叭。每 20ms 取一块（和采集同粒度），保证媒体缓冲水位平稳。
 * ========================================================================== */

static void *play_task(void *arg)
{
    uint8_t frame[SC_AUDIO_FRAME_BYTES];
    int silence_run = 0;

    (void)arg;

    if (!sc_audio_playback_ok()) {
        syslog(LOG_ERR, "[%s] 没有可用播放设备，播放任务退出\n", TAG);
        return NULL;
    }

    while (g_running) {
        int n = sc_audio_rx_pop(frame, sizeof(frame));

        if (n <= 0) {
            /* 没数据就睡 10ms 再看。这里**不要**补静音去"喂饱"设备 ——
             * 那会让嘴形一直在动，看起来像在自言自语。 */
            if (++silence_run > 3) {
                sc_face_post_level(-1, 0);
            }
            usleep(10000);
            continue;
        }
        silence_run = 0;

        {
            int rc = sc_audio_play_once(frame, (size_t)n);
            if (rc < 0) {
                syslog(LOG_WARNING, "[%s] 播放失败 %d\n", TAG, rc);
                usleep(20000);
            }
        }

        /* 嘴形跟着**真实播放电平**动 —— 口型同步。
         * 只按固定节奏开合嘴的话，一耳朵就能听出"假"。 */
        sc_face_post_level(-1, sc_audio_level(frame, (size_t)n));
    }
    return NULL;
}

/* ==========================================================================
 * UI 循环
 * ========================================================================== */

static void ui_loop(lv_nuttx_result_t *result)
{
    uint32_t next = now_ms();
    uint32_t frames = 0;            /* 统计窗口内的帧数 */
    uint32_t slow = 0;              /*   其中：掉帧次数 */
    uint32_t worst = 0;             /* 最差一帧的总耗时 */
    uint32_t worst_set = 0;         /*   其中：设属性 */
    uint32_t worst_draw = 0;        /*   其中：渲染+刷屏 */
    uint32_t last_stat = now_ms();  /* 上次报统计的时间 */
    uint32_t last_drop_log = 0;     /* 上次报掉帧的时间（限流用） */
    uint32_t flush_calls = 0;       /* 统计窗口内 flush 次数（见 sc_disp） */
    uint32_t flush_rows = 0;        /*   其中：累计推送的**行数** */
    uint32_t sum_draw = 0;          /* 统计窗口内 lv_timer_handler 总耗时 ms */
    uint32_t sum_flush_us = 0;      /*   其中：**刷屏**那部分（µs） */

    /*
     * ⚠️ 埋点不是调试残留，是这台机器上**必须常驻**的东西。
     *
     * 现场反馈"表情一卡一卡的"时，原因是"设属性慢"还是"渲染/刷屏慢"，
     * 看起来完全一样，但修法相反：
     *   · 设属性慢 → 别的任务在抢 CPU（本板没开 SCHED_RR，
     *                同优先级任务是纯 FIFO，不会互相抢占）
     *   · 渲染/刷屏慢 → LVGL 要重画的面积太大（比如整屏重绘）
     * 所以这里把两段**分开计时**，卡的时候一眼就能分清。
     */
#ifdef CONFIG_LV_USE_NUTTX_LIBUV
    /* 有的配置走 libuv 事件循环（官方示例的做法）。我们不用它 ——
     * 那个循环不会让我们插进 sc_face_tick()，表情就不会动。 */
    (void)result;
#endif

    if (result != NULL && result->indev != NULL) {
        /* 触摸用轮询模式，和表情动画同一个节奏 */
        lv_indev_set_mode(result->indev, LV_INDEV_MODE_TIMER);
    }

    while (g_running) {
        uint32_t t0 = now_ms();
        uint32_t t1;
        uint32_t t2;
        uint32_t dt;
        uint32_t set_ms;
        uint32_t draw_ms;
        uint32_t idle;

        /* 第一段：算这一帧该长什么样，把属性写进 LVGL 控件 */
        sc_face_tick(t0);
        t1 = now_ms();

        /* 第二段：LVGL 真正去渲染 + 刷屏 */
        idle = lv_timer_handler();
        t2 = now_ms();

        /*
         * 取走这一帧的 flush 统计。
         *
         * 这是判断"PARTIAL 到底有没有用"的硬指标：
         * FULL 模式下每帧恒定 1 次 flush / 240 行；
         * PARTIAL 下应该远少于 240 行。
         */
        {
            int fc = 0;
            int fr = 0;
            uint32_t fu = 0;

            sc_disp_take_flush_stat(&fc, &fr, &fu);

            /* 没接管显示时返回 -1，此时不累加（否则会变成天文数字） */
            if (fc >= 0) {
                flush_calls += (uint32_t)fc;
                flush_rows += (uint32_t)fr;
                sum_flush_us += fu;
            }
        }

        sum_draw += draw_ms;

        if (idle == 0) {
            idle = 1;
        }
#if SC_FACE_STATIC
        /*
         * 静态模式下画面本来就不动，没必要每 60ms 醒一次。
         * 拉长到 200ms —— CPU 占用明显下降，观感毫无差别。
         */
        if (idle > 200u) {
            idle = 200u;
        }
#else
        if (idle > CONFIG_SUPERCHILD_UI_TICK_MS) {
            idle = CONFIG_SUPERCHILD_UI_TICK_MS;
        }
#endif
        usleep(idle * 1000);
        next += idle;

        dt = t2 - t0;
        set_ms = t1 - t0;
        draw_ms = t2 - t1;

        frames++;
        if (dt > worst) {
            worst = dt;
            worst_set = set_ms;
            worst_draw = draw_ms;
        }

        /* 超过 1.5 倍预算就算掉帧（60ms 预算 → 90ms 以上） */
        if (dt > (uint32_t)(CONFIG_SUPERCHILD_UI_TICK_MS * 3 / 2)) {
            slow++;

            /*
             * ⚠️ 限流必须**按时间**，不能像第一版那样"只报前 12 次"。
             *
             * 第一版写的是 `if (slow <= 12 || (slow % 100) == 0)` ——
             * 结果日志在第 12 条就断了，看起来像"后来就不卡了"，
             * 实际上计数器还在涨、卡顿一直在发生。
             * **这个错误的上限把最关键的严重程度证据藏起来了**，
             * 让我差点得出"只启动时卡一下"的结论。
             * 现在改成最多 10 秒报一条，且**每条都带累计次数**。
             */
            if (slow <= 12 || (uint32_t)(t2 - last_drop_log) >= 10000u) {
                last_drop_log = t2;
                syslog(LOG_WARNING,
                       "[%s] 掉帧 #%u：本帧 %ums（设属性 %ums + 渲染刷屏 %ums）\n",
                       TAG, (unsigned)slow, (unsigned)dt,
                       (unsigned)set_ms, (unsigned)draw_ms);
            }
        }

        /*
         * 每 30 秒报一次**统计**。
         *
         * 这一条才是真正回答"到底卡不卡"的：它给出真实的帧率
         * （frames / 30s）。只有掉帧明细是不够的 ——
         * 因为帧率低也可能是"每帧都慢但不超标"，那样一条掉帧日志都不会有。
         */
        if ((uint32_t)(t2 - last_stat) >= 30000u) {
            uint32_t span = t2 - last_stat;
            uint32_t f = frames ? frames : 1u;
            uint32_t flush_ms = sum_flush_us / 1000u / f;
            uint32_t draw_ms_avg = sum_draw / f;
            /*
             * "渲染" = lv_timer_handler 总耗时 − 刷屏部分。
             * 这两者原来混在一起测，导致我上一轮误判成"屏太慢"。
             */
            uint32_t render_ms = (draw_ms_avg > flush_ms)
                                 ? (draw_ms_avg - flush_ms) : 0u;

            syslog(LOG_INFO,
                   "[%s] 帧统计：%u 帧 / %us = %u fps，掉帧 %u 次；"
                   "每帧 渲染 %ums + 刷屏 %ums（刷屏 %.1f 次、%.0f 行）；"
                   "最差一帧 %ums\n",
                   TAG, (unsigned)frames, (unsigned)(span / 1000u),
                   (unsigned)(frames * 1000u / (span ? span : 1u)),
                   (unsigned)slow,
                   (unsigned)render_ms, (unsigned)flush_ms,
                   (double)flush_calls / (double)f,
                   (double)flush_rows / (double)f,
                   (unsigned)worst);

            /*
             * 同时写一份到 /data。
             *
             * 为什么不能只靠 syslog：这块板子上 **WiFi 的 SPI 错误
             * 会疯狂刷屏**（`spi_err_dump` 一次几十行），dmesg 的环形
             * 缓冲几秒钟就被冲干净 —— 实测我的统计输出刚打完就被挤掉了。
             * 写文件则不会丢，随时能 cat 出来看。
             */
            {
                FILE *fp = fopen("/data/etc/superchild/last_stat.txt", "w");

                if (fp != NULL) {
                    fprintf(fp, "fps=%u  frames=%u  span=%us  drop=%u\n",
                            (unsigned)(frames * 1000u / (span ? span : 1u)),
                            (unsigned)frames, (unsigned)(span / 1000u),
                            (unsigned)slow);
                    fprintf(fp, "render_ms_per_frame=%u  flush_ms_per_frame=%u\n",
                            (unsigned)render_ms, (unsigned)flush_ms);
                    fprintf(fp, "flush_calls_per_frame=%.1f  flush_rows_per_frame=%.0f"
                                "  (full_screen_rows=240)\n",
                            (double)flush_calls / (double)f,
                            (double)flush_rows / (double)f);
                    fprintf(fp, "worst_frame_ms=%u\n", (unsigned)worst);
                    fclose(fp);
                }
            }

            frames = 0;
            slow = 0;
            worst = 0;
            worst_set = 0;
            worst_draw = 0;
            flush_calls = 0;
            flush_rows = 0;
            sum_draw = 0;
            sum_flush_us = 0;
            last_stat = t2;
        }
    }

    syslog(LOG_INFO, "[%s] 界面退出：本窗口 %u 帧 / 掉帧 %u 次；"
           "最差一帧 %ums（设属性 %ums + 渲染刷屏 %ums）\n",
           TAG, (unsigned)frames, (unsigned)slow, (unsigned)worst,
           (unsigned)worst_set, (unsigned)worst_draw);
}

/* ==========================================================================
 * 主函数
 * ========================================================================== */

int main(int argc, FAR char *argv[])
{
    lv_nuttx_dsc_t dsc;
    lv_nuttx_result_t result;
    pthread_t tid;
    pthread_attr_t attr;

    (void)argc;
    (void)argv;

    syslog(LOG_INFO, "[%s] 启动（方案 C：板端自持）\n", TAG);

    /* ---- 1. 音频 + 实时会话子系统 ---- */
    if (sc_audio_init() != 0) {
        /* 不致命：界面照常起来，孩子至少能看到一张会动的脸。
         * sc_rt 那边会把状态标成 OFFLINE 让屏幕显示出来。 */
        syslog(LOG_ERR, "[%s] 音频初始化不完整，继续启动界面\n", TAG);
    }

    /* 初始化情绪引擎 / 心情 / 角色提示词 */
    sc_rt_init();

    /* ---- 2. LVGL ---- */
    if (lv_is_initialized()) {
        syslog(LOG_ERR, "[%s] LVGL 已被别人初始化过了 —— "
               "是不是同时开了 mini_memo / lvgldemo？\n", TAG);
        return -1;
    }

    lv_init();
    lv_nuttx_dsc_init(&dsc);
#ifdef CONFIG_LV_USE_NUTTX_LCD
    dsc.fb_path = "/dev/lcd0";
#endif
    lv_nuttx_init(&dsc, &result);
    if (result.disp == NULL) {
        syslog(LOG_ERR, "[%s] 显示初始化失败 —— 屏幕起不来\n", TAG);
        lv_deinit();
        sc_audio_deinit();
        return -1;
    }
    syslog(LOG_INFO, "[%s] 显示 %ldx%ld，帧缓冲 %s\n", TAG,
           (long)lv_display_get_horizontal_resolution(result.disp),
           (long)lv_display_get_vertical_resolution(result.disp),
           dsc.fb_path ? dsc.fb_path : "(默认)");

    /*
     * 把显示驱动换成 **PARTIAL 局部刷新**。
     *
     * 系统自带的 lv_nuttx_lcd 只有 FULL 模式（每帧推整屏 153,600 字节），
     * 实测只有 ~1 fps —— 这是"表情一卡一卡"的真正原因。
     * 详见 sc_disp.h 文件头。
     *
     * 放在 lv_nuttx_init() 之后：这样刻度源、分辨率、触摸 indev
     * 都还是系统装好的，我们只换缓冲模式和 flush 回调，改动面最小。
     */
    if (sc_disp_takeover(result.disp) != 0) {
        syslog(LOG_WARNING, "[%s] 局部刷新切换失败 —— 退回系统默认，"
               "会明显变慢（每帧推整屏）\n", TAG);
    }

    /* 实测屏的写入成本，决定后续往哪个方向优化（带宽 vs 逐行开销） */
    sc_disp_bench();

    /* ---- 3. 表情界面（必须在 UI 任务里、且在此之前不能有别的 LVGL 调用）---- */
    if (sc_face_init() != 0) {
        syslog(LOG_ERR, "[%s] 表情界面初始化失败\n", TAG);
        lv_nuttx_deinit(&result);
        lv_deinit();
        sc_audio_deinit();
        return -1;
    }
    sc_face_set_link(2);          /* 连接中 */
    lv_timer_handler();            /* 先把第一帧推出去 */

    /*
     * 把一帧拆成「纯渲染」和「刷屏」分别测。
     * 这两个数直接决定下一步该动哪里 —— 见 sc_disp.c 的 sc_disp_bench2()。
     */
    sc_disp_bench2();

    /* ---- 4. 起任务 ----
     *
     * WiFi 守卫先起、独立于会话任务：
     *   它只做"检查 wlan0 有没有 IP + 必要时重连"，与 TLS 层无关。
     *   合成一个任务的话，WiFi 最长要等 1 分钟，会把重连节奏一起拖住。
     */
#ifdef CONFIG_SUPERCHILD_WIFI_GUARD
    pthread_attr_init(&attr);
    /* 栈给大一点：守卫里会用 system()，那会拉起 nsh 解析命令 */
    pthread_attr_setstacksize(&attr, 8192);
    if (pthread_create(&tid, &attr, sc_wifi_guard_task, NULL) != 0) {
        syslog(LOG_ERR, "[%s] 创建 WiFi 守卫任务失败\n", TAG);
    }
    pthread_attr_destroy(&attr);
#endif

    /*
     * 会话任务。
     *
     * 栈给 20KB：mbedtls 握手 + cJSON 解析都有一定的栈深度，
     * 给小了会在握手的某个分支上栈溢出 —— 而那个现象是随机的
     * 数据损坏，完全看不出是栈的问题。
     * （几个大缓冲都在 sc_rt.c 里做成 static 了，不走栈。）
     */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, CONFIG_SUPERCHILD_RT_STACKSIZE);
    if (pthread_create(&tid, &attr, sc_rt_task, NULL) != 0) {
        syslog(LOG_ERR, "[%s] 创建实时会话任务失败\n", TAG);
    }
    pthread_attr_destroy(&attr);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr,
        CONFIG_SUPERCHILD_AUDIO_STACKSIZE > 0
            ? CONFIG_SUPERCHILD_AUDIO_STACKSIZE : 4096);
    if (pthread_create(&tid, &attr, cap_task, NULL) != 0) {
        syslog(LOG_ERR, "[%s] 创建采集任务失败\n", TAG);
    }
    if (pthread_create(&tid, &attr, play_task, NULL) != 0) {
        syslog(LOG_ERR, "[%s] 创建播放任务失败\n", TAG);
    }
    pthread_attr_destroy(&attr);

    /* ---- 5. UI 循环（本任务）----
     * 主任务直接当 UI 任务用：省一个任务栈，而且 LVGL 的生命周期
     * 和进程生命周期天然一致，不会出现"界面还在画、进程已经退了"。 */
    syslog(LOG_INFO, "[%s] 进入界面循环（%dms/帧）\n", TAG,
           CONFIG_SUPERCHILD_UI_TICK_MS);
    ui_loop(&result);

    /* ---- 6. 收尾 ---- */
    g_running = 0;
    sc_face_deinit();
    sc_disp_deinit();
    lv_nuttx_deinit(&result);
    lv_deinit();
    sc_audio_deinit();
    syslog(LOG_INFO, "[%s] 退出\n", TAG);
    return 0;
}
