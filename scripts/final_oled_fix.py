#!/usr/bin/env python3
"""
OLED显示最终修复诊断脚本
"""

import os
import re

def check_fixes():
    """检查修复是否已应用"""
    print("🔧 检查OLED修复状态...")
    
    board_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/lichuang_dev_board.cc"
    
    with open(board_file, 'r') as f:
        content = f.read()
    
    fixes = [
        ("SH1106头文件", r'#include.*esp_lcd_panel_sh1106\.h'),
        ("SH1106驱动支持", r'esp_lcd_new_panel_sh1106'),
        ("I2C速度50kHz", r'\.scl_speed_hz\s*=\s*50\s*\*\s*1000'),
        ("初始化延迟", r'vTaskDelay.*100'),
        ("错误处理", r'if.*init_ret.*!=.*ESP_OK'),
        ("调试日志", r'ESP_LOGI.*Initializing OLED'),
    ]
    
    all_applied = True
    for fix_name, pattern in fixes:
        if re.search(pattern, content, re.IGNORECASE):
            print(f"   ✅ {fix_name}")
        else:
            print(f"   ❌ {fix_name}")
            all_applied = False
    
    return all_applied

def provide_next_steps():
    """提供下一步操作"""
    print("\n🎯 下一步操作:")
    print("1. 编译并烧录代码:")
    print("   idf.py build")
    print("   idf.py flash")
    print("   idf.py monitor")
    
    print("\n2. 观察串口日志中的关键信息:")
    print("   - 'Found OLED display at address 0x3C'")
    print("   - 'SSD1306 init failed, trying SH1106...'")
    print("   - 'OLED driver installed'")
    print("   - 'Initializing OLED display...'")
    
    print("\n3. 如果仍有问题:")
    print("   - 检查模块规格（SSD1306 vs SH1106）")
    print("   - 确认I2C地址跳线设置")
    print("   - 更换OLED模块测试")
    print("   - 检查电源稳定性")

def check_module_type():
    """检查模块类型"""
    print("\n📱 OLED模块类型确认:")
    print("   SSD1306 vs SH1106区别:")
    print("   - SSD1306: 更常见，0x3C地址")
    print("   - SH1106: 兼容SSD1306，但初始化不同")
    print("   - 查看模块背面芯片型号")
    print("   - 尝试两种驱动都支持")

if __name__ == "__main__":
    print("🖥️  OLED最终修复诊断")
    print("=" * 40)
    
    fixes_applied = check_fixes()
    check_module_type()
    provide_next_steps()
    
    if fixes_applied:
        print("\n✅ 所有修复已应用，请编译测试")
    else:
        print("\n⚠️  部分修复未应用，请重新应用修复")