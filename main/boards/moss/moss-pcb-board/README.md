# moss-pcb-board

旧 8M 分区 MOSS PCB 桌搭在本仓库的板型。`/health.board` = `moss-pcb-board`。
分区已改为 `partitions/moss-desktop-16m.csv`，禁止沿用旧 8M 表，禁止与其它 `type` 互 OTA。

有：ES8311+ES7210、眼灯（GPIO15）、流水灯 74HC595、红外、音量键、云享记、局域网 HTTP。
无：面板灯、底灯、眼部电机、板载 OV2640 / 云台。外接 ONVIF 走桌面，不走板载预览。

屏是 SSD1306 128×64 I2C（SDA=6 SCL=7），不是 ST7735。首次烧录 `erase-flash`。

```bash
python3 scripts/build.py moss/moss-pcb-board --name moss-pcb-board
```
