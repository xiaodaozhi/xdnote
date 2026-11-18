# macOS 更新网络准入

原生自动安装更新的准入为 macOS 14+、Apple Silicon arm64，覆盖系统代理、直连及无凭据 HTTP CONNECT／SOCKS5 自定义代理。低于该范围不承诺原生自动安装，不采用旧会话接口扩展支持。应用的最低系统与 CPU 范围以[平台目标](platform-support.md)为准。产品已链接本项目准备的固定 Sparkle，以下依赖和网络准入仍不能代表已交付完整安装能力。正式安装还须满足[更新契约](release-and-updates.md)的独立信任根、文档保护、任务交接和恢复要求。

## 固定依赖与构建

[sparkle-dependency.json](../../tools/sparkle-dependency.json)固定 Sparkle 2.10.0 官方 SDK、源码、许可原文和本项目[网络补丁](../../patches/sparkle/2.10.0-download-proxy.patch)的 SHA-256。原始输入和本机构建均在本项目 `build/Sparkle/2.10.0-xdnote-proxy.2/`；不读取其他项目的 SDK 或工具，不执行上游 Xcode shell 构建阶段。准备器保留框架必需的内部链接，拒绝路径逃逸、链接入口、不明目录和改变的文件清单。

```text
python scripts/sparkle.py prepare --download
python scripts/sparkle.py build
python scripts/sparkle.py verify
python scripts/cleanup_cycles.py --init cycle-161-sparkle
python scripts/check_sparkle.py --work-dir build/cycle-161-sparkle/probe --evidence build/evidence/sparkle
```

只有 `prepare --download` 可以下载；`prepare` 也可复用已核验的本项目归档缓存，`build`／`verify` 不访问网络。构建使用原生 Apple Silicon arm64 的系统 Apple 编译器和 Command Line Tools，逐项读取固定 Xcode 源码阶段，编译 67 个 framework 与 10 个 Ed25519 翻译单元，目标为 macOS 14.0。记录编译器、SDK、宿主、源码和实际代码摘要；目标字段不是最低系统运行验收。

主库、公共代理头及框架元数据包含本项目修改；移除与新代码不对应的上游 dSYM，只重建该框架的 ad-hoc 完整性封印。四个 helper、资源和签名工具保持官方 SDK 字节身份，不宣称它们由源码重建。原生检查分别记录 helper 架构、部署目标、签名 flags 和 entitlements，拒绝调试附加权限；不使用递归重签替代逐项签名策略。依赖整体许可为 `MIT AND BSD-2-Clause AND Zlib`，保留[完整原文](../../LICENSES/third-party/Sparkle.txt)。正式部署的逐文件归属和许可还须另验。

## 原生代理接口

Apple 的 Network `proxyConfigurations`、HTTP CONNECT／SOCKS5 构造器及显式 failover 控制从 macOS 14 起可用。自定义代理使用这组 API，禁止失败后回退直连。直连仍使用 [`connectionProxyDictionary`](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/connectionproxydictionary?language=objc)明确关闭各类代理、PAC 和自动发现；`nil` 字典或空的现代代理数组仍表示系统策略，不能用作直连。

| 路径 | 准入实现 | 尚需证明 |
| --- | --- | --- |
| 系统代理／直连 | 共享会话工厂；系统保留原生策略，直连明确关闭 HTTP、HTTPS、SOCKS、FTP、PAC 和自动发现 | 最低支持系统的实际配置、PAC／自动发现及完整原生更新 |
| macOS 14+ 的 Network API | 启动前配置固定；无凭据 HTTP CONNECT／SOCKS5，显式禁止代理 failover，非系统模式不使用未适配的 Downloader XPC | 同一候选的 feed／归档、取消、安装和恢复 |

不为旧系统另建自定义代理下载路径，不引入本地中转协议、TLS 解密或系统代理修改。库内的 Network 自定义代理入口保留可用性守卫；探针拒绝在 macOS 14 以下执行原生更新准入。最低系统运行、完整安装及恢复仍须在实际支持范围内分别验收，不能从 API 可用或新宿主结果推断。

## 会话与探针边界

共享工厂使用临时 session，关闭 Cookie、凭据和 URL 缓存，保留系统 TLS 验证及 TLS 1.2 下限；不接收信任根、TLS 绕过或代理凭据。`SPUUpdater` 的策略仅能在主线程、启动前设置，供 feed 与归档的同一 downloader 使用。产品提供方沿用同一工厂，当前生产公钥为空；缺钥时不会开放自动安装。

[原生探针](../../experiments/sparkle-admission/probe.m)验证配置、非法输入、线程与启动状态；只在自有临时 bundle 中同步启动已禁用自动检查的实例，立即退出，不执行排队的更新周期。公开测试向量密钥只存在于该临时探针 plist，不能成为产品信任根。

网络夹具仅绑定回环端口。直连 TLS 使用回环 IP；代理使用保留的 `.invalid` 目标名，避免系统对回环 IP 的特殊绕过。拒绝代理记录 CONNECT／SOCKS5 握手及错误；固定映射的隧道代理只连接自有 TLS 服务，确认默认系统信任处理拒绝自签证书。探针观察信任结果后始终交给默认处理，不接受证书、不添加锚点。默认本机探针只访问自有回环服务，不修改系统网络偏好。

手动 CI 额外在一次性 GitHub 托管运行器启用仅匹配专用测试域的系统 PAC。先用相同主机名证明源站可被直接访问，再记录 CFNetwork 看到的 PAC、PAC 下载、代理 CONNECT、代理拒绝和直连源站计数。测试退出时禁用 PAC 并恢复临时 hosts 项。真实自动发现、最低系统与正式更新候选独立验收。

输出区分实际宿主、拒绝路径、TLS 隧道和 SDK 来源。测试不代替最低支持系统、真实公网 HTTPS／代理、原生安装、helper 退出及恢复验收。手动 `scope=sparkle-macos` 执行所选 macOS 架构的这一范围，不准备 Qt、不运行完整桌面矩阵，也不发布软件。
