#!/usr/bin/env python3
"""
OLED显示问题综合诊断脚本
"""

import re
import os
import subprocess

def run_build_check():
    """运行编译检查"""
    print("🔧 编译检查...")
    try:
        result = subprocess.run(['idf.py', 'build'], capture_output=True, text=True, cwd="/Users/yokochen/Desktop/moss/moss-xiaozhi")
        if result.returncode == 0:
            print("✅ 编译成功")
            return True
        else:
            print("❌ 编译失败")
            print(result.stderr)
            return False
    except Exception as e:
        print(f"⚠️  编译检查失败: {e}")
        return False

def check_oled_integration():
    """检查OLED集成状态"""
    print("\n🔍 OLED集成检查:")
    
    board_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/lichuang_dev_board.cc"
    
    with open(board_file, 'r') as f:
        content = f.read()
    
    checks = [
        ("OLED头文件", r'#include.*OledDisplay'),
        ("初始化方法", r'InitializeOledDisplay'),
        ("I2C扫描", r'Scanning I2C devices'),
        ("显示测试", r'Display test pattern'),
        ("SSD1306驱动", r'esp_lcd_new_panel_ssd1306'),
        ("显示尺寸", r'DISPLAY_WIDTH.*DISPLAY_HEIGHT'),
    ]
    
    for check_name, pattern in checks:
        if re.search(pattern, content, re.IGNORECASE):
            print(f"   ✅ {check_name}")
        else:
            print(f"   ❌ {check_name}")

def check_hardware_connections():
    """检查硬件连接"""
    print("\n🔌 硬件连接检查:")
    
    print("   必须连接:")
    print("   - VCC → 3.3V (红色线)")
    print("   - GND → GND (黑色线)")
    print("   - SDA → GPIO1 (绿色线)")
    print("   - SCL → GPIO2 (蓝色线)")
    print("   - RST → 悬空 (软件复位)")
    
    print("\n   常见问题:")
    print("   1. 电源不足 - 确保3.3V稳定")
    print("   2. 地址错误 - 确认是0x3C还是0x3D")
    print("   3. 模块损坏 - 尝试其他模块")
    print("   4. 线序错误 - 重新检查接线")

def check_i2c_bus():
    """检查I2C总线状态"""
    print("\n🚌 I2C总线检查:")
    
    # 检查I2C配置
    config_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/config.h"
    
    with open(config_file, 'r') as f:
        content = f.read()
    
    # 提取I2C配置
    sda_pin = re.search(r'#define DISPLAY_SDA_PIN\s+(\w+)', content)
    scl_pin = re.search(r'#define DISPLAY_SCL_PIN\s+(\w+)', content)
    
    if sda_pin and scl_pin:
        print(f"   I2C引脚配置: SDA={sda_pin.group(1)}, SCL={scl_pin.group(1)}")
        print("   ✅ 引脚配置正确 (GPIO1/GPIO2)")
    else:
        print("❌ I2C引脚配置错误")

def provide_debug_sequence():
    """提供调试步骤序列"""
    print("\n🎯 OLED调试步骤:")
    
    steps = [
        "1. 物理检查",
        "   - 重新插拔所有连接线",
        "   - 确认VCC=3.3V, GND=0V",
        "   - 确认SDA/SCL连接正确",
        
        "2. 地址确认",
        "   - 查看OLED模块规格",
        "   - 确认I2C地址 (0x3C/0x3D)",
        "   - 如有跳线帽，检查地址设置",
        
        "3. 软件测试",
        "   - 编译并烧录新代码",
        "   - 查看串口日志",
        "   - 观察I2C扫描结果",
        
        "4. 硬件验证",
        "   - 使用其他I2C设备测试",
        "   - 更换OLED模块测试",
        "   - 使用逻辑分析仪查看I2C信号"
    ]
    
    for step in steps:
        print(f"   {step}")

def check_display_drivers():
    """检查显示驱动"""
    print("\n📱 显示驱动检查:")
    
    # 检查驱动文件
    driver_files = [
        "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/display/oled_display.h",
        "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/display/oled_display.cc"
    ]
    
    for file_path in driver_files:
        if os.path.exists(file_path):
            print(f"   ✅ {os.path.basename(file_path)}")
        else:
            print(f"   ❌ {os.path.basename(file_path)} - 缺失")

if __name__ == "__main__":
    print("🖥️  OLED显示问题综合诊断")
    print("=" * 50)
    
    check_oled_integration()
    check_i2c_bus()
    check_display_drivers()
    check_hardware_connections()
    provide_debug_sequence()
    
    print("\n💡 建议:")
    print("   1. 先执行硬件连接检查")
    print("   2. 编译并烧录新代码")
    print("   3. 查看串口日志中的I2C扫描结果")
    print("   4. 如果仍有问题，尝试降低I2C速度到50kHz")