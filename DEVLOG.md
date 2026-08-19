# 开发日志

> 2026-08-19，单日从调研到跑通。本文用于换机器继续开发时快速接手。

## 一、需求是怎么定下来的

初始诉求经过几轮澄清才收敛，中间推翻过三次方案，这些弯路值得记下来：

1. **最初想法**：参考 `tianyupaipai-cmd/stackchan-cloud-mcp`，上 VPS + Cloudflare Tunnel + OAuth，让手机 claude.ai 直连机器人。
   → **放弃**。那个仓库只是给上游 `kisaragi-mochi/stackchan-mcp` 上公网的一层运维补丁，OAuth 是作者自承的单租户玩具实现；而且真实需求只是本地打字控制，不值这个复杂度。

2. **第二版**：刷 `kisaragi-mochi/stackchan-mcp` 的固件。
   → **放弃**。不接受第三方固件。

3. **第三版**：改 `CONFIG_OTA_URL` 把设备引到自建服务器。
   → **放弃**。小智固件的服务端地址由 OTA 响应下发，改了 OTA 地址设备就离开小智云，**官方语音助手会没**，与「必须保留官方助手」冲突。

4. **最终方案（双通道）**：不抢官方服务端，给设备上**已有的** `McpServer` 单例再接一条局域网 HTTP 传输通道。小智云链路零改动。

### 最终确定的约束

- 可以刷固件，但必须是**在 m5stack/StackChan 官方源码基础上自己改的**，不接受第三方固件
- **必须保留官方小智语音助手**
- 目标：PC 的 Claude Code 里打字 → 机器人做动作/表情/显示文字
- 只做本地，不上 VPS / 域名 / Cloudflare / OAuth
- 第一版不做语音播报，不搬 HtSz 的传感器交互

## 二、几个决定性的调研发现

**官方固件就是 xiaozhi-esp32 的分支。** 串口启动日志里有 `[HAL] xiaozhi board init`、`SKU=m5stack-stack-chan`，OTA 查 `api.tenclass.net`，然后连 `mqtt.xiaozhi.me`。

**官方固件出厂即内置 MCP Server。** 启动日志里一整片 `MCP: Add tool: self.robot.*`。这意味着**不需要写任何设备控制逻辑**，是整个方案能这么轻的根本原因。

**设备身份靠 eFuse 里的出厂 MAC，不在 flash 里。** `espefuse summary` 确认密钥块全空、`CUSTOM_MAC` 全 0，只有 `MAC (BLOCK1) = 68:ee:8f:d7:44:5c`。小智服务端认 M5Stack 的 MAC 段 + SKU 直接放行，所以**从没输过验证码**，而且此前刷过官方恢复固件后助手依然可用。
→ 结论：刷固件不会弄丢官方助手，**前提是继续上报同样的 SKU 且不改 OTA 地址**。

**BluFi 配网只能配 WiFi。** `docs/blufi_zh.md` 确认没有 OTA 地址字段，所以「不刷固件、只改配置」这条路走不通。

## 三、实现要点

### 设备端

`firmware/main/hal/hal_mcp_local.cpp`（新增，约 280 行）

用 `esp_http_server` 监听 **8080**（避开小智配网门户的 80），处理 `POST /mcp`：

1. 取出原始 `id`，换成 `>= 0x40000000` 区间的本地 id
2. 登记 `本地id -> {信号量, 输出缓冲}`
3. 调 `McpServer::GetInstance().ParseMessage()`
4. 等信号量（超时 10 秒），把 `id` 换回去再返回

`tools/call` 是**异步**的（走 `app.Schedule()` 丢回主线程执行），所以必须用信号量等，不能指望同步返回。

### 小智内核补丁（约 15 行）

`McpServer::ParseMessage()` 是 public 可直接调，但 `ReplyResult` / `ReplyError` 是 private，且两者都固定走 `Application::GetInstance().SendMcpMessage(payload)` **发往小智云**。

补丁给 `McpServer` 加了一个 `LocalReplySink`，在两处 `SendMcpMessage` 之前插入拦截：

```cpp
if (local_reply_sink_ && local_reply_sink_(id, payload)) { return; }
```

sink 只认 `>= 0x40000000` 的 id。返回 `false` 时行为与原版逐字一致，**小智云链路零影响**。

### 三个非做不可的细节

- **id 必须改写**：小智的 `ParseMessage` 里有 `if (id == nullptr || !cJSON_IsNumber(id)) return;` —— 硬性要求数字 id，而 MCP 允许字符串 id。不改写的话 Claude Code 的请求会被静默丢弃。
- **`tools/list` 要带 `withUserTools: true`**：否则拿不到标 `[user]` 的工具（`reboot` / `get_system_info` / `upgrade_firmware` 等）。带上是 20 个，不带 13 个。
- **`tools/list` 有分页**：固件按 8000 字节切页，桥接必须循环跟 `nextCursor` 翻完再合并。

## 四、踩过的坑（按严重程度）

