# 开发日志

> 2026-08-19 ~ 08-20。本文用于换机器继续开发时快速接手。
>
> 第四节「踩过的坑」是重点，14 条全部是实测踩出来的，不看会重复浪费时间。

## 开发时间线

**第一天（08-19）：从调研到跑通**

推翻三次方案后定下双通道架构（详见第一节），当天完成：备份出厂固件、装 ESP-IDF、
写设备端 HTTP 端点和小智内核 reply sink 补丁、写 PC 桥接、编译刷机、20 个工具全部
实测通过。中途踩到自建固件被小智云静默 OTA 覆盖、`httpd_start()` 早调导致启动循环、
Windows CRLF 让上游补丁静默失效三个大坑。

**第二天（08-20）：表情、动作、气泡**

- **表情跟着聊天内容走**。发现语音链路本来就通，问题是词表对不上——M5Stack 只认
  8 个词而小智云端会发 21 个，13 个落空全回落 neutral。改成表驱动 + 5 个装饰器
  （爱心/怒气/汗滴/脸红/晕眩），并把待机动画抑制内置进 `SetEmotion`。
- **动作搬到设备端**。新增 `nod` / `shake_head` / `look_around`。PC 侧连发做不出
  快节奏（3 下点头 9 秒且逐次变慢），搬到设备后 2.6 秒。
- **气泡重做成漫画风**。白气泡 + 偏移投影 + 圆角分四档，按字数自动选上下位置。
- **`sc:` 前缀**。MCP 服务提升到全局作用域，任意项目里 `sc: xxx` 都能指挥机器人，
  配置见 `docs/global-setup.md`。

第二天的坑集中在第四节 7~14 条：待机动画会覆盖表情、弹簧动画不能当节奏判据、
装饰器要显式 `setPosition` 才显示、`screen.snapshot` 颜色不可信、LVGL 的 shadow
语义和缩放锚点、字体只能用 20px。


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

### 7. 表情设了会被待机动画覆盖（不是 bug，是设计冲突）

`set_emotion` 返回 true、视觉上也确实变了，但**几秒后就被改回去**。

原因：小智助手空闲时，`stackchan_display.cc` 会挂上 `IdleExpressionModifier`
（见 `stackchan/modifiers/idle_expression.h`）。它每 2-6 秒随机动一次五官：

- 70% 概率位移眼睛(±20px)和嘴巴
- 10% 概率歪嘴
- **20% 概率 `reset_to_neutral()`** —— 把眼睛 size、嘴巴 rotation/weight 全部归零

而表情正是靠这些属性表现的。机器人大部分时间是空闲的，所以表情总是活不过几秒。
**这是官方固件的既定行为，不是缺陷** —— 我们的 MCP 工具在跟它抢。

**解法**：给 `StackChanAvatarDisplay` 加 `SuppressIdleExpression(seconds)`，
临时摘掉这个修饰器，用 esp_timer 到点自动恢复。`set_emotion` 加 `hold_seconds`
参数（默认 15）调它。

**没有用 `avatar.setModifyLock(true)`**：那个虽然一行就能挡住待机动画，但
`BlinkModifier` 也检查同一个锁，会把眨眼一起停掉，脸看起来像死了。

顺带 `show_text` 也补了 `duration_seconds`（默认 8 秒自动清空）——
原来设完就永久留在屏幕上，不会自己消失。

### 8. 验证视觉效果的正确方法

`self.screen.snapshot` 能把屏幕 JPEG 传到指定 URL，**在 PC 上起个接收服务就能
真的看到屏幕**，不用靠肉眼观察和口头描述。这个能力应该早点用上。

两个坑：

- 设备用 `multipart/form-data` + **`Transfer-Encoding: chunked`** 上传，
  接收端只读 `Content-Length` 会拿到 0 字节。要按 chunked 解析，或者干脆
  裸 socket 收完再从字节流里按 `ÿØ` / `ÿÙ` 抠 JPEG。
- 工具**总是返回 true**。上传失败也返回 true，只有串口日志
  （`MCP: Upload snapshot N bytes to ...` 和 `Snapshot screen result: ...`）
  才能确认到底成没成。

