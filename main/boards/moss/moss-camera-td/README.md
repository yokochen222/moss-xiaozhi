# moss-camera-td

旧版 MOSS（`lichuang-dev` + SSD1306）在本仓库的板型。`/health.board` = `moss-camera-td`。

有：ES8311+ES7210、眼灯、流水灯、红外、音量键、云享记、局域网 HTTP。
无：面板灯、底灯、眼部电机、板载 OV2640 / 云台。外接 ONVIF 走桌面，不走板载预览。

屏是 SSD1306 128×64 I2C（SDA=7 SCL=6），不是 ST7735。首次烧录 `erase-flash`，禁止与其它 `type` 互 OTA。
