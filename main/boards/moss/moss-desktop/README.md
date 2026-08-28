# MOSS 桌面助手 (Moss Desktop Assistant)

## 简介

MOSS 桌面助手固件身份（`type`/`name` = `moss-desktop`）。显示与开机动画对齐
`bread-compact-wifi-096lcd`：0.96 寸 ST7735S（160×80）+ 嵌入 `emote-assets.bin` 开机 splash，
随后代码滚动背景替代原 LVGL UI。聆听/说话时的「检测信号」弹窗右侧示波器跟 codec 已有 PCM
包络走（麦/喇叭各一条无锁环，不另开 I2S），弹窗关闭即停算。

## 硬件资源

- **主控**：ESP32-S3
- **屏幕**：ST7735S SPI 0.96"（160×80）
  - MOSI=GPIO41, SCK=GPIO40, CS=GPIO42, DC=GPIO39, RST=NC（绑芯片 EN）
    （原理图 Pin3/4 标 MOSI/SCK，实物模组为 SCL/SDA，软件已对调）
  - 背光：PCA9685 LED0（I2C @ 0x40，与音频共用 SDA/SCL）
  - 偏移默认 `(1,26)`，颜色反相 `INVON`
- **开机画面**：固件嵌入 `main/assets/moss/emote-assets.bin`，播放 `start.eaf` 后进入代码滚动
- **音频 Codec**：ES8311 + ES7210；功放 NS4150B EN ← **PCA9685 ch1**
- **摄像头**：OV2640 DVP（按需启停，JPEG QVGA）
  - XCLK=5 PCLK=7 VSYNC=3 HREF=46；D0..D7=16/18/8/17/15/6/4/9
  - SCCB 复用 I2C IO1/IO2；PWDN=PCA9685 ch2（低电平工作，上电保持）
- **按键**：BOOT（GPIO0）
- **PCA9685**：视枢 ch3–5、瞳光 ch6、流光 ch8–12、舷灯/锚灯 ch13–15
- **双轴云台**：74HC595（SER=21, RCK=47, SCK=48）→ 2×ULN2003 → 24BYJ-48；MCP `self.gimbal.control`
  - 停转/上电强制线圈关断，避免常通发烫
- **红外**：GPIO17/18 已给 DVP，当前 IR UART 禁用（引脚 NC）
- **外部 MQTT 文本唤醒**：`ExternalMqttClient` 订阅 `codesuccess` → 打开音频通道并 `SendTextChat`

## 构建方法

```bash
python3 scripts/build.py moss/moss-desktop --name moss-desktop
```

## 参考

- 外设/云台源：`moss-xiaozhi` 的 `boards/bread-compact-wifi-096lcd` + `main/mcp`
- 板子添加指南：`docs/custom-board.md`
