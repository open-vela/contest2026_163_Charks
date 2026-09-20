/*
 * 情绪引擎。规则表逐条对齐 `emotion.py` 的 `_EMO_RULES`。
 * 设计说明见 sc_emo.h。
 *
 * ⚠️ 顺序即优先级，改表时**不要重排**，只往对应组里追加。
 */

#include "sc_emo.h"
#include "sc_port.h"
#include "sc_proto.h"

#include <string.h>

typedef struct {
    const char *label;
    const char *const *keys;
    int         nkeys;
    int         face;
    int         hold_ms;
    int         dv, da, de;
} sc_emo_rule_t;

/* 关键词数组。写成 static 数组是为了让规则表保持"表"的形状，
 * 而不是散落一堆 if。 */
static const char *K_EXCITED[]   = { "哇塞", "太开心", "好开心", "我赢了",
                                     "成功啦", "我会了", "我会啦" };
static const char *K_PRAISE[]    = { "你真棒", "好厉害", "真聪明", "太厉害了",
                                     "真乖", "做得好", "好棒", "你会自己" };
static const char *K_SHOWOFF[]   = { "给你看", "你看我", "快看", "我搭",
                                     "我画的", "我做了一", "我自己做" };
static const char *K_LOVE[]      = { "喜欢你", "爱你", "抱抱", "亲亲",
                                     "我想你", "最好的", "你最好" };
static const char *K_LAUGH[]     = { "哈哈", "嘻嘻", "呵呵", "好玩",
                                     "太好玩", "好笑", "咯咯" };
static const char *K_CELEBRATE[] = { "生日", "蛋糕", "礼物", "过年",
                                     "放假", "出去玩", "去公园" };
static const char *K_HAPPY[]     = { "开心", "高兴", "好玩儿", "喜欢",
                                     "舒服", "真好" };
static const char *K_SURPRISED[] = { "天哪", "真的吗", "不会吧", "怎么这样",
                                     "这么大", "这么多", "好神奇" };
static const char *K_CURIOUS[]   = { "为什么", "是什么", "这是什么", "怎么会",
                                     "那呢", "在哪", "干什么用的", "怎么弄" };
static const char *K_WINK[]      = { "猜猜", "你猜", "不告诉你", "秘密",
                                     "才不呢", "嘻嘻" };
static const char *K_SING[]      = { "唱歌", "小星星", "儿歌", "唱一", "一首歌",
                                     "两只老虎", "两只老", "拔萝卜", "小燕子" };
static const char *K_EAT[]       = { "吃饭", "吃饭饭", "好吃", "饿", "吃糖",
                                     "喝水", "喝奶", "饼干", "水果" };
static const char *K_GREET[]     = { "你好", "早上好", "晚安", "再见", "拜拜",
                                     "hello", "hi" };
static const char *K_CONFUSED[]  = { "不知道", "听不懂", "什么意思", "没听懂",
                                     "不会", "怎么说" };
static const char *K_SHY[]       = { "不好意思", "害羞", "别看", "不要看",
                                     "讨厌啦" };
static const char *K_BORED[]     = { "没意思", "无聊", "不想玩", "好没劲",
                                     "不想" };
static const char *K_DIZZY[]     = { "绕晕", "晕了", "搞不清", "乱乱的",
                                     "算不清" };
static const char *K_SCARED[]    = { "害怕", "怕怕", "好黑", "怪物", "大灰狼",
                                     "一个人睡", "有鬼", "怕" };
static const char *K_SAD[]       = { "难过", "伤心", "哭了", "想妈妈", "想爸爸",
                                     "不要了", "走了", "不理我", "不喜欢我" };
static const char *K_TIRED[]     = { "困了", "好困", "有点困", "睡觉", "睡吧",
                                     "想睡觉", "睡不着", "累了", "晚安",
                                     "眯一会儿" };

#define N(a)  ((int)(sizeof(a) / sizeof((a)[0])))

/*
 * 规则表。**有序**，先匹配先返回。
 *
 * 分组顺序（与 Python 版一致）：
 *   强正向 → 探索/认知 → 弱负向 → 强负向（同时也是安全相关）
 *
 * ⚠️ 组内顺序也有讲究：`excited` 必须在 `praise` 前面（见 sc_emo.h）。
 */