**比对图片时要避开眨眼的干扰。** 直接做整图像素差会被眨眼主导，得出错误结论
（实测因此误判过一次「保持未生效」）。可靠指标是**白色像素的重心坐标**：
待机动画靠位移五官制造变化，会明显移动重心；而眨眼只改变面积、基本不动重心。

实测数据（angry 表情，每 3 秒采样一次，共 6 次）：

| | x 漂移 | y 漂移 |
|---|---|---|
| `hold_seconds=60` | 0.22 px | 3.44 px |
| `hold_seconds=0` | 2.58 px | 10.89 px |

横向差 12 倍。残留的纵向漂移是眨眼造成的，符合预期。

### 9. 弹簧动画不能拿来当动作节奏的判据

给点头/摇头做设备端动作时，最初用 `motion.isMoving()` 轮询「等舵机到位再反向」。
结果 3 下点头要 10 秒。

原因：`Servo::isMoving()` 是 `_angle_anim.done() == false || is_moving_impl()`，
而 `_angle_anim` 是 **smooth_ui_toolkit 的弹簧动画**，渐近收敛、尾巴很长。等它彻底
静止每段要 1 秒以上，7 段（3 下点头 + 回位）刚好凑满 7×1200ms 超时。

**解法**：改成按**位置容差**判断，走到目标附近（容差取幅度的 1/4）就继续下一段，
不等弹簧收尾。改完 3 下点头 2622 ms，实测数据由设备自己打日志给出。

顺带一提，动作**必须放在设备端跑**。PC 侧逐条发 `set_head_angles` 做不出快节奏：
设备开了 WiFi 省电，密集请求延迟会累积恶化，实测同样 3 下点头要 9 秒且逐次变慢
（1.19s → 3.42s → 7.97s）。

### 10. 装饰器创建后不显示，要显式 setPosition 一下

给表情配 heart / angry / sweat / shy / dizzy 五个装饰器时，shy 和 dizzy 正常显示，
heart / angry / sweat 完全画不出来（像素统计确认不是看漏）。

三个失败的默认位置 y 都在 -70 附近，成功的是 -16 和 28，一度以为是那块区域被裁掉。
但改完位置后发现 sweat 的动画每 700ms 会把 x 重置回默认值，它照样显示了——**说明
原位置本身没问题**。

真正起作用的是构造后多调了一次 `setPosition()`，推测是它触发了 LVGL 的重绘失效标记，
否则对象虽然创建了但所在区域没被标记重绘。**这个机制未经证实**，只是比原假设更符合观察。
做法是先构造、`setPosition()`、再 `addDecorator()`。

### 11. screen.snapshot 的颜色不可信（RGB565 字节序）

设备端 `SnapshotToJpeg` 读帧缓冲时字节序反了，截图里的颜色全是错的。
实测爱心源码写死 `0xE13232`（红），实机显示红色，截图里是绿色。

验算对得上：`0xE13232` → RGB565 `0xE186` → 字节交换 `0x86E1` → 解回 RGB 是 (128,220,8)，
绿色。**形状、位置、有无都是准的，只有颜色不能信。**

不要试图在 PC 侧反变换——那是位级操作，撑不过 JPEG 有损压缩（实测还原不出红色）。
要修得改 `xiaozhi-esp32/main/display/lvgl_display/lvgl_display.cc`，但这只影响验证工具，
没修。

### 12. 气泡重做：位置、造型、以及我搞错的几件事

把气泡从「嘴下方单行横向滚动」改成漫画风、按字数自动选位置。过程中错了三次，
都值得记：

**错误一：以为下方空间不够。** 原气泡在屏幕 194..246（底部 6px 被切掉），我据此
认为下方放不下，把气泡整个挪到脸上方。实际嘴巴中心在 146，到屏幕底有 93px，
原设计只是没用满。**但结论歪打正着**：嘴巴说话时会张大（最大 60x50）并偏移 ±16，
最坏情况底边到 187，真正可用只剩 53px —— 下方确实只能单行。官方用单行 +
`SCROLL_CIRCULAR` 是自洽的设计。

**错误二：否掉了用户「按长度切换位置」的提议。** 我的理由是「临界点会来回跳」，
这个理由不成立：文字不是连续变化的量，每条消息定下位置后就固定到清除，不存在
抖动。绕了一圈还是回到按长度切换。

