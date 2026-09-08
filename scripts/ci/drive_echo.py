#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CI 用：驱动第 16 章的 Reactor 回显服务器，验证它能接受连接并正确回显。

第 16 章的 echo_server 行为：
  - 连上后先发一句欢迎语
  - 之后发什么回什么
  - 发 "stat" 返回连接信息
  - 发 "quit" 优雅关闭（先回一句再关写端）

用法:
    python scripts/ci/drive_echo.py <服务器可执行文件> <端口> [线程数]
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

# -----------------------------------------------------------------------------
# 关于子进程的 stdout：必须重定向到**文件**，不能用 subprocess.PIPE。
#
# 服务器会持续打日志。如果用 PIPE 而测试期间不去读它，管道缓冲区
# （Linux 64 KB / Windows 更小）一满，服务器就会**阻塞在 write(stdout) 上**，
# 于是停止处理网络事件 —— 测试看起来像「服务器不回数据」，
# 实际是被自己的日志卡死了。这个坑在 CI 里极难定位。
# 写文件不会阻塞，测试结束后再读回来打印即可。
# -----------------------------------------------------------------------------

TIMEOUT = 5.0


def dump_log(path, max_lines=30):
    """打印服务器日志。用 ascii 兜底，避免 Windows GBK 控制台
    遇到 UTF-8 中文时抛 UnicodeEncodeError 把真正的失败信息盖掉。"""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return
    if not lines:
        return
    print(f"    --- 服务器输出（前 {max_lines} 行）---", flush=True)
    for line in lines[:max_lines]:
        safe = line.encode(sys.stdout.encoding or "utf-8", "replace").decode(
            sys.stdout.encoding or "utf-8", "replace")
        print(f"    | {safe}", flush=True)
    try:
        os.remove(path)
    except OSError:
        pass


def log(msg):
    print(f"    [drive_echo] {msg}", flush=True)


def wait_port(port, proc, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"服务器提前退出，退出码 {proc.returncode}")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"等待端口 {port} 超时")


def recv_some(sock, timeout=TIMEOUT):
    sock.settimeout(timeout)
    try:
        return sock.recv(8192)
    except socket.timeout:
        return b""


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    binary = os.path.normpath(os.path.abspath(sys.argv[1]))
    port = int(sys.argv[2])
    threads = sys.argv[3] if len(sys.argv) > 3 else "2"

    if not os.path.exists(binary):
        log(f"找不到可执行文件: {binary}")
        return 1

    log(f"启动 {os.path.basename(binary)} {port} {threads}")
    logfile = tempfile.NamedTemporaryFile(
        mode="w+", suffix=".log", prefix="server_", delete=False, encoding="utf-8",
        errors="replace")
    proc = subprocess.Popen(
        [binary, str(port), threads],
        stdout=logfile, stderr=subprocess.STDOUT,
    )

    socks = []
    try:
        wait_port(port, proc)

        # ---- 基本回显 ----
        s = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT)
        socks.append(s)
        welcome = recv_some(s)
        if not welcome:
            raise AssertionError("没有收到欢迎语")
        log(f"欢迎语 {len(welcome)} 字节")

        s.sendall(b"hello-reactor\n")
        echo = recv_some(s)
        if b"hello-reactor" not in echo:
            raise AssertionError(f"回显不正确，收到 {echo!r}")
        log("基本回显通过")

        # ---- stat 命令 ----
        s.sendall(b"stat\n")
        stat = recv_some(s)
        if b"conn" not in stat.lower() and b"127.0.0.1" not in stat:
            raise AssertionError(f"stat 响应异常: {stat!r}")
        log("stat 命令通过")

        # ---- 多连接并发回显 ----
        n = 20
        peers = []
        for i in range(n):
            p = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT)
            p.settimeout(TIMEOUT)
            recv_some(p, 2.0)                      # 吃掉欢迎语
            peers.append(p)
        socks.extend(peers)
        for i, p in enumerate(peers):
            p.sendall(f"peer-{i}\n".encode())
        ok = 0
        for i, p in enumerate(peers):
            data = recv_some(p, 3.0)
            if f"peer-{i}".encode() in data:
                ok += 1
        if ok != n:
            raise AssertionError(f"{n} 个并发连接只有 {ok} 个回显正确")
        log(f"{n} 个并发连接回显全部正确")

        # ---- 大消息（验证应用层 Buffer 的分片拼接）----
        payload = ("X" * 200000) + "\n"
        s.sendall(payload.encode())
        got = b""
        deadline = time.time() + TIMEOUT
        while len(got) < 200000 and time.time() < deadline:
            chunk = recv_some(s, 1.0)
            if not chunk:
                break
            got += chunk
        if len(got) < 200000:
            raise AssertionError(f"大消息回显不完整：只收到 {len(got)} / 200001 字节")
        log(f"200 KB 大消息回显完整（收到 {len(got)} 字节）")

        # ---- quit 优雅关闭：应该先收到告别语，再收到 EOF ----
        s.sendall(b"quit\n")
        tail = b""
        deadline = time.time() + TIMEOUT
        closed = False
        while time.time() < deadline:
            chunk = recv_some(s, 1.0)
            if chunk == b"":
                closed = True
                break
            tail += chunk
        if not closed:
            raise AssertionError("quit 之后连接没有被关闭")
        log(f"quit 优雅关闭通过（关闭前收到 {len(tail)} 字节告别语）")

        log("回显服务器全部验证通过")
        return 0

    except Exception as e:
        log(f"失败: {type(e).__name__}: {e}")
        return 1

    finally:
        for s in socks:
            try:
                s.close()
            except OSError:
                pass
        time.sleep(0.2)
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        logfile.close()
        dump_log(logfile.name, 30)


if __name__ == "__main__":
    sys.exit(main())
