# stackchan-mcp

让 **M5Stack StackChan（CoreS3）** 在保留官方小智语音助手的同时，接受来自 PC 上
Claude Code 的文字指令 —— 转头、表情、LED、拍照、屏幕文字等。

```
                    ┌──► mqtt.xiaozhi.me            官方语音助手（唤醒词对话，原样保留）
StackChan ──────────┤
   自建固件          └──► :8080/mcp ──► 桥接 ──► Claude Code   打字控制
```

## 这是怎么做到的

M5Stack 的官方固件本身就是 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的分支，
**出厂就内置了一个 MCP Server**，注册好了 `self.robot.set_head_angles`、`self.camera.take_photo`
等一整套工具，只不过这些工具原本只对小智云开放。

所以这个项目**不需要重新实现任何设备控制逻辑**，只做两件事：

1. 给设备上已有的 `McpServer` 单例**再接一条传输通道** —— 局域网 HTTP 端点 `POST :8080/mcp`
2. PC 上放一个 stdio MCP 桥接，把 Claude Code 的调用转发过去

小智云那条链路一行代码没动，两条通道互不干扰。

## 快速开始

已经有编译环境的话：

```powershell
# 1. 拉取 M5Stack 官方源码并打上本项目的补丁
#    详见 firmware-patch/README.md

# 2. 编译并刷入（COM 口默认 COM6，可用 $env:STACKCHAN_PORT_COM 覆盖）
.\build.ps1 flash

# 3. 看串口确认端点起来了
.\build.ps1 monitor
#    应出现：[HAL-MCP-LOCAL] local MCP endpoint listening on POST http://<IP>:8080/mcp
```

然后把 `.mcp.json` 里的 `STACKCHAN_HOST` 改成设备实际 IP，重启 Claude Code 即可。

从零开始搭环境请看 **[DEVLOG.md](DEVLOG.md)**，里面有完整的踩坑记录。

## 目录

| 路径 | 说明 |
|---|---|
| `firmware-patch/` | 对 m5stack/StackChan 的改动（补丁 + 新增文件） |
| `bridge/stackchan_mcp_bridge.py` | PC 侧 stdio MCP 桥接，仅用标准库 |
| `build.ps1` | 构建助手，含补丁完整性校验 |
| `.mcp.json` | Claude Code 的 MCP 注册 |
| `backup/RESTORE.md` | 出厂固件备份与还原说明 |
| `DEVLOG.md` | 开发日志、设计决策、全部踩坑记录 |

## 可用工具（20 个）

设备自带 18 个 + 本项目新增 2 个：

- **新增**：`self.robot.set_emotion`、`self.robot.show_text`
- 头部：`set_head_angles` / `get_head_angles`
- 灯光：`set_led_color`
- 相机：`take_photo`
- 屏幕：`set_brightness` / `set_theme` / `get_info` / `snapshot` / `preview_image`
- 音频：`audio_speaker.set_volume`
- 提醒：`create_reminder` / `get_reminders` / `stop_reminder`
- 系统：`get_device_status` / `get_system_info` / `reboot` / `upgrade_firmware` / `assets.set_download_url`

实测响应约 0.1 秒。

## 注意

- 固件版本号被**故意设成 `9.9.9`** 以永久关闭小智云的自动 OTA，否则自建固件会被云端
  静默覆盖。代价是收不到官方更新。原因详见 DEVLOG。
- 局域网端点**默认不鉴权**，仅适用于可信内网。需要的话在
  `firmware/main/Kconfig.projbuild` 里配 `LOCAL_MCP_TOKEN`。
- 机器人带麦克风和摄像头，注意隐私。

## 许可

固件改动部分遵循上游 [m5stack/StackChan](https://github.com/m5stack/StackChan)（MIT）
与 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的许可。本仓库自有代码 MIT。
