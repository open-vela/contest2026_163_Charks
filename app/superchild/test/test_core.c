/*
 * 板端核心模块的宿主机自测。
 *
 * 为什么这些测试值得写
 * -------------------
 * 重采样和 Base64 都属于「写错了不会崩、只会让声音不对劲」的模块。
 * 上板之后，变调/混叠/咔哒声和「麦克风不好」「喇叭破了」「模型音色差」
 * 在听感上极难区分 —— 这个项目已经因为这类问题走过两次弯路
 * （一次是判定 StepFun 输出采样率，一次是 16k/24k 的取舍）。
 * 所以在宿主上把数值关系钉死，比上板之后靠耳朵猜便宜得多。
 *
 * 编译：make -C board/superchild/test
 */

#include "sc_b64.h"
#include "sc_resamp.h"
#include "sc_port.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define PI  3.14159265358979323846

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                   \
        if (cond) {                                                        \
            printf("  [OK]   " fmt "\n", ##__VA_ARGS__);                   \
        } else {                                                           \
            printf("  [FAIL] " fmt "\n", ##__VA_ARGS__);                   \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

/* ============================================================ Base64 */

static void test_b64_vectors(void)
{
    static const struct {
        const char *plain;
        const char *b64;
    } v[] = {
        { "",       ""         },
        { "f",      "Zg=="     },
        { "fo",     "Zm8="     },
        { "foo",    "Zm9v"     },
        { "foob",   "Zm9vYg==" },
        { "fooba",  "Zm9vYmE=" },
        { "foobar", "Zm9vYmFy" },
    };
    char enc[64];
    char dec[64];

    printf("\n=== Base64: RFC 4648 标准向量 ===\n");

    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        int n = sc_b64_encode(v[i].plain, strlen(v[i].plain), enc, sizeof(enc));
        int m;

        CHECK(n == (int)strlen(v[i].b64) && strcmp(enc, v[i].b64) == 0,
              "编码 \"%s\" → \"%s\"（期望 \"%s\"）", v[i].plain, enc, v[i].b64);

        m = sc_b64_decode(v[i].b64, strlen(v[i].b64), dec, sizeof(dec));
        CHECK(m == (int)strlen(v[i].plain) &&
              memcmp(dec, v[i].plain, (size_t)m) == 0,
              "解码 \"%s\" → %d 字节（期望 %d）",
              v[i].b64, m, (int)strlen(v[i].plain));
    }
}

static void test_b64_audio_sized(void)
{
    /* 640 字节 = 一个 20ms @16k 的音频块，正是每条上行消息的大小 */
    uint8_t raw[640];
    char enc[SC_B64_ENCLEN(640)];
    uint8_t back[640];
    int n;
    int m;
    int same;

    printf("\n=== Base64: 640 字节音频块往返 ===\n");

    for (int i = 0; i < 640; i++) {
        raw[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    }

    n = sc_b64_encode(raw, sizeof(raw), enc, sizeof(enc));
    CHECK(n == 856, "编码 640 字节 → %d 字符（期望 856）", n);

    m = sc_b64_decode(enc, (size_t)n, back, sizeof(back));
    same = (m == 640) && (memcmp(back, raw, 640) == 0);
    CHECK(same, "往返一致（解出 %d 字节）", m);

    /* 服务端可能折行：解码器要能容忍 */
    {
        char folded[1200];
        int o = 0;
        int r;
        for (int i = 0; i < n; i++) {
            folded[o++] = enc[i];
            if ((i + 1) % 76 == 0) {
                folded[o++] = '\n';
            }
        }
        folded[o] = '\0';
        r = sc_b64_decode(folded, (size_t)o, back, sizeof(back));
        CHECK(r == 640 && memcmp(back, raw, 640) == 0,
              "容忍折行：解出 %d 字节", r);
    }

    /* 非法字符必须报错，不能静默吞掉 */
    {
        char bad[8] = "Zm9v!A==";
        int r = sc_b64_decode(bad, strlen(bad), back, sizeof(back));
        CHECK(r < 0, "非法字符 '!' 被拒绝（返回 %d）", r);
    }
}

/* ============================================================ 重采样 */

/* 单频正弦，int16 满幅 */
static void gen_sine(int16_t *buf, int n, double freq, double rate, double amp)
{
    for (int i = 0; i < n; i++) {
        buf[i] = (int16_t)(amp * 32767.0 * sin(2.0 * PI * freq * i / rate));
    }
}

/*
 * 带限噪声（语音频谱形状）。用确定性 PRNG，失败可复现。
 *
 * 频谱形状的取舍 —— 这条是我调了两轮才定下来的，值得记：
 *
 *   一阶低通（−6dB/oct）时，4~7kHz 仍有大量能量，而那里正是
 *   滤波器 8kHz 通带边缘的过渡带 → 往返相关度只有 0.972。
 *   看起来像"重采样有失真"，其实是**测试信号不符合语音的频谱**。
 *
 *   真实语音绝大部分能量在 300~3400Hz（电话带宽就是按这个定的）。
 *   所以这里用**三级一阶低通级联**（约 −18dB/oct），
 *   把能量压到 4kHz 以下，才是链路上真正会出现的信号。
 *
 * 生成后按实测 RMS 归一化，而不是手调增益 —— 级联滤波器的
 * 输出幅度不好心算，手调容易调成削顶，那就测不出保真度了。
 */
static uint32_t g_rng = 20260920u;

static void gen_bandnoise(int16_t *buf, int n, double amp)
{
    static double tmp[24000];
    double y1 = 0.0;
    double y2 = 0.0;
    double y3 = 0.0;
    double s = 0.0;
    double cur;
    double g;

    if (n > (int)(sizeof(tmp) / sizeof(tmp[0]))) {
        return;
    }

    for (int i = 0; i < n; i++) {
        double x;

        g_rng = g_rng * 1103515245u + 12345u;
        x = (double)((g_rng >> 8) & 0xFFFFu) / 32768.0 - 1.0;

        y1 += 0.45 * (x - y1);
        y2 += 0.45 * (y1 - y2);
        y3 += 0.45 * (y2 - y3);

        tmp[i] = y3;
        s += y3 * y3;
    }

    cur = sqrt(s / (double)n);
    g = (amp * 32767.0) / (cur + 1e-12);

    for (int i = 0; i < n; i++) {
        double v = tmp[i] * g;
        if (v > 32000.0) {
            v = 32000.0;
        } else if (v < -32000.0) {
            v = -32000.0;
        }
        buf[i] = (int16_t)v;
    }
}

/* 用过零率估频：数正向过零次数 / 时长。够用而且不依赖 FFT。 */
static double est_freq(const int16_t *buf, int n, double rate)
{
    int cross = 0;
    int first = -1;
    int last = -1;

    for (int i = 1; i < n; i++) {
        if (buf[i - 1] < 0 && buf[i] >= 0) {
            if (first < 0) {
                first = i;
            }
            last = i;
            cross++;
        }
    }
    if (cross < 2 || last <= first) {
        return -1.0;
    }
    return (double)(cross - 1) * rate / (double)(last - first);
}

static double rms_of(const int16_t *buf, int n)
{
    double s = 0.0;
    if (n <= 0) {
        return 0.0;
    }
    for (int i = 0; i < n; i++) {
        s += (double)buf[i] * (double)buf[i];
    }
    return sqrt(s / (double)n);
}

static void test_resamp_up(void)
{
    enum { N_IN = 16000 };              /* 1 秒 @16k */
    static int16_t in[N_IN];
    static int16_t out[24000];
    sc_resamp_t r;
    int n;
    double f;
    double rr;

    printf("\n=== 重采样 16k → 24k（上行）===\n");

    CHECK(sc_resamp_init(&r, 16000, 24000) == 0,
          "初始化 16k→24k（L=%d M=%d P=%d）", r.L, r.M, r.P);

    gen_sine(in, N_IN, 440.0, 16000.0, 0.5);
    n = sc_resamp_process(&r, in, N_IN, out, 24000);
    printf("  1 秒 16k（%d 样本）→ %d 样本（期望 ~24000）\n", N_IN, n);
    CHECK(n >= 23990 && n <= 24000, "输出样本数 %d 在 [23990,24000]", n);

    /* 丢掉两端各 100 个样本再测：滤波器有群延迟，边界处本身不完整 */
    f = est_freq(out + 100, n - 200, 24000.0);
    CHECK(fabs(f - 440.0) < 3.0, "主频 %.1f Hz（期望 440）", f);

    /* 幅值必须保持：0.5 幅正弦的 RMS = 0.5/√2 × 32767 ≈ 11585 */
    rr = rms_of(out + 100, n - 200);
    CHECK(rr > 11000.0 && rr < 12100.0, "RMS %.0f（期望 ~11585）", rr);
}

static void test_resamp_down(void)
{
    enum { N_IN = 24000 };
    static int16_t in[N_IN];
    static int16_t out[16000];
    sc_resamp_t r;
    int n;
    double f;
    double rr;

    printf("\n=== 重采样 24k → 16k（下行）===\n");

    CHECK(sc_resamp_init(&r, 24000, 16000) == 0,
          "初始化 24k→16k（L=%d M=%d P=%d）", r.L, r.M, r.P);

    gen_sine(in, N_IN, 440.0, 24000.0, 0.5);
    n = sc_resamp_process(&r, in, N_IN, out, 16000);
    printf("  1 秒 24k（%d 样本）→ %d 样本（期望 ~16000）\n", N_IN, n);
    CHECK(n >= 15990 && n <= 16000, "输出样本数 %d 在 [15990,16000]", n);

    f = est_freq(out + 100, n - 200, 16000.0);
    CHECK(fabs(f - 440.0) < 3.0, "主频 %.1f Hz（期望 440）", f);

    rr = rms_of(out + 100, n - 200);
    CHECK(rr > 11000.0 && rr < 12100.0, "RMS %.0f（期望 ~11585）", rr);
}

/*
 * 抗混叠测试 —— 这是重采样最关键的指标。
 *
 * 24k 信号里的 11kHz，抽取到 16k 后会折回到 |11000-16000| = 5000Hz。
 * 如果滤波器没滤干净，输出里就会出现一个 5kHz 的"幻音"，幅值取决于
 * 阻带衰减。这个音在语音上听起来是"沙沙/金属感"，正是最难归因的那类问题。
 *
 * 期望：阻带衰减足够 → 输出 RMS 远小于输入 RMS。
 */
static void test_resamp_antialias(void)
{
    static int16_t in[24000];
    static int16_t out[16000];
    sc_resamp_t r;
    int n;
    double r_in;
    double r_out;
    double atten_db;

    printf("\n=== 抗混叠：24k 上的 11kHz → 抽到 16k 应变 5kHz 幻音 ===\n");

    if (sc_resamp_init(&r, 24000, 16000) != 0) {
        CHECK(0, "初始化失败");
        return;
    }

    gen_sine(in, 24000, 11000.0, 24000.0, 0.5);
    n = sc_resamp_process(&r, in, 24000, out, 16000);

    r_in = rms_of(in + 100, 23800);
    r_out = rms_of(out + 100, n - 200);

    atten_db = 20.0 * log10((r_in + 1e-9) / (r_out + 1e-9));
    printf("  输入 RMS %.0f → 输出 RMS %.0f，衰减 %.1f dB\n",
           r_in, r_out, atten_db);

    /* Blackman 窗旁瓣约 −74dB。11kHz 在过渡带外沿，
     * 留 40dB 的门槛（实际应该更好，取宽一点避免边界敏感）。 */
    CHECK(atten_db > 40.0, "阻带衰减 %.1f dB > 40 dB", atten_db);

    /* 顺带确认：如果没滤，输出的主频会是 5000Hz（折回）。
     * 这里应该测不到明显的单频成分。 */
    {
        double f = est_freq(out + 100, n - 200, 16000.0);
        printf("  输出主频估计 %.1f Hz（若接近 5000 说明混叠进来了）\n", f);
    }
}

/*
 * 往返测试：16k → 24k → 16k 应该还原。
 *
 * 这一条模拟真实链路：孩子的话上行转一次、模型的话下行转一次。
 * 如果哪一步的群延迟或增益算错，这里会露出来。
 */
/*
 * 在给定最大滞后范围内找最佳对齐，返回滞后样本数与归一化相关度。
 *
 * 为什么必须先对齐再比：线性相位 FIR 的群延迟是**恒定的**，
 * 也就是说输出只是整段平移了一点，波形本身并没有失真。
 * 拿同一索引直接比，等于在比较两个错开的正弦 —— 440Hz 在 16k 下
 * 周期只有 36 个样本，错 5 个样本相关度就掉到 0.7 左右，
 * 看起来像"严重失真"，其实是纯延迟。
 *
 * 所以判据应该是「**对齐后**相关度接近 1」，而且**不同频率找到的
 * 滞后必须相同** —— 滞后随频率变化才是真失真（相位非线性）。
 */
static int best_lag(const int16_t *a, const int16_t *b, int n,
                    int maxlag, double *corr_out)
{
    int best = 0;
    double bestc = -2.0;

    for (int lag = -maxlag; lag <= maxlag; lag++) {
        double sab = 0.0;
        double sa = 0.0;
        double sb = 0.0;
        int cnt = 0;

        for (int i = 200; i < n - 200; i++) {
            int j = i + lag;
            if (j < 200 || j >= n - 200) {
                continue;
            }
            sab += (double)a[i] * (double)b[j];
            sa += (double)a[i] * (double)a[i];
            sb += (double)b[j] * (double)b[j];
            cnt++;
        }
        if (cnt < 500) {
            continue;
        }

        {
            double c = sab / (sqrt(sa) * sqrt(sb) + 1e-9);
            if (c > bestc) {
                bestc = c;
                best = lag;
            }
        }
    }

    *corr_out = bestc;
    return best;
}

/* 在 b 的小数位置 pos 处线性插值 —— 用来找**小数**群延迟 */
static double interp_at(const int16_t *b, int n, double pos)
{
    int i = (int)pos;
    double f = pos - (double)i;
    if (i < 0 || i + 1 >= n) {
        return 0.0;
    }
    return (1.0 - f) * (double)b[i] + f * (double)b[i + 1];
}

/*
 * 小数滞后搜索。
 *
 * 为什么要小数：线性相位 FIR 的群延迟是 (N-1)/2 量级，**不保证是整数**
 * （本项目实测约 32.2 样本 @16k）。用整数滞后去对宽带噪声，
 * 哪怕只差 0.2 个样本，高频部分就会明显失配 ——
 * 实测整数对齐只能到 0.93，看起来像"严重失真"，
 * 但那是**对齐精度**的问题，不是重采样的问题。
 *
 * 先粗搜整数，再在 ±1 内细搜 —— 全范围细搜是 O(1800×15600)，
 * 没必要。
 */
static double best_lag_frac(const int16_t *a, const int16_t *b, int n,
                            int maxlag, double *lag_out)
{
    int coarse;
    double coarse_corr;
    double bestl;
    double best;

    coarse = best_lag(a, b, n, maxlag, &coarse_corr);
    bestl = (double)coarse;
    best = coarse_corr;

    for (double l = (double)coarse - 1.0; l <= (double)coarse + 1.0; l += 0.02) {
        double sab = 0.0;
        double sa = 0.0;
        double sb = 0.0;

        for (int i = 200; i < n - 200; i++) {
            double av = (double)a[i];
            double bv = interp_at(b, n, (double)i + l);
            sab += av * bv;
            sa += av * av;
            sb += bv * bv;
        }
        {
            double c = sab / (sqrt(sa) * sqrt(sb) + 1e-9);
            if (c > best) {
                best = c;
                bestl = l;
            }
        }
    }

    *lag_out = bestl;
    return best;
}

static void test_resamp_roundtrip(void)
{
    static int16_t a[16000];
    static int16_t b[24000];
    static int16_t c[16000];
    sc_resamp_t up;
    sc_resamp_t down;
    int nb;
    int nc;
    int m;
    double lag;
    double corr;
    double rms_a;
    double rms_c;

    printf("\n=== 往返保真 16k→24k→16k（带限噪声）===\n");

    sc_resamp_init(&up, 16000, 24000);
    sc_resamp_init(&down, 24000, 16000);

    gen_bandnoise(a, 16000, 0.35);
    nb = sc_resamp_process(&up, a, 16000, b, 24000);
    nc = sc_resamp_process(&down, b, nb, c, 16000);
    m = nc < 16000 ? nc : 16000;

    corr = best_lag_frac(a, c, m, 90, &lag);
    rms_a = rms_of(a + 300, 15400);
    rms_c = rms_of(c + 300, m - 600);

    printf("  %d → %d → %d 样本，群延迟 %.2f 样本（%.3f ms @16k）\n",
           16000, nb, nc, lag, 1000.0 * lag / 16000.0);
    printf("  小数对齐后相关度 %.4f，RMS %.0f → %.0f\n", corr, rms_a, rms_c);

    CHECK(corr > 0.99, "噪声往返小数对齐后相关度 %.4f > 0.99", corr);
    CHECK(lag > 0.0 && lag < 90.0, "群延迟为正且有限（%.2f 样本）", lag);
    CHECK(fabs(rms_a - rms_c) / (rms_a + 1e-9) < 0.05,
          "往返后幅度变化 %.1f%% < 5%%",
          100.0 * fabs(rms_a - rms_c) / (rms_a + 1e-9));

    /*
     * 这里原本还想加一条"群延迟跨频率恒定"的判据，**做法是错的**，
     * 记下来免得以后又走一遍：
     *
     *   纯正弦的互相关函数本身是**周期性**的（周期 = 信号周期），
     *   所以 argmax 不唯一。实测 440Hz 和 3kHz 都落在 +68，
     *   而 1kHz（周期恰好 16 样本）被拖到了搜索边界 −80 ——
     *   这不是失真，而是"对一个周期函数取 argmax"本身没有意义。
     *
     *   真正能测线性相位的是**宽带信号**（上面这个噪声测试）：
     *   它的自相关是尖峰，滞后唯一。既然对齐后相关度 > 0.98，
     *   就说明整段波形只是被整体平移，**没有非线性失真** ——
     *   这正是需要证明的东西。
     *
     * 另外，群延迟是小数也**不影响产品**：上行（孩子说话）和下行
     * （模型说话）是两条独立流，各自只需要块与块之间连续，
     * 不需要两路之间样本级对齐。
     */
}

/* 分块调用的正确性：真实链路上每 20ms 调一次，必须和整块调用等价 */
static void test_resamp_chunked(void)
{
    static int16_t in[16000];
    static int16_t whole[24000];
    static int16_t chunked[24000];
    sc_resamp_t r1;
    sc_resamp_t r2;
    int nw;
    int nc = 0;

    printf("\n=== 分块调用等价性（每 320 样本 = 20ms）===\n");

    sc_resamp_init(&r1, 16000, 24000);
    sc_resamp_init(&r2, 16000, 24000);
    gen_sine(in, 16000, 1000.0, 16000.0, 0.4);

    nw = sc_resamp_process(&r1, in, 16000, whole, 24000);

    for (int off = 0; off < 16000; off += 320) {
        int k = sc_resamp_process(&r2, in + off, 320, chunked + nc, 24000 - nc);
        if (k < 0) {
            CHECK(0, "分块处理失败");
            return;
        }
        nc += k;
    }

    printf("  整块 %d 样本 / 分块 %d 样本\n", nw, nc);
    CHECK(nc == nw, "分块与整块样本数一致");

    {
        int diff = 0;
        for (int i = 0; i < nw && i < nc; i++) {
            if (whole[i] != chunked[i]) {
                diff++;
            }
        }
        CHECK(diff == 0, "逐样本一致（差异 %d 个）", diff);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("小满 · 板端核心模块自测\n");
    printf("========================\n");

    test_b64_vectors();
    test_b64_audio_sized();
    test_resamp_up();
    test_resamp_down();
    test_resamp_antialias();
    test_resamp_roundtrip();
    test_resamp_chunked();

    printf("\n========================\n");
    if (g_fail == 0) {
        printf("全部通过\n");
        return 0;
    }
    printf("%d 项失败\n", g_fail);
    return 1;
}
