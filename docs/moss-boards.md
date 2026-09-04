# MOSS 两块板：差异与禁止分叉

给后续 Agent / 开发者。改固件或桌面前先读完。

本仓库只出两款板：

| `board`（`/health` 字段） | 目录 | 桌面类型名 |
|---|---|---|
| `moss-onvif` | `main/boards/moss/moss-onvif` | 外接视觉 |
| `moss-ov2640` | `main/boards/moss/moss-ov2640` | 板载视觉 |

**产品约定：除云台和板载摄像头（含人脸追踪）外，两块板对用户、对桌面、对 HTTP 控制面必须一致。**  
不要因为「这块是 onvif」就关掉灯、电机、红外、唤醒、AEC、亮度、按住说话。那些不是板型差异。

桌面能力开关只看 `moss-desktop/server/board-presets.mjs` 的 `capsForBoard`。未知 `board` 按 `moss-onvif` 处理。

---

## 1. 允许分叉（PCB / 分区）

这些是硬件布线或 flash 布局不同，**必须**分板实现，**禁止**为了对齐软件去改另一块板的引脚。

| 项 | moss-onvif | moss-ov2640 |
|---|---|---|
| 板载 OV2640 / DVP | 无 | 有 |
| 双轴云台 74HC595 | 无 | SER=21 RCK=47 SCK=48 |
| 人脸追踪 | 无 | 有（模型在 ov2640 分区） |
| 灯 / 眼电机驱动 | GPIO + 灯用 74HC595 | PCA9685 |
| 功放 NS4150B EN | GPIO48 | PCA9685 ch1 |
| LCD 背光 | GPIO21 PWM | PCA9685 ch0 |
| LCD SPI 脚 | MOSI=41 SCK=42 CS=47 DC=39 RST=40 | MOSI=41 SCK=40 CS=42 DC=39 RST=NC |
| 红外 UART | TX=17 RX=18 | TX=10 RX=11（DVP 占用 17/18） |
| 分区表 | `partitions/moss-desktop-16m.csv` | `partitions/v2/16m_moss_desktop.csv` |
| `CONFIG_FREERTOS_HZ` | 默认 | `1000`（步进/相机时序） |
| HTTP `/camera/*` `/gimbal/*` `/face_track/*` | **不注册** | 注册 |

**禁止**两块板互相 OTA。分区表不同，首次烧录用 `erase-flash`。

灯、电机、红外的 **HTTP 语义相同**（`GET/POST /hw`、`/ir/*`），只是底层 GPIO vs PCA9685。改 `/hw` JSON 或动作名必须两板一起改。

---

## 2. 禁止分叉（软件行为）

下列必须两板同一套实现、同一套默认值。改一边就改另一边，并用 `scripts/tests/test_moss_boards.py` 锁住。

### 2.1 局域网控制面

共用：`GET /health`、`GET/PUT /config/device`、`GET/PUT /config/mqtt`、全部 `/ir/*`、`GET/POST /hw`、`POST /chat/wake`、`POST /chat/say`、`GET /chat/sync`。  
实现在 `main/api/`、`main/config/`，由 `CONFIG_BOARD_FAMILY_MOSS` 编进两板。  
协议正文：[moss-desktop/docs/protocol/device-v1.md](../../moss-desktop/docs/protocol/device-v1.md)。

### 2.2 唤醒词与麦克风（手感必须一致）

模拟增益和 AEC 参考增益写在 **唯一** 头文件，两板 `config.h` 只 include，不准再 `#define`：

`main/boards/moss/moss_shared_audio.h`

| 项 | 值 | 说明 |
|---|---|---|
| MIC 模拟增益 | `AUDIO_CODEC_INPUT_GAIN` = 37.5 dB | ES7210 MIC1，不是扬声器音量 |
| AEC 参考增益 | `AUDIO_CODEC_REFERENCE_GAIN` = 37.5 dB | 必须与 MIC 同增益，否则线性 AEC 消不干净 |
| 参考声道 | `AUDIO_CODEC_REFERENCE_CHANNEL` = 2 | |
| 出厂唤醒词 | `mo si` / `MOSS` | 两板 `config.json` 相同 |
| 出厂灵敏度 | `CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20` | 数值越小越灵敏；1–99 |
| 引擎 | MultiNet 自定义唤醒 + 设备端 AEC | `CONFIG_USE_CUSTOM_WAKE_WORD` + `CONFIG_USE_DEVICE_AEC` |

