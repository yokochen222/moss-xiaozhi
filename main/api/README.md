# 红外设备 Web API 接口文档

## 概述

本模块实现了一个轻量级的HTTP API服务器，提供红外设备的控制接口。API服务器通过MCP工具进行启停控制，默认监听端口5500。

## 接口列表

### 1. 发送红外指令

**接口地址：** `POST /ir/send`

**功能：** 发送红外指令到红外设备

**请求参数：**

```json
{
  "ir_code": "红外指令码"
}
```

**响应示例：**

```json
{
  "status": "success",
  "message": "Infrared command sent successfully",
  "ir_code": "红外指令码"
}
```

**错误响应：**

```json
{
  "status": "error",
  "message": "Failed to send infrared command"
}
```

### 2. 读取红外接收数据

**接口地址：** `GET /ir/read`

**功能：** 获取从串口接收到的红外数据

**响应示例：**

```json
{
  "status": "success",
  "ir_data": [
    "接收到的红外数据1",
    "接收到的红外数据2"
  ],
  "count": 2
}
```

## 服务器控制

### 通过MCP工具控制API服务器

API服务器通过MCP工具 `self.api_server.control` 进行控制：

1. **启动API服务器**：

   ```json
   {
     "action": "start",
     "port": 5500
   }
   ```
2. **停止API服务器**：

   ```json
   {
     "action": "stop"
   }
   ```
3. **查询服务器状态**：

   ```json
   {
     "action": "status"
   }
   ```

## 使用示例

### 使用curl发送红外指令

```bash
curl -X POST http://设备IP:5500/ir/send \
  -H "Content-Type: application/json" \
  -d '{"ir_code": "FF00FF"}'
```

### 使用curl读取红外数据

```bash
curl http://设备IP:5500/ir/read
```

## 技术实现

- 使用ESP-IDF的HTTP服务器组件
- 单例模式确保只有一个API服务器实例
- 线程安全的数据存储，支持并发访问
- 自动限制存储的数据条数，防止内存溢出
- 低耦合设计，不影响其他模块的功能

## 测试

### 测试步骤

1. **启动API服务器**：通过MCP工具发送启动命令
2. **测试接口**：使用curl或测试脚本调用API接口
3. **停止服务器**：通过MCP工具发送停止命令

## 注意事项

1. API服务器需要通过MCP工具手动启动，不会在设备启动时自动启动
2. 默认端口为5500，可在启动时指定其他端口
3. 红外数据存储有数量限制（最多100条），超出会自动删除最旧的数据
4. 所有接口都返回JSON格式的响应
5. 确保设备已连接到网络，API才能被外部访问
6. 建议只在需要时启动API服务器，以节省系统资源
