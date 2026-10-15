# 运行依赖补全

新增源码替代实现，不是找回原作者发布的 DLL。主要目标为当前仓库的 Windows x64 开发构建。

## Windows 构建与测试

安装 Visual Studio 的 MSVC v142 工具集和 Windows SDK 后，在仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File RuntimeDependencies\Build-RuntimeDependencies.ps1
```

输出在 `x64\Release\bin`：`AIHelper.dll`、`D-encryption.dll` 和 `TestRuntime.exe`。脚本会运行测试并输出 DLL SHA-256；测试只读导出表、检查参数并在临时目录测试加解密，不加载驱动或注入进程。

WinUI 项目已引用这两个 DLL 工程，正常构建会自动生成并复制到 `x64\WinUI\Release\bin`。**必须同时重新编译 UnrealDbgProDll**，不能只把新 AIHelper DLL 放进旧程序。

## 接口与边界

- AIHelper 的 `LoadNTW` / `UnloadNTW` 使用稳定 C ABI，参数是 UTF-16 指针。后端已改用这两个导出，避免旧版按值传递 `std::wstring` 的 STL/CRT 兼容问题。MSVC 构建另外保留旧 `LoadNT` / `UnloadNT` 导出；旧接口仍要求相同 MSVC ABI。
- 驱动服务通过 Windows SCM 创建、启动、停止和删除，保留 Win32 错误；已有服务由后端检查身份和路径，不在 helper 中自动接管。启动失败只删除本次创建的服务。
- `InjectDll` 实现普通 Windows x64 `LoadLibraryW` 远程线程注入，按目标模块基址定位入口。仅支持调用者具备访问权限的 x64 进程，不提供原版可能存在的内核注入或跨位数能力。等待超时不释放仍可能被远程线程使用的参数内存，记录错误并返回。
- `outDebug` 将格式化消息传给宿主 `PrintLog`；宿主不存在回调时使用调试输出。
- D-encryption 依据归档 C++ 源码实现 UTF-8 + CryptoAPI SHA-256 派生 AES-256，保留导出名称与序号。支持 Unicode 文件路径，限制文件大小，保留错误码。没有原版加密样本，历史 `copyright.db` 的兼容性尚未实测。
- 解密遵循现有前端的“两次调用”方式：先传空输出指针查询字节数，再在**同一线程**使用相同路径和密钥复制结果。查询结果固定保存，避免两次调用之间文件变长造成越界。旧 ABI 无容量参数，调用者必须分配查询返回的字节数。
- x64 Debug/Release 均使用仓库原有的 `UNREALDBG_VMP_STUB` 开发模式，不需要 `VMProtectSDK64.dll`，也不具备 VMProtect 商业保护功能。没有伪造或下载商业 SDK。若要发布受保护版本，应另行使用正式 SDK 配置。

## 本次验证情况

在 macOS 上使用 MinGW-w64 交叉构建了两个 Windows x64 DLL 和测试程序，检查了导出表、PE 架构与导入依赖。交叉构建产物位于 `x64\Release\bin`，只导出新的 W 服务接口，不含 MSVC 旧 STL 接口。C++ 运行库和线程库静态链接，仍依赖 Windows 自带 API/UCRT。

尚未在 Windows 上运行测试程序，未执行 MSVC/WDK 全项目构建、驱动加载、真实注入和历史密文兼容测试。交叉构建成功不能视为上述运行验证通过。