全双工（AEC 开）走 ESP-SR `AFE_TYPE_FD` + `AEC_MODE_FD_HIGH_PERF`，NLP 用官方默认 `AEC_NLP_LEVEL_AGGR`，VAD 开 `vad_mute_playback`（让检测看不到喇叭）。TTS 期间用 AEC 残差确认近端，约 220 ms 后打断。新一句 MQTT `sentence_start` 后忽略约 600 ms（AEC 起音残差会跟着播放跳，约 300 ms），只触发一次，禁止用 PCM 播放能量沿续窗（语音每个音节都会跳 20%+，整段会变聋）。禁止用每句去清 `speaking_started_us_`。TTS 自然结束后先等残差静音并丢掉余响预卷，开门时只丢掉真正静音帧。连续 fetch 不要拼接 `vad_cache`。

`PUT /config/device` 的 `wake_word` **只改词条和灵敏度**，不得改 MIC 增益、不得改扬声器音量。灵敏度存在唤醒词 NVS，扬声器音量走 codec NVS，互不覆盖。

桌面硬件页同一套控件：音量、AEC、按住说话、亮度、唤醒词、灵敏度、灯/电机、远程唤醒。  
**不要**为 onvif 隐藏其中任何一项。onvif 没有 LVGL 主题（splash 屏），`screen.theme` 不出现是对的，两板都如此。

双工 I2S：麦克风还在跑时 **两板都不得关 TX**。关掉 TX 会卡住 ES7210，唤醒变聋。onvif 在 `CheckAndUpdateAudioPowerState` 里跳过 `EnableOutput(false)`；ov2640 关 PCA9685 功放时同样只关 PA、不关 I2S。禁止只给其中一块加 MIC 增益来「修」唤醒。

ov2640 的 PA 和 I2S 是分开的：idle 听唤醒时 TX 可开、功放关掉。出声时必须再 `MossDesktopPreparePlayback`，**不能**因为 `output_enabled()` 已是 true 就跳过，否则 TTS 进 DAC 但喇叭没电。onvif 功放在 GPIO48，同样不能只看 `output_enabled()`：双工会一直开着 TX，必须再 `PreparePlayback()` 把 GPIO48 拉高。

### 2.3 显示

两板都是 0.96" ST7735 160×80 + 嵌入 splash，**不用 LVGL 主题**。硬件页不显示「界面主题」。

### 2.4 桌面 caps

```
moss-onvif : ir / lamps / motor = true；onboard_camera / gimbal / face_track = false
moss-ov2640: 以上全 true（onboard_preview 目前仍为 false）
```

加能力：先改固件 `/health.board`，再改 `board-presets.mjs`，不要用 `product=moss-xiaozhi` 当板型。

---

## 3. 核心里仅 ov2640 的 `#ifdef`

这些是摄像头/云台/功放 PCA9685 的特例，**不要**复制到 onvif，也 **不要** 把共用控制面塞进这些宏：

- `application.cc`：人脸追踪在 idle/listening 恢复；相机流/追踪占用时不进休眠
- `mcp_server.cc`：`MossCameraVoiceGuard` 只在 DVP 抓帧时关 MIC；抓完立刻恢复，TTS 讲解期间必须能 barge-in（不要等 listening）
- `audio_codec.cc`：`MossDesktopPreparePlayback` / `MossDesktopReleasePlayback`（PCA9685 上的 NS4150B）。SCCB / PCA9685 / ES8311 共用 IO1/IO2：DVP 期间必须 `MossDesktopHoldSharedI2c`，禁止音频定时器在总线上关 codec。双工时不要关 I2S TX。
- `main/api/api.cc`：`/camera` `/gimbal` `/face_track` 路由
- `main/CMakeLists.txt`：`device/ov2640/*`、云台、人脸、`camera_handlers.cc`

onvif 功放在 GPIO48，出声走 `PreparePlayback()` 拉高，不要只抄 ov2640 的 PCA9685 PA。

---

## 4. Agent 改动清单

1. 用户可感知的行为（唤醒、音量、AEC、灯、红外、对话）→ 改 **family 共用代码**，不要按板 `#ifdef`。
2. 模拟增益 / 出厂唤醒阈值 → 只改 `moss_shared_audio.h` 和两份 `config.json` 里相同的 `CONFIG_CUSTOM_WAKE_WORD*`。
3. 引脚、PCA9685、相机、分区 → 只改对应板的 `config.h` / `config.json` / 板源文件。
4. 新 HTTP 路径：能进 family 就进；相机相关必须包在 `CONFIG_BOARD_TYPE_MOSS_OV2640`。
5. 测例：`python3 -m unittest scripts.tests.test_moss_boards -v`（含「两板增益/唤醒默认值相同」）；桌面 `pnpm test`。
6. 真机：两块都要听一遍同一唤醒词。禁止只调其中一块的 MIC 增益。

构建：

```bash
python3 scripts/build.py moss/moss-onvif --name moss-onvif
python3 scripts/build.py moss/moss-ov2640 --name moss-ov2640
```