### 1. 自建固件会被小智云静默 OTA 覆盖（最险）

M5Stack 开源源码是 `PROJECT_VER "1.4.3"`，云端发布版是 **1.4.4**。设备一联网就：

```
Ota: Upgrading firmware from ...v1.4.4_m5stack-stack-chan_ota.bin
Ota: Writing to partition ota_1
```

重启后自建固件全没，而且**整个过程是静默的**，只会发现「功能怎么自己没了」。

**解法**：`firmware/CMakeLists.txt` 里把 `PROJECT_VER` 设成 `9.9.9`，OTA 检查恒返回 `Current is the latest version`。代价是收不到官方更新，要回官方就刷 `backup/` 的镜像。

### 2. httpd_start() 不能在网络栈起来之前调用

最初在 `main.cpp` 里于 `startXiaozhi()` 之前直接调用，结果**设备开机循环**：

```
assert failed: tcpip_send_msg_wait_sem /IDF/components/lwip/lwip/src/api/tcpip.c:454 (Invalid mbox)
```

当时的错误判断是「httpd 绑 0.0.0.0，不需要 netif 就绪」。**绑地址确实不需要，但 httpd 会经由 LwIP 的 tcpip 线程发消息，而那个线程要 `esp_netif_init()` 之后才存在。** 注意 `startXiaozhi()` 是不返回的，网络也是在它内部才连上。

**解法**：起一个任务轮询 `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")`，拿到非零 IP 再 `httpd_start`。

### 3. Windows core.autocrlf 让 M5Stack 自己的补丁打不上（上游 bug）

clone 后 `firmware/patches/xiaozhi-esp32.patch` 本身被转成 CRLF，`git apply` 失败。而 `fetch_repos.py` 在补丁打不上时**静默跳过整个补丁**，只打印一行就继续。

后果很隐蔽：没有该补丁，`assets.cc` 会引用被 CMakeLists 注释掉的 `emote_display.cc`，**要到链接阶段才报错**。

**解法**：仓库根加 `.gitattributes` 写 `*.patch -text`（见 `firmware-patch/02-gitattributes`）。`build.ps1` 里也加了硬校验：`fetch_repos.py` 跑完后必须正好 7 个文件被修改，否则直接报错退出。

### 4. 这台机器构建期有偶发故障（别浪费时间排查）

当天出现三次无法复现的故障：

- `git submodule` 死锁：20 分钟 CPU 只走了 0.14 秒，进程活着但完全不动
- 编译器报一个**不存在的路径**：`esp_adc/esp32s3/include/limits: Invalid argument`
- `ninja: fatal: CreateProcess: The parameter is incorrect. (is the command line too long?)`

后两个**重试即过**。第三个特别有迷惑性 —— ninja 那句「命令行太长」只是它附带的猜测，实测最长命令 914 字符、环境块 5199 字节，都远未到 32767 上限，**不是长度问题**。怀疑是杀软拦截大量短生命周期进程。

**遇到莫名其妙的构建错误，先重试一次再排查。**

### 5. esptool 读 flash 要加 --no-stub

ESP32-S3 原生 USB-Serial/JTAG 配 stub flasher 做大块读取会报 `Packet content transfer stopped`。加 `--baud 921600` 会加剧（USB CDC 本就不受波特率影响）。另外 `--flash-size` 不是 `read-flash` 的合法选项。

可用命令见 `backup/RESTORE.md`。16MB 约 14 分钟（约 19.5 KB/s）。

### 6. PowerShell 的三个坑

- **含中文的 `.ps1` 必须存成 UTF-8 with BOM**，否则 PS 5.1 按系统 ANSI（本机 CP932）解码，中文注释变乱码导致语法错误。
- **单元素数组会解包成字符串**，splat 时按字符枚举（`build` 变成 `b,u,i,l,d`，报 `ninja: unknown target 'b'`）。必须用 `@(...)` 强制成数组。
- 别用 `$ErrorActionPreference = 'Stop'` 配合流重定向：PS 5.1 会把原生命令（git 等）写到 stderr 的**正常信息**包装成 `NativeCommandError` 直接中断脚本。一律显式检查 `$LASTEXITCODE`。

## 五、验证结果

```
App version:      9.9.9
Ota: Current version: 9.9.9
Ota: Current is the latest version          <- 自动 OTA 已关闭
Ota: Running partition: ota_0
WifiStation: Got IP: 172.16.10.36
[HAL-MCP-LOCAL] waiting for network before starting local MCP endpoint
[HAL-MCP-LOCAL] local MCP endpoint listening on POST http://172.16.10.36:8080/mcp
MQTT: Connecting to endpoint mqtt.xiaozhi.me <- 官方语音助手正常
```

- 0 次异常重启、0 次 assert、0 次 OTA 下载
- `tools/list` 返回 20 个工具、0.25 秒
- `set_emotion` / `show_text` / `set_head_angles` / `set_led_color` / `get_head_angles` / `get_device_status` / `reboot` 均实测通过，约 0.1 秒
- 字符串 id（`"abc-字符串-id"`）能正确原样回显，说明 id 改写有效
- 调不存在的工具返回规范的 JSON-RPC error

