# 出厂固件备份与还原

`stackchan-v1.4.4-full.bin` —— 2026-08-19 从设备完整读出的 16MB flash 镜像。

- 官方固件 v1.4.4（ESP-IDF v5.5.4，编译于 2026-07-10）
- 含 bootloader / 分区表 / ota_0 应用 / assets / **NVS（WiFi 凭据、已保存设置）**
- 设备 MAC `68:ee:8f:d7:44:5c`（烧在 eFuse，与本文件无关，刷什么都不会变）

## 完整还原

```
python -m esptool --port COM6 --no-stub write-flash 0 backup/stackchan-v1.4.4-full.bin
```

连 WiFi 凭据一起回到备份那一刻的状态。

## 只还原应用、保留当前 NVS

```
python -m esptool --port COM6 --no-stub write-flash 0x20000 backup/ota_0.bin
```

需要先切出 ota_0 分区：

```
python -c "import io; d=io.open('backup/stackchan-v1.4.4-full.bin','rb').read(); io.open('backup/ota_0.bin','wb').write(d[0x20000:0x20000+0x4f0000])"
```

## 注意

- **必须加 `--no-stub`**。ESP32-S3 的原生 USB-Serial/JTAG 配 stub flasher 传大块数据会报
  `Packet content transfer stopped`。也不要加 `--baud`，USB CDC 不受波特率影响。
- 全片读写约 14 分钟（~19.5 KB/s）。
- **不要动 eFuse。** 一次性熔丝，写错不可逆，而设备身份（免验证码激活小智）就靠里面的出厂 MAC。
- M5Stack 官网也有出厂固件恢复包，这份备份是额外保险。
