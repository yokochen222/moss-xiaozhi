# MOSS PCB v1 (`moss-pcb-v1`)

第一代 MOSS PCB：SSD1306 OLED，无眼部电机、无面板灯、无底灯。
固件身份：`type`/`name`/`board` = `moss-pcb-v1`。

与 `moss-onvif` / `moss-ov2640` 的差异见
**[docs/moss-boards.md](../../../../docs/moss-boards.md)**。

## 硬件资源

- **主控**：ESP32-S3
- **屏幕**：SSD1306 I2C 128×64（SDA=GPIO6 SCL=GPIO7，地址 0x3C）
- **音频 Codec**：ES8311 + ES7210；功放 NS4150B，`CTRL` ← **GPIO48**（`IO48_SPK_EN`）
- **按键**：BOOT（GPIO0）、音量+（GPIO40）、音量-（GPIO39）
- **外设**：流水灯 74HC595（SER=3 RCK=4 SCK=5，Q0–Q4）、眼灯 GPIO15、红外 UART2 TX=17 RX=18
- **无**：眼部电机、面板灯、底灯、板载摄像头
- **分区表**：`partitions/moss-desktop-16m.csv`（与 onvif 相同，禁止跨板 OTA）

## 构建方法

```bash
python3 scripts/build.py moss/moss-pcb-v1 --name moss-pcb-v1
```

首次刷机请擦除后整片烧录：

```bash
idf.py -p PORT erase-flash flash
```
