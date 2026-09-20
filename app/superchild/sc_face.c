/*
 * 小满的脸 —— 实现。设计动机见 sc_face.h 文件头。
 *
 * 本文件是**唯一碰 LVGL 的地方**，于是线程规则很简单：
 * 只有 UI 任务可以调这里的任何函数。别的任务想改状态必须投递到
 * UI 任务的队列（sc_ui_post_*，见 sc_ui.h）。
 * 这条规矩一旦破了，症状是随机花屏/崩溃，且极难复现。
 *
 * 目标 LVGL：**9.1.0**（实测 NuttX apps/graphics/lvgl 里的版本）。
 * 已核对过、与本文件相关的三处 9.1 特有写法：
 *
 *   1. 缩放是 `lv_obj_set_style_transform_scale_x/y`，
 *      **9.1 里没有 `transform_zoom`**（那是 9.2 才改的名）。
 *   2. 旋转是 `lv_obj_set_style_transform_rotation`，角度单位 0.1 度。
 *   3. 旋转/缩放的对象必须给 `transform_width/height` 留出余量，
 *      否则会被 LVGL 按原尺寸裁掉 —— 表现是"一转就缺个角"。
 */

#include "sc_face.h"
#include "sc_mood.h"
#include "sc_port.h"    /* SC_LOG/SC_ERR —— 少了这行会"隐式声明"，
                         * 编译只报 warning，**要到链接才炸**（undefined reference）。 */
#include "sc_proto.h"

#include <lvgl/lvgl.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>      /* update_badge() 用 snprintf —— 少了这行会"隐式声明"，
                         * 在 ARM32 上恰好能跑，所以一直没暴露。别删。 */
#include <string.h>
#include <unistd.h>

/* ======================================================================
 * 颜色 —— 暖色系、低饱和。幼儿长时间盯着看不能刺眼。
 * ====================================================================== */

#define C_HEAD       lv_color_hex(0xFFF7EC)
#define C_HEAD_SAD   lv_color_hex(0xF2F0F5)
#define C_EYE        lv_color_hex(0x3A3533)
#define C_HILITE     lv_color_hex(0xFFFFFF)
#define C_CHEEK      lv_color_hex(0xFFB3C1)
#define C_MOUTH      lv_color_hex(0x9C3A32)
#define C_BROW       lv_color_hex(0x5A4B45)
#define C_HEART      lv_color_hex(0xFF6B81)
#define C_STAR       lv_color_hex(0xFFC93C)
#define C_TEAR       lv_color_hex(0x7EC8F2)
#define C_NOTE       lv_color_hex(0x8E7CFF)
#define C_FOOD       lv_color_hex(0xD9A55B)
#define C_ZZZ        lv_color_hex(0x9AA7BC)
#define C_TEXT       lv_color_hex(0x6B5B52)
#define C_BADGE_OFF  lv_color_hex(0xD08A8A)
#define C_BADGE_DIM  lv_color_hex(0xB0A79E)

#define TICK_MS      60      /* 动画周期。ILI9341 走 SPI，约 16fps 已够顺，
                              * 再快只会把显示总线占满、反而更卡 */
#define BLINK_TICKS  2

/*
 * ============================================================================
 * 静态模式：关掉一切自动动画，只在**内容真的变化**时重画。
 * ============================================================================
 *
 * 为什么改成静态（2026-09-20）
 * ---------------------------
 * 实测：这个屏幕一次"整屏写入"的耗时**不稳定，最坏接近 1 秒**
 * （`bench:` 8 行 742ms、240 行 998ms，而 128 行只要 50ms —— 这不是
 * 带宽能解释的，是被总线上别的东西卡住）。而只要每帧都在重画，
 * 就每帧都可能撞上这个停顿 → 表现就是"一卡一卡"。
 *
 * 反过来推：**只要画面不变，就没有重绘，也就没有停顿。**
 * 所以先不追动画，把画面做稳。
 *
 * 关了哪些：
 *   · 眨眼          （原来是唯一的"空闲也在动"的东西）
 *   · 抖头/弹跳/歪头（整头位移会牵动一大片重绘）
 *   · 口型开合      （说唱类表情）
 *   · 装饰漂移      （眼泪/音符/纸屑的来回浮动）
 *   · 兴奋时的整脸弹跳
 *
 * 保留的：表情之间的切换、字幕、链路角标 —— 这些是**事件驱动**的，
 * 只在状态变化时重绘一次，正是想要的语义。
 *
 * 开关定义在 sc_face.h（UI 循环也要用），想恢复动画就把它改成 0。
 */

#define N_DECO       7
#define N_ZZZ        3

/* ======================================================================
 * 内部结构
 * ====================================================================== */

struct sc_face_gfx {
    lv_obj_t *root;
    lv_obj_t *head;
    lv_obj_t *eye[2];
    lv_obj_t *hi[2];
    lv_obj_t *mark[2][3];    /* 每只眼的 3 个辅助形状（心/星/弧端/螺旋环） */
    lv_obj_t *brow[2];
    lv_obj_t *cheek[2];
    lv_obj_t *mouth;
    lv_obj_t *corner[2];

    lv_obj_t *deco[N_DECO];
    lv_obj_t *zzz[N_ZZZ];
    lv_obj_t *subtitle;
    lv_obj_t *badge;

    int S;

    int cur_face;
    int state;
    int event_face;          /* -1 = 无瞬时表情 */
    uint32_t event_until;    /* 0 = 保持到下次调用 */

    struct sc_mood mood;
    uint32_t now_ms;
    uint32_t last_activity_ms;
    uint32_t next_blink_ms;
    uint32_t blink_until_ms;
    uint32_t tick_count;
    int inited;

    int audio_cap;
    int audio_play;

    int link_state;     /* 与网关的链路：0=掉线 1=已连 2=连接中 */
    int wifi_state;     /* enum sc_wifi_state */

    int bg_bucket;      /* 当前背景档位（-1 = 还没设过）。见 bg_from_mood() */
};

static struct sc_face_gfx g;

/* ======================================================================
 * 表情表 —— 改视觉**只改这张表**
 * ====================================================================== */

struct sc_look {
    int eye_h;      /* 眼高（短边千分比） */
    int eye_w;
    int eye_dy;     /* 眼整体上下（正 = 往下看） */
    int shape;      /* enum sc_eye_shape */
    int shape_l;    /* 单眼覆盖，-1 = 用 shape */
    int shape_r;
    int brow_l;     /* 眉毛角度（0.1 度）。正 = 内低外高，负 = 内高外低 */
    int brow_r;
    int brow_dy;
    int mouth_w;
    int mouth_h;
    int mouth_dy;
    int corner;     /* 嘴角：>0 上扬，<0 下垂 */
    int cheek;      /* 腮红不透明度 —— 只能用 LVGL 提供的档位，别写别的值 */
    int deco;
    int anim;
    int anim_amp;
};

