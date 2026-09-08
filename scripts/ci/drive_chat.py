#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CI 用：驱动第 11 章的聊天室服务器（select / poll / epoll 三种模式）并验证广播。

第 11 章的服务器是纯 TCP 行协议：
  - 客户端发一行（以 \\n 结尾）
  - 服务端把这一行广播给**其他**客户端（不回给发送者）
  - 客户端发 "quit" 断开

这个脚本做的事：
  1. 启动服务器子进程
  2. 等端口可连接
  3. 连 3 个客户端
  4. A 发一行 -> 断言 B 和 C 都收到，且 A 自己没收到
  5. B 再发一行 -> 断言 A 和 C 都收到
  6. C 发 quit -> 断言 C 被断开
  7. 关掉服务器，检查它没有异常退出

用法:
    python scripts/ci/drive_chat.py <服务器可执行文件> <端口> <模式>
例:
    python scripts/ci/drive_chat.py build/bin/Debug/ch11_multiplex 18890 epoll

退出码 0 表示全部通过。
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

TAG = "drive_chat"
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


def ci_error(msg):
    """在 GitHub Actions 里把失败原因输出成 annotation。
    这样即使拿不到完整日志（logs API 需要鉴权），也能通过公开的
    annotations API 看到具体失败原因。"""
    if os.environ.get("GITHUB_ACTIONS") == "true":
        one_line = str(msg).replace("", " ").replace("
", " ")[:900]
        print(f"::error::[{TAG}] {one_line}", flush=True)


def log(msg):
    print(f"    [drive_chat] {msg}", flush=True)


def wait_port(port, proc, timeout=10.0):
    """等服务器把端口监听起来。同时检查子进程是否已经挂了。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"服务器提前退出，退出码 {proc.returncode}")
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            s.close()
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"等待端口 {port} 超时")


def connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT)
    s.settimeout(TIMEOUT)
    return s


def expect_line(sock, expect, who):
    """读到包含 expect 的数据为止。"""
    buf = b""
    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            raise AssertionError(f"{who} 连接被关闭，但期望收到 {expect!r}")
        buf += chunk
        if expect.encode() in buf:
            return buf
    raise AssertionError(f"{who} 没有收到 {expect!r}，实际收到 {buf!r}")


def expect_nothing(sock, who, wait=0.4):
    """断言这个 socket 在 wait 秒内收不到东西（验证不回显给发送者）。"""
    sock.settimeout(wait)
    try:
        data = sock.recv(4096)
    except socket.timeout:
        sock.settimeout(TIMEOUT)
        return
    sock.settimeout(TIMEOUT)
    if data:
        raise AssertionError(f"{who} 本不该收到数据，却收到 {data!r}")


def expect_closed(sock, who):
    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        try:
            data = sock.recv(4096)
        except socket.timeout:
            continue
        if not data:
            return              # recv 返回 0 = 对端关闭，正确
    raise AssertionError(f"{who} 应该已被服务端断开，但连接仍然打开")


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2

    binary, port_s, mode = sys.argv[1], sys.argv[2], sys.argv[3]
    port = int(port_s)

    if not os.path.exists(binary):
        log(f"找不到可执行文件: {binary}")
        ci_error(f"找不到可执行文件: {binary}")
        return 1

    # 必须转成绝对路径并规范化分隔符：Windows 的 CreateProcess 不接受
    # "build/bin/Debug/x.exe" 这种带正斜杠的相对路径（os.path.exists 却认它，
    # 所以这个坑很隐蔽 —— 检查通过但启动失败）。
    binary = os.path.normpath(os.path.abspath(binary))

    log(f"启动 {binary} {port} {mode}")
    logfile = tempfile.NamedTemporaryFile(
        mode="w+", suffix=".log", prefix="server_", delete=False, encoding="utf-8",
        errors="replace")
    proc = subprocess.Popen(
        [binary, str(port), mode],
        stdout=logfile, stderr=subprocess.STDOUT,
    )

    clients = []
    try:
        wait_port(port, proc)
        log("端口已就绪")

        a = connect(port); clients.append(("A", a))
        b = connect(port); clients.append(("B", b))
        c = connect(port); clients.append(("C", c))
        time.sleep(0.3)                     # 让服务端把三个连接都注册好
        log("3 个客户端已连接")

        # A 发一行，B/C 应该收到，A 自己不该收到
        a.sendall(b"hello-from-A\n")
        expect_line(b, "hello-from-A", "B")
        expect_line(c, "hello-from-A", "C")
        expect_nothing(a, "A（发送者）")
        log("广播验证通过：B/C 收到，A 未收到自己的消息")

        # 换 B 发，验证不是只有第一个连接能广播
        b.sendall(b"reply-from-B\n")
        expect_line(a, "reply-from-B", "A")
        expect_line(c, "reply-from-B", "C")
        log("反向广播验证通过")

        # 一次发多行（粘包），应该被拆成两条分别广播
        a.sendall(b"line1\nline2\n")
        got = expect_line(b, "line2", "B")
        if b"line1" not in got:
            raise AssertionError(f"粘包未被正确拆分，B 收到 {got!r}")
        log("粘包拆分验证通过（一次发两行，都被广播）")

        # quit 断开
        c.sendall(b"quit\n")
        expect_closed(c, "C")
        log("quit 命令断开连接验证通过")

        # 剩下两个连接还能正常通信
        a.sendall(b"still-alive\n")
        expect_line(b, "still-alive", "B")
        log("一个客户端退出后其余连接仍正常")

        log(f"模式 {mode} 全部验证通过")
        return 0

    except Exception as e:
        log(f"失败: {type(e).__name__}: {e}")
        ci_error(f"{type(e).__name__}: {e}")
        return 1

    finally:
        for _, s in clients:
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
