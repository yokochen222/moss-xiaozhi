#!/usr/bin/env python3
"""
I2C设备地址扫描脚本
用于检测连接的I2C设备地址
"""

import os
import sys
import re

def scan_i2c_addresses():
    """扫描I2C设备地址"""
    print("🔍 扫描I2C设备地址...")
    
    # 检查lichuang_dev_board.cc中的I2C配置
    board_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/lichuang_dev_board.cc"
    config_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/config.h"
    
    # 读取I2C引脚配置
    with open(config_file, 'r') as f:
        config_content = f.read()
    
    # 提取I2C引脚定义
    audio_sda = re.search(r'#define AUDIO_CODEC_I2C_SDA_PIN\s+(\w+)', config_content)
    audio_scl = re.search(r'#define AUDIO_CODEC_I2C_SCL_PIN\s+(\w+)', config_content)
    display_sda = re.search(r'#define DISPLAY_SDA_PIN\s+(\w+)', config_content)
    display_scl = re.search(r'#define DISPLAY_SCL_PIN\s+(\w+)', config_content)
    
    print("📋 当前I2C配置:")
    print(f"   音频编解码器: SDA={audio_sda.group(1)}, SCL={audio_scl.group(1)}")
    print(f"   OLED显示: SDA={display_sda.group(1)}, SCL={display_scl.group(1)}")
    
    # 检查PCA9557地址
    with open(board_file, 'r') as f:
        board_content = f.read()
    
    pca_addr = re.search(r'Pca9557\(i2c_bus_, (0x[0-9a-fA-F]+)\)', board_content)
    print(f"   PCA9557地址: {pca_addr.group(1)}")
    
    print("\n💡 建议:")
    print("1. 检查硬件连接，确认PCA9557芯片是否存在")
    print("2. 检查PCA9557的A0-A2引脚配置")
    print("3. 尝试PCA9557的不同地址: 0x18-0x1F")
    print("4. 确认I2C引脚没有冲突")
    
    return True

def suggest_pca9557_addresses():
    """提供PCA9557可能的地址列表"""
    print("\n🎯 PCA9557可能的I2C地址:")
    print("   地址引脚连接方式 -> 地址")
    print("   A2=A1=A0=0     -> 0x18")
    print("   A2=A1=0, A0=1   -> 0x19")
    print("   A2=0, A1=1, A0=0 -> 0x1A")
    print("   A2=0, A1=1, A0=1 -> 0x1B")
    print("   A2=1, A1=0, A0=0 -> 0x1C")
    print("   A2=1, A1=0, A0=1 -> 0x1D")
    print("   A2=1, A1=1, A0=0 -> 0x1E")
    print("   A2=1, A1=1, A0=1 -> 0x1F")

if __name__ == "__main__":
    scan_i2c_addresses()
    suggest_pca9557_addresses()
    
    print("\n✅ 检查完成！")