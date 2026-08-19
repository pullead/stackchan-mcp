<#
StackChan 固件构建助手。

用法（在项目根目录）：
    .\build.ps1                 # 构建
    .\build.ps1 flash           # 构建并刷入 COM6
    .\build.ps1 monitor         # 只看串口
    .\build.ps1 flash monitor   # 刷入后接串口
    .\build.ps1 fullclean       # 清理

会自动跑 fetch_repos.py，确保 xiaozhi-esp32 已拉取且补丁已应用。
#>

# 注意：不要用 'Stop'。PowerShell 5.1 会把原生命令（git 等）写到 stderr 的
# 普通信息包装成 NativeCommandError，配合流重定向会直接中断脚本。
# 这里一律显式检查 $LASTEXITCODE。
$ErrorActionPreference = 'Continue'

$IDF      = 'C:\Espressif\frameworks\esp-idf-v5.5.5'
$FIRMWARE = Join-Path $PSScriptRoot 'StackChan\firmware'
$PORT     = if ($env:STACKCHAN_PORT_COM) { $env:STACKCHAN_PORT_COM } else { 'COM6' }

$env:IDF_TOOLS_PATH = 'C:\Espressif'
# export.ps1 用裸 `python` 推导虚拟环境名。系统 Python 是 3.12，而 ESP-IDF 的
# venv 建在 3.11.2 下（idf5.5_py3.11_env），不把 idf-python 顶到 PATH 最前会报
# "virtual environment idf5.5_py3.12_env not found"。官方 idf_cmd_init.bat 同理。
$env:PATH = 'C:\Espressif\tools\idf-python\3.11.2;' + $env:PATH

if (-not (Test-Path "$IDF\export.ps1")) { throw "找不到 ESP-IDF: $IDF" }
if (-not (Test-Path $FIRMWARE))         { throw "找不到固件目录: $FIRMWARE" }

Set-Location $FIRMWARE

# 依赖 + 补丁。fetch_repos.py 在补丁打不上时是静默跳过的，所以这里必须校验，
# 否则会带着未打补丁的源码一路编译到链接阶段才炸。
Write-Host "== 同步依赖与补丁 ==" -ForegroundColor Cyan
& 'C:\Espressif\tools\idf-python\3.11.2\python.exe' fetch_repos.py
if ($LASTEXITCODE -ne 0) { throw "fetch_repos.py 失败，退出码 $LASTEXITCODE" }
$patched = (git -C xiaozhi-esp32 status --porcelain | Measure-Object).Count
if ($patched -lt 7) {
    throw "xiaozhi-esp32 补丁未完整应用（只有 $patched 个文件被修改，应为 7）。检查 patches/xiaozhi-esp32.patch 是否为 LF 换行。"
}
Write-Host "   补丁 OK（$patched 个文件已修改）" -ForegroundColor Green

Write-Host "== 加载 ESP-IDF 环境 ==" -ForegroundColor Cyan
. "$IDF\export.ps1"

# 必须用 @() 强制成数组：单元素时 PowerShell 会解包成字符串，
# 而 splat 一个字符串会按字符枚举（build -> b,u,i,l,d）。
$idfArgs = @(if ($args.Count -gt 0) { $args } else { 'build' })
if ($idfArgs -contains 'flash' -or $idfArgs -contains 'monitor') {
    $idfArgs = @('-p', $PORT) + $idfArgs
}

Write-Host "== idf.py $($idfArgs -join ' ') ==" -ForegroundColor Cyan
& idf.py @idfArgs
exit $LASTEXITCODE
