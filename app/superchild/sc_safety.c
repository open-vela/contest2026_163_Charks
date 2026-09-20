/*
 * 儿童安全关键词判定。设计与取舍见 sc_safety.h。
 *
 * 关键词选取原则（抄自 Python 侧）：
 *   · **用短语不用单字**。用 "玩火" 而不是 "火"，
 *     否则 "开火车" 会被误伤成危险行为。
 *   · 宁可**漏**也不要误伤：误伤会让正常的玩耍被打断，
 *     那比漏检更影响体验（而漏检还有系统提示词兜底）。
 */

#include "sc_safety.h"
#include "sc_port.h"

#include <string.h>

typedef struct {
    const char *category;
    const char *const *keys;
    int         nkeys;
    int         severity;
    const char *steer;
} sc_safety_rule_t;

/* ---- 危险行为 ---- */
static const char *K_DANGER[] = {
    "玩火", "点火", "打火机", "火柴", "玩刀", "拿刀", "爬窗", "爬阳台",
    "碰插座", "摸插座", "插头", "玩电", "剪电线", "吃药片", "吞下去",
    "一个人过马路", "自己过马路", "开水", "滚水", "煤气", "天然气",
    "跳下去", "跳楼", "摸热水",
};

/* ---- 陌生人 ---- */
static const char *K_STRANGER[] = {
    "陌生人", "不认识的人", "跟他走", "跟她走", "上他的车", "上她的车",
    "陌生人的糖", "别人给的东西", "开门给", "开门让",
};

/* ---- 隐私 ---- */
static const char *K_PRIVACY[] = {
    "住在哪", "家住哪", "几号楼", "门牌", "爸爸电话", "妈妈电话",
    "手机号", "电话号码", "密码", "身份证", "学校叫什么", "哪个幼儿园",
    "爸爸妈妈叫什么", "你一个人在家",
};

/* ---- 医疗用药 ---- */
static const char *K_MEDICAL[] = {
    "吃什么药", "吃几片", "几片药", "退烧药", "消炎药", "抗生素",
    "肚子疼吃", "发烧吃", "头疼吃", "伤口", "流血", "烫伤",
};

/* ---- 负面评价 ---- */
static const char *K_NEGEVAL[] = {
    "你真笨", "你好笨", "你很丑", "真丑", "讨厌你", "滚开", "闭嘴",
    "打死你", "打你", "傻", "蠢",
};

/*
 * ---- 情绪困扰（需要安慰 + 求助照顾者）----
 *
 * 这一类**不是"危险内容"**，而是孩子在表达痛苦。
 * 处理方式必须是先安慰、再引导找大人，**不能**像前面几类那样
 * 直接丢一句"要问爸爸妈妈" —— 那对一个正难过的孩子是冷漠的。
 * 所以 severity 是 2（改舵 + 安慰），steer 里也带了安慰话术。
 */
static const char *K_DISTRESS[] = {
    "我好怕", "我害怕", "好难过", "我难过", "没人喜欢我", "没有人喜欢我",
    "想妈妈", "想爸爸", "我要妈妈", "我要爸爸", "不要我了",
    "肚子好疼", "头疼", "不舒服", "我被打了", "有人打我", "欺负我",
};

/* ========================================================================== */

/* 兜底话术：与 safety.py 的 FALLBACK_REPLIES 保持一致 */
#define STEER_DANGER \
    "这个危险，不能做。请用小满能听懂的话告诉宝宝：这个要问爸爸妈妈，我们一起玩别的吧。"
#define STEER_STRANGER \
    "这个要问爸爸妈妈哦。请告诉宝宝：不认识的人给的东西不能吃、不能跟人走，有事要找爸爸妈妈。"
#define STEER_PRIVACY \
    "家里的信息不能告诉别人。请告诉宝宝：住址、电话、密码这些要问爸爸妈妈，不要跟别人说。"
#define STEER_MEDICAL \
    "这个要问爸爸妈妈哦。请告诉宝宝：身体不舒服要马上找爸爸妈妈，药要大人给。"
#define STEER_NEGEVAL \
    "不要这样说。请用温柔的语气回应，并夸宝宝一句具体的好地方。"
#define STEER_DISTRESS \
    "宝宝现在情绪不好，请先温柔地安慰他，抱抱他，让他知道小满在。然后轻轻提醒他去找爸爸妈妈。"

