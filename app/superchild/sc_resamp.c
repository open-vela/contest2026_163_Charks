/*
 * 多相定点重采样。算法与取舍见 sc_resamp.h。
 *
 * 索引推导（这一段是整份代码里最容易写错的地方，写下来免得以后重推）
 * ------------------------------------------------------------------
 * 标准「上采 L → 低通 h → 下采 M」流程：
 *     u[m] = x[m/L]   （仅当 L | m，否则 0）
 *     v[m] = Σ_t h[t]·u[m-t]
 *     y[k] = v[kM]
 *
 * 展开 y[k]：需要 u[kM - t] ≠ 0，即 t ≡ kM (mod L)。令
 *     frac = (kM) mod L          → 相位（选哪一组子滤波器）
 *     n0   = (kM) div L = ⌊kM/L⌋ → 需要的最新输入样本下标
 *     t    = frac + jL
 * 于是
 *     y[k] = Σ_j h[frac + jL] · x[n0 - j]
 *
 * 即：**相位 frac 的子滤波器 = h[frac + jL]，输入窗口以 n0 为最新端**。
 *
 * 产出时机：x[n0-j] 对 j=0..P-1 都要有值，所以要求 n0 ≤ fed-1
 * （fed = 已喂入的样本数）。
 *
 * 历史读取偏移：x[n0-j] 距最新样本 x[fed-1] 的偏移是 (fed-1-n0)+j。
 * 由于这里**每喂入一个样本就检查一次产出**，而 n0 单调不减，
 * 所以被产出的 k 一定满足 fed-1 == n0（偏移恒为 0）。多留 8 格历史
 * 只是防御性写法；真的用上说明上面这个不变量被破坏了。
 */

#include "sc_resamp.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#ifndef SC_PI
#  define SC_PI  3.14159265358979323846
#endif

static int gcd_int(int a, int b)
{
    while (b != 0) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* 四舍五入到 int32。不用 lround：NuttX 的 libm 不一定有，自己写两行更省心。 */
static int32_t round_i32(double v)
{
    return (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

/*
 * 设计原型低通 + 逐相位归一化。
 *
 * 原型：理想低通 × Blackman 窗
 *     h[n] = 2·fc·sinc(2·fc·(n-c)) · w[n]
 *     fc = 0.5 · min(1/L, 1/M)      （归一化到 L·fs_in）
 *
 * 为什么用 Blackman 而不是 Hann：Blackman 旁瓣约 −74dB，够把下行
 * 抽取时的混叠压到听不见。代价是过渡带更宽，所以抽头数要够。
 */
static int design(sc_resamp_t *r)
{
    int use = (SC_RESAMP_NTAPS / r->L) * r->L;
    double h[SC_RESAMP_NTAPS];
    double fL = (double)r->L;
    double fM = (double)r->M;
    double fc;
    double c;
    int P;

    if (use < r->L * 8) {
        return -1;                       /* 比例太离谱，抽头不够用 */
    }
    P = use / r->L;
    r->P = P;

    fc = 0.5 * ((1.0 / fL < 1.0 / fM) ? 1.0 / fL : 1.0 / fM);
    c = (use - 1) / 2.0;

    for (int n = 0; n < use; n++) {
        double x = (double)n - c;
        double s;
        double w;

        if (fabs(x) < 1e-12) {
            s = 1.0;
        } else {
            double a = 2.0 * SC_PI * fc * x;
            s = sin(a) / a;
        }

        w = 0.42
          - 0.50 * cos(2.0 * SC_PI * (double)n / (double)(use - 1))
          + 0.08 * cos(4.0 * SC_PI * (double)n / (double)(use - 1));

        h[n] = 2.0 * fc * s * w;
    }

    /*
     * 逐相位归一化到 1.0（Q15 下即 32768）。
     * 整体归一化会让各相位直流增益有微小差异 → 语音上是一种很轻的
     * 嗡嗡声，而且**没法通过听感定位到重采样**。逐相位归一化直接消掉。
     */
    for (int fr = 0; fr < r->L; fr++) {
        double sum = 0.0;
        double scale;

        for (int j = 0; j < P; j++) {
            sum += h[fr + j * r->L];
        }
        scale = (fabs(sum) > 1e-12) ? (32768.0 / sum) : 0.0;

        for (int j = 0; j < P; j++) {
            int32_t q = round_i32(h[fr + j * r->L] * scale);
            if (q > 32767) {
                q = 32767;
            } else if (q < -32768) {
                q = -32768;
            }
            r->coef[fr * P + j] = q;
        }
    }

    return 0;
}

int sc_resamp_init(sc_resamp_t *r, int in_rate, int out_rate)
{
    int g;

    if (r == NULL || in_rate <= 0 || out_rate <= 0) {
        return -1;
    }

    memset(r, 0, sizeof(*r));

    g = gcd_int(out_rate, in_rate);
    r->L = out_rate / g;
    r->M = in_rate / g;
    r->in_rate = in_rate;
    r->out_rate = out_rate;

    if (r->L > SC_RESAMP_NTAPS) {
        return -1;
    }
    if (design(r) != 0) {
        return -1;
    }

    sc_resamp_reset(r);
    r->ready = 1;
    return 0;
}

void sc_resamp_reset(sc_resamp_t *r)
{
    memset(r->hist, 0, sizeof(r->hist));
    r->wpos = 0;
    r->fed = 0;
    r->k = 0;
}

int sc_resamp_process(sc_resamp_t *r, const int16_t *in, int in_len,
                      int16_t *out, int out_cap)
{
    int olen = 0;

    if (r == NULL || !r->ready || in == NULL || out == NULL) {
        return -1;
    }

    for (int i = 0; i < in_len; i++) {
        r->hist[r->wpos] = in[i];
        r->wpos++;
        if (r->wpos >= SC_RESAMP_HIST) {
            r->wpos = 0;
        }
        r->fed++;

        for (;;) {
            int64_t kM = r->k * (int64_t)r->M;
            int64_t n0 = kM / r->L;
            int frac;
            int32_t idx;
            const int32_t *c;
            int64_t acc = 0;

            if (n0 > r->fed - 1) {
                break;                       /* 输入还不够，等下一个样本 */
            }
            if (olen >= out_cap) {
                return -1;                   /* 调用方缓冲给小了 */
            }

            frac = (int)(kM % r->L);
            c = &r->coef[frac * r->P];

            /* 见文件头推导：产出时 (fed-1-n0) 恒为 0 */
            idx = r->wpos - 1 - (int)(r->fed - 1 - n0);
            while (idx < 0) {
                idx += SC_RESAMP_HIST;
            }

            for (int j = 0; j < r->P; j++) {
                acc += (int64_t)c[j] * (int64_t)r->hist[idx];
                if (--idx < 0) {
                    idx = SC_RESAMP_HIST - 1;
                }
            }

            {
                int32_t v = (int32_t)(acc >> 15);
                if (v > 32767) {
                    v = 32767;
                } else if (v < -32768) {
                    v = -32768;
                }
                out[olen++] = (int16_t)v;
            }

            r->k++;
        }
    }

    return olen;
}

int sc_resamp_in_per_out(const sc_resamp_t *r)
{
    /* 上取整：保证按这个数预留输入缓冲不会偏小 */
    return (r->M + r->L - 1) / r->L;
}
