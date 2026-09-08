# moss-bread-compact

旧版手搓 MOSS（`bread-compact-wifi`：INMP441 + MAX98357A + SSD1306）在本仓库的板型。
`/health.board` = `moss-bread-compact`。

有：半双工 I2S 麦/喇叭、眼灯（GPIO12）、五路 GPIO 流水灯、红外、BOOT、触摸按住说话（GPIO47）、云享记、局域网 HTTP。
无：面板灯、底灯、眼部电机、ES8311/ES7210、设备端 AEC、实时打断、板载 OV2640 / 云台。

音频是 simplex，播报时关麦，`local_aec_supported=false`。不要和 pcb-v1 / camera-td / pcb-board 的全双工 AEC 搞混。

屏是 SSD1306 128×64 I2C（SDA=1 SCL=2）。分区 `partitions/moss-desktop-16m.csv`。首次烧录 `erase-flash`。

```bash
python3 scripts/build.py moss/moss-bread-compact --name moss-bread-compact
```
