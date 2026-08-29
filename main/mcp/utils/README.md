# MCP 布局

- 通用 MCP 工具：`main/mcp/tools/`（灯、电机、红外、API、云享记）
- 板级 MCP 工具：`main/boards/moss/<board>/mcp/`（由 CMake 按当前 BOARD_DIR glob）
- 板级驱动：`main/boards/moss/<board>/drivers/`
  - onvif：74HC595 灯条/面板合并
  - ov2640：74HC595 云台 8 位直出、PCA9685
