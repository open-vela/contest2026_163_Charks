/*
 * 小满的脸 —— LVGL 表情界面
 *
 * 设计原则
 * ========
 *
 * **1. 屏幕是"孩子唯一能读懂的反馈通道"。**
 * 2-3 岁还没有"等待"的概念。从他说话到机器人开口有两三秒，
 * 这期间没有可见反馈，他会以为设备坏了 —— 然后重复喊，或者走开。
 * 所以每个状态、每种情绪都必须有一张**明显不同**的脸，不能只是"同一张脸换个嘴形"。
 *
 * **2. 情绪维度要够多，不能只有 4 个。**
 * 上一版（board/child_face）只有 4 个可调维度（眼高/嘴宽/嘴高/腮红），
 * 结果 12 个表情里有一半彼此看不出差别。这一版扩到 **10 个维度**：
 *
 *     眼睛：左右独立高度、宽度、整体高低、形状（6 种）
 *     眉毛：左右独立角度、整体高低     ← 表达力提升最大的一维
 *     嘴：  宽、高、上下、嘴角方向
 *     头：  倾斜、上下弹跳
 *     脸：  腮红浓度、背景色温
 *     装饰：Zzz / 音符 / 泪 / 汗 / 星光 / 心 / 撒花 / 食物
 *     动作：抖 / 弹 / 飘 / 嚼 / 挥 / 歪头
 *
 * **3. 数值全部是"短边千分比"，不写死像素。**
 * 屏幕尺寸可能变（320×240 / 240×320 / 7 寸 MIPI），
 * 写死像素值一换屏就全乱。
 *
 * ⚠️ 所有视觉数值都是**按比例推定的，未经上板逐项确认**。
 *    上板后请对着 `sc_face.c` 的 `g_looks[]` 表一项一项调，
 *    那里是唯一需要改的地方。
 */

#ifndef __SUPERCHILD_SC_FACE_H
#define __SUPERCHILD_SC_FACE_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ 形状 */

enum sc_eye_shape {
    SC_EYE_OVAL = 0,   /* 普通椭圆 */
    SC_EYE_WIDE,       /* 瞪大（比 OVAL 更圆更满） */
    SC_EYE_CLOSED,     /* 闭眼：一条细横线 */
    SC_EYE_ARC,        /* 眯眼笑：^-^ 的弧 */
    SC_EYE_HEART,      /* 心心眼 */
    SC_EYE_STAR,       /* 星星眼（十字星光） */
    SC_EYE_SPIRAL,     /* 螺旋眼（晕） */
};

enum sc_deco {
    SC_DECO_NONE = 0,
    SC_DECO_ZZZ,       /* 睡着：飘 Z */
    SC_DECO_NOTE,      /* 唱歌：飘音符 */
    SC_DECO_TEAR,      /* 委屈：眼泪 */
    SC_DECO_SWEAT,     /* 怕：汗滴 */
    SC_DECO_SPARKLE,   /* 得意/兴奋：眼周星光 */
    SC_DECO_HEART,     /* 喜欢：飘心 */
    SC_DECO_CONFETTI,  /* 庆祝：撒花 */
    SC_DECO_FOOD,      /* 吃东西：小饼干 */
};

enum sc_anim {
    SC_ANIM_NONE = 0,
    SC_ANIM_SHAKE,     /* 抖（笑到抖 / 害怕发抖） */
    SC_ANIM_BOUNCE,    /* 上下弹（兴奋/开心） */
    SC_ANIM_FLOAT,     /* 缓缓上下（睡着/无聊） */
    SC_ANIM_CHEW,      /* 嘴一张一合（唱歌/吃） */
    SC_ANIM_WAVE,      /* 左右轻摆（打招呼） */
    SC_ANIM_TILT,      /* 歪头（好奇/困惑） */
};

/* ------------------------------------------------------------------ 接口 */

/*
 * 创建表情界面。必须在 `lv_init()` + `lv_nuttx_init()` 之后调用。
 * 幂等：重复调用直接返回 0。
 * @return 0 成功；-1 LVGL 未就绪或创建控件失败
 */
int sc_face_init(void);

void sc_face_deinit(void);

/*
 * 闪一个表情。
 *
 * hold_ms = 0 表示"保持到下一次调用"（用于状态表情）；
 * 非 0 表示"这么多毫秒后自动回到心情表情"（用于瞬时情绪表情）。
 *
 * ⚠️ 瞬时表情要短。留久了会把"说话"阶段推迟，破坏对话节奏 ——
 *    孩子看到"它懂了"就够了，不需要一直挂着。
 */
void sc_face_set(int face, uint32_t hold_ms);