/* LVGL 只有这几档不透明度 */
#define OPA_0   0
#define OPA_20  LV_OPA_20
#define OPA_30  LV_OPA_30
#define OPA_40  LV_OPA_40
#define OPA_60  LV_OPA_60
#define OPA_70  LV_OPA_70
#define OPA_80  LV_OPA_80
#define OPA_90  LV_OPA_90
#define OPA_100 LV_OPA_100
#define OPA_FF  LV_OPA_COVER

#define OV   SC_EYE_OVAL
#define WD   SC_EYE_WIDE
#define CL   SC_EYE_CLOSED
#define AR   SC_EYE_ARC
#define HT   SC_EYE_HEART
#define ST   SC_EYE_STAR
#define SP   SC_EYE_SPIRAL

#define DN   SC_DECO_NONE
#define DZ   SC_DECO_ZZZ
#define DNO  SC_DECO_NOTE
#define DT   SC_DECO_TEAR
#define DS   SC_DECO_SWEAT
#define DSP  SC_DECO_SPARKLE
#define DH   SC_DECO_HEART
#define DC   SC_DECO_CONFETTI
#define DF   SC_DECO_FOOD

#define AN   SC_ANIM_NONE
#define AS   SC_ANIM_SHAKE
#define AB   SC_ANIM_BOUNCE
#define AF   SC_ANIM_FLOAT
#define AC   SC_ANIM_CHEW
#define AW   SC_ANIM_WAVE
#define AT   SC_ANIM_TILT

/*  表情             eye_h eye_w  dy  shape sL sR  brL   brR brdy  mw   mh  mdy  cor cheek deco  anim  amp */
static const struct sc_look g_looks[SC_FACE_COUNT] = {
/* IDLE      */ {  170, 150,   0, OV, -1, -1,    0,    0,   0, 100,  32,   0,  20, OPA_60, DN,  AN,  0 },
/* LISTENING */ {  205, 160, -10, WD, -1, -1,  -60,  -60, -20, 110,  55,   0,  10, OPA_40, DN,  AN,  0 },
/* THINKING  */ {  160, 150, -20, OV, -1, -1, -200,  100, -30,  95,  30,   0,   0, OPA_80, DN,  AN,  0 },
/* SPEAKING  */ {  170, 150,   0, OV, -1, -1,  -20,  -20, -10, 110,  60,   0,  10, OPA_60, DN,  AC, 55 },
/* HAPPY     */ {   58, 168,   0, AR, -1, -1,  -90,  -90, -10, 185,  95,  10,  60, OPA_100,DN,  AB, 22 },
/* SLEEPY    */ {   70, 150,  10, AR, -1, -1,  -30,  -30,  10,  80,  22,   0,   0, OPA_30, DN,  AF, 14 },
/* CURIOUS   */ {  218, 165, -15, WD, -1, -1, -150, -150, -30,  62,  42,   0,   0, OPA_40, DN,  AT, 55 },
/* SURPRISED */ {  225, 170, -15, WD, -1, -1, -250, -250, -40,  38,  40,   0,   0, OPA_100,DN,  AN,  0 },
/* CONFUSED  */ {  140, 150, -10, OV, -1, -1, -300,  250, -10,  42,  38,   0, -20, OPA_60, DN,  AT, -70 },
/* SHY       */ {   92, 150,   5, OV, -1, -1, -100, -100,  15,  52,  16,   0,  30, OPA_FF, DN,  AN,  0 },
/* PROUD     */ {   85, 150,   0, AR, -1, -1, -120, -120,  -8, 120,  26,   0,  80, OPA_80, DSP, AN,  0 },
/* SAD       */ {  118, 150,  10, OV, -1, -1,  250, -250,  20,  88,  15,  10, -70, OPA_40, DT,  AN,  0 },
/* EXCITED   */ {  230, 175, -15, ST, -1, -1, -200, -200, -35, 150, 120,  10,  50, OPA_90, DSP, AB, 35 },
/* LAUGHING  */ {   48, 170,   0, AR, -1, -1, -120, -120,  -6, 200, 130,  12,  70, OPA_100,DN,  AS, 20 },
/* WINKING   */ {  170, 155,   0, OV, -1, CL, -120, -120,  -8, 140,  60,   0,  60, OPA_80, DN,  AN,  0 },
/* SLEEPING  */ {   12, 150,  10, CL, -1, -1,    0,    0,  20,  60,  26,   0,   0, OPA_40, DZ,  AF, 20 },
/* LOVE      */ {  200, 165, -10, HT, -1, -1, -140, -140, -25, 130,  70,   0,  50, OPA_100,DH,  AB, 25 },
/* SINGING   */ {  130, 150,  -5, AR, -1, -1,  -60,  -60,  -8, 110,  70,   0,  40, OPA_80, DNO, AC, 80 },
/* SCARED    */ {  180, 120,   5, WD, -1, -1,  180,  180,  25,  70,  70,   0, -40, OPA_20, DS,  AS, 28 },
/* WAVING    */ {  140, 150,   0, AR, -1, -1, -100, -100, -10, 150,  70,   0,  70, OPA_70, DN,  AW, 40 },
/* EATING    */ {  110, 150,   0, AR, -1, -1,  -40,  -40,   0,  90,  45,   0,  20, OPA_90, DF,  AC, 70 },
/* BORED     */ {   95, 150,  15, OV, -1, -1,  -20,  -20,  12,  70,  20,   5, -10, OPA_30, DN,  AF, 10 },
/* DIZZY     */ {  175, 155,   0, SP, -1, -1,  100, -100,  10,  80,  60,   0, -30, OPA_70, DN,  AT, -60 },
/* CELEBRATE */ {   55, 168,   0, AR, -1, -1,  -90,  -90,  -8, 190, 110,  10,  70, OPA_100,DC,  AB, 30 },
};

/* 加了表情却没补表 —— 用编译期断言拦住（比运行期崩了好得多） */
typedef char sc_look_count_must_match[(
    sizeof(g_looks) / sizeof(g_looks[0]) == SC_FACE_COUNT) ? 1 : -1];

/* ======================================================================
 * 小工具
 * ====================================================================== */

