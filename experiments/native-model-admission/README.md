# 原生语义模型无界面验证

本入口构建产品的 `NativePreviewModel` 接收端和实际 Rust C ABI，仅依赖 Qt 6.8.2 Core／Test，没有 GUI、平台插件或浏览器子进程。它验证[原生预览模型契约](../../docs/engineering/native-preview-model.md)，不是阅读控件、产品迁移或内存预算验收。

准备固定 Rust、项目 Cargo 缓存、Qt 和本项目 CMake／Ninja 后，在仓库根运行。Windows 非开发者终端显式指定已安装 VS2022 的环境脚本：

```text
python scripts/cleanup_cycles.py --init cycle-native-model-admission
python experiments/native-model-admission/verify.py --qt-prefix build/Qt/6.8.2/msvc2022_64 --vcvars "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat"
```

已有周期目录时只运行第二行；`--build-dir` 可以指定其他本项目输出。Linux／macOS 提供本项目实际 `--qt-prefix` 并省略 `--vcvars`，不假定其他平台已经运行通过。入口复用构建脚本的项目工具归属检查，拒绝借用其他仓库的 SDK 或构建工具。

脚本依次运行 Rust fmt、clippy、工作区测试和 release 库构建，再用同一份库构建 Qt Core 契约测试。Windows 临时为测试进程指定 Qt DLL 路径，不修改系统 PATH；递归检查实际导入，拒绝 GUI 或浏览器依赖。它不调用旧产品运行环境、不启动桌面窗口、不下载依赖；缺少缓存或工具时明确失败。

输出包括工具链和源码摘要、库／测试二进制摘要、实际导入和完整测试报告，均放在忽略的构建目录。`model-verification.json` 的 `complete=true` 只表示本范围测试和报告完整通过，不表示原生布局或产品已可交付。报告中的编码字节数不包含解析／接收瞬时拷贝，不等于进程峰值内存。
