/*
 * Base64 编解码。见 sc_b64.h 里的说明。
 *
 * 解码器刻意写得「宽容但严格」：
 *   - 跳过空白（服务端可能折行）
 *   - 但遇到任何非字母表字符立刻返回错误，**不做静默修正** ——
 *     音频数据里出现一个错字节，听感上是一声咔哒，很难定位；
 *     宁可在日志里报出来。
 */

#include "sc_b64.h"

static const char B64_ENC[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* 反查表：-1 = 非法，-2 = 空白（跳过），-3 = '=' 补齐 */
static int8_t b64_val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return (int8_t)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int8_t)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (int8_t)(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -3;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return -2;
    return -1;
}

int sc_b64_encode(const void *in, size_t in_len, char *out, size_t out_cap)
{
    const uint8_t *p = (const uint8_t *)in;
    size_t need = SC_B64_ENCLEN(in_len);
    size_t o = 0;

    if (out == NULL || need > out_cap) {
        return -1;
    }

    while (in_len >= 3) {
        uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        out[o++] = B64_ENC[(v >> 18) & 0x3F];
        out[o++] = B64_ENC[(v >> 12) & 0x3F];
        out[o++] = B64_ENC[(v >> 6) & 0x3F];
        out[o++] = B64_ENC[v & 0x3F];
        p += 3;
        in_len -= 3;
    }

    if (in_len == 2) {
        uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8);
        out[o++] = B64_ENC[(v >> 18) & 0x3F];
        out[o++] = B64_ENC[(v >> 12) & 0x3F];
        out[o++] = B64_ENC[(v >> 6) & 0x3F];
        out[o++] = '=';
    } else if (in_len == 1) {
        uint32_t v = ((uint32_t)p[0] << 16);
        out[o++] = B64_ENC[(v >> 18) & 0x3F];
        out[o++] = B64_ENC[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    }

    out[o] = '\0';
    return (int)o;
}

int sc_b64_decode(const char *in, size_t in_len, void *out, size_t out_cap)
{
    uint8_t *o = (uint8_t *)out;
    uint32_t acc = 0;
    int nbits = 0;
    size_t olen = 0;

    if (in == NULL || out == NULL) {
        return -1;
    }

    for (size_t i = 0; i < in_len; i++) {
        int8_t v = b64_val((unsigned char)in[i]);

        if (v == -2) {
            continue;                   /* 空白：跳过 */
        }
        if (v == -3) {
            break;                      /* '=' 之后都是补齐，结束 */
        }
        if (v < 0) {
            return -1;                  /* 非法字符：不猜，直接报错 */
        }

        acc = (acc << 6) | (uint32_t)v;
        nbits += 6;

        if (nbits >= 8) {
            nbits -= 8;
            if (olen >= out_cap) {
                return -1;
            }
            o[olen++] = (uint8_t)((acc >> nbits) & 0xFF);
        }
    }

    return (int)olen;
}
