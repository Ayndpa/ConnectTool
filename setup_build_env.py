#!/usr/bin/env python3
"""
一键配置不同平台上的构建环境脚本
支持 Windows、Linux 和 macOS 平台
"""

import os
import sys
import subprocess
import platform
import shutil
from pathlib import Path


def detect_platform():
    """检测当前操作系统平台"""
    system = platform.system().lower()
    if system == "windows":
        return "windows"
    elif system == "linux":
        return "linux"
    elif system == "darwin":
        return "macos"
    else:
        raise RuntimeError(f"不支持的操作系统: {system}")


def check_prerequisites():
    """检查先决条件"""
    print("检查先决条件...")
    
    # 检查是否安装了Git
    try:
        subprocess.run(["git", "--version"], check=True, capture_output=True)
        print("✓ Git 已安装")
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("✗ 未找到 Git，请先安装 Git")
        return False
    
    # 检查是否安装了CMake
    try:
        subprocess.run(["cmake", "--version"], check=True, capture_output=True)
        print("✓ CMake 已安装")
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("✗ 未找到 CMake，请先安装 CMake 3.10 或更高版本")
        return False
        
    return True


def init_submodules():
    """初始化子模块"""
    print("初始化子模块...")
    try:
        subprocess.run(["git", "submodule", "init"], check=True)
        subprocess.run(["git", "submodule", "update"], check=True)
        print("✓ 子模块初始化完成")
        return True
    except subprocess.CalledProcessError:
        print("✗ 子模块初始化失败")
        return False


def check_steamworks_sdk():
    """检查Steamworks SDK是否存在"""
    steamworks_path = Path("steamworks")
    if not steamworks_path.exists():
        print("⚠ 未找到 Steamworks SDK")
        print("  请从 https://partner.steamgames.com/ 下载 Steamworks SDK")
        print("  并将其解压到项目根目录的 'steamworks' 文件夹中")
        return False
    
    # 检查各平台的库文件是否存在
    platform_libs = {
        "windows": ["redistributable_bin/win64/steam_api64.lib", 
                   "redistributable_bin/win64/steam_api64.dll"],
        "linux": ["redistributable_bin/linux64/libsteam_api.so"],
        "macos": ["redistributable_bin/osx/libsteam_api.dylib"]
    }
    
    current_platform = detect_platform()
    missing_libs = []
    
    for lib in platform_libs[current_platform]:
        if not (steamworks_path / lib).exists():
            missing_libs.append(lib)
            
    if missing_libs:
        print(f"⚠ 当前平台 ({current_platform}) 缺少以下 Steamworks 库文件:")
        for lib in missing_libs:
            print(f"  - {lib}")
        return False
        
    print("✓ Steamworks SDK 已正确配置")
    return True


def setup_windows():
    """Windows平台环境配置"""
    print("配置 Windows 构建环境...")
    
    # 检查vcpkg
    vcpkg_root = os.environ.get("VCPKG_ROOT")
    if not vcpkg_root or not Path(vcpkg_root).exists():
        print("⚠ 未找到 VCPKG_ROOT 环境变量或路径不存在")
        print("  建议安装 vcpkg 并设置 VCPKG_ROOT 环境变量")
        print("  参考: https://github.com/microsoft/vcpkg")
        return False
    
    vcpkg_exe = Path(vcpkg_root) / "vcpkg.exe"
    if not vcpkg_exe.exists():
        print("⚠ 未找到 vcpkg 可执行文件")
        return False
    
    # 安装依赖
    try:
        print("正在安装依赖项 (glfw3, boost-system)...")
        subprocess.run([str(vcpkg_exe), "install", "glfw3", "boost-system"], check=True)
        print("✓ Windows 依赖项安装完成")
        return True
    except subprocess.CalledProcessError:
        print("✗ Windows 依赖项安装失败")
        return False