**未验证**：没有实际用唤醒词跟官方语音助手完整对话过一轮，只确认了 MQTT 链路正常连接。

## 六、换机器怎么接着做

### 环境

| 项 | 版本 / 说明 |
|---|---|
| ESP-IDF | **v5.5.5**，Windows 官方 Offline Installer（1.62 GB） |
| Python | 桥接用系统 Python 3.x 即可；ESP-IDF 用它自带的 3.11.2 |
| 其它 | git、esptool（`pip install esptool`） |

不要用 WSL —— COM 口要走 usbipd，而刷机和读串口很频繁。

源码版本要求是 `>=5.5.2`（`dependencies.lock` 里全是下界，无上界），5.5.5 与官方固件所用的 5.5.4 同属 v5.5 补丁线，兼容。

**如果安装器卡在「在子模块中更新 fileMode」**：那一步对编译无影响，但它后面还有创建 Python 虚拟环境的步骤。当时的处理是杀掉卡死的 git 进程，然后手动补装：

```powershell
$env:IDF_TOOLS_PATH='C:\Espressif'
$py='C:\Espressif\tools\idf-python\3.11.2\python.exe'
$idf='C:\Espressif\frameworks\esp-idf-v5.5.5'
& $py "$idf\tools\idf_tools.py" --idf-path $idf --non-interactive install --targets=esp32s3
& $py "$idf\tools\idf_tools.py" --idf-path $idf --non-interactive install-python-env --features=core
```

注意 `export.ps1` 用裸 `python` 推导虚拟环境名。系统 Python 若是 3.12 会去找 `idf5.5_py3.12_env` 而报错，**必须先把 idf-python 顶到 PATH 最前**（`build.ps1` 已处理）。

### 拉源码打补丁

见 `firmware-patch/README.md`。

### 编译刷机

```powershell
.\build.ps1              # 构建
.\build.ps1 flash        # 构建并刷入
.\build.ps1 monitor      # 看串口
.\build.ps1 flash monitor
.\build.ps1 fullclean
```

COM 口默认 `COM6`，用 `$env:STACKCHAN_PORT_COM` 覆盖。

### 接入 Claude Code

改 `.mcp.json` 里的 `STACKCHAN_HOST` 为设备实际 IP，重启 Claude Code，允许项目级 MCP 服务。

> **换网络必做**：设备 IP 会变。建议在路由器上给 MAC `68:ee:8f:d7:44:5c` 绑定固定 IP。`.mcp.json` 里的 `args` 目前是绝对路径，换机器要改。

## 七、已知问题与后续方向

**已知问题**

- `.mcp.json` 用绝对路径，换机器必须改
- 设备 IP 硬编码，DHCP 换址后要改
- 局域网端点默认不鉴权（`LOCAL_MCP_TOKEN` 留了口子但没启用）
- `laughing` 和 `happy` 在固件里映射到同一张脸，`crying` 和 `sad` 同理。实际只有 6 张不同的脸：neutral / happy / angry / sad / sleepy / doubtful
- ESP32 开了 WiFi 省电（`wifi:pm start, type: 1`），首次请求偶发超时，ping 延迟能涨到 2 秒 —— 桥接里的重试是必要的

**可以做的**

- **语音播报**：PC 做 TTS 推音频到设备。需在固件加音频接收通道，且要处理与小智音频服务的资源竞争
- **传感器被动交互**（摸头 SI12T 三分区 / BMI270 摇晃拿起 / FT6336 屏幕手势）：官方固件没把传感器事件暴露成 MCP 工具，只能在固件里做。[mo-hantang/Stackchan-HtSz](https://github.com/mo-hantang/Stackchan-HtSz) 是同款 CoreS3 的 xiaozhi fork（MIT），这些都实现了，可以直接搬。它还有 21 种 emotion 到 LED 颜色的映射和定时早安播报
- **给 M5Stack 提 PR**：`.gitattributes` 修 CRLF 补丁问题，以及 `fetch_repos.py` 补丁失败时应该报错而不是静默跳过

## 八、参考

- [m5stack/StackChan](https://github.com/m5stack/StackChan) —— 官方固件源码（本项目的上游）
- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) —— 小智内核，官方固件锁在 v2.2.4。`docs/mcp-protocol_zh.md` 是设备侧 MCP 的协议说明
- [mo-hantang/Stackchan-HtSz](https://github.com/mo-hantang/Stackchan-HtSz) —— 传感器交互参考
- [kisaragi-mochi/stackchan-mcp](https://github.com/kisaragi-mochi/stackchan-mcp) —— 另一条路线，自带固件 + Python 网关，需替换官方固件
- [tianyupaipai-cmd/stackchan-cloud-mcp](https://github.com/tianyupaipai-cmd/stackchan-cloud-mcp) —— 上公网的运维补丁层，`fake_ota.py` 的思路有参考价值
