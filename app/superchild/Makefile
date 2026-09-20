############################################################################
# 小满（superchild）—— NuttX 应用 Makefile（方案 C：板端自持）
#
# 几个容易踩的地方，都写在注释里免得以后忘：
#
#  1) **aw-alsa-lib 的 include 路径必须自己加**。它不在全局 include 路径里，
#     而是靠各应用自己在 Makefile 里 CFLAGS += ${INCDIR_PREFIX}<path>。
#     不加的后果是 `fatal error: aw-alsa-lib/pcm.h: No such file or directory`，
#     看起来像"板子没有这个库"，其实是路径没加。
#
#  2) **静态库的链接靠 MODULE/PROGNAME 注册**。只把 .c 放进 CSRCS
#     而没有 CONFIG_SUPERCHILD=y，这个目标根本不会被编 ——
#     上一版 child_face 就是因此"代码在仓库里、镜像里没有"。
#
#  3) **mbedtls 的 include 路径不用自己加**。apps/crypto/mbedtls/Make.defs
#     在 CONFIG_CRYPTO_MBEDTLS=y 时会往**全局 CFLAGS** 追加两个路径
#     （apps/crypto/mbedtls/include 和 .../mbedtls/include），
#     所以 `#include <mbedtls/ssl.h>` 直接可用。
#     自己再加一遍不会错，但会让人以为"必须自己加"，下次照抄到别的
#     应用里就漏了 —— 这里刻意不加，靠 Kconfig 的 select 保证。
#
#  4) **cJSON 的头在 apps/include/netutils/ 下**，所以是
#     `#include <netutils/cJSON.h>`，不是 `#include <cJSON.h>`。
#     依赖 CONFIG_NETUTILS_CJSON，已由 Kconfig select。
############################################################################

include $(APPDIR)/Make.defs

ifeq ($(CONFIG_SUPERCHILD),y)

PROGNAME  = superchild
PRIORITY  = $(CONFIG_SUPERCHILD_PRIORITY)
STACKSIZE = $(CONFIG_SUPERCHILD_STACKSIZE)
MODULE    = $(CONFIG_SUPERCHILD)

# ---- 界面 / 音频 / 联网 ----
CSRCS  = sc_mood.c
CSRCS += sc_disp.c            # 显示驱动：PARTIAL 局部刷新（绕开移植层的 FULL 模式）
CSRCS += sc_face.c
CSRCS += sc_audio.c
CSRCS += sc_microute.c
CSRCS += sc_wifi.c

# ---- 方案 C 新增：板端自持所需的全部协议栈 ----
CSRCS += sc_port.c            # 可移植薄层（宿主测试与板端共用）
CSRCS += sc_b64.c             # Base64（音频塞在 JSON 里）
CSRCS += sc_resamp.c          # 16k⇄24k 定点重采样
CSRCS += sc_tcp.c             # 裸 TCP 传输（备用/降级路径）
CSRCS += sc_tls.c             # TLS + 内嵌 DigiCert G2 根证书
CSRCS += sc_ws.c              # WebSocket 客户端
CSRCS += sc_stepfun.c         # StepFun Realtime 会话
CSRCS += sc_emo.c             # 情绪判定（emotion.py 移植）
CSRCS += sc_safety.c          # 儿童安全（safety.py 移植）
CSRCS += sc_key.c             # API Key（从 /data 读，不进固件）
CSRCS += sc_time.c            # 时间同步（SNTP + HTTP Date + /data 持久化）
CSRCS += sc_rt.c              # 编排：闸门 / 音频泵 / 重连 / 界面投递

MAINSRC = sc_main.c

# aw-alsa-lib（/dev/audio/pcm 的 ALSA 封装）+ aw-osa 的头文件
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/hal
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/osal
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/audioutils/alsa-lib/include

# sc_port.h 在板端分支里主动 include <nuttx/config.h>，
# 这里显式关掉 -Wno-error 之外不需要的东西；-Wno-error 保留是因为
# 表情表里有 -1 之类的字面量参与三元运算。
CFLAGS += -Wno-error

endif

include $(APPDIR)/Application.mk
