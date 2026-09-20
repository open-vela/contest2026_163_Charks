/*
 * 显示驱动（LVGL 9.1，走 /dev/lcd0）。
 *
 * 为什么不用系统自带的 lv_nuttx_lcd_create()
 * ========================================
 * `lv_nuttx_lcd.c` 是这么选渲染模式的：
 *
 *     #if LV_NUTTX_LCD_BUFFER_COUNT > 0
 *         render_mode = LV_DISPLAY_RENDER_MODE_FULL;      // 只此一条路
 *     #else
 *         render_mode = LV_DISPLAY_RENDER_MODE_PARTIAL;   // 到不了
 *     #endif
 *
 * 而 Kconfig 里只有 SINGLE / DOUBLE / CUSTOM 三个选项，
 * **三个都让 BUFFER_COUNT > 0** —— 也就是说这个移植层
 * **根本没有提供局部刷新**。
 *
 * 后果：每帧都重画并推送整块 320×240 屏（153,600 字节），
 * 不管你实际改了多少东西。实测帧耗时约 1000ms → **1 fps**。
 *
 * 这一点和官方建议是相反的：LVGL 官方文档与 openvela 社区文章都写明
 * SPI 屏应当用 PARTIAL + 小缓冲（10–20 行），FULL 模式"实际很少使用"。
 *
 * 所以这里自己建显示驱动：**保留系统装好的 display（刻度源、
 * 分辨率、触摸 indev 都还在），只把缓冲模式和 flush 回调换掉**。
 * 这样改动面最小，也不会碰到厂商代码。
 *
 * 另一个必须知道的事实
 * ==================
 * NuttX 的 ILI9341 驱动**只实现了 putrun、没有 putarea**
 * （`nuttx/drivers/lcd/ili9341.c` 里 `pinfo->putrun = ili9341_putrun;`），
 * 于是 `lcd_dev.c` 会退化成"**逐行**调用 putrun"：
 *
 *     // Emulate putarea() using putrun()
 *     for (row = row_start; row <= row_end; row++) putrun(...);
 *
 * （这里原来写成 C 块注释，注释里嵌了斜杠星号，编译器会给
 *   -Wcomment 警告。改成行注释，别改回去。）
 *
 * 每行都要重新 select + 设地址窗口 + 发 640 字节 + deselect。
 * 实测 240 行 ≈ 1000ms，即 **每行 4.2ms** —— 这个数字决定了
 * 后续该往哪个方向优化，所以 sc_disp_bench() 专门去测它。
 */

#ifndef __SUPERCHILD_SC_DISP_H
#define __SUPERCHILD_SC_DISP_H

#include <lvgl/lvgl.h>

/*
 * 接管一个已经创建好的 display：换成 PARTIAL 模式 + 小缓冲 + 我们自己的
 * flush 回调（只把脏区经 LCDDEVIO_PUTAREA 推出去）。
 *
 * 必须在 lv_nuttx_init() 之后调用。
 *
 * @return 0 成功
 */
int sc_disp_takeover(lv_display_t *disp);

/* 释放（进程退出时调）。 */
void sc_disp_deinit(void);

/*
 * 实测 LCDDEVIO_PUTAREA 在不同行数下的耗时，结果写进日志。
 *
 * **这是决定优化方向的实验**，不是调试残留：
 *   · 耗时随行数线性增长、且斜率很大  → 纯带宽问题 → 该去提高 SPI 时钟
 *   · 耗时 ≈ 固定值 + 很小的每行增量  → 逐行开销问题 → 该去做整块 putarea
 *
 * 两种结论对应的修法完全不同，猜错方向会白干很久。
 */
void sc_disp_bench(void);

/*
 * 把一帧拆成「纯渲染」和「刷屏」两个数分别测。
 *
 * ⚠️ 必须在表情界面创建之后调用（要有真实内容可画）。
 */
void sc_disp_bench2(void);

/*
 * UI 循环每帧调一次：取走这一帧的 flush 统计并清零。
 *
 * 这个数字直接回答『PARTIAL 到底省了多少』：
 *   · FULL 模式下每帧恒定 1 次 flush / 240 行
 *   · PARTIAL 下应该是若干次 flush / 远少于 240 行
 * 如果行数没有明显下降，说明脏区本来就很大 —— 那就得换思路。
 */
void sc_disp_take_flush_stat(int *calls, int *rows, uint32_t *us);

/* 是否真的接管了显示驱动。
 * 现在恒为 0（见 sc_disp.c 里撤销接管那段说明）。 */
int sc_disp_is_owner(void);

#endif /* __SUPERCHILD_SC_DISP_H */
