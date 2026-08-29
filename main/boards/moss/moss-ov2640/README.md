# MOSS OV2640 (`moss-ov2640`)

板载 OV2640 DVP 摄像头的 MOSS 桌面助手板。固件身份：`type`/`name`/`board` = `moss-ov2640`。
moss-desktop 按该板名走局域网 `:5500/camera`、云台与人脸追踪。

## 硬件资源

- **主控**：ESP32-S3
- **屏幕**：ST7735S SPI 0.96"（160×80）
  - MOSI=GPIO41, SCK=GPIO40, CS=GPIO42, DC=GPIO39, RST=NC, BL=PCA9685 ch0
- **音频 Codec**：ES8311 + ES7210；功放 NS4150B EN ← PCA9685 ch1
- **摄像头**：OV2640 DVP（按需启停，JPEG QVGA）
  - XCLK=IO4 PCLK=7 VSYNC=3 HREF=46；D0..D7=16/18/8/17/15/6/4/9
  - SCCB 复用 I2C IO1/IO2；PWDN=PCA9685 ch2
- **PCA9685**：背光/功放/PWDN/眼电机/灯效
- **双轴云台**：74HC595（SER=21, RCK=47, SCK=48）
- **红外**：UART2 TX=GPIO10 RX=GPIO11
- **分区表**：`partitions/v2/16m_moss_desktop.csv`（ota 槽 0x5A0000，含人脸模型；不能与 onvif 表 OTA 互刷）

## 构建方法

```bash
python3 scripts/build.py moss/moss-ov2640 --name moss-ov2640
```

首次刷机请擦除后整片烧录：

```bash
idf.py -p PORT erase-flash flash
```

## 真机验收

- mDNS 发现后列表显示 `moss-ov2640`
- HTTP 绑定 MQTT → `chat.wake` / 对话 relay
- 红外学习与导入导出、`device.config` 读写唤醒词
- moss-desktop 按板名走 `:5500/camera/stream|snapshot`、云台、人脸追踪
- 不要用 OTA 从旧 `moss-desktop` 或 onvif 分区表升级
