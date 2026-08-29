# MOSS ONVIF (`moss-onvif`)

无板载摄像头的 MOSS 桌面助手板。视频走 moss-desktop 侧 ONVIF/RTSP。
固件身份：`type`/`name`/`board` = `moss-onvif`。

显示与开机动画对齐 `bread-compact-wifi-096lcd`：0.96 寸 ST7735S（160×80）+ 嵌入
`emote-assets.bin` 开机 splash，随后代码滚动背景替代原 LVGL UI。

## 硬件资源

- **主控**：ESP32-S3
- **屏幕**：ST7735S SPI 0.96"（160×80）
  - MOSI=GPIO41, SCK=GPIO42, CS=GPIO47, DC=GPIO39, RST=GPIO40, BL=GPIO21
  - 偏移默认 `(1,26)`，颜色反相 `INVON`
- **开机画面**：固件嵌入 `main/assets/moss/emote-assets.bin`
- **音频 Codec**：ES8311 + ES7210；功放 NS4150B，`CTRL` ← **GPIO48**
- **按键**：BOOT（GPIO0）
- **熄屏**：空闲 30 秒后关背光+面板
- **外设**：灯条/面板灯/眼灯/电机/红外（引脚见 `config.h` 的 `MOSS_*`）
- **摄像头**：无板载；moss-desktop 连接后按板名走 ONVIF
- **红外码表**：`storage` SPIFFS。分区表 `partitions/moss-desktop-16m.csv`（不能与 ov2640 的 v2 表 OTA 互刷）

## 构建方法

```bash
python3 scripts/build.py moss/moss-onvif --name moss-onvif
```

首次刷机请擦除后整片烧录：

```bash
idf.py -p PORT erase-flash flash
```

## 真机验收

- mDNS 发现后列表显示 `moss-onvif`
- HTTP 绑定 MQTT → `chat.wake` / 对话 relay
- 红外学习与导入导出、`device.config` 读写唤醒词
- moss-desktop 按板名走 ONVIF/RTSP，不要出现板载 MJPEG
- 不要用 OTA 从旧 `moss-desktop` 身份跨分区表升级
