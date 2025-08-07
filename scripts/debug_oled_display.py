#!/usr/bin/env python3
"""
OLED显示问题排查脚本
"""

import re
import os

def check_oled_config():
    """检查OLED配置"""
    print("🔍 OLED显示问题排查...")
    
    config_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/config.h"
    
    with open(config_file, 'r') as f:
        content = f.read()
    
    print("📋 当前OLED配置:")
    
    # 提取配置
    width = re.search(r'#define DISPLAY_WIDTH\s+(\d+)', content)
    height = re.search(r'#define DISPLAY_HEIGHT\s+(\d+)', content)
    sda_pin = re.search(r'#define DISPLAY_SDA_PIN\s+(\w+)', content)
    scl_pin = re.search(r'#define DISPLAY_SCL_PIN\s+(\w+)', content)
    
    print(f"   尺寸: {width.group(1)}x{height.group(1)} 像素")
    print(f"   I2C引脚: SDA={sda_pin.group(1)}, SCL={scl_pin.group(1)}")
    
    # 检查常见配置问题
    if int(width.group(1)) == 128 and int(height.group(1)) in [32, 64]:
        print("✅ 尺寸配置正确")
    else:
        print("⚠️  尺寸配置可能不正确")
    
    return True

def check_i2c_address():
    """检查I2C地址"""
    print("\n🔍 I2C地址检查:")
    
    board_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/lichuang_dev_board.cc"
    
    with open(board_file, 'r') as f:
        content = f.read()
    
    # 检查OLED地址
    addr_match = re.search(r'\.dev_addr\s*=\s*(0x[0-9a-fA-F]+)', content)
    if addr_match:
        addr = addr_match.group(1)
        print(f"   OLED I2C地址: {addr}")
        
        # 常见地址对照
        addresses = {
            "0x3C": "SSD1306/SH1106 (常见)",
            "0x3D": "SSD1306/SH1106 (备用)",
            "0x78": "SSD1306/SH1106 (8位地址)",
            "0x7A": "SSD1306/SH1106 (8位备用)"
        }
        
        if addr in addresses:
            print(f"   ✅ {addresses[addr]}")
        else:
            print("⚠️  非标准地址，请确认模块规格")
    
    return True

def check_power_requirements():
    """检查电源要求"""
    print("\n🔍 电源要求检查:")
    print("   OLED模块通常需要:")
    print("   - VCC: 3.3V (部分支持5V)")
    print("   - GND: 共地")
    print("   - 电流: 20-40mA (显示内容相关)")
    print("   ⚠️  确保电源稳定，电压足够")

def check_reset_pin():
    """检查复位引脚"""
    print("\n🔍 复位引脚检查:")
    
    board_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/lichuang_dev_board.cc"
    
    with open(board_file, 'r') as f:
        content = f.read()
    
    # 检查复位引脚配置
    reset_match = re.search(r'panel_config\.reset_gpio_num\s*=\s*(-?\d+)', content)
    if reset_match:
        reset_pin = reset_match.group(1)
        if reset_pin == "-1":
            print("   复位引脚: 软件复位 (-1)")
            print("   ✅ 配置为软件复位，无需硬件连接")
        else:
            print(f"   复位引脚: GPIO{reset_pin}")
            print("   ⚠️  需要检查该GPIO连接")

def provide_debug_steps():
    """提供调试步骤"""
    print("\n🔧 调试步骤:")
    print("1. 检查物理连接:")
    print("   - VCC → 3.3V")
    print("   - GND → GND")
    print("   - SDA → GPIO1")
    print("   - SCL → GPIO2")
    print("   - RST → 悬空或GPIO")
    
    print("\n2. 检查模块规格:")
    print("   - 确认是SSD1306还是SH1106")
    print("   - 确认I2C地址 (0x3C或0x3D)")
    print("   - 确认供电电压 (3.3V/5V)")
    
    print("\n3. 软件调试:")
    print("   - 在代码中添加I2C扫描")
    print("   - 检查初始化返回值")
    print("   - 尝试显示简单图案")

if __name__ == "__main__":
    check_oled_config()
    check_i2c_address()
    check_power_requirements()
    check_reset_pin()
    provide_debug_steps()
    
    print("\n💡 如果以上都正确，可能需要:")
    print("   - 更换OLED模块测试")
    print("   - 检查I2C总线是否有其他设备冲突")
    print("   - 降低I2C速度 (100kHz)")