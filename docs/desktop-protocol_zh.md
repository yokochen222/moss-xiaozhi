# 接入 moss-desktop

设备侧协议以桌面仓库这份为准，实现前先读完：

**[moss-desktop/docs/protocol/device-v1.md](../../moss-desktop/docs/protocol/device-v1.md)**

本仓库对应实现（局域网 HTTP 控制面）：

- `main/config/moss_config_service.cc` — HTTP / mDNS 始终开启、身份缓存
- `main/config/moss_chat_log.cc` — 对话 ring，供 `GET /chat/sync`
- `main/config/moss_hw.cc` — 灯 / 电机状态
- `main/api/methods/config/config_handlers.cc` — `/health`、`/config/*`
- `main/api/methods/hw/hw_handlers.cc` — `/hw`
- `main/api/methods/chat/chat_handlers.cc` — `/chat/wake` `/chat/say` `/chat/sync`
- `main/api/api.cc` — HTTP 路由表

小智云语音仍走 `mqtt_protocol`，与桌面控制面无关。

两块板（`moss-onvif` / `moss-ov2640`）除云台和板载摄像头外必须行为一致。差异与禁止分叉见 **[moss-boards.md](moss-boards.md)**。
