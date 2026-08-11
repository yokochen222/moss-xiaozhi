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
- **Moss MCP 外设**：灯条/面板灯/眼灯/电机/红外/API（引脚见 `config.h` 的 `MOSS_*`）
- **外部 MQTT 文本唤醒**：`ExternalMqttClient` 订阅 `codesuccess` → 打开音频通道并 `SendTextChat`

## 构建方法

```bash
python3 scripts/build.py moss/moss-desktop --name moss-desktop
```

## 参考



