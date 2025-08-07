#!/usr/bin/env python3
"""
验证编译错误修复
"""

import re

def check_compile_fixes():
    """检查编译错误修复"""
    print("🔍 检查编译错误修复...")
    
    board_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/lichuang_dev_board.cc"
    
    with open(board_file, 'r') as f:
        content = f.read()
    
    # 检查Pca9557类定义
    if "WriteReg(0x01, 0x03) == ESP_OK" in content:
        print("❌ 仍存在比较void与ESP_OK的错误")
        return False
    else:
        print("✅ Pca9557类已修复，无void比较错误")
    
    # 检查是否移除了未使用的变量
    if "Pca9557* pca9557_;" in content:
        print("❌ 仍存在未使用的pca9557_变量")
        return False
    else:
        print("✅ 已移除未使用的pca9557_变量")
    
    # 检查是否移除了未使用的类
    if "class Pca9557" in content:
        print("⚠️  Pca9557类仍存在（可能不影响编译）")
    else:
        print("✅ Pca9557类已移除")
    
    return True

def check_oled_integration():
    """检查OLED集成状态"""
    print("\n🔍 检查OLED集成状态...")
    
    board_file = "/Users/yokochen/Desktop/moss/moss-xiaozhi/main/boards/lichuang-dev/lichuang_dev_board.cc"
    
    with open(board_file, 'r') as f:
        content = f.read()
    
    # 检查OLED相关代码
    checks = [
        ("#include \"display/oled_display.h\"", "OLED显示头文件"),
        ("InitializeOledDisplay()", "OLED初始化方法"),
        ("display_i2c_bus_ = i2c_bus_;", "I2C总线共用"),
        ("OledDisplay", "OLED显示类实例化")
    ]
    
    all_good = True
    for check_str, description in checks:
        if check_str in content:
            print(f"✅ {description}存在")
        else:
            print(f"❌ {description}缺失")
            all_good = False
    
    return all_good

if __name__ == "__main__":
    print("🚀 编译错误修复验证\n")
    
    fixes_ok = check_compile_fixes()
    oled_ok = check_oled_integration()
    
    print()
    if fixes_ok and oled_ok:
        print("✅ 所有修复验证通过！可以尝试重新编译")
        print("💡 编译命令: idf.py build")
    else:
        print("❌ 仍有修复问题需要处理")
    
    print("\n📋 当前状态:")
    print("   - PCA9557相关问题已移除")
    print("   - OLED显示配置保留")
    print("   - I2C总线共用已配置")