static const sc_emo_rule_t g_rules[] = {
    /* ---- 强正向 ---- */
    { "excited",  K_EXCITED,  N(K_EXCITED),  SC_FACE_EXCITED,  1000, 24, 28, 32 },
    { "praise",   K_PRAISE,   N(K_PRAISE),   SC_FACE_PROUD,    1000, 22, 12, 18 },
    { "showoff",  K_SHOWOFF,  N(K_SHOWOFF),  SC_FACE_PROUD,    1000, 20, 14, 20 },
    { "love",     K_LOVE,     N(K_LOVE),     SC_FACE_LOVE,     1100, 26,  8, 20 },
    { "laugh",    K_LAUGH,    N(K_LAUGH),    SC_FACE_LAUGHING, 1100, 20, 26, 30 },
    { "celebrate", K_CELEBRATE, N(K_CELEBRATE), SC_FACE_CELEBRATE, 1100, 20, 20, 24 },
    { "happy",    K_HAPPY,    N(K_HAPPY),    SC_FACE_HAPPY,     900, 16, 10, 14 },

    /* ---- 探索 / 认知 ---- */
    { "surprised", K_SURPRISED, N(K_SURPRISED), SC_FACE_SURPRISED, 800,  6, 22, 16 },
    { "curious",  K_CURIOUS,  N(K_CURIOUS),  SC_FACE_CURIOUS,   800,  8, 12, 12 },
    { "wink",     K_WINK,     N(K_WINK),     SC_FACE_WINKING,   900, 14, 12, 16 },
    { "sing",     K_SING,     N(K_SING),     SC_FACE_SINGING,  1200, 18, 18, 22 },
    { "eat",      K_EAT,      N(K_EAT),      SC_FACE_EATING,   1000, 14,  8, 16 },
    { "greet",    K_GREET,    N(K_GREET),    SC_FACE_WAVING,   1000, 14, 10, 14 },

    /* ---- 弱负向 ---- */
    { "confused", K_CONFUSED, N(K_CONFUSED), SC_FACE_CONFUSED,  900, -6,  4,  4 },
    { "shy",      K_SHY,      N(K_SHY),      SC_FACE_SHY,       900,  4,  8, 10 },
    { "bored",    K_BORED,    N(K_BORED),    SC_FACE_BORED,    1000, -14, -14, -10 },
    { "dizzy",    K_DIZZY,    N(K_DIZZY),    SC_FACE_DIZZY,    1000, -8, -6, -6 },

    /* ---- 强负向（也与安全相关）---- */
    { "scared",   K_SCARED,   N(K_SCARED),   SC_FACE_SCARED,   1200, -22, 24, 10 },
    { "sad",      K_SAD,      N(K_SAD),      SC_FACE_SAD,      1300, -26, -8,  4 },
    { "tired",    K_TIRED,    N(K_TIRED),    SC_FACE_SLEEPY,   1200, -4, -18, -14 },
};

#define NRULES  ((int)(sizeof(g_rules) / sizeof(g_rules[0])))

/* 去抖状态 */
static int      g_last_face = -1;
static uint32_t g_last_at;

void sc_emo_init(void)
{
    g_last_face = -1;
    g_last_at = 0;
}

int sc_emo_analyze(const char *text, int child, sc_emo_result_t *out)
{
    uint32_t now;
    int i;
    int j;

    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->face = -1;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    for (i = 0; i < NRULES; i++) {
        const sc_emo_rule_t *r = &g_rules[i];

        for (j = 0; j < r->nkeys; j++) {
            if (strstr(text, r->keys[j]) == NULL) {
                continue;
            }

            /* 命中 */
            out->face = r->face;
            out->hold_ms = r->hold_ms;
            out->label[0] = '\0';
            strncpy(out->label, r->label, sizeof(out->label) - 1);

            if (child) {
                /* 孩子说的：权重 1.0，另外给参与能量。
                 * "有人跟我说话"这件事本身就有能量。 */
                out->dv = r->dv;
                out->da = r->da + 3;
                out->de = r->de + 8;
            } else {
                /* 机器人自己的回复：权重 0.6，整数下用 3/5 近似 */
                out->dv = r->dv * 3 / 5;
                out->da = r->da * 3 / 5;
                out->de = r->de * 3 / 5;
            }

            /*
             * 去抖：0.8 秒内重复触发同一张脸 → 不重新闪，
             * 但**心情照常更新**（连续说好玩好玩时情绪在累积，
             * 只是表情不重置）。
             */
            now = sc_now_ms();
            if (out->face == g_last_face && (now - g_last_at) < 800) {
                out->face = -1;
                out->hold_ms = 0;
                strncat(out->label, "(去抖)", sizeof(out->label) - strlen(out->label) - 1);
            } else {
                g_last_face = out->face;
                g_last_at = now;
            }

            return 1;
        }
    }

    return 0;
}