static void show(lv_obj_t *o, bool visible)
{
    if (o == NULL) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 短边千分比 → 像素，至少 1 */
static int pct(int s, int thousandths)
{
    int v = s * thousandths / 1000;
    return v < 1 ? 1 : v;
}

static void place(lv_obj_t *o, int w, int h, lv_align_t align, int dx, int dy)
{
    if (o == NULL) {
        return;
    }
    lv_obj_set_size(o, w < 1 ? 1 : w, h < 1 ? 1 : h);
    lv_obj_align(o, align, dx, dy);
}

static void set_rot(lv_obj_t *o, int deg10)
{
    if (o != NULL) {
        lv_obj_set_style_transform_rotation(o, deg10, 0);
    }
}

static void set_color(lv_obj_t *o, lv_color_t c)
{
    if (o != NULL) {
        lv_obj_set_style_bg_color(o, c, 0);
    }
}

/* 素净的圆（无边框、无阴影、不滚动） */
static lv_obj_t *mk(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    if (o == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* ======================================================================
 * 外部图片：/data/etc/superchild/face.bin
 * ====================================================================== */

#define SC_FACE_IMG_PATH  "/data/etc/superchild/face.bin"

/*
 * 试着把 /data 里那张图放上屏。
 *
 * 格式：**裸 RGB565**，小端，320×240 = 153,600 字节，无文件头。
 *
 * 为什么用裸数据而不是 PNG：板上没开 PNG 解码，而 LVGL 要额外
 * 链进来会明显加体积；裸数据是零成本的 —— 我这边用一条命令
 * 就能把任意 PNG 转成这个文件。
 *
 * @return 0 成功（已上图）；-1 没有这个文件或读失败（调用方走"代码画脸"）
 */
static int load_image_override(lv_obj_t *parent, int w, int h)
{
    static uint8_t *buf;
    static lv_image_dsc_t dsc;
    size_t need = (size_t)w * (size_t)h * 2u;
    int fd;
    ssize_t n;

    fd = open(SC_FACE_IMG_PATH, O_RDONLY);
    if (fd < 0) {
        SC_LOG("face: 没有 %s，用代码画的脸\n", SC_FACE_IMG_PATH);
        return -1;
    }

    buf = lv_malloc(need);
    if (buf == NULL) {
        SC_ERR("face: 分配 %u 字节图片缓冲失败\n", (unsigned)need);
        close(fd);
        return -1;
    }

    n = read(fd, buf, need);
    close(fd);

    if (n != (ssize_t)need) {
        SC_ERR("face: %s 大小不对 —— 需要 %u 字节，读到 %d。"
               "（要 320x240 裸 RGB565，不是 PNG）\n",
               SC_FACE_IMG_PATH, (unsigned)need, (int)n);
        lv_free(buf);
        buf = NULL;
        return -1;
    }

    memset(&dsc, 0, sizeof(dsc));
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = (uint32_t)w;
    dsc.header.h = (uint32_t)h;
    dsc.header.stride = (uint32_t)(w * 2);
    dsc.data_size = (uint32_t)need;
    dsc.data = buf;

    {
        lv_obj_t *im = lv_image_create(parent);

        if (im == NULL) {
            lv_free(buf);
            buf = NULL;
            return -1;
        }
        lv_image_set_src(im, &dsc);
        lv_obj_align(im, LV_ALIGN_CENTER, 0, 0);
    }

    SC_LOG("face: 已显示 %s（%dx%d RGB565）\n", SC_FACE_IMG_PATH, w, h);
    return 0;
}

/* ======================================================================
 * 初始化
 * ====================================================================== */

int sc_face_init(void)
{
    struct sc_face_gfx *f = &g;
    lv_display_t *disp;
    int W, H, S, i, k;

    if (f->inited) {
        return 0;
    }
    if (!lv_is_initialized()) {
        return -1;
    }
    disp = lv_display_get_default();
    if (disp == NULL || lv_scr_act() == NULL) {
        return -1;
    }

    W = (int)lv_display_get_horizontal_resolution(disp);
    H = (int)lv_display_get_vertical_resolution(disp);
    S = (W < H) ? W : H;
    if (S <= 0) {
        S = 240;
    }
    f->S = S;

    /* ---- 根 ---- */
    f->root = lv_obj_create(lv_scr_act());
    if (f->root == NULL) {
        return -1;
    }
    lv_obj_remove_style_all(f->root);
    lv_obj_set_size(f->root, W, H);
    lv_obj_set_pos(f->root, 0, 0);
    lv_obj_set_style_bg_color(f->root, lv_color_hex(0xFFF0E1), 0);
    lv_obj_set_style_bg_opa(f->root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(f->root, LV_OBJ_FLAG_SCROLLABLE);

#if SC_FACE_IMAGE_ONLY
    /*
     * 只显示一张图。
     *
     * 优先用 /data 里那张（换了图不用重刷固件）；没有就往下走，
     * 用代码画的脸（g_looks[] 里的 IDLE 那一套）。
     *
     * 两种情况都**只画这一次** —— sc_face_tick() 之后直接返回，
     * 不再发任何 LVGL 调用。所以画完后屏幕完全静止。
     */
    if (load_image_override(f->root, W, H) == 0) {
        f->inited = 1;
        f->cur_face = SC_FACE_IDLE;
        f->last_activity_ms = 0;
        f->bg_bucket = -1;      /* 外部图不设背景色，保持 NULL 分支不触发 */
        sc_mood_init(&f->mood); /* 别的任务可能投递心情，初始化掉免得读到脏值 */
        return 0;               /* 不创建任何表情控件 */
    }
    SC_LOG("face: 用代码画的脸（只画一次，之后不动）\n");
#endif

    /* ---- 头 ---- */
    f->head = mk(f->root, C_HEAD);
    if (f->head == NULL) {
        return -1;
    }
    /* 歪头是靠 rotation 做的，必须留 transform 余量，否则会被裁掉一角 */
    lv_obj_set_style_transform_width(f->head, pct(S, 40), 0);
    lv_obj_set_style_transform_height(f->head, pct(S, 40), 0);
    place(f->head, pct(S, 720), pct(S, 720), LV_ALIGN_CENTER, 0, -pct(S, 20));

    /* ---- 眼（每只：主眼 + 高光 + 3 个辅助形状）---- */
    for (i = 0; i < 2; i++) {
        f->eye[i] = mk(f->head, C_EYE);
        f->hi[i] = mk(f->eye[i], C_HILITE);
        for (k = 0; k < 3; k++) {
            f->mark[i][k] = mk(f->head, C_EYE);
            show(f->mark[i][k], false);
        }
    }

    /* ---- 眉 ---- */
    for (i = 0; i < 2; i++) {
        f->brow[i] = mk(f->head, C_BROW);
        lv_obj_set_style_transform_width(f->brow[i], pct(S, 30), 0);
        lv_obj_set_style_transform_height(f->brow[i], pct(S, 30), 0);
    }

    /* ---- 腮红 ---- */
    for (i = 0; i < 2; i++) {
        f->cheek[i] = mk(f->head, C_CHEEK);
    }

    /* ---- 嘴 + 嘴角 ---- */
    f->mouth = mk(f->head, C_MOUTH);
    if (f->mouth == NULL) {
        return -1;
    }
    for (i = 0; i < 2; i++) {
        f->corner[i] = mk(f->head, C_MOUTH);
    }

    /* ---- 装饰池 ---- */
    for (i = 0; i < N_DECO; i++) {
        f->deco[i] = mk(f->root, C_STAR);
        show(f->deco[i], false);
    }
    for (i = 0; i < N_ZZZ; i++) {
        f->zzz[i] = lv_label_create(f->root);
        if (f->zzz[i] != NULL) {
            lv_label_set_text(f->zzz[i], "Z");
            lv_obj_set_style_text_color(f->zzz[i], C_ZZZ, 0);
            /* 没有更大号字体可用，用 scale 放大。LVGL 9.1 是 256 = 1x */
            lv_obj_set_style_transform_scale_x(f->zzz[i], 256 + i * 100, 0);
            lv_obj_set_style_transform_scale_y(f->zzz[i], 256 + i * 100, 0);
            lv_obj_set_style_transform_width(f->zzz[i], pct(S, 40), 0);
            lv_obj_set_style_transform_height(f->zzz[i], pct(S, 40), 0);
            show(f->zzz[i], false);
        }
    }

    /* ---- 字幕 ---- */
    f->subtitle = lv_label_create(f->root);
    if (f->subtitle != NULL) {
        lv_label_set_text(f->subtitle, "");
        lv_obj_set_style_text_color(f->subtitle, C_TEXT, 0);
        lv_obj_set_style_text_align(f->subtitle, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(f->subtitle, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(f->subtitle, W - pct(S, 80));
        lv_obj_align(f->subtitle, LV_ALIGN_BOTTOM_MID, 0, -pct(S, 20));
        show(f->subtitle, false);
    }

    /* ---- 链路角标 ---- */
    f->badge = lv_label_create(f->root);
    if (f->badge != NULL) {
        lv_label_set_text(f->badge, "");
        lv_obj_set_style_text_color(f->badge, C_BADGE_DIM, 0);
        lv_obj_align(f->badge, LV_ALIGN_TOP_LEFT, pct(S, 22), pct(S, 14));
    }

    /* ---- 状态 ---- */
    f->cur_face = SC_FACE_IDLE;
    f->state = SC_STATE_CONNECTING;
    f->event_face = -1;
    f->event_until = 0;
    f->now_ms = 0;
    f->last_activity_ms = 0;
    f->next_blink_ms = 2200;
    f->blink_until_ms = 0;
    f->tick_count = 0;
    f->link_state = 2;      /* 连接中 */
    f->wifi_state = 0;      /* 未知，等 WiFi 守卫来报 */
    f->bg_bucket = -1;      /* 强制第一帧真的设一次背景色 */
    sc_mood_init(&f->mood);
    f->inited = 1;

    /* 立刻画一帧，避免上电后先闪一下空白屏 */
    sc_face_tick(0);
    return 0;
}

void sc_face_deinit(void)
{
    struct sc_face_gfx *f = &g;
    if (!f->inited) {
        return;
    }
    if (f->root != NULL) {
        lv_obj_delete(f->root);
    }
    memset(f, 0, sizeof(*f));
}

/* ======================================================================
 * 画一只眼
 * ====================================================================== */

static void paint_eye(struct sc_face_gfx *f, int side, int shape,
                      int ew, int eh, int dy)
{
    int S = f->S;
    int x = ((side == 0) ? -1 : 1) * pct(S, 120) / 2;
    int k;
    lv_obj_t *eye = f->eye[side];

    for (k = 0; k < 3; k++) {
        show(f->mark[side][k], false);
    }

    if (shape == SC_EYE_CLOSED) {
        /* 闭眼 = 一条细横线。**绝不能用 0 高度** ——
         * LVGL 会画不出来，而且某些版本会在 0 尺寸上报错。 */
        show(eye, true);
        show(f->hi[side], false);
        set_color(eye, C_EYE);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
        set_rot(eye, 0);
        place(eye, ew * 3 / 4, 4, LV_ALIGN_CENTER, x, dy);
        return;
    }

    if (shape == SC_EYE_ARC) {
        /* 眯眼笑：横条 + 两端各一个上翘小圆 = ^-^ 的弧。
         * 只画横条的话和"闭眼"分不开；加两个小圆成本极低但辨识度高很多。 */
        int bar = eh < 4 ? 4 : eh;
        show(eye, true);
        show(f->hi[side], false);
        set_color(eye, C_EYE);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
        set_rot(eye, 0);
        place(eye, ew, bar, LV_ALIGN_CENTER, x, dy);
        for (k = 0; k < 2; k++) {
            lv_obj_t *m = f->mark[side][k];
            show(m, true);
            set_color(m, C_EYE);
            lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(m, 0, 0);
            lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
            set_rot(m, 0);
            place(m, bar * 2, bar * 2, LV_ALIGN_CENTER,
                  x + (k == 0 ? -ew / 2 : ew / 2), dy - bar / 2);
        }
        return;
    }

    if (shape == SC_EYE_HEART) {
        /* 心 = 两个圆 + 一个 45° 方块。
         * 为什么不用 "♥" 字符：默认字体里没有这个字形，会画成空心方块。 */
        int r = (ew * 55 / 100) > 6 ? (ew * 55 / 100) : 6;
        show(eye, false);
        show(f->hi[side], false);
        for (k = 0; k < 3; k++) {
            lv_obj_t *m = f->mark[side][k];
            show(m, true);
            set_color(m, C_HEART);
            lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(m, 0, 0);
            if (k < 2) {
                lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
                set_rot(m, 0);
                place(m, r, r, LV_ALIGN_CENTER,
                      x + (k == 0 ? -r / 2 : r / 2), dy - r / 4);
            } else {
                lv_obj_set_style_radius(m, 2, 0);
                place(m, r * 5 / 4, r * 5 / 4, LV_ALIGN_CENTER, x, dy + r / 4);
                set_rot(m, 450);
            }
        }
        return;
    }

    if (shape == SC_EYE_STAR || shape == SC_EYE_SPIRAL) {
        lv_color_t c = (shape == SC_EYE_STAR) ? C_STAR : C_EYE;
        show(eye, true);
        show(f->hi[side], false);
        set_color(eye, c);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
        set_rot(eye, 0);
        place(eye, eh * 55 / 100, eh * 55 / 100, LV_ALIGN_CENTER, x, dy);

        for (k = 0; k < 2; k++) {
            lv_obj_t *m = f->mark[side][k];
            show(m, true);
            set_rot(m, 0);
            if (shape == SC_EYE_STAR) {
                /* 十字星光：一横一竖两根细条 */
                set_color(m, c);
                lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(m, 0, 0);
                lv_obj_set_style_radius(m, 2, 0);
                place(m, (k == 0) ? ew : 4, (k == 0) ? 4 : eh,
                      LV_ALIGN_CENTER, x, dy);
            } else {
                /* 螺旋：两个同心描边环 */
                lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_color(m, c, 0);
                lv_obj_set_style_border_width(m, 2, 0);
                lv_obj_set_style_border_opa(m, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
                place(m, eh / 2 - k * (eh / 5), eh / 2 - k * (eh / 5),
                      LV_ALIGN_CENTER, x, dy);
            }
        }
        return;
    }

    /* OVAL / WIDE */
    show(eye, true);
    set_color(eye, C_EYE);
    lv_obj_set_style_border_width(eye, 0, 0);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    set_rot(eye, 0);
    place(eye, ew, eh, LV_ALIGN_CENTER, x, dy);

    if (eh >= pct(S, 40)) {
        show(f->hi[side], true);
        place(f->hi[side], pct(S, 46), pct(S, 46), LV_ALIGN_TOP_LEFT,
              ew / 4, ew / 4);
    } else {
        show(f->hi[side], false);
    }
}

/* ======================================================================
 * 背景色随心情
 * ====================================================================== */

/*
 * 背景色随心情 —— **性能关键区，改动前先读这段注释**
 * ================================================
 *
 * root 铺满整屏，改它的 bg_color 会让 LVGL **整屏重绘**：
 * 320×240×2 = 150KB 全都要重新画并经 SPI 推给屏。
 * 而帧预算只有 TICK_MS = 60ms —— 一次整屏重绘就能吃掉大半。
 *
 * 最初的写法是按 cur_v **连续**取值（cr 有 19 档、cb 有 22 档）：
 * 心情每漂移一点点颜色就变一次，于是整屏重绘极其频繁。
 * 现场表现就是**表情一卡一卡的** —— 而且因为"偶尔才卡一下"，
 * 很容易被误判成"CPU 不够"或"屏不行"，往错的方向查很久。
 *
 * 现在改成：
 *   1. **量化到 5 档**（下面是手调的 5 个颜色）——
 *      心情是慢变量，档位几分钟才变一次，观感几乎无差别；
 *   2. **只在档位真的变化时才调用 LVGL**。
 *
 * 这样整屏重绘从"频繁"降到"几分钟一次"。
 *
 * ⚠️ 变量名不能叫 g —— 文件里有个全局 `static struct sc_face_gfx g;`，
 *    同名会被 -Wshadow 警告，而且一旦有人在这里误用 g 就是灾难。
 */
static void bg_from_mood(struct sc_face_gfx *f)
{
    /* 由冷到暖的 5 档（低落 → 愉快） */
    static const uint8_t tab[5][3] = {
        { 0xED, 0xF0, 0xF6 },
        { 0xF2, 0xF0, 0xF1 },
        { 0xF7, 0xF0, 0xEC },
        { 0xFB, 0xF0, 0xE6 },
        { 0xFF, 0xF0, 0xE1 },
    };
    int b;

    /* cur_v ∈ [-100, 100] → 档位 0..4 */
    b = (f->mood.cur_v + 100) * 4 / 200;
    if (b < 0) {
        b = 0;
    } else if (b > 4) {
        b = 4;
    }

    if (b == f->bg_bucket) {
        return;                 /* 档位没变：一个 LVGL 调用都不发 */
    }
    f->bg_bucket = b;

    lv_obj_set_style_bg_color(f->root,
        lv_color_make(tab[b][0], tab[b][1], tab[b][2]), 0);
}

/* ======================================================================
 * 装饰池
 * ====================================================================== */

static void paint_deco(struct sc_face_gfx *f, int deco, int amp)
{
    int S = f->S;
    int i;
    int tri;    /* 0..20 的三角波，用来做"缓慢来回" */
    int drift;

#if SC_FACE_STATIC
    /*
     * 静态模式：固定在三角波的**中点**（tri=10）。
     * 这样所有装饰（眼泪、音符、纸屑）都停在"行程中央"这个自然位置，
     * 而不是停在起点那种明显偏一边的地方。
     */
    (void)amp;
    tri = 10;
    drift = 0;
    (void)tri;      /* 静态下 tri 只用来算 drift（=0），不用它会报 unused */
#else
    {
        int ph = (int)(f->tick_count % 40);
        tri = (ph < 20) ? ph : (40 - ph);
        drift = (tri - 10) * amp / 10;
    }
#endif

    for (i = 0; i < N_DECO; i++) {
        show(f->deco[i], false);
    }
    for (i = 0; i < N_ZZZ; i++) {
        show(f->zzz[i], false);
    }
    if (deco == SC_DECO_NONE) {
        return;
    }

    if (deco == SC_DECO_ZZZ) {
        for (i = 0; i < N_ZZZ; i++) {
            if (f->zzz[i] == NULL) {
                continue;
            }
            show(f->zzz[i], true);
            lv_obj_align(f->zzz[i], LV_ALIGN_CENTER,
                         pct(S, 220) + i * pct(S, 62),
                         -pct(S, 175) - i * pct(S, 58) - drift);
        }
        return;
    }

    for (i = 0; i < N_DECO; i++) {
        lv_obj_t *d = f->deco[i];
        int x = 0, y = 0, w = pct(S, 30), h = pct(S, 30);
        lv_color_t c = C_STAR;
        int rot = 0;
        int ring = 0;
        int visible = 1;

        if (d == NULL) {
            continue;
        }

        switch (deco) {
        case SC_DECO_SPARKLE:
            /* 眼睛周围四处小星光 —— 只用来"提亮"，不抢主体 */
            if (i >= 4) {
                visible = 0;
                break;
            }
            x = ((i < 2) ? -1 : 1) * pct(S, 185);
            y = ((i % 2) ? 1 : -1) * pct(S, 105) - pct(S, 55);
            w = h = pct(S, 26);
            break;

        case SC_DECO_TEAR:
            if (i >= 2) {
                visible = 0;
                break;
            }
            x = ((i == 0) ? -1 : 1) * pct(S, 62);
            y = pct(S, 95) + drift / 2;
            w = pct(S, 32);
            h = pct(S, 50);
            c = C_TEAR;
            break;

        case SC_DECO_SWEAT:
            if (i >= 1) {
                visible = 0;
                break;
            }
            x = pct(S, 300);
            y = -pct(S, 55) + drift / 2;
            w = pct(S, 38);
            h = pct(S, 56);
            c = C_TEAR;
            break;

        case SC_DECO_NOTE:
            /* 音符：三个由小到大的圆点往上飘（不画符杆，成本不值） */
            if (i >= 3) {
                visible = 0;
                break;
            }
            x = pct(S, 145) + i * pct(S, 66);
            y = -pct(S, 115) - i * pct(S, 66) + drift / 2;
            w = h = pct(S, 44) - i * pct(S, 6);
            c = C_NOTE;
            break;

        case SC_DECO_HEART:
            if (i >= 1) {
                visible = 0;
                break;
            }
            x = pct(S, 135);
            y = -pct(S, 135) + drift / 2;
            w = h = pct(S, 54);
            c = C_HEART;
            break;

        case SC_DECO_CONFETTI:
            /* 7 片彩色纸屑斜着铺开，随时间缓缓下落 */
            x = ((i % 4) - 2) * pct(S, 115) + ((i % 3) - 1) * pct(S, 25);
            y = -pct(S, 170) + (int)((f->tick_count * 3 + i * 8) % 90)
                * pct(S, 3) / 10;
            w = pct(S, 30);
            h = pct(S, 16);
            rot = 300 + i * 190;
            c = lv_color_make((uint8_t)(120 + i * 18),
                              (uint8_t)(100 + (6 - i) * 20),
                              (uint8_t)(200 - i * 15));
            break;

        case SC_DECO_FOOD:
            if (i >= 1) {
                visible = 0;
                break;
            }
            x = pct(S, 245);
            y = pct(S, 55) + drift / 2;
            w = h = pct(S, 54);
            c = C_FOOD;
            ring = 1;
            break;

        default:
            visible = 0;
            break;
        }

        if (!visible) {
            continue;
        }

        show(d, true);
        set_color(d, c);
        if (ring) {
            lv_obj_set_style_bg_opa(d, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(d, c, 0);
            lv_obj_set_style_border_width(d, 3, 0);
            lv_obj_set_style_border_opa(d, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(d, 0, 0);
        }
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        place(d, w, h, LV_ALIGN_CENTER, x, y);
        set_rot(d, rot);
    }
}

/* ======================================================================
 * 把一张 look 落到控件上
 * ====================================================================== */

static void apply_look(struct sc_face_gfx *f, int face)
{
    const struct sc_look *L;
    int S = f->S;
    int ew, eh, dy = 0, tilt = 0, blink = 0, i;
    int tri;

    if (face < 0 || face >= SC_FACE_COUNT) {
        face = SC_FACE_IDLE;
    }
    L = &g_looks[face];

    {
        int ph = (int)(f->tick_count % 40);
        tri = (ph < 20) ? ph : (40 - ph);
    }

#if SC_FACE_STATIC
        /* 静态模式：不眨眼、不位移、不歪头。见 sc_face.h 的 SC_FACE_STATIC。 */
        (void)tri;
        blink = 0;
        dy = 0;
        tilt = 0;
#else
    /* ---- 眨眼 --------------------------------------------------------
     * 只对"普通睁眼"的脸生效。眯眼笑/闭眼类的脸上再眨眼没有意义，
     * 而且会把"闭眼"闪成睁眼，看着像坏了。
     */
    if (face == SC_FACE_IDLE || face == SC_FACE_HAPPY ||
        face == SC_FACE_PROUD || face == SC_FACE_BORED ||
        face == SC_FACE_EATING || face == SC_FACE_WAVING) {
        if ((int32_t)(f->now_ms - f->next_blink_ms) >= 0) {
            f->blink_until_ms = f->now_ms + BLINK_TICKS * TICK_MS;
            f->next_blink_ms = f->blink_until_ms
                             + sc_mood_blink_period_ms(&f->mood);
        }
        if (f->now_ms < f->blink_until_ms) {
            blink = 1;
        }
    }

    /* ---- 动画偏移 ---- */
    {
        int amp = L->anim_amp;
        switch (L->anim) {
        case SC_ANIM_SHAKE:
            dy = ((f->tick_count & 1) ? 1 : -1) * pct(S, amp) / 2;
            break;
        case SC_ANIM_BOUNCE:
            dy = -pct(S, amp) * tri / 20;
            break;
        case SC_ANIM_FLOAT:
            dy = pct(S, amp) * (tri - 10) / 20;
            break;
        case SC_ANIM_TILT:
            tilt = amp * tri / 20;
            break;
        case SC_ANIM_WAVE:
            tilt = amp * (tri - 10) / 12;
            break;
        case SC_ANIM_CHEW:
        case SC_ANIM_NONE:
        default:
            break;
        }
    }
    /* 极度兴奋时整张脸轻轻弹一下 —— 观感上"活"很多 */
    if ((face == SC_FACE_EXCITED || face == SC_FACE_LAUGHING) &&
        (f->tick_count % 3) == 0) {
        dy -= pct(S, 10);
    }
#endif  /* SC_FACE_STATIC */

    /* ---- 头 ---- */
    place(f->head, pct(S, 720), pct(S, 720), LV_ALIGN_CENTER, 0,
          -pct(S, 20) + dy);
    set_rot(f->head, tilt);
    set_color(f->head, (f->mood.cur_v < -30) ? C_HEAD_SAD : C_HEAD);

    /* ---- 眼 ---- */
    ew = pct(S, L->eye_w);
    eh = pct(S, L->eye_h);
    {
        int sl = (L->shape_l >= 0) ? L->shape_l : L->shape;
        int sr = (L->shape_r >= 0) ? L->shape_r : L->shape;
        if (blink) {
            sl = sr = SC_EYE_CLOSED;
        }
        paint_eye(f, 0, sl, ew, eh, pct(S, L->eye_dy));
        paint_eye(f, 1, sr, ew, eh, pct(S, L->eye_dy));
    }

    /* ---- 眉 ---- */
    for (i = 0; i < 2; i++) {
        int ang = (i == 0) ? L->brow_l : L->brow_r;
        show(f->brow[i], ang != 0);
        if (ang == 0) {
            continue;
        }
        lv_obj_set_style_radius(f->brow[i], pct(S, 11), 0);
        place(f->brow[i], pct(S, 108), pct(S, 22), LV_ALIGN_CENTER,
              ((i == 0) ? -1 : 1) * pct(S, 118),
              pct(S, L->eye_dy) - pct(S, 138) + pct(S, L->brow_dy));
        set_rot(f->brow[i], ang);
    }

    /* ---- 腮红 ---- */
    for (i = 0; i < 2; i++) {
        lv_obj_set_style_opa(f->cheek[i], L->cheek, 0);
        place(f->cheek[i], pct(S, 130), pct(S, 74), LV_ALIGN_CENTER,
              ((i == 0) ? -1 : 1) * pct(S, 330), pct(S, 70) + dy);
    }

    /* ---- 嘴 ----
     * SPEAKING / SINGING / EATING 的嘴跟着**真实播放电平**动。
     * 这一步是口型同步，对"它真的在跟我说话"的可信度提升非常大；
     * 没有电平数据时退回固定节奏，不至于僵住。
     */
    {
        int mw = pct(S, L->mouth_w);
        int mh = pct(S, L->mouth_h);
        int mo = pct(S, L->mouth_dy);
        int my = pct(S, 150) + mo + dy;

#if SC_FACE_STATIC
        /*
         * 静态模式：嘴固定成 look 表里那个形状，**不跟播放电平动**。
         *
         * 这一条是必须关的：原来的口型跟着 `f->audio_play` 每帧变，
         * 于是**每一帧都产生重绘** —— 正好是这次要消掉的东西。
         * 代价是说话时嘴不动；换来的是画面从"每秒都可能卡一下"
         * 变成完全稳定。
         */
        (void)tri;
#else
        if (L->anim == SC_ANIM_CHEW) {
            int lv;
            if (f->audio_play > 0) {
                lv = f->audio_play;
            } else {
                lv = tri * 5;                 /* 0..100 的假电平 */
            }
            if (lv > 100) {
                lv = 100;
            }
            mh = mh * (30 + lv * 70 / 100) / 100;
            mw = mw * (70 + lv * 30 / 100) / 100;
            if (mh < 4) {
                mh = 4;
            }
        }
#endif

        lv_obj_set_style_radius(f->mouth, pct(S, 14), 0);
        place(f->mouth, mw, mh, LV_ALIGN_CENTER, 0, my);

        /* 嘴角：两小块。上扬 = 笑，下垂 = 撇嘴。 */
        for (i = 0; i < 2; i++) {
            show(f->corner[i], L->corner != 0);
            if (L->corner == 0) {
                continue;
            }
            lv_obj_set_style_radius(f->corner[i], LV_RADIUS_CIRCLE, 0);
            place(f->corner[i], pct(S, 32), pct(S, 32), LV_ALIGN_CENTER,
                  ((i == 0) ? -1 : 1) * (mw / 2 + pct(S, 6)),
                  my - mh / 2 - pct(S, L->corner) / 10);
        }
    }

    /* ---- 装饰 + 背景 ---- */
    paint_deco(f, L->deco, L->anim_amp);
    bg_from_mood(f);
}

/* ======================================================================
 * 优先级：瞬时表情 > 会话状态 > 心情
 * ====================================================================== */

static int state_face(int state)
{
    switch (state) {
    case SC_STATE_LISTENING: return SC_FACE_LISTENING;
    case SC_STATE_THINKING:  return SC_FACE_THINKING;
    case SC_STATE_SPEAKING:  return SC_FACE_SPEAKING;
    case SC_STATE_OFFLINE:   return SC_FACE_CONFUSED;  /* 掉线得看得出来 */
    default:                 return -1;                /* 交给心情 */
    }
}

/*
 * 前向声明。`drain_posts` 定义在文件末尾（和投递接口放在一起更易读），
 * 但它被上面的 sc_face_tick() 调用 —— 少了这行，C 会先按隐式的
 * `int drain_posts()`（非 static）声明，再遇到 static 定义时报
 * "static declaration follows non-static declaration"。
 * 这个报错在文件末尾，跟出错点隔了 300 行，第一次找会绕。
 */
static void drain_posts(void);

static int decide_face(struct sc_face_gfx *f)
{
    int sf;

    if (f->event_face >= 0) {
        if (f->event_until == 0 ||
            (int32_t)(f->now_ms - f->event_until) < 0) {
            return f->event_face;
        }
        f->event_face = -1;
    }

    sf = state_face(f->state);
    if (sf >= 0) {
        return sf;
    }
    return sc_mood_idle_face(&f->mood,
                             (f->now_ms - f->last_activity_ms) / 1000);
}

void sc_face_tick(uint32_t now_ms)
{
    struct sc_face_gfx *f = &g;
    int face;

    if (!f->inited) {
        return;
    }
    f->now_ms = now_ms;
    f->tick_count++;

#if SC_FACE_IMAGE_ONLY
    /*
     * 只显示一张图：开机那次（tick_count==1，由 sc_face_init 调用）
     * 画完之后，**再也不碰屏幕**。
     *
     * 注意这里连 drain_posts() / 心情都不跑 —— 一个 LVGL 调用都不发，
     * 就绝对不会有脏区、不会有刷屏、也就绝不撞上那次接近 1 秒的停顿。
     * 队列积满会自动丢最旧的（见 pq_push），不需要我们消费。
     */
    if (f->tick_count > 1) {
        return;
    }
#endif

    /* 先把别的任务投递过来的指令吃掉，再算这一帧该显示什么 */
    drain_posts();

    sc_mood_tick(&f->mood, now_ms);

    face = decide_face(f);

    /*
     * ★★★ 只有"该显示的脸"真的变了，才去重画。★★★
     *
     * 这一行是"卡顿"的真正修法。之前我只关掉了**动画**（blink/位移/口型），
     * 但 apply_look() 仍然每 60ms 被调用一次，把 20 多个控件的位置、
     * 颜色、旋转重新设一遍。只要 LVGL 对其中任何一个不提前返回，
     * **整张脸（约 210 行）就被判为脏** → 又触发一次接近 1 秒的整屏写入。
     *
     * 实测证据（2026-09-20）：帧耗时在 91ms / 905ms 之间**交替**
     * （`掉帧 #1：91ms  #2：904ms  #3：92ms  #4：905ms …`）——
     * 这个交替规律本身就说明"每帧都在重画"，而不是"偶尔撞上慢写入"。
     *
     * 改成事件驱动之后，空闲帧**一个 LVGL 调用都不发**，
     * 于是没有脏区、没有刷屏、也就没有那次停顿。
     *
     * ⚠️ 别改回"无条件 apply_look"。要加动画的话，动画必须自己
     *    declare 需要重画（比如让某个控件动），而不是靠每帧重设全部属性。
     */
    if (face != f->cur_face) {
        f->cur_face = face;
        apply_look(f, face);
    }
}

/* ======================================================================
 * 对外接口
 * ====================================================================== */

void sc_face_set(int face, uint32_t hold_ms)
{
    struct sc_face_gfx *f = &g;
    if (!f->inited || face < 0 || face >= SC_FACE_COUNT) {
        return;
    }
    f->event_face = face;
    f->event_until = (hold_ms != 0) ? (f->now_ms + hold_ms) : 0;
    f->last_activity_ms = f->now_ms;
}

void sc_face_set_state(int state)
{
    struct sc_face_gfx *f = &g;
    if (!f->inited) {
        return;
    }
    f->state = state;
    if (state != SC_STATE_OFFLINE) {
        f->last_activity_ms = f->now_ms;
    }
}

void sc_face_set_mood(int v, int a, int e)
{
    if (g.inited) {
        sc_mood_set(&g.mood, v, a, e);
    }
}

void sc_face_set_subtitle(const char *utf8)
{
    struct sc_face_gfx *f = &g;
    if (!f->inited || f->subtitle == NULL) {
        return;
    }
    if (utf8 == NULL || utf8[0] == '\0') {
        show(f->subtitle, false);
        return;
    }
    lv_label_set_text(f->subtitle, utf8);
    show(f->subtitle, true);
}

/* 角标文案 = WiFi 状态 + 网关链路状态。
 *
 * 两者合成一格而不是两格：屏幕只有 320×240，角标区域就那么点地方。
 * 优先级上 **WiFi 更重要** —— WiFi 没连上时，网关那一格的"连接中"
 * 是必然结果、没有信息量；直接说"没 WiFi"才能指对排查方向。 */
static void update_badge(void)
{
    struct sc_face_gfx *f = &g;
    char buf[48];

    if (!f->inited || f->badge == NULL) {
        return;
    }

    if (f->wifi_state != 0 && f->wifi_state != 1) {
        /* SC_WIFI_NO_IP(2) / SC_WIFI_NO_DEV(3) */
        const char *what = (f->wifi_state == 3) ? "没有 wlan0" : "WiFi 未连";
        snprintf(buf, sizeof(buf), "! %s", what);
        lv_label_set_text(f->badge, buf);
        lv_obj_set_style_text_color(f->badge, C_BADGE_OFF, 0);
        return;
    }

    if (f->link_state == 0) {
        lv_label_set_text(f->badge, "x 掉线");
        lv_obj_set_style_text_color(f->badge, C_BADGE_OFF, 0);
    } else if (f->link_state == 1) {
        lv_label_set_text(f->badge, "");
    } else {
        lv_label_set_text(f->badge, "o 连接中");
        lv_obj_set_style_text_color(f->badge, C_BADGE_DIM, 0);
    }
}

void sc_face_set_link(int link)
{
    g.link_state = link;
    update_badge();
}

void sc_face_set_wifi(int state)
{
    g.wifi_state = state;
    update_badge();
}

void sc_face_set_audio_level(int captured, int playing)
{
    struct sc_face_gfx *f = &g;
    if (!f->inited) {
        return;
    }
    f->audio_cap = (captured < 0) ? 0 : (captured > 100 ? 100 : captured);
    f->audio_play = (playing < 0) ? 0 : (playing > 100 ? 100 : playing);
}

int sc_face_current(void)
{
    return g.inited ? g.cur_face : -1;
}

/* ======================================================================
 * 跨任务投递队列
 *
 * 为什么不用信号量唤醒 UI 任务：UI 任务的节奏本来就是 60ms 一帧，
 * 队列里的东西最多晚 60ms 生效 —— 人眼分辨不出来，
 * 却省掉了一整套"唤醒/超时/竞态"的复杂度。
 * ====================================================================== */

#define SC_POSTQ_LEN   24
#define SC_POST_TEXT   128

#define SC_POST_STATE  1
#define SC_POST_FACE   2
#define SC_POST_MOOD   3
#define SC_POST_TEXT_T 4
#define SC_POST_LINK   5
#define SC_POST_WIFI   6

struct sc_post {
    uint8_t type;
    int a, b, c;
    char text[SC_POST_TEXT];
};

static struct sc_post g_pq[SC_POSTQ_LEN];
static volatile int g_pq_head;      /* 下一个写入位（生产者） */
static volatile int g_pq_tail;      /* 下一个读出位（消费者） */
static pthread_mutex_t g_pq_lock = PTHREAD_MUTEX_INITIALIZER;

/* 音频电平：不进队列，直接覆盖 */
static volatile int g_lv_cap;
static volatile int g_lv_play;

static void pq_push(const struct sc_post *p)
{
    int next;

    pthread_mutex_lock(&g_pq_lock);
    next = (g_pq_head + 1) % SC_POSTQ_LEN;
    if (next == g_pq_tail) {
        /* 满：丢最旧的（界面状态是"最新覆盖旧值"的语义） */
        g_pq_tail = (g_pq_tail + 1) % SC_POSTQ_LEN;
    }
    g_pq[g_pq_head] = *p;
    g_pq_head = next;
    pthread_mutex_unlock(&g_pq_lock);
}

static int pq_pop(struct sc_post *out)
{
    int ok = 0;

    pthread_mutex_lock(&g_pq_lock);
    if (g_pq_tail != g_pq_head) {
        *out = g_pq[g_pq_tail];
        g_pq_tail = (g_pq_tail + 1) % SC_POSTQ_LEN;
        ok = 1;
    }
    pthread_mutex_unlock(&g_pq_lock);
    return ok;
}

static void post_simple(int type, int a, int b, int c, const char *text)
{
    struct sc_post p;
    memset(&p, 0, sizeof(p));
    p.type = (uint8_t)type;
    p.a = a;
    p.b = b;
    p.c = c;
    if (text != NULL) {
        strncpy(p.text, text, SC_POST_TEXT - 1);
        p.text[SC_POST_TEXT - 1] = '\0';
    }
    pq_push(&p);
}

void sc_face_post_state(int state)      { post_simple(SC_POST_STATE, state, 0, 0, NULL); }
void sc_face_post_face(int face, int hold_ms)
                                        { post_simple(SC_POST_FACE, face, hold_ms, 0, NULL); }
void sc_face_post_mood(int v, int a, int e)
                                        { post_simple(SC_POST_MOOD, v, a, e, NULL); }
void sc_face_post_text(const char *t)   { post_simple(SC_POST_TEXT_T, 0, 0, 0, t); }
void sc_face_post_link(int link)        { post_simple(SC_POST_LINK, link, 0, 0, NULL); }
void sc_face_post_wifi(int state)       { post_simple(SC_POST_WIFI, state, 0, 0, NULL); }

void sc_face_post_level(int captured, int playing)
{
    /* 直接赋值即可 —— 单个 int 在本平台上不会撕裂，且这里只用于观感 */
    g_lv_cap = (captured < 0) ? 0 : (captured > 100 ? 100 : captured);
    g_lv_play = (playing < 0) ? 0 : (playing > 100 ? 100 : playing);
}

/* UI 任务调用：把队列里的指令应用掉。每帧最多处理 8 条，
 * 避免一次积压很多时把一帧拖长导致掉帧。 */
static void drain_posts(void)
{
    struct sc_post p;
    int n = 0;

    while (n++ < 8 && pq_pop(&p)) {
        switch (p.type) {
        case SC_POST_STATE:
            sc_face_set_state(p.a);
            break;
        case SC_POST_FACE:
            sc_face_set(p.a, (uint32_t)p.b);
            break;
        case SC_POST_MOOD:
            sc_face_set_mood(p.a, p.b, p.c);
            break;
        case SC_POST_TEXT_T:
            sc_face_set_subtitle(p.text);
            break;
        case SC_POST_LINK:
            sc_face_set_link(p.a);
            break;
        case SC_POST_WIFI:
            sc_face_set_wifi(p.a);
            break;
        default:
            break;
        }
    }

    sc_face_set_audio_level(g_lv_cap, g_lv_play);
}
