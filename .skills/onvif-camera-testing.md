# ONVIF Camera Testing Guide

本文档记录使用curl测试ONVIF协议摄像头的方法。

## 摄像头信息

- IP: `192.168.31.24`
- 端口: `80`
- 用户名: `admin`
- 密码: `admin123`
- 厂商: Security Dev

## 测试环境准备

确保curl支持认证:
```bash
curl --version
```

## API端点

- 设备服务: `http://192.168.31.24/onvif/device_service`
- PTZ服务: `http://192.168.31.24/onvif/ptz_service`
- 截图: `http://192.168.31.24/onvif/getsnapshot/?channel=1`

## 1. 获取设备信息

```bash
curl -s -X POST "http://192.168.31.24/onvif/device_service" \
  --anyauth -u "admin:admin123" \
  -H "Content-Type: application/soap+xml; charset=utf-8" \
  -H "SOAPAction: http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation" \
  -d '<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope">
  <soap:Header/>
  <soap:Body>
    <GetDeviceInformation xmlns="http://www.onvif.org/ver10/device/wsdl"/>
  </soap:Body>
</soap:Envelope>'
```

## 2. 获取媒体配置

```bash
curl -s -X POST "http://192.168.31.24/onvif/device_service" \
  --anyauth -u "admin:admin123" \
  -H "Content-Type: application/soap+xml; charset=utf-8" \
  -H "SOAPAction: http://www.onvif.org/ver10/media/wsdl/GetProfiles" \
  -d '<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope" xmlns:tt="http://www.onvif.org/ver10/schema">
  <soap:Header/>
  <soap:Body>
    <GetProfiles xmlns="http://www.onvif.org/ver10/media/wsdl"/>
  </soap:Body>
</soap:Envelope>'
```

返回的Profile信息:
- Token: `protoken_ch0001` (主码流)
- Token: `protoken_ch0002` (子码流)
- PTZ Node: `node_0001`

## 3. 获取截图URI

```bash
curl -s -X POST "http://192.168.31.24/onvif/device_service" \
  --anyauth -u "admin:admin123" \
  -H "Content-Type: application/soap+xml; charset=utf-8" \
  -H "SOAPAction: http://www.onvif.org/ver10/media/wsdl/GetSnapshotUri" \
  -d '<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope" xmlns:trt="http://www.onvif.org/ver10/media/wsdl">
  <soap:Header/>
  <soap:Body>
    <trt:GetSnapshotUri>
      <trt:ProfileToken>protoken_ch0001</trt:ProfileToken>
    </trt:GetSnapshotUri>
  </soap:Body>
</soap:Envelope>'
```

返回截图URI: `http://192.168.31.24:80/onvif/getsnapshot/?channel=1`

## 4. 下载截图

```bash
curl -s -o snapshot.jpg \
  --anyauth -u "admin:admin123" \
  "http://192.168.31.24/onvif/getsnapshot/?channel=1"

# 验证图片
file snapshot.jpg
ls -la snapshot.jpg
```

## 5. 云台控制 (PTZ)

### 5.1 连续移动

**向右移动:**
```bash
curl -s -X POST "http://192.168.31.24/onvif/ptz_service" \
  --anyauth -u "admin:admin123" \
  -H "Content-Type: application/soap+xml; charset=utf-8" \
  -H "SOAPAction: http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove" \
  -d '<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope" xmlns:ptz="http://www.onvif.org/ver20/ptz/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <soap:Header/>
  <soap:Body>
    <ptz:ContinuousMove>
      <ptz:ProfileToken>protoken_ch0001</ptz:ProfileToken>
      <ptz:Velocity>
        <tt:PanTilt x="0.5" y="0"/>
      </ptz:Velocity>
    </ptz:ContinuousMove>
  </soap:Body>
</soap:Envelope>'
```

**向左移动:**
```bash
curl -s -X POST "http://192.168.31.24/onvif/ptz_service" \
  --anyauth -u "admin:admin123" \
  -H "Content-Type: application/soap+xml; charset=utf-8" \
  -H "SOAPAction: http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove" \
  -d '<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope" xmlns:ptz="http://www.onvif.org/ver20/ptz/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <soap:Header/>
  <soap:Body>
    <ptz:ContinuousMove>
      <ptz:ProfileToken>protoken_ch0001</ptz:ProfileToken>
      <ptz:Velocity>
        <tt:PanTilt x="-0.5" y="0"/>
      </ptz:Velocity>
    </ptz:ContinuousMove>
  </soap:Body>
</soap:Envelope>'
```

**向上移动:**
```bash
curl -s -X POST "http://192.168.31.24/onvif/ptz_service" \
  --anyauth -u "admin:admin123" \
  -H "Content-Type: application/soap+xml; charset=utf-8" \
  -H "SOAPAction: http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove" \
  -d '<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope" xmlns:ptz="http://www.onvif.org/ver20/ptz/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <soap:Header/>
  <soap:Body>
    <ptz:ContinuousMove>
      <ptz:ProfileToken>protoken_ch0001</ptz:ProfileToken>
      <ptz:Velocity>
        <tt:PanTilt x="0" y="-0.5"/>
      </ptz:Velocity>
    </ptz:ContinuousMove>
  </soap:Body>
</soap:Envelope>'
```

**向下移动:**
```bash
curl -s -X POST "http://192.168.31.24/onvif/ptz_service" \
  --anyauth -u "admin:admin123" \
  -H "Content-Type: application/soap+xml; charset=utf-8" \
  -H "SOAPAction: http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove" \
  -d '<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope" xmlns:ptz="http://www.onvif.org/ver20/ptz/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
  <soap:Header/>
  <soap:Body>
    <ptz:ContinuousMove>
      <ptz:ProfileToken>protoken_ch0001</ptz:ProfileToken>
      <ptz:Velocity>
        <tt:PanTilt x="0" y="0.5"/>
      </ptz:Velocity>
    </ptz:ContinuousMove>
  </soap:Body>
</soap:Envelope>'
```

### 5.2 停止移动

```bash
curl -s -X POST "http://192.168.31.24/onvif/ptz_service" \
  --anyauth -u "admin:admin123" \
  -H "Content-Type: application/soap+xml; charset=utf-8" \
  -H "SOAPAction: http://www.onvif.org/ver20/ptz/wsdl/Stop" \
  -d '<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope" xmlns:ptz="http://www.onvif.org/ver20/ptz/wsdl">
  <soap:Header/>
  <soap:Body>
    <ptz:Stop>
      <ptz:ProfileToken>protoken_ch0001</ptz:ProfileToken>
      <ptz:PanTilt>true</ptz:PanTilt>
    </ptz:Stop>
  </soap:Body>
</soap:Envelope>'
```

## 重要参数说明

### 云台速度参数
- `x`: 水平方向速度 (-1.0 到 1.0)
  - 正值: 向右
  - 负值: 向左
- `y`: 垂直方向速度 (-1.0 到 1.0)
  - 正值: 向下
  - 负值: 向上
- 速度值 0.5 表示50%的速度

### Profile Token
- `protoken_ch0001`: 主码流配置文件
- `protoken_ch0002`: 子码流配置文件

## 注意事项

1. PTZ控制需要使用 `/onvif/ptz_service` 端点，而不是 `/onvif/device_service`
2. PTZ控制需要使用 `ver20/ptz/wsdl` 命名空间
3. 移动命令发送后会一直持续，需要发送Stop命令停止
4. 所有请求都需要HTTP认证
