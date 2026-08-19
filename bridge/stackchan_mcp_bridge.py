#!/usr/bin/env python3
"""
StackChan 本地 MCP 桥接。

把 Claude Code 的 stdio MCP 调用转发到 StackChan 设备上的局域网 HTTP 端点
(POST http://<host>:8080/mcp)，设备那头是官方固件里已有的 McpServer。

设备只跟小智云说话的那条链路不受影响，这是并行的第二条通道。

环境变量：
    STACKCHAN_HOST      设备 IP，默认 172.16.10.36
    STACKCHAN_PORT      端口，默认 8080
    STACKCHAN_TOKEN     可选，对应固件的 CONFIG_LOCAL_MCP_TOKEN
    STACKCHAN_TIMEOUT   单次请求超时秒数，默认 15
    STACKCHAN_USER_TOOLS 是否请求 [user] 级工具（reboot 等），默认 1
"""

import json
import os
import sys
import urllib.error
import urllib.request

HOST = os.environ.get("STACKCHAN_HOST", "172.16.10.36")
PORT = os.environ.get("STACKCHAN_PORT", "8080")
TOKEN = os.environ.get("STACKCHAN_TOKEN", "")
TIMEOUT = float(os.environ.get("STACKCHAN_TIMEOUT", "15"))
USER_TOOLS = os.environ.get("STACKCHAN_USER_TOOLS", "1") not in ("0", "false", "no")

ENDPOINT = "http://{}:{}/mcp".format(HOST, PORT)

# 设备固件回的是这个版本；没有客户端要求时用它
DEFAULT_PROTOCOL = "2024-11-05"

# 桥接自己用的 id 区间，避免和转发下去的客户端 id 混淆
_internal_id = 900000


def log(msg):
    """诊断信息只能走 stderr —— stdout 是 MCP 通道。"""
    print("[stackchan-bridge] {}".format(msg), file=sys.stderr, flush=True)


def device_call(payload):
    """向设备发一条 JSON-RPC，返回解析后的响应 dict。失败抛异常。"""
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(ENDPOINT, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    if TOKEN:
        req.add_header("X-Auth-Token", TOKEN)
    with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
        body = resp.read()
    if not body:
        return {}
    return json.loads(body.decode("utf-8"))


def next_internal_id():
    global _internal_id
    _internal_id += 1
    return _internal_id


def list_all_tools():
    """把设备的分页 tools/list 全部翻完再合并。

    固件按 8000 字节切页，工具一多就会分页；不翻完 Claude Code 只能看到第一页。
    """
    tools = []
    cursor = ""
    seen_cursors = set()
    while True:
        params = {}
        if cursor:
            params["cursor"] = cursor
        if USER_TOOLS:
            params["withUserTools"] = True
        resp = device_call({
            "jsonrpc": "2.0",
            "id": next_internal_id(),
            "method": "tools/list",
            "params": params,
        })
        if "error" in resp:
            raise RuntimeError(resp["error"].get("message", "tools/list failed"))
        result = resp.get("result", {})
        tools.extend(result.get("tools", []))
        cursor = result.get("nextCursor", "")
        if not cursor:
            break
        if cursor in seen_cursors:
            log("nextCursor 出现回环，在 {} 处停止翻页".format(cursor))
            break
        seen_cursors.add(cursor)
    return {"tools": tools}


def send(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def reply(req_id, result):
    send({"jsonrpc": "2.0", "id": req_id, "result": result})


def reply_error(req_id, code, message):
    send({"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}})


def handle(msg):
    method = msg.get("method")
    req_id = msg.get("id")

    # 通知没有 id，也不需要回复
    if req_id is None:
        return

    if method == "initialize":
        # 本地应答握手：固件的 McpServer 只实现了 MCP 子集，
        # 直接把它的 initialize 透传给客户端未必满足期望。
        client_proto = (msg.get("params") or {}).get("protocolVersion") or DEFAULT_PROTOCOL
        reply(req_id, {
            "protocolVersion": client_proto,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "stackchan-local", "version": "0.1.0"},
        })
        return

    if method == "ping":
        reply(req_id, {})
        return

    try:
        if method == "tools/list":
            reply(req_id, list_all_tools())
            return

        if method == "tools/call":
            resp = device_call({
                "jsonrpc": "2.0",
                "id": next_internal_id(),
                "method": "tools/call",
                "params": msg.get("params", {}),
            })
            if "error" in resp:
                reply_error(req_id, -32000, resp["error"].get("message", "tool call failed"))
            else:
                reply(req_id, resp.get("result", {}))
            return

    except urllib.error.URLError as e:
        reply_error(req_id, -32001,
                    "连不上 StackChan ({}): {}。检查设备是否在线、IP 是否正确。".format(ENDPOINT, e))
        return
    except Exception as e:  # noqa: BLE001 - 桥接不能因为单条请求出错就退出
        reply_error(req_id, -32002, "{}: {}".format(type(e).__name__, e))
        return

    reply_error(req_id, -32601, "Method not found: {}".format(method))


def main():
    log("endpoint={} user_tools={}".format(ENDPOINT, USER_TOOLS))
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError as e:
            log("收到非法 JSON，已忽略: {}".format(e))
            continue
        try:
            handle(msg)
        except Exception as e:  # noqa: BLE001
            log("handle() 未捕获异常: {}: {}".format(type(e).__name__, e))


if __name__ == "__main__":
    main()
