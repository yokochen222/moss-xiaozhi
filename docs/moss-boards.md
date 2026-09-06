# MOSS 板型：差异与禁止分叉

给后续 Agent / 开发者。改固件或桌面前先读完。

本仓库出三款板：

| `board`（`/health` 字段） | 目录 | 桌面类型名 |
|---|---|---|
| `moss-onvif` | `main/boards/moss/moss-onvif` | 外接视觉 |
| `moss-ov2640` | `main/boards/moss/moss-ov2640` | 板载视觉 |
| `moss-pcb-v1` | `main/boards/moss/moss-pcb-v1` | PCB v1 |

**onvif 与 ov2640 产品约定：除云台和板载摄像头（含人脸追踪）外，这两块板对用户、对桌面、对 HTTP 控制面必须一致。**  
不要因为「这块是 onvif」就关掉灯、电机、红外、唤醒、AEC、亮度、按住说话。那些不是这两板的差异。

**pcb-v1 是硬件缺件板**：SSD1306 OLED，无眼部电机、无面板灯、无底灯；有实体音量键。桌面用 caps **隐藏**缺件控件，不做成灰按钮。其它控制面（发现、绑定、`/health`、配置、对话、红外、云享记）与现板同一套 API。

桌面能力开关只看 `moss-desktop/server/board-presets.mjs` 的 `capsForBoard`。未知 `board` 按 `moss-onvif` 处理（三项灯/电机都显示）。必须登记 `moss-pcb-v1` 才会藏面板灯/底灯/电机。

禁止修改 `moss-onvif/`、`moss-ov2640/` 目录内文件来迁就 pcb-v1。共用代码只加 `MOSS_PCB_V1` / `moss-pcb-v1` 分支。

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

pcb-v1 相对这两板的硬件差（GPIO 以该板 `config.h` 为准，禁止为了对齐软件去改脚）：

| 项 | moss-pcb-v1 |
|---|---|
| 屏 | SSD1306 I2C 128×64（SDA=6 SCL=7），不是 ST7735；无 splash / emote |
| 功放 NS4150B EN | GPIO48（与 onvif 相同，走 BoxAudioCodec PA） |
| 音量键 | 上=GPIO40 下=GPIO39（现板这两脚给 LCD DC/RST） |
| 灯 74HC595 | SER=3 RCK=4 SCK=5，只用 Q0–Q4；眼灯=GPIO15 |
| 红外 UART | TX=17 RX=18 |
| 眼部电机 / 面板灯 / 底灯 | **无** |
| 分区表 | `partitions/moss-desktop-16m.csv`（与 onvif 相同，`type` 不同，**禁止跨板 OTA**） |

**禁止**三块板互相 OTA。分区表不同或 `type` 不同都不行，首次烧录用 `erase-flash`。

onvif 与 ov2640 的灯、电机、红外 **HTTP 语义相同**（`GET/POST /hw`、`/ir/*`），只是底层 GPIO vs PCA9685。改 `/hw` JSON 或动作名必须这两板一起改。pcb-v1 的 `/hw` 对 `panel`/`bottom`/`motor` 返回失败，`all` 只动眼灯+流水灯。

---

## 2. 禁止分叉（软件行为）

下列必须 **onvif 与 ov2640** 同一套实现、同一套默认值。改一边就改另一边，并用 `scripts/tests/test_moss_boards.py` 锁住。pcb-v1 在唤醒/MIC/AEC 上跟这两板共用 `moss_shared_audio.h` 与同一套 `config.json` 唤醒项；不要在板头覆盖 `AUDIO_CODEC_INPUT_GAIN`。

### 2.1 局域网控制面

共用：`GET /health`、`GET/PUT /config/device`、`GET/PUT /config/mqtt`、全部 `/ir/*`、`GET/POST /hw`、`POST /chat/wake`、`POST /chat/say`、`GET /chat/sync`。  
实现在 `main/api/`、`main/config/`，由 `CONFIG_BOARD_FAMILY_MOSS` 编进三板。  
协议正文：[moss-desktop/docs/protocol/device-v1.md](../../moss-desktop/docs/protocol/device-v1.md)。

