# 板端核心模块的宿主机自测。
#
#   make            编译 test_core 与 test_ws
#   make run        跑 test_core
#   make run-ws     起本机 WS 服务器并跑 test_ws
#   make clean
#
# 关于 -D_POSIX_C_SOURCE
# ---------------------
# ⚠️ 必须通过**编译选项**传，不能只在头文件里 #define。
# 严格 -std=c11 下 glibc 默认不暴露 clock_gettime / CLOCK_MONOTONIC /
# nanosleep 这类 POSIX 符号，而特性宏只在处理**第一个系统头文件之前**
# 生效。我们的头文件里会 include <stdbool.h> 等，等轮到 .c 里的
# #define 时已经晚了 —— 这就是第一次编译报 "nanosleep 隐式声明" 的原因。
# 放在命令行上则早于一切。
#
# 板端构建**不需要**这个宏（NuttX 的 libc 默认就带这些符号）。

CC       ?= gcc
CFLAGS   ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -DSC_HOST_TEST -D_POSIX_C_SOURCE=200809L -I..
LDLIBS   += -lm

CORE_SRCS = test_core.c ../sc_port.c ../sc_b64.c ../sc_resamp.c
WS_SRCS   = test_ws.c   ../sc_port.c ../sc_b64.c ../sc_ws.c ../sc_tcp.c

.PHONY: all run run-ws clean

all: test_core test_ws

test_core: $(CORE_SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

test_ws: $(WS_SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

run: test_core
	./test_core

run-ws: test_ws
	@python3 ws_server.py 8765 & echo $$! > .ws.pid; \
	 sleep 1; \
	 ./test_ws 8765; rc=$$?; \
	 kill $$(cat .ws.pid) 2>/dev/null; rm -f .ws.pid; \
	 exit $$rc

clean:
	rm -f test_core test_ws .ws.pid