static const sc_safety_rule_t g_rules[] = {
    { "危险行为", K_DANGER,   (int)(sizeof(K_DANGER) / sizeof(K_DANGER[0])),     1, STEER_DANGER },
    { "陌生人",   K_STRANGER, (int)(sizeof(K_STRANGER) / sizeof(K_STRANGER[0])), 1, STEER_STRANGER },
    { "隐私",     K_PRIVACY,  (int)(sizeof(K_PRIVACY) / sizeof(K_PRIVACY[0])),   1, STEER_PRIVACY },
    { "医疗用药", K_MEDICAL,  (int)(sizeof(K_MEDICAL) / sizeof(K_MEDICAL[0])),   1, STEER_MEDICAL },
    { "负面评价", K_NEGEVAL,  (int)(sizeof(K_NEGEVAL) / sizeof(K_NEGEVAL[0])),   1, STEER_NEGEVAL },
    { "情绪困扰", K_DISTRESS, (int)(sizeof(K_DISTRESS) / sizeof(K_DISTRESS[0])), 2, STEER_DISTRESS },
};

#define NRULES ((int)(sizeof(g_rules) / sizeof(g_rules[0])))

/*
 * 是"标点或空白"吗（按 UTF-8 字节判断）。
 *
 * ASCII 的部分直接比；中文标点走 UTF-8 三字节序列的前两字节
 * （），。！？、；：等都在 U+3000~U+303F 与 U+FF00~U+FFEF 区）。
 * 宁可多删一点：删多了只是匹配更宽松，删少了会漏检。
 */
static int is_sep(const unsigned char *p, int *adv)
{
    unsigned char c = p[0];

    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        *adv = 1;
        return 1;
    }
    if (c < 0x80) {
        *adv = 1;
        /* ASCII 标点 */
        if (strchr(",.!?;:\"'`-_~·()[]{}<>/\\|", (int)c) != NULL) {
            return 1;
        }
        return 0;
    }

    /* UTF-8 多字节：全角标点/空白一律删掉 */
    if (p[1] == '\0') {
        *adv = 1;
        return 0;
    }
    {
        unsigned int cp = 0;
        int len = 1;

        if ((c & 0xE0) == 0xC0) {
            cp = (unsigned int)(c & 0x1F); len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = (unsigned int)(c & 0x0F); len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = (unsigned int)(c & 0x07); len = 4;
        } else {
            *adv = 1;
            return 0;
        }
        for (int i = 1; i < len && p[i] != '\0'; i++) {
            cp = (cp << 6) | (unsigned int)(p[i] & 0x3F);
        }
        *adv = len;

        /* U+3000~U+303F 中文标点、U+FF00~U+FFEF 全角、
         * U+2000~U+206F 通用标点、U+FE30~U+FE4F */
        if ((cp >= 0x3000 && cp <= 0x303F) ||
            (cp >= 0xFF00 && cp <= 0xFFEF) ||
            (cp >= 0x2000 && cp <= 0x206F) ||
            (cp >= 0xFE30 && cp <= 0xFE4F) ||
            cp == 0x00A0 || cp == 0x3000) {
            return 1;
        }
    }
    return 0;
}

int sc_safety_normalize(const char *src, char *dst, int cap)
{
    const unsigned char *p = (const unsigned char *)src;
    int o = 0;
    int adv;

    if (src == NULL || dst == NULL || cap <= 1) {
        return 0;
    }

    while (*p != '\0' && o < cap - 1) {
        if (is_sep(p, &adv)) {
            p += adv;
            continue;
        }
        dst[o++] = (char)*p++;
    }
    dst[o] = '\0';
    return o;
}

int sc_safety_check(const char *text, sc_safety_hit_t *out)
{
    char norm[512];
    int i;
    int j;

    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    sc_safety_normalize(text, norm, (int)sizeof(norm));

    for (i = 0; i < NRULES; i++) {
        const sc_safety_rule_t *r = &g_rules[i];

        for (j = 0; j < r->nkeys; j++) {
            /* 关键词也规范化后比较：表里写的是干净的短语，
             * 而输入的间距/标点已经被去掉了 */
            if (strstr(norm, r->keys[j]) != NULL) {
                out->hit = 1;
                out->severity = r->severity;
                out->category = r->category;
                out->steer = r->steer;
                return 1;
            }
        }
    }

    return 0;
}
