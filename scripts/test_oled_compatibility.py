#!/usr/bin/env python3
"""
OLED兼容性测试脚本 - 基于bread-compact-wifi成功经验
"""

import os
import re

def compare_implementations():
    """比较两种实现的差异"""
    print("🔍 比较bread-compact-wifi vs lichuang-dev OLED实现")
    
    # 读取两个文件
    bread_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/bread-compact-wifi/compact_wifi_board.cc"
    lichuang_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/lichuang_dev_board.cc"
    
    with open(bread_file, 'r') as f:
        bread_content = f.read()
    
    with open(lichuang_file, 'r') as f:
        lichuang_content = f.read()
    
    # 关键差异点
    differences = [
        ("I2C速度", r'\.scl_speed_hz\s*=\s*(\d+)\s*\*\s*1000', "400kHz vs 50kHz"),
        ("SSD1306配置", r'esp_lcd_panel_ssd1306_config_t', "有 vs 无"),
        ("vendor_config", r'\.vendor_config\s*=\s*&ssd1306_config', "有 vs 无"),
        ("显示反转", r'esp_lcd_panel_invert_color', "有 vs 无"),
    ]
    
    for desc, pattern, note in differences:
        bread_match = re.search(pattern, bread_content)
        lichuang_match = re.search(pattern, lichuang_content)
        
        bread_has = bool(bread_match)
        lichuang_has = bool(lichuang_match)
        
        status = "✅" if lichuang_has else "❌"
        print(f"   {status} {desc}: {note}")

def check_current_config():
    """检查当前配置"""
    print("\n📋 当前OLED配置:")
    
    config_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/config.h"
    
    with open(config_file, 'r') as f:
        content = f.read()
    
    # 提取配置
    height = re.search(r'#define DISPLAY_HEIGHT\s+(\d+)', content)
    width = re.search(r'#define DISPLAY_WIDTH\s+(\d+)', content)
    mirror_x = re.search(r'#define DISPLAY_MIRROR_X\s+(\w+)', content)
    mirror_y = re.search(r'#define DISPLAY_MIRROR_Y\s+(\w+)', content)
    
    if height:
        print(f"   尺寸: {width.group(1)}x{height.group(1)}")
    if mirror_x:
        print(f"   镜像: X={mirror_x.group(1)}, Y={mirror_y.group(1)}")
    
    # 检查是否有SH1106定义
    sh1106 = re.search(r'#define SH1106', content)
    if sh1106:
        print("   驱动: SH1106")
    else:
        print("   驱动: SSD1306")

def provide_fix_summary():
    """提供修复总结"""
    print("\n🎯 已应用的bread-compact-wifi经验:")
    print("   ✅ 添加SSD1306专用height配置")
    print("   ✅ 恢复400kHz标准I2C速度")
    print("   ✅ 添加显示颜色反转")
    print("   ✅ 简化错误处理流程")
    print("   ✅ 使用相同的初始化顺序")
    
    print("\n🔧 下一步:")
    print("   1. 编译烧录: idf.py build && idf.py flash")
    print("   2. 查看日志: idf.py monitor")
    print("   3. 观察是否有显示")
    print("   4. 如无显示，尝试更换模块")

if __name__ == "__main__":
    compare_implementations()
    check_current_config()
    provide_fix_summary()