def setup_linux():
    """Linux平台环境配置"""
    print("配置 Linux 构建环境...")
    
    # 检查是否有sudo权限
    try:
        subprocess.run(["sudo", "-v"], check=True, capture_output=True)
    except subprocess.CalledProcessError:
        print("⚠ 需要 sudo 权限来安装依赖项")
        return False
    
    # 检查包管理器
    package_manager = None
    package_cmd = None
    
    if shutil.which("apt"):
        package_manager = "apt"
        package_cmd = ["sudo", "apt", "install", "-y"]
    elif shutil.which("yum"):
        package_manager = "yum"
        package_cmd = ["sudo", "yum", "install", "-y"]
    elif shutil.which("dnf"):
        package_manager = "dnf"
        package_cmd = ["sudo", "dnf", "install", "-y"]
    elif shutil.which("pacman"):
        package_manager = "pacman"
        package_cmd = ["sudo", "pacman", "-S", "--noconfirm"]
    else:
        print("✗ 未找到支持的包管理器 (apt, yum, dnf, pacman)")
        return False
    
    print(f"检测到包管理器: {package_manager}")
    
    # 安装依赖
    deps = ["libglfw3-dev", "libboost-system-dev"]
    try:
        print(f"正在安装依赖项: {', '.join(deps)}...")
        subprocess.run(package_cmd + deps, check=True)
        print("✓ Linux 依赖项安装完成")
        return True
    except subprocess.CalledProcessError:
        print("✗ Linux 依赖项安装失败")
        return False


def setup_macos():
    """macOS平台环境配置"""
    print("配置 macOS 构建环境...")
    
    # 检查Homebrew
    if not shutil.which("brew"):
        print("✗ 未找到 Homebrew，请先安装 Homebrew:")
        print("  /bin/bash -c \"$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\"")
        return False
    
    # 安装依赖
    deps = ["glfw", "boost"]
    try:
        print(f"正在安装依赖项: {', '.join(deps)}...")
        subprocess.run(["brew", "install"] + deps, check=True)
        print("✓ macOS 依赖项安装完成")
        return True
    except subprocess.CalledProcessError:
        print("✗ macOS 依赖项安装失败")
        return False


def generate_build_instructions():
    """生成构建说明"""
    platform_name = detect_platform()
    
    instructions = {
        "windows": """
Windows 构建说明:
1. 创建构建目录:
   mkdir build
   cd build
   
2. 配置 CMake:
   cmake .. -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
   
3. 构建项目:
   cmake --build . --config Release
   
4. 运行程序:
   Release\\ConnectTool.exe
""",
        
        "linux": """
Linux 构建说明:
1. 创建构建目录:
   mkdir build
   cd build
   
2. 配置 CMake:
   cmake ..
   
3. 构建项目:
   make
   
4. 运行程序:
   ./ConnectTool
""",
        
        "macos": """
macOS 构建说明:
1. 创建构建目录:
   mkdir build
   cd build
   
2. 配置 CMake:
   cmake ..
   
3. 构建项目:
   make
   
4. 运行程序:
   ./ConnectTool
"""
    }
    
    print(instructions[platform_name])


def main():
    """主函数"""
    print("ConnectTool 构建环境配置脚本")
    print("=" * 40)
    
    # 显示当前平台
    current_platform = detect_platform()
    print(f"检测到当前平台: {current_platform}")
    
    # 检查先决条件
    if not check_prerequisites():
        print("\n请安装所需的先决条件后再运行此脚本。")
        sys.exit(1)
    
    # 初始化子模块
    if not init_submodules():
        print("\n子模块初始化失败。")
        sys.exit(1)
    
    # 检查Steamworks SDK
    if not check_steamworks_sdk():
        print("\n请确保 Steamworks SDK 正确配置。")
        # 不退出，因为用户可能稍后手动配置
    
    # 根据平台配置环境
    setup_result = False
    if current_platform == "windows":
        setup_result = setup_windows()
    elif current_platform == "linux":
        setup_result = setup_linux()
    elif current_platform == "macos":
        setup_result = setup_macos()
    
    # 显示结果
    print("\n" + "=" * 40)
    if setup_result:
        print("✓ 构建环境配置完成!")
        print("\n下一步构建说明:")
        generate_build_instructions()
    else:
        print("⚠ 构建环境配置过程中出现问题")
        print("\n手动构建说明:")
        generate_build_instructions()
        print("\n请根据上述说明手动配置环境。")


if __name__ == "__main__":
    main()