# macOS 更新网络准入

macOS 13 同样必须支持无凭据 HTTP CONNECT／SOCKS5 自定义代理下的原生自动安装，不能以手动更新或提高最低系统替代。当前产品仍未链接 Sparkle，以下是依赖和网络准入工程，不是已交付的安装能力。正式安装还须满足[更新契约](release-and-updates.md)的独立信任根、文档保护、任务交接和恢复要求。

## 固定依赖与构建

[sparkle-dependency.json](../../tools/sparkle-dependency.json)固定 Sparkle 2.10.0 官方 SDK、源码、许可原文和本项目[网络补丁](../../patches/sparkle/2.10.0-download-proxy.patch)的 SHA-256。原始输入和本机构建均在本项目 `build/Sparkle/2.10.0/`；不读取其他项目的 SDK 或工具，不执行上游 Xcode shell 构建阶段。准备器保留框架必需的内部链接，拒绝路径逃逸、链接入口、不明目录和改变的文件清单。

```text
python scripts/sparkle.py prepare --download
python scripts/sparkle.py build
python scripts/sparkle.py verify
python scripts/cleanup_cycles.py --init cycle-161-sparkle
python scripts/check_sparkle.py --work-dir build/cycle-161-sparkle/probe --evidence build/evidence/sparkle
```

只有 `prepare --download` 可以下载；`prepare` 也可复用已核验的本项目归档缓存，`build`／`verify` 不访问网络。构建使用原生 Intel 或 ARM64 的系统 Apple 编译器和 Command Line Tools，逐项读取固定 Xcode 源码阶段，编译 67 个 framework 与 10 个 Ed25519 翻译单元，目标为 macOS 13.0。记录编译器、SDK、宿主、源码和实际代码摘要；目标字段不是最低系统运行验收。

主库、公共代理头及框架元数据包含本项目修改；移除与新代码不对应的上游 dSYM，只重建该框架的 ad-hoc 完整性封印。四个 helper、资源和签名工具保持官方 SDK 字节身份，不宣称它们由源码重建。原生检查分别记录 helper 架构、部署目标、签名 flags 和 entitlements，拒绝调试附加权限；不使用递归重签替代逐项签名策略。依赖整体许可为 `MIT AND BSD-2-Clause AND Zlib`，保留[完整原文](../../LICENSES/third-party/Sparkle.txt)。正式部署的逐文件归属和许可还须另验。

## 两种原生代理接口

Apple 的 [`connectionProxyDictionary`](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/connectionproxydictionary?language=objc)可以为单个 URL session 指定代理，`nil` 表示系统策略；HTTP／HTTPS／SOCKS、PAC、自动发现及例外字段见[公开代理键](https://developer.apple.com/documentation/cfnetwork/global-proxy-settings-constants)。这些 CFNetwork 键在 Apple SDK 中标为 macOS 10.6 起可用。新的 Network `proxyConfigurations`、HTTP CONNECT／SOCKS5 构造器及显式 failover 控制则标为 macOS 14 起可用，不能由此推导整个产品要求 macOS 14。

| 路径 | 准入实现 | 尚需证明 |
| --- | --- | --- |
| 系统代理／直连 | 共享会话工厂；系统保留原生策略，直连明确关闭 HTTP、HTTPS、SOCKS、FTP、PAC 和自动发现 | macOS 13 实际系统配置、PAC／自动发现及完整原生更新 |
| macOS 14+ 的 Network API | 启动前配置固定；无凭据 HTTP CONNECT／SOCKS5，显式禁止代理 failover，非系统模式不使用未适配的 Downloader XPC | 同一候选的 feed／归档、取消、安装和恢复 |
| macOS 13 的 CFNetwork 候选 | 准入探针单独构造会话字典，明确指定一个代理，关闭 PAC／自动发现及例外；用 `CFNetworkCopyProxiesForURL` 检查单一代理候选且无 DIRECT | 真正 macOS 13 上的代理、重定向、TLS、取消及失败不直连；通过后才合入共享下载工厂 |

CFNetwork 候选使用公开键，不把设备管理配置的 `FallBackAllowed` 当成 URL session 的可靠开关，也不引入本地中转协议、TLS 解密或系统代理修改。旧接口在新系统上通过，仍不表示旧系统通过；正式接入前必须取得 macOS 13 证据。当前库内的 Network 自定义代理入口保留可用性守卫，macOS 13 候选仅在独立探针中研究；这不是最终产品对 macOS 13 的功能降级决定。

## 会话与探针边界

共享工厂使用临时 session，关闭 Cookie、凭据和 URL 缓存，保留系统 TLS 验证及 TLS 1.2 下限；不接收信任根、TLS 绕过或代理凭据。`SPUUpdater` 的策略仅能在主线程、启动前设置，供 feed 与归档的同一 downloader 使用。当前没有产品调用或生产公钥。

[原生探针](../../experiments/sparkle-admission/probe.m)验证配置、非法输入、线程与启动状态；只在自有临时 bundle 中同步启动已禁用自动检查的实例，立即退出，不执行排队的更新周期。公开测试向量密钥只存在于该临时探针 plist，不能成为产品信任根。

网络夹具仅绑定回环端口。直连 TLS 使用回环 IP；代理使用保留的 `.invalid` 目标名，避免系统对回环 IP 的特殊绕过。拒绝代理记录 CONNECT／SOCKS5 握手及错误；固定映射的隧道代理只连接自有 TLS 服务，确认默认系统信任处理拒绝自签证书。探针观察信任结果后始终交给默认处理，不接受证书、不添加锚点。它不访问真实 GitHub、文档、安装目录或系统网络偏好。

输出区分实际宿主、两种接口、拒绝路径、TLS 隧道和 SDK 来源。测试不代替 macOS 13 实机、真实公网 HTTPS／代理、原生安装、helper 退出及恢复验收。手动 `scope=sparkle-macos` 只在 macOS Intel／ARM64 执行这一范围，不准备 Qt、不运行完整桌面矩阵，也不发布软件。
