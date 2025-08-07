#!/usr/bin/env python3
"""
验证OLED配置是否正确
"""

import os
import re

def check_oled_config():
    """检查OLED配置"""
    
    config_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/config.h"
    board_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/lichuang_dev_board.cc"
    
    print("=== 检查OLED配置 ===")
    
    # 检查配置文件
    if os.path.exists(config_file):
        with open(config_file, 'r') as f:
            config_content = f.read()
            
        print("\n📁 配置文件检查:")
        if "DISPLAY_WIDTH   128" in config_content:
            print("✅ DISPLAY_WIDTH 设置为 128")
        else:
            print("❌ DISPLAY_WIDTH 未设置为 128")
            
        if "DISPLAY_HEIGHT  64" in config_content or "DISPLAY_HEIGHT  32" in config_content:
            print("✅ DISPLAY_HEIGHT 设置为 OLED 标准值")
        else:
            print("❌ DISPLAY_HEIGHT 未设置为 OLED 标准值")
            
        if "DISPLAY_SDA_PIN" in config_content and "DISPLAY_SCL_PIN" in config_content:
            print("✅ 已配置 OLED I2C 引脚")
        else:
            print("❌ 未配置 OLED I2C 引脚")
    
    # 检查板子实现文件
    if os.path.exists(board_file):
        with open(board_file, 'r') as f:
            board_content = f.read()
            
        print("\n📁 板子实现文件检查:")
        if "display/oled_display.h" in board_content:
            print("✅ 已包含 OLED 显示头文件")
        else:
            print("❌ 未包含 OLED 显示头文件")
            
        if "InitializeOledDisplay" in board_content:
            print("✅ 已添加 OLED 初始化方法")
        else:
            print("❌ 未添加 OLED 初始化方法")
            
        if "InitializeSt7789Display" not in board_content:
            print("✅ 已移除 LCD 初始化方法")
        else:
            print("❌ 仍存在 LCD 初始化方法")
            
        if "InitializeSpi" not in board_content:
            print("✅ 已移除 SPI 初始化")
        else:
            print("❌ 仍存在 SPI 初始化")

    print("\n=== 配置检查完成 ===")

if __name__ == "__main__":
    check_oled_config()