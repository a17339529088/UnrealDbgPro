#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
GitHub CI 专用：云端流水线瞬间全自动无痕换心与编译清单动态插桩脚本
"""
import os
import re
from pathlib import Path

# 对齐 GitHub 虚拟机及本地环境的通用拓扑路径
BASE_DIR = Path(__file__).resolve().parent
SANDBOX_CORE = BASE_DIR / "VmxBenchSandbox" / "Core"
UNREAL_DRIVER = BASE_DIR / "UnrealDbgPro" / "VT_Driver"

def execute_merge():
    print("=" * 70)
    print("      🚀 GitHub Actions 运行期：驱动核心引擎无痕换心流开始")
    print("=" * 70)
    
    # 阶段 1：物理层文件强制覆盖与平移
    print("[*] 1. 正在将独立沙箱内核资产无痕并入构建区...")
    
    # 用沙箱的每 vCPU 隔离影子状态机覆盖原厂旧 EPT.cpp/h
    (UNREAL_DRIVER / "EPT.cpp").write_text((SANDBOX_CORE / "EptHandler.cpp").read_text(encoding='utf-8-sig'), encoding='utf-8')
    (UNREAL_DRIVER / "EPT.h").write_text((SANDBOX_CORE / "EptHandler.h").read_text(encoding='utf-8-sig'), encoding='utf-8')
    print("    [+] EPT 核心物理层换心成功！")
    
    # 将高精时钟单元平移入驱动目录
    (UNREAL_DRIVER / "VmxTiming.cpp").write_text((SANDBOX_CORE / "VmxTiming.cpp").read_text(encoding='utf-8-sig'), encoding='utf-8')
    (UNREAL_DRIVER / "VmxTiming.h").write_text((SANDBOX_CORE / "VmxTiming.h").read_text(encoding='utf-8-sig'), encoding='utf-8')
    print("    [+] 时钟计算单元平移入库成功！")

    # 阶段 2：内核状态机退出流动态接线
    print("\n[*] 2. 正在解除原厂注释，对接隐藏拦截与时钟硬扣除状态机...")
    handler_file = UNREAL_DRIVER / "vmexit_handler.cpp"
    code = handler_file.read_text(encoding='utf-8-sig', errors='ignore')
    
    # 解封并接通 VmxTiming.cpp 里的硬件时钟平抑 Measure 引擎
    code = re.sub(r'//\s*(hide_vm_exit_overhead.*?);', r'vbs::timing::Measure(\1);', code)
    code = re.sub(r'//\s*(__vmx_vmwrite.*?);', r'\1;', code)
    
    # 将原本共享页表的空壳回调透明替换为每 vCPU 独立影子页拦截器
    code = code.replace("vmexit_ept_violation_handler(vcpu);", "vbs::ept::HandleEptViolation(vcpu->s_vcpu);")
    code = code.replace("vmexit_monitor_trap_flag_handler(vcpu);", "vbs::ept::HandleMonitorTrapFlag(vcpu->s_vcpu);")
    
    handler_file.write_text(code, encoding='utf-8')
    print("    [+] 硬件级内核状态机派遣链动态接线完毕！")

    # 阶段 3：MSBuild 项目文件动态插桩
    print("\n[*] 3. 正在对 VT_Driver.vcxproj 进行依赖树 XML 注入...")
    proj_file = UNREAL_DRIVER / "VT_Driver.vcxproj"
    xml = proj_file.read_text(encoding='utf-8-sig', errors='ignore')
    
    if "VmxTiming.cpp" not in xml:
        # 在编译组节点尾部动态插桩 ClCompile 项，确保 MSVC 顺利放行链接
        xml = xml.replace('  </ItemGroup>', '    <ClCompile Include="VmxTiming.cpp" />\n  </ItemGroup>', 1)
        proj_file.write_text(xml, encoding='utf-8')
        print("    [+] vxproj 编译树 XML 动态插桩成功！")
        
    print("\n" + "=" * 70)
    print("🎉 [集成完毕] 云端无痕合并成功！项目已具备直接 Release/x64 构建成片资质。")
    print("=" * 70)

if __name__ == "__main__":
    if SANDBOX_CORE.exists() and UNREAL_DRIVER.exists():
        execute_merge()
    else:
        print("[-] 错误: 路径核对失败，请确保在包含两个独立项目的根目录下执行。")
