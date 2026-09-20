/*
 * API Key 的读取。设计与取舍见 sc_key.h。
 */

#include "sc_key.h"
#include "sc_port.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* /data 是 YAFFS 可写分区（rcS 里 mount -t yaffs /dev/usrdata /data） */
#define SC_KEY_PATH  "/data/etc/superchild/stepfun.key"

const char *sc_key_path(void)
{
    return SC_KEY_PATH;
}

int sc_key_load(char *out, int cap)
{
    int fd;
    int n;
    int len;
    int i;
    int j;

    if (out == NULL || cap < 2) {
        return -EINVAL;
    }
    out[0] = '\0';

    fd = open(SC_KEY_PATH, O_RDONLY);
    if (fd < 0) {
        /*
         * 这是**预期内**可能发生的情况（还没下发过密钥），
         * 所以不打成"错误"，而是给出怎么做。
         * 排查时最费时间的不是"失败了"，而是"不知道该做什么"。
         */
        SC_WARN("key: 读不到 %s（errno=%d）。\n"
                "     下发方法：把密钥写入该文件，例如\n"
                "       adb  : adb push stepfun.key /data/etc/superchild/stepfun.key\n"
                "       nsh  : echo <你的key> > %s\n"
                "     注意目录要先存在：mkdir -p /data/etc/superchild\n",
                SC_KEY_PATH, errno, SC_KEY_PATH);
        return -ENOENT;
    }

    n = read(fd, out, (size_t)(cap - 1));
    close(fd);

    if (n <= 0) {
        SC_ERR("key: %s 是空的（读了 %d 字节）\n", SC_KEY_PATH, n);
        out[0] = '\0';
        return -EIO;
    }
    out[n] = '\0';
    len = n;

    /*
     * 去掉首尾空白与换行。
     *
     * 这一步**不是可选的**：用 `echo xxx > file` 写入时几乎必然带一个
     * 换行符，而带换行的 key 发出去是鉴权失败 —— 服务端回 401/403，
     * 看起来像"key 不对"，完全不会想到是多了一个 \n。
     * 这个坑在别处踩过太多次了。
     */
    i = 0;
    while (i < len && (out[i] == ' ' || out[i] == '\t' ||
                       out[i] == '\r' || out[i] == '\n')) {
        i++;
    }
    j = 0;
    while (i < len) {
        char c = out[i++];
        if (c == '\r' || c == '\n') {
            break;                 /* 只取第一行 */
        }
        out[j++] = c;
    }
    /* 再去掉尾部空格 */
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\t')) {
        j--;
    }
    out[j] = '\0';

    if (j == 0) {
        SC_ERR("key: %s 里没有有效内容（全是空白）\n", SC_KEY_PATH);
        return -EINVAL;
    }

    SC_LOG("key: 已从 %s 读取 %d 字节\n", SC_KEY_PATH, j);
    return j;
}

void sc_key_wipe(char *key, int cap)
{
    if (key != NULL && cap > 0) {
        sc_memzero(key, (size_t)cap);
    }
}