### 2.2 唤醒词与麦克风（手感必须一致）

模拟增益写在 **唯一** 头文件，各板 `config.h` 只 include，不准再 `#define`：

`main/boards/moss/moss_shared_audio.h`

| 项 | 值 | 说明 |
|---|---|---|
| MIC 模拟增益 | `AUDIO_CODEC_INPUT_GAIN` = 37.5 dB | ES7210 MIC1，不是扬声器音量 |
| AEC 参考 | ES7210 MIC3 电气回灌（TDM slot 1） | 官方也只用一颗模拟麦；MIC2 不用。AFE 是 `MR`：MIC1=M，MIC3 喇叭回路=R |
| 出厂唤醒词 | `mo si` / `MOSS` | 三板 `config.json` 相同 |
| 出厂灵敏度 | `CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20` | 数值越小越灵敏；1–99 |
| 引擎 | MultiNet 自定义唤醒 + 设备端 AEC | `CONFIG_USE_CUSTOM_WAKE_WORD` + `CONFIG_USE_DEVICE_AEC` |

#### 全双工 AEC / VAD（已实机验证，2026-09-05）

对齐 `78/xiaozhi-esp32` 立创实战派 `lichuang-dev` + `AfeAudioEngine`。**当前手感正常：TTS 说完、对着喇叭能打断、ASR 不再首字重复。** 再改音频先读本节，不要发明第二套门限。

硬件（和立创一样，不是「缺一颗麦所以要软件参考」）：

- 物理人声麦只有 **MIC1**。MIC2 可以不焊，AFE 也不用。
- 第二路是 **ES7210 MIC3 ← ES8311 喇叭回灌**，不是第二颗麦。
- TDM 槽序是 MIC1, MIC3, MIC2, MIC4（xiaozhi #2036）。`mask(0)|mask(1)` 因此是 MIC1 + MIC3，AFE 格式永远 `MR`，不是 `MMR`。
- 虾哥：对话 AEC 只用一个 M + 一个 R（speaker 回路）。`AFE_TYPE_VC` 不支持两个 M。

软件（必须保持）：

| 项 | 官方 / MOSS 现状 |
|---|---|
| AFE | `AFE_TYPE_VC` + `AEC_MODE_VOIP_HIGH_PERF` + `AEC_NLP_LEVEL_VERYAGGR` + `VAD_MODE_0` + `vad_min_noise_ms=100` |
| 采集 | `BoxAudioCodec` `mask(0)\|mask(1)`，只给 MIC1 设 `AUDIO_CODEC_INPUT_GAIN` |
| 全双工 | AEC 开 → `kListeningModeRealtime`；TTS 期间 **保持** voice processing，把 AEC 后的麦送上云端 |
| 半双工 | AEC 关 → TTS 期间关麦 |
| 打断 | 云端听上行后 abort。`CONFIG_ENABLE_VAD_INTERRUPT=n` |
| 上行 | `HandleVoiceResult` 只送 `result->data`，**不要**再拼 `vad_cache` |

禁止（都会把已验证手感打回去）：

- 用 DAC PCM 覆盖 R 通道（软件参考）。立创也是一颗模拟麦；R 是模拟回灌。覆盖会毁掉参考。
- 打开 `CONFIG_ENABLE_VAD_INTERRUPT`，或在 speaking 里用 AFE VAD 掐 TTS。`VAD_MODE_0` 会把喇叭残余当成 SPEECH（日志里 `res=50~100` 仍会 `VAD barge-in confirmed`）。
- 把 `vad_cache` 拼进 Opus 上行 → ASR 首字重复（「给给」「你你」）。
- 能量门、echo floor、残差比去「确认是不是人在说话」。AEC 残差低仍会误判。
- 改成 `AFE_TYPE_FD` / `AEC_MODE_FD_*` / `vad_min_speech_ms` / `vad_mute_playback` 来「修」打断。

