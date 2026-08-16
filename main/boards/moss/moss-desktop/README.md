# MOSS 桌面助手 (Moss Desktop Assistant)

## 简介

MOSS 桌面助手固件身份（`type`/`name` = `moss-desktop`）。显示与开机动画对齐
`bread-compact-wifi-096lcd`：0.96 寸 ST7735S（160×80）+ 嵌入 `emote-assets.bin` 开机 splash，
随后代码滚动背景替代原 LVGL UI。

## 硬件资源

- **主控**：ESP32-S3
- **屏幕**：ST7735S SPI 0.96"（160×80）
  - MOSI=GPIO41, SCK=GPIO42, CS=GPIO47, DC=GPIO39, RST=GPIO40, BL=GPIO21
  - 偏移默认 `(1,26)`，颜色反相 `INVON`
- **开机画面**：固件嵌入 `main/assets/moss/emote-assets.bin`，播放 `start.eaf` 后进入代码滚动
- **音频 Codec**：ES8311 + ES7210；功放 NS4150B，`CTRL` ← **GPIO48**
- **按键**：BOOT（GPIO0）
- **熄屏**：空闲（无唤醒/对话）30 秒后关背光+面板；唤醒词或按键对话时自动亮屏
- **Moss MCP 外设**：灯条/面板灯/眼灯/电机/红外/API（引脚见 `config.h` 的 `MOSS_*`）
- **配置绑定**：未绑定外部云通道时，Wi-Fi 连上后自动打开局域网 HTTP `:5500` 并广播 mDNS（`moss-AABB.local`，服务 `_moss-http._tcp`）。桌面配置客户端写入 MQTT 后，板子改走 MQTT；收到 `bind.hello` 后关闭 HTTP。MQTT 连续失败 3 次会重新打开绑定页。
- **红外码表**：存在专用 `storage` SPIFFS 分区（256KB）。当前工程 `sdkconfig` 必须指向 `partitions/moss-desktop-16m.csv`。旧固件用的是 `partitions/v2/16m.csv`（没有 storage），**不能 OTA**，需要整片烧录分区表+固件。
- **外部 MQTT 文本唤醒**：绑定后 `ExternalMqttClient` 仍识别 `codesuccess` → 打开音频通道并 `SendTextChat`

## 构建方法

```bash
python3 scripts/build.py moss/moss-desktop --name moss-desktop
```

首次刷机或从旧 16m 分区表切换过来时，请擦除后整片烧录：

```bash
idf.py -p PORT erase-flash flash
```

烧录成功后启动日志应出现 `Found storage partition` 和 `SPIFFS mounted at /storage`。若只有 `No SPIFFS partition named 'storage'`，说明分区表仍是旧的。



