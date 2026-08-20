# 全局配置：让 `sc:` 在任意项目里可用

默认情况下 MCP 服务只注册在本项目的 `.mcp.json` 里，换个项目就用不了。
要在**任何项目**的对话框里用 `sc:` 前缀指挥机器人，需要两步全局配置。

这两处都在仓库外（`~/.claude.json` 和 `~/.claude/CLAUDE.md`），换机器不会自动带过来，
所以内容备份在这里。

## 一、把 MCP 服务提升到全局作用域

```bash
claude mcp add -s user stackchan \
  -e STACKCHAN_HOST=172.16.10.36 \
  -e STACKCHAN_PORT=8080 \
  -- python C:/Users/tei_s/Documents/stackchan/bridge/stackchan_mcp_bridge.py
```

> **注意 `-e` 是可变参数**，会把紧跟其后的第一个非 `-e` 参数也吞成环境变量。
> 服务名必须写在 `-e` 之前（`claude mcp add -s user stackchan -e KEY=V ...`），
> 官方帮助里的示例顺序在这里会报 `Invalid environment variable format`。

写入 `~/.claude.json` 的 `mcpServers`，与其它全局服务并列。验证：

```bash
claude mcp list
```

项目里的 `.mcp.json` 可以保留（作为仓库文档），同名服务不同作用域时
Claude Code 按优先级取一个，不会出现重复工具。

## 二、把 `sc:` 约定写进全局 CLAUDE.md

追加到 `~/.claude/CLAUDE.md`（**追加，不要覆盖**，那里通常已有其它个人规范）。
以下是完整内容：

---

```markdown
# StackChan 机器人

用户有一台 M5Stack StackChan 桌面机器人，通过全局 MCP 服务 `stackchan` 控制
（工具名 `mcp__stackchan__*`）。它在任何项目里都可用。

## sc: 前缀

用户在**任意项目**的对话框里输入以 `sc:` 开头的内容时，那是给机器人的指令：

- `sc: 转头看左边` -> 调 `set_head_angles`
- `sc: 点三下头` -> 调 `nod`
- `sc: 开心一下` -> 调 `set_emotion`
- `sc: 今天天气不错` -> 当成闲聊，配合适的表情，把回应放到气泡上

`sc:` 后面是自然语言，不是固定命令格式，按意思翻译成工具调用即可。
执行完简短回一句就行，不用长篇解释调了哪些工具。

**只有带 `sc:` 前缀时才操作机器人。** 其余时候正常做手头的开发工作，不要主动
去动它 —— 用户可能正在专注写代码。

## 操作约定

**气泡摘要**：执行 `sc:` 指令后，把结果压成一句话用 `show_text` 送到气泡上。
上限 28 个汉字，超出会被截断。位置由固件按字数自动选（不超过 12 字在嘴下方
单行，更多则在脸上方两行），不用管。不要往气泡里塞代码、路径或链接。

**表情**：只在语气有明显变化时调 `set_emotion`（完成用 `happy`、出错用 `sad`、
被夸用 `loving`、困惑用 `confused`、思考用 `thinking`），平时保持 `neutral`。
不要每轮都变，那样很聒噪。默认保持 15 秒后自然回到待机动作。

**连贯动作**用设备端工具 `nod` / `shake_head` / `look_around`，不要在 PC 侧循环
发 `set_head_angles` —— 设备开了 WiFi 省电，密集请求延迟会累积恶化（实测点头
3 下：设备端 2.6 秒，PC 侧连发 9 秒且逐次变慢）。同一时刻只能跑一个动作。

**不要主动设 LED 颜色。** 两侧灯带是小智语音助手的状态指示（听=绿、待机=灭、
说=蓝），由固件驱动。随手设一个颜色会一直亮着不走。除非用户明确要求改灯色。

**不要在转头指令里顺带塞 `pitch`。** 它默认 `-9999` 表示「不改变」。用户定的
默认俯仰角是 8，塞别的值会让机器人显得在仰头。只有明确说抬头/低头时才动。

**MCP 操作不发声**，只在屏幕显示文字。表情、转头、LED、`show_text`、拍照本来
就静默；唯一会发声的是 `create_reminder`，只在用户明确要求设提醒时才用。
不要为此调整设备音量，音量归小智语音助手用。

## 注意

设备 IP 写死在全局 MCP 配置里（`~/.claude.json` 的 `STACKCHAN_HOST`）。换网络或
DHCP 换址后要改。固件源码和开发日志在 `C:\Users\tei_s\Documents\stackchan`。
```

---

## 三、生效

**重启 Claude Code**。全局 MCP 服务和 `CLAUDE.md` 都是会话启动时加载的。

之后在任何项目里：

```
sc: 点三下头
sc: 开心一下
sc: 拍张照看看前面是什么
```

## 换机器要改什么

| 项 | 说明 |
|---|---|
| `STACKCHAN_HOST` | 设备 IP。建议在路由器给 MAC `68:ee:8f:d7:44:5c` 绑固定 IP |
| 桥接脚本路径 | `claude mcp add` 里的绝对路径要改成新机器上的仓库位置 |
| `CLAUDE.md` 末尾的仓库路径 | 同上 |