真机日志对照：TTS 时 `ref` 应跟着播放走（不是 5）；`res` 远小于 `pb` 表示 AEC 在干活。此时若再自打断，查的是本地 VAD，不是增益。

锁：`scripts/tests/test_moss_boards.py`（`MossBargeInTests`）。三板 `config.json` 必须 `CONFIG_ENABLE_VAD_INTERRUPT=n`。

`PUT /config/device` 的 `wake_word` **只改词条和灵敏度**，不得改 MIC 增益、不得改扬声器音量。灵敏度存在唤醒词 NVS，扬声器音量走 codec NVS，互不覆盖。

桌面硬件页同一套控件：音量、AEC、按住说话、亮度、唤醒词、灵敏度、灯/电机、远程唤醒。  
**不要**为 onvif 隐藏其中任何一项。onvif 没有 LVGL 主题（splash 屏），`screen.theme` 不出现是对的，onvif / ov2640 都如此。pcb-v1 是 OLED + LVGL，硬件页显示主题是对的；没有背光则没有 brightness。

双工 I2S：麦克风还在跑时 **各板都不得关 TX**。关掉 TX 会卡住 ES7210，唤醒变聋。onvif 在 `CheckAndUpdateAudioPowerState` 里跳过 `EnableOutput(false)`；ov2640 关 PCA9685 功放时同样只关 PA、不关 I2S。禁止只给其中一块加 MIC 增益来「修」唤醒。

ov2640 的 PA 和 I2S 是分开的：idle 听唤醒时 TX 可开、功放关掉。出声时必须再 `MossDesktopPreparePlayback`，**不能**因为 `output_enabled()` 已是 true 就跳过，否则 TTS 进 DAC 但喇叭没电。onvif 功放在 GPIO48，同样不能只看 `output_enabled()`：双工会一直开着 TX，必须再 `PreparePlayback()` 把 GPIO48 拉高。

### 2.3 显示

onvif / ov2640 都是 0.96" ST7735 160×80 + 嵌入 splash，**不用 LVGL 主题**。硬件页不显示「界面主题」。  
pcb-v1 是 SSD1306 128×64 + `OledDisplay`（14 号字体），无 splash / emote；`config.json` 不要开 `USE_EMOTE_MESSAGE_STYLE`。

### 2.4 桌面 caps

```
moss-onvif : ir / lamps / panel / bottom / motor = true；onboard_camera / gimbal / face_track = false
moss-ov2640: 以上全 true（onboard_preview 目前仍为 false）
moss-pcb-v1: ir / lamps = true；panel / bottom / motor / onboard_camera / gimbal / face_track = false
```

隐藏缺件用「显式 `false` 才藏」：缺字段或 `true` 都显示，避免旧 caps 把 onvif / ov2640 界面改掉。

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
2. 模拟增益 / 出厂唤醒阈值 → 只改 `moss_shared_audio.h` 和各板 `config.json` 里相同的 `CONFIG_CUSTOM_WAKE_WORD*`。
3. 引脚、PCA9685、相机、分区 → 只改对应板的 `config.h` / `config.json` / 板源文件。
4. 新 HTTP 路径：能进 family 就进；相机相关必须包在 `CONFIG_BOARD_TYPE_MOSS_OV2640`。
5. 测例：`python3 -m unittest scripts.tests.test_moss_boards -v`（含「增益/唤醒默认值相同」；两板 LCD/电机对齐测例不要塞 pcb-v1）；桌面 `pnpm test`。
6. 真机：onvif 与 ov2640 都要听一遍同一唤醒词。禁止只调其中一块的 MIC 增益。pcb-v1 另验 OLED 出字、GPIO48 喇叭、音量键、BOOT、发现 `board=moss-pcb-v1`、硬件页只有眼灯和流水灯。

构建：

```bash
python3 scripts/build.py moss/moss-onvif --name moss-onvif
python3 scripts/build.py moss/moss-ov2640 --name moss-ov2640
python3 scripts/build.py moss/moss-pcb-v1 --name moss-pcb-v1
```