/* 设置会话状态（IDLE/LISTENING/THINKING/SPEAKING/OFFLINE）。
 * 这是"第三优先级"：只在不处于瞬时表情时生效。 */
void sc_face_set_state(int state);

/* 收到网关的心情帧（绝对值，-100..100）。 */
void sc_face_set_mood(int v, int a, int e);

/* 底部字幕。传 NULL 或空串则隐藏。
 * 板子屏幕上显示字幕对 2-3 岁的孩子没意义，但对**调试**极有价值：
 * 不看串口也能知道 ASR 听成了什么。 */
void sc_face_set_subtitle(const char *utf8);

/* 顶部角标：与网关的链路状态（0=掉线 1=已连 2=连接中） */
void sc_face_set_link(int state);

/*
 * 顶部角标：WiFi 状态（enum sc_wifi_state）。
 *
 * 为什么值得单独占一格：**"没联网"和"它不理我"是两件完全不同的事**，
 * 但孩子（和家长）看到的表象一样 —— 都是"说了它没反应"。
 * 不把 WiFi 状态摆出来，现场排查就只能靠猜。
 */
void sc_face_set_wifi(int state);

/*
 * 周期性推进（建议 20~60ms 一次）。
 * 由 UI 任务独占调用 —— LVGL 不是线程安全的，所有 LVGL 调用都必须在同一任务里。
 */
void sc_face_tick(uint32_t now_ms);

/* 当前真正在显示的表情编号（诊断用；瞬时表情过期后返回心情表情） */
int sc_face_current(void);

/*
 * ============================================================================
 * SC_FACE_STATIC —— 静态模式开关（0 = 带动画）
 * ============================================================================
 *
 * 定义在**头文件**里而不是 sc_face.c，因为 UI 循环也要用它来决定
 * 自己的唤醒周期。详细动机见 sc_face.c 里那段长注释。
 *
 * 一句话：这块屏的单次写入耗时**不稳定，最坏接近 1 秒**，而每帧重画
 * 就每帧都可能撞上它。所以在没解决底层之前，先让画面**不变**。
 */
#ifndef SC_FACE_STATIC
#  define SC_FACE_STATIC 1
#endif

/*
 * ============================================================================
 * SC_FACE_IMAGE_ONLY —— **只显示一张图**模式
 * ============================================================================
 *
 * 语义（用户要求）：
 *   开机画出一张图，然后**再也不碰屏幕**。
 *   不显示字幕、不显示角标、不换表情 —— 屏幕上永远只有这一张图。
 *
 * 这是"没有卡顿"的最强保证：**画完之后零重绘、零刷屏**，
 * 所以不可能撞上那次接近 1 秒的整屏写入。
 *
 * 图上哪来的（按优先级）：
 *   1. `/data/etc/superchild/face.bin` —— 320×240 的 RGB565 原始数据
 *      （小端，共 153,600 字节）。**有就用它**，
 *      这样换图只要 `adb push` 一个文件，不用重编重刷。
 *   2. 没有那个文件 → 用代码画的那张脸（`g_looks[]` 里的 IDLE 表情）。
 *
 * 对话、语音、WiFi 全部照常工作，只是屏幕不再反映它们。
 */
#ifndef SC_FACE_IMAGE_ONLY
#  define SC_FACE_IMAGE_ONLY 1
#endif

/* 有语音在播放/采集时告诉界面 —— 用于让嘴形跟着真实音频动（口型同步） */
void sc_face_set_audio_level(int captured, int playing);

/*
 * ============================ 跨任务投递接口 ============================
 *
 * **LVGL 不是线程安全的。** 上面那些 set 函数只能在 UI 任务里调用。
 * 网络任务、音频任务想改界面，必须走下面这组 post_* ——
 * 它们只往一把互斥锁保护的环形队列里塞一条记录，UI 任务在自己的
 * tick 里取出来执行。
 *
 * 这条规矩一旦破了，症状是随机花屏或崩溃，而且**极难复现**
 * （取决于两个任务恰好撞在同一帧）。所以宁可多一层队列。
 *
 * 队列满了会**丢最旧的**（界面状态是"最新覆盖旧值"的语义，
 * 丢旧的完全没问题；反过来丢新的就会卡在过时状态上）。
 */

void sc_face_post_state(int state);
void sc_face_post_face(int face, int hold_ms);
void sc_face_post_mood(int v, int a, int e);
void sc_face_post_text(const char *utf8);
void sc_face_post_link(int link);
void sc_face_post_wifi(int state);

/* 音频电平走独立的 volatile 变量而不是队列 —— 它更新最频繁（每秒几十次），
 * 又不需要严格保序，塞队列只会把队列挤爆。 */
void sc_face_post_level(int captured, int playing);

#endif /* __SUPERCHILD_SC_FACE_H */
