# MOSS 桌面助手 (Moss Desktop Assistant)

## 简介

MOSS 桌面助手是独立固件身份（`type`/`name` = `moss-desktop`），用于独立 OTA 通道。
音频部分（ES8311 / ES7210 / NS4150B）按板级原理图 `V5_BUTTON_MITE_SPK` 配置：
**功放使能脚为 GPIO48（`IO48_SPK_EN`）**，不是立创实战派的 PCA9557 bit1。

## 硬件资源

- **主控**：ESP32-S3
- **屏幕**：ST7789 SPI LCD（320×240）；若板载 PCA9557 则可用其控制 LCD CS
- **触摸**：FT5x06 电容触摸（I2C，可选）
- **音频 Codec**：ES8311（喇叭）+ ES7210（麦克风，支持 AEC 回采）
- **功放**：NS4150B，`CTRL` ← **GPIO48 (`IO48_SPK_EN`)**，默认下拉关闭
- **摄像头**：OV2640 类（可选）；若板载 PCA9557 则可用其控制电源
- **按键**：BOOT（GPIO0）——单击切换唤醒/休眠，长按切换单击/长按说话模式，
  双击切换 AEC
- **实时打断**：默认开启设备端 AEC + VAD barge-in（TTS 播放中直接开口即可打断，
  从打断点开始采音并上传；待机唤醒仍用唤醒词）
- **支持表情动画**：通过 `EmoteDisplay` 渲染
- **支持 Press-to-Talk MCP 工具**

## 麦克风 / 唤醒灵敏度调整

优先改板级增益（最常用）：

```c
// main/boards/moss/moss-desktop/config.h
#define AUDIO_CODEC_INPUT_GAIN 37.5f   // 偏钝加大，误唤醒/破音减小（约 28~37.5）
```

进阶（影响所有 AFE 板型），在 `main/audio/engines/afe_audio_engine.cc`：

- `afe_linear_gain`：数字增益（当前约 `3.0f`，范围约 `0.1~10`）
- `wakenet_mode`：`DET_MODE_95` 更易唤醒（误唤醒也会更多）
- 待机唤醒已关闭 AEC；仅在通话/打断时开启，避免小声被压掉

## 构建方法

```bash
# 推荐：使用 build.py 自动配置
python3 scripts/build.py moss/moss-desktop --name moss-desktop
```

手动使用 `idf.py`：

```bash
source /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py menuconfig  # 选择 "Board Type -> MOSS Desktop Assistant (MOSS 桌面助手)"
idf.py build
idf.py flash monitor
```

## 与原 `lichuang-dev` 的区别

| 项 | lichuang-dev | moss-desktop |
|---|---|---|
| 硬件原理图 | 立创·实战派 ESP32-S3 | 完全相同 |
| 板级 `type` | `lichuang-dev` | `moss-desktop` |
| 固件 OTA 通道 | 立创官方 | MOSS 桌面助手独立通道 |
| 标识 LOG TAG | `LichuangDevBoard` | `MossDesktopBoard` |
| 类名 | `LichuangDevBoard` | `MossDesktopBoard` |

保持板级 `config.h` 与原 `lichuang_dev_board.cc` 一致的目的，
是为了在原硬件上直接烧录 MOSS 固件即可工作；
如需更换硬件，请在保留 `moss-desktop` 身份的前提下修改 `config.h` 与源码。

## 参考资料

- 立创·实战派 ESP32-S3 资料：https://wiki.lckfb.com/zh-hans/szpi-esp32s3
- 板子添加指南：`docs/custom-board.md`
