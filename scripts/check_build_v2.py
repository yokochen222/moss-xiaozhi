#!/usr/bin/env python3
"""
更新后的编译检查脚本
检查I2C配置和PCA9557问题修复
"""

import os
import sys
import re

def check_i2c_config():
    """检查I2C配置"""
    print("🔍 检查I2C配置...")
    
    config_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/config.h"
    board_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/lichuang_dev_board.cc"
    
    # 检查I2C引脚配置
    with open(config_file, 'r') as f:
        config_content = f.read()
    
    # 提取I2C引脚定义
    audio_sda = re.search(r'#define AUDIO_CODEC_I2C_SDA_PIN\s+(\w+)', config_content)
    audio_scl = re.search(r'#define AUDIO_CODEC_I2C_SCL_PIN\s+(\w+)', config_content)
    display_sda = re.search(r'#define DISPLAY_SDA_PIN\s+(\w+)', config_content)
    display_scl = re.search(r'#define DISPLAY_SCL_PIN\s+(\w+)', config_content)
    
    print("📋 I2C配置:")
    print(f"   音频编解码器: SDA={audio_sda.group(1)}, SCL={audio_scl.group(1)}")
    print(f"   OLED显示: SDA={display_sda.group(1)}, SCL={display_scl.group(1)}")
    
    # 检查地址冲突
    if audio_sda.group(1) == display_sda.group(1) and audio_scl.group(1) == display_scl.group(1):
        print("❌ 地址冲突: 音频编解码器和OLED显示使用相同的I2C引脚")
        return False
    else:
        print("✅ 无I2C地址冲突")
    
    return True

def check_pca9557_handling():
    """检查PCA9557处理"""
    print("🔍 检查PCA9557处理...")
    
    board_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/lichuang_dev_board.cc"
    
    with open(board_file, 'r') as f:
        content = f.read()
    
    # 检查是否禁用了PCA9557强制初始化
    if "PCA9557 not found, continuing without GPIO expander" in content:
        print("✅ PCA9557初始化已改为可选")
    else:
        print("❌ PCA9557初始化仍为强制")
        return False
    
    return True

def check_oled_config():
    """检查OLED配置"""
    print("🔍 检查OLED配置...")
    
    config_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/config.h"
    
    with open(config_file, 'r') as f:
        content = f.read()
    
    # 检查OLED尺寸
    width = re.search(r'#define DISPLAY_WIDTH\s+(\d+)', content)
    height = re.search(r'#define DISPLAY_HEIGHT\s+(\d+)', content)
    
    if width and height and int(width.group(1)) == 128 and int(height.group(1)) == 64:
        print("✅ OLED尺寸配置正确: 128x64")
    else:
        print("❌ OLED尺寸配置错误")
        return False
    
    return True

if __name__ == "__main__":
    print("🚀 更新后的编译检查...\n")
    
    all_good = True
    all_good &= check_i2c_config()
    all_good &= check_pca9557_handling()
    all_good &= check_oled_config()
    
    print()
    if all_good:
        print("✅ 所有检查通过！可以尝试重新编译")
    else:
        print("❌ 存在配置问题，请修复后重试")
    
    print("\n💡 编译建议:")
    print("   idf.py build")
    print("   如果遇到I2C错误，检查硬件连接")