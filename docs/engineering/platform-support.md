# 平台目标与验证基线

本页记录已授权的研发目标：Windows 最低大版本为 Windows 10；桌面 PC 采用 64 位 x86_64／AMD64，macOS 同时覆盖 Intel 与 Apple Silicon。其余最低版本与 Linux 图形环境按下表确定。它约束后续选型、构建和验收，不表示当前已有产品或这些环境已全部通过测试。

## 目标矩阵

| 平台 | 最低目标与参考环境 | CPU 架构 | 图形环境 |
| --- | --- | --- | --- |
| Windows | Windows 10 1809 及以上，覆盖 Windows 11 | x86_64／AMD64 | Windows 原生桌面 |
| macOS | macOS 13 及以上 | x86_64、arm64；分别构建和核验 | macOS 原生桌面 |
| Linux | Ubuntu 22.04 LTS 为最低参考基线，Ubuntu 24.04 LTS 为后续参考环境 | x86_64／AMD64 | 优先 GNOME；Wayland 与 X11 均纳入验证 |

本轮目标不包含 32 位 x86，也不包含 Windows 和 Linux 的原生 ARM64 包。macOS 的 ARM 系列指 Apple Silicon 的 arm64；Intel 与 ARM 的产物不能以同一台设备上的一次运行相互代替。是否合成通用二进制由正式打包验证确定。

其他 Linux 发行版、桌面环境和文件系统按实际结果单独登记，不能仅凭同属 Linux 或具有相近库版本就宣称支持。系统版本高于最低目标也不自动代表已经验收。

## 基线依据与构建约束

当前 Qt 6.8 的[平台支持表](https://doc.qt.io/qt-6.8/supported-platforms.html)列出了 Windows 10 1809+ 的 x86_64、macOS 12+ 的 x86_64／arm64，以及 Ubuntu 22.04／24.04 的桌面配置。上述 xdnote 目标在此范围内选取；macOS 13 是本项目的最低目标选择。这是现有候选的选型依据，不将试验使用的 Qt 版本直接变成正式产品工具链。

1. Windows 选用的 SDK、编译器运行库和依赖须保留 Windows 10 1809 的运行条件，不能只在更新的 CI 系统构建成功就提高最低版本。
2. macOS 构建显式设置最低部署目标，并逐一核验输出架构和全部随包依赖；构建主机系统版本与部署目标分别记录。
3. Linux 以 Ubuntu 22.04 的用户态为最低参考；产品和随包依赖都要核对 glibc、libstdc++ 等 ABI 要求。不得用更高版本主机的构建结果直接宣称最低环境兼容，也不能将其他架构的 Qt 二进制要求套用到 x86_64。
4. Wayland 与 X11 分别验证窗口激活、标题栏、输入法、剪贴板、拖放、缩放和辅助功能；平台固有差异按设计系统与系统约定处理。
5. 任何依赖升级若要求提高最低系统、改变已定 CPU 范围或取消图形环境，先记录影响并与用户讨论，再修改本基线。

## 自动化与实际验收

正式工程的自动化矩阵应覆盖 Windows x86_64、macOS x86_64、macOS arm64 和 Linux x86_64，并记录构建主机、SDK、目标系统、输出架构及依赖身份。Linux 至少有最低参考环境的构建验证；对可自动运行的 X11／Wayland 场景分别记录真实执行入口。

现有 GitHub Actions 只验证材料和技术试验。Windows Server runner 不等于 Windows 10 1809 运行验收，macOS runner 标签不等于已核对输出架构和最低部署目标，Ubuntu 离屏测试也不等于 GNOME、Wayland 或 X11 的真实桌面验收。不能将当前 CI 描述为正式产品构建或已认证的支持矩阵。

按用户此前安排，macOS／Linux 实机、真实中文输入法和屏幕阅读器验收暂缓，继续推进自动化及工程准备。每次运行的证据和未执行项记录在 Issue、PR 或 CI；本页保留目标和通用约束。发布前仍须核对最低系统上的实际产品、安装和运行结果。

工具链、正式工程及产品 CI 由 [Issue #11](https://github.com/xiaodaozhi/xdnote/issues/11) 跟踪，平台验收方法见[验收规格](../qa/acceptance.md)。
