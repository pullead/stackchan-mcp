# 固件改动

本目录保存对 [m5stack/StackChan](https://github.com/m5stack/StackChan) 的全部改动。
上游源码 40MB 且是第三方仓库，不纳入本仓库，改动以补丁形式保存。

**基线 commit**：`b72b3ed`（Merge pull request #100 from m5stack/firmware-dev）

| 文件 | 说明 |
|---|---|
| `01-tracked-files.patch` | 对已跟踪文件的改动（7 个文件） |
| `02-gitattributes` | 新增到仓库根的 `.gitattributes` |
| `03-hal_mcp_local.cpp` | 新增文件，放到 `firmware/main/hal/` |

## 应用步骤

```bash
# 1. 克隆上游到本仓库根目录下的 StackChan/
git clone https://github.com/m5stack/StackChan.git StackChan
cd StackChan
git checkout b72b3ed

# 2. 先放 .gitattributes —— 必须在拉依赖之前！
#    否则 Windows 的 core.autocrlf 会把 patches/*.patch 转成 CRLF，
#    导致 fetch_repos.py 里的 git apply 失败，而它是静默跳过的。
cp ../firmware-patch/02-gitattributes .gitattributes

# 3. 确保 patches/ 下的补丁是 LF（若已被转成 CRLF 要转回来）
python -c "import io; p='firmware/patches/xiaozhi-esp32.patch'; d=io.open(p,'rb').read(); io.open(p,'wb').write(d.replace(b'\r\n',b'\n'))"

# 4. 应用本项目的改动
git apply ../firmware-patch/01-tracked-files.patch
cp ../firmware-patch/03-hal_mcp_local.cpp firmware/main/hal/hal_mcp_local.cpp

# 5. 拉取依赖并应用 M5Stack 自己的补丁
cd firmware
python fetch_repos.py
```

## 验证补丁是否真的生效

`fetch_repos.py` 在补丁打不上时**只打印一行就跳过**，不会报错。必须显式检查：

```bash
git -C xiaozhi-esp32 status --porcelain
```

应当**正好 7 个文件**被修改：

```
 M main/application.cc          <- M5Stack 原有
 M main/assets.cc               <- M5Stack 原有
 M main/assets.h                <- M5Stack 原有
 M main/boards/common/i2c_device.cc   <- M5Stack 原有
 M main/boards/common/i2c_device.h    <- M5Stack 原有
 M main/mcp_server.cc           <- 本项目追加的 reply sink
 M main/mcp_server.h            <- 本项目追加的 reply sink
```

少于 7 个说明补丁没打上，**继续编译会在链接阶段才报错**（`assets.cc` 引用被
CMakeLists 注释掉的 `emote_display.cc`）。

项目根的 `build.ps1` 已内置这项校验，正常走它构建即可。

## 改动清单

**新增**

- `firmware/main/hal/hal_mcp_local.cpp` —— 局域网 MCP HTTP 端点（`POST :8080/mcp`）
- 仓库根 `.gitattributes` —— `*.patch -text`，修 Windows CRLF 问题

**修改**

| 文件 | 改动 |
|---|---|
| `firmware/CMakeLists.txt` | `PROJECT_VER` 改 `9.9.9`，关闭小智云自动 OTA |
| `firmware/main/CMakeLists.txt` | `PRIV_REQUIRES` 加 `esp_http_server` |
| `firmware/main/Kconfig.projbuild` | 新增 `LOCAL_MCP_TOKEN`（可选鉴权，默认空） |
| `firmware/main/hal/hal.h` | public 区加 `mcp_local_server_init()` 声明 |
| `firmware/main/hal/hal_mcp.cpp` | 新增 `self.robot.set_emotion` 和 `self.robot.show_text` |
| `firmware/main/hal/board/stackchan_display.h` | 加 `SuppressIdleExpression()` 声明和恢复用定时器 |
| `firmware/main/stackchan/avatar/skins/default/speech_bubble.cpp` | 气泡重做：漫画风、按字数自动选上下位置、偏移投影、加粗尾巴 |
| `firmware/main/stackchan/avatar/skins/default/default.h` | 气泡类加投影与尾巴投影图层成员 |
| `firmware/main/hal/board/stackchan_display.cc` | 实现 `SuppressIdleExpression()`；`SetEmotion` 改表驱动，认全 21 个小智情绪词并配装饰器 |
| `firmware/main/main.cpp` | `startXiaozhi()` 前调用 `mcp_local_server_init()` |
| `firmware/patches/xiaozhi-esp32.patch` | 追加 4 个 hunk，给 `McpServer` 加 reply sink |

`firmware/dependencies.lock` 会因 ESP-IDF 版本自动变动（5.5.4 -> 5.5.5），属构建产物，
未纳入补丁。
