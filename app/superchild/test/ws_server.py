#!/usr/bin/env python3
"""
WebSocket 自测用的本机服务器（**裸 asyncio 实现，不用 websockets 库**）。

为什么不用库
-----------
我们要验的是**自己的帧解析器**。用现成库的话：
  - 它的大消息自动分片阈值是内部实现细节，测不到"三段分片"这种确定场景
  - 它拒绝的非法帧我们也就测不到
自己写 150 行，每个字节能控制，测出来的才是我们的代码。

目的：把「帧格式对不对」和「TLS 通不通」分开验证。
    - 对着这个服务器测 → 出错必然是握手/掩码/分片/长度编码的问题
    - 再对着真实 StepFun 测 → 出错必然是证书/SNI/时间的问题

命令（客户端发什么，服务器怎么应）：
    <任意文本>   → "echo:<原文>"
    BIG16:<n>    → 一个 n 字节的文本帧（n ≤ 65535，走 16 位长度）
    BIG64:<n>    → 一个 n 字节的文本帧（n > 65535，走 64 位长度）
    FRAG:<n>     → n 个 'F'，**手工拆成 3 帧**（TEXT/0 + CONT/0 + CONT/1）
    UNICODE      → 中文 + emoji
    PING         → 先发 ping，等客户端回 pong，再回 "PONG-OK"
    QUIT         → 发 close 帧
"""

import asyncio
import base64
import hashlib
import struct
import sys

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_CONT, OP_TEXT, OP_BIN, OP_CLOSE, OP_PING, OP_PONG = 0, 1, 2, 8, 9, 10


def make_frame(opcode, payload, fin=True):
    if isinstance(payload, str):
        payload = payload.encode()
    hdr = bytearray()
    hdr.append((0x80 if fin else 0x00) | opcode)
    n = len(payload)
    if n < 126:
        hdr.append(n)
    elif n <= 0xFFFF:
        hdr.append(126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(127)
        hdr += struct.pack(">Q", n)
    return bytes(hdr) + payload


async def read_frame(r):
    h = await r.readexactly(2)
    fin = bool(h[0] & 0x80)
    op = h[0] & 0x0F
    masked = bool(h[1] & 0x80)
    ln = h[1] & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", await r.readexactly(2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", await r.readexactly(8))[0]
    mask = await r.readexactly(4) if masked else None
    data = await r.readexactly(ln) if ln else b""
    if mask:
        data = bytes(c ^ mask[i % 4] for i, c in enumerate(data))
    return fin, op, data


async def handle(reader, writer):
    # ---- HTTP Upgrade ----
    header = b""
    while b"\r\n\r\n" not in header:
        chunk = await reader.read(1024)
        if not chunk:
            writer.close()
            return
        header += chunk

    key = ""
    for line in header.decode(errors="replace").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()

    if not key:
        print("[server] 请求里没有 Sec-WebSocket-Key，拒绝", flush=True)
        writer.close()
        return

    accept = base64.b64encode(
        hashlib.sha1((key + GUID).encode()).digest()).decode()

    writer.write((
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "\r\n").encode())
    await writer.drain()
    print("[server] 握手完成", flush=True)

    writer.write(make_frame(OP_TEXT, "hello"))
    await writer.drain()

    pending_text = ""          # 分片重组缓冲
    pong_ok = False
    awaiting_pong = False

    while True:
        try:
            fin, op, data = await read_frame(reader)
        except (asyncio.IncompleteReadError, ConnectionResetError, OSError):
            print("[server] 客户端断开", flush=True)
            break

        if op == OP_CLOSE:
            print("[server] 收到 close", flush=True)
            try:
                writer.write(make_frame(OP_CLOSE, b""))
                await writer.drain()
            except OSError:
                pass
            break

        if op == OP_PING:
            writer.write(make_frame(OP_PONG, data))
            await writer.drain()
            continue

        if op == OP_PONG:
            pong_ok = True
            print(f"[server] 收到 pong: {data!r}", flush=True)
            # 只有"正在等 pong"时才回应。见上面 PING 分支的说明：
            # 不能在发完 ping 之后就地 sleep 等 —— 那会把自己挡在读循环外面。
            if awaiting_pong:
                awaiting_pong = False
                writer.write(make_frame(OP_TEXT, "PONG-OK"))
                await writer.drain()
            continue

        if op in (OP_TEXT, OP_CONT):
            pending_text += data.decode("utf-8", errors="replace")
            if not fin:
                continue

            msg = pending_text
            pending_text = ""

            print(f"[server] 收到 {len(msg)} 字符: {msg[:40]!r}", flush=True)

            if msg == "QUIT":
                writer.write(make_frame(OP_CLOSE, b""))
                await writer.drain()
                print("[server] 主动关闭", flush=True)
                break

            # ⚠️ 取数字一律用 split(":", 1)[1]，不要手数前缀长度。
            # 第一版 BIG64 写成 msg[7:] —— "BIG64:" 其实是 6 个字符，
            # 于是 int("0000")=0，服务器发了个**零长度帧**，
            # 70000 字节压根没发出去。而客户端忠实地报告"收到 0 字节消息"，
            # 看起来像客户端解析 64 位长度失败 —— 白白查了一轮客户端。
            if msg.startswith("BIG16:"):
                n = int(msg.split(":", 1)[1])
                writer.write(make_frame(OP_TEXT, "B" * n))

            elif msg.startswith("BIG64:"):
                n = int(msg.split(":", 1)[1])
                writer.write(make_frame(OP_TEXT, "G" * n))

            elif msg.startswith("FRAG:"):
                n = int(msg.split(":", 1)[1])
                payload = ("F" * n).encode()
                step = max(1, len(payload) // 3)
                chunks = [payload[i:i + step] for i in range(0, len(payload), step)]
                # 第一段：TEXT + FIN=0
                writer.write(make_frame(OP_TEXT, chunks[0], fin=False))
                # 中间段：CONT + FIN=0
                for c in chunks[1:-1]:
                    writer.write(make_frame(OP_CONT, c, fin=False))
                # 最后段：CONT + FIN=1
                writer.write(make_frame(OP_CONT, chunks[-1], fin=True))
                print(f"[server] 已分片发送 {n} 字节，共 {len(chunks)} 帧", flush=True)

            elif msg == "UNICODE":
                writer.write(make_frame(OP_TEXT, "你好，小满！表情随心情变化 🙂🎈"))

            elif msg == "PING":
                # ⚠️ 不能在这里 sleep 等 pong。
                # 这个 handler 是单协程的：就地等待会把自己挡在读循环外面，
                # pong 到了也没人读 —— 于是永远超时，误判成"客户端没回 pong"。
                # 第一版就是这么把测试搞挂的（客户端其实是回对了的）。
                # 正确做法：发完继续读，等 pong 帧到了再回应。
                pong_ok = False
                awaiting_pong = True
                writer.write(make_frame(OP_PING, b"abc"))
                await writer.drain()

            else:
                writer.write(make_frame(OP_TEXT, "echo:" + msg))

            await writer.drain()

    try:
        writer.close()
        await writer.wait_closed()
    except Exception:
        pass


async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    server = await asyncio.start_server(handle, "127.0.0.1", port)
    print(f"[server] 监听 127.0.0.1:{port}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