**错误三：想当然地照搬参考图的黑描边。** 参考图底色是白的所以黑边显眼；这块屏幕
**底色就是黑的，黑描边等于隐形**。立体感只能靠白气泡本身 + 偏移投影。

### 13. LVGL 的两个坑：shadow 语义和缩放锚点

**`shadow_width` 是模糊半径，不是阴影尺寸。** 想要参考图那种硬边偏移色块，设
`setShadowWidth(0)` 等于不画阴影。正确做法是用一个独立 Container 铺在下面、
偏移几像素，尺寸和圆角跟主气泡同步。

**`setScale` 放大图片是以中心为基准向四周扩展的**，对象坐标不变、绘制内容溢出
原始边框。1.5 倍下一个 32px 高的图上下各多出 8px，定位时不补这段偏移，尾巴就会
偏。公式里要算 `(draw_h - h) / 2`。

另外**多行换行和 `SCROLL_CIRCULAR` 互斥** —— 后者是单行横向滚动，要换行就得用
`LV_LABEL_LONG_MODE_WRAP`，长文本超出只能截断。

### 14. 字体：能用的最大字号就是 20px

想给气泡换更小/更大的字号时摸清的账：

- 实际在用的是**资源分区里的 `font_puhui_common_20_4`**（完整常用字集），不是
  `BUILTIN_TEXT_FONT` 宏指向的编译期符号。一度换成编译进固件的
  `font_puhui_basic_16_4`，结果满屏缺字——那个只有 **801 个字形**。
- 编译进固件的完整版 `font_puhui_16_4` 有 6650 字形够用，但要多占约 860KB，
  而 app 分区只剩 1.29MB。
- 想要 30px 的话：cbin 版 `font_puhui_common_30_4` 是 2.38MB，assets 分区只剩
  1.81MB，**装不下**；除非把 20px 整个换掉（换后 3.39MB 能装下），但那样全系统
  字体都变大，而且气泡容量从两行 28 字掉到两行 18 字。

结论：20px 是能装下的最大档，维持现状。

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
- `tools/list` 返回 **23 个工具**（设备自带 18 + 本项目新增 5）、0.25 秒
- `set_emotion` / `show_text` / `set_head_angles` / `set_led_color` / `get_head_angles` / `get_device_status` / `reboot` 均实测通过，约 0.1 秒
- 字符串 id（`"abc-字符串-id"`）能正确原样回显，说明 id 改写有效
- 调不存在的工具返回规范的 JSON-RPC error

第二天补充验证：

- 5 个装饰器全部截图确认可见（heart / angry / sweat / shy / dizzy）
- `set_emotion` 的 `hold_seconds` 有效：重心漂移 hold=60 时 0.22px，hold=0 时 2.58px，差 12 倍
- 设备端动作实测（设备自己打的耗时日志）：点头 3 下 2622ms、摇头 2248ms、张望 2991ms
- 气泡上下两个位置、四档大小均截图确认

**仍未验证**：没有实际用唤醒词跟官方语音助手完整对话过一轮，只确认了 MQTT 链路
正常连接。**表情映射和气泡重做在语音对话时同样生效，但只有实际对话才能确认云端
agent 会不会下发情绪**——不同 agent 配置不同，如果聊下来表情不动，先去小智控制台
看 agent 设置，不一定是固件问题。

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
- `set_emotion` 的 `hold_seconds` 到期后表情会回归待机动画，这是刻意设计；需要长期保持就传大一点的值（上限 300 秒）
- ESP32 开了 WiFi 省电（`wifi:pm start, type: 1`），首次请求偶发超时，ping 延迟能涨到 2 秒 —— 桥接里的重试是必要的
- 固件新增工具后**必须重启 Claude Code**，MCP 工具列表是会话启动时加载的
- `screen.snapshot` 无论成败都返回 `true`，只能靠串口日志确认是否真的上传了
- `screen.snapshot` 的**颜色不可信**（RGB565 字节序，见第四节第 11 条），形状位置是准的
- 气泡上限 28 个汉字，超出截断；下方只能单行、上方最多两行，这是屏幕空间的硬限制
- 全局 `sc:` 配置在仓库外（`~/.claude.json` + `~/.claude/CLAUDE.md`），换机器要按 `docs/global-setup.md` 重做

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
