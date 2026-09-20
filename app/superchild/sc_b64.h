/*
 * Base64 —— Realtime 的音频是**塞在 JSON 字符串里**的（`input_audio_buffer.append`
 * 的 `audio` 字段、`response.audio.delta` 的 `delta` 字段），所以编解码是
 * 每个音频块都要走的路径。
 *
 * 为什么不直接用树里的 apps/include/netutils/base64.h：那份依赖
 * `CONFIG_NETUTILS_BASE64`，而且接口是「一次一整块」。我们要的是
 * **按 640 字节一块、每 20ms 一次**的流式调用，自己写 80 行更可控，
 * 也免掉一个配置依赖（配置依赖越多，编不过时越难查）。
 *
 * 只支持标准字母表 + `=` 补齐（Realtime 用的就是这个）。
 * 不支持 URL-safe 变体 —— 用到了再说，不要假装支持。
 */

#ifndef __SUPERCHILD_SC_B64_H
#define __SUPERCHILD_SC_B64_H

#include <stddef.h>
#include <stdint.h>

/* 编码后长度（含结尾 '\0'）：ceil(n/3)*4 + 1 */
#define SC_B64_ENCLEN(n)  ((((n) + 2) / 3) * 4 + 1)
/* 解码后最大长度（不含结尾）：ceil(n/4)*3 */
#define SC_B64_DECLEN(n)  ((((n) + 3) / 4) * 3)

/*
 * 编码。out 必须以 '\0' 结尾，容量至少 SC_B64_ENCLEN(in_len)。
 * @return 写入的字符数（不含 '\0'），<0 表示容量不足。
 */
int sc_b64_encode(const void *in, size_t in_len, char *out, size_t out_cap);

/*
 * 解码。**容忍空白字符**（换行/空格），因为有的服务端会折行。
 * @return 解出的字节数，<0 表示输入非法。
 */
int sc_b64_decode(const char *in, size_t in_len, void *out, size_t out_cap);

#endif /* __SUPERCHILD_SC_B64_H */
