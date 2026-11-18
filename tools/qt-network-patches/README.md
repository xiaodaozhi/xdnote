# Qt Network 6.8.2 修正

本目录的补丁适用于独立 Qt Base 6.8.2 源码。它们是 Qt 派生修改，采用 [LGPL-3.0-only](../../LICENSES/third-party/Qt-LGPL-3.0-only.txt)，保留 Qt 原文件的版权与许可声明；不因位于工具目录而改标为项目 Apache-2.0 代码。本文采用项目文档许可。

固定输入和摘要见 [qt-network-backports.json](../qt-network-backports.json)：

| 补丁 | 范围与来源 |
| --- | --- |
| `socks5-notifications.patch` | 回移 [Qt 官方修复 da1fd728](https://github.com/qt/qtbase/commit/da1fd7285725d0e84a9ba5f1b033c0dec5a055fa) 的实现部分：持续通知已缓存数据，并保留远端关闭前的数据。 |
| `http-connect-notifications.patch` | 回移 [Qt 官方修复 ac90cc53](https://github.com/qt/qtbase/commit/ac90cc53672ee570e629c8931b2347ac0de4f5e4) 的实现部分：HTTP CONNECT 重启读取时通知已有缓存。 |
| `socks5-backpressure.patch` | 项目修正：内部引擎保留一块数据、通过受限控制 socket 施加背压，补齐阻塞读取及暂停后远端 EOF 的排空。 |
| `securetransport-pending-data.patch` | 项目 macOS 修正：通过已有 TLS 后端回调和 Apple `SSLGetBufferedReadSize()` 暴露待消费明文。 |
| `socks5-windows-close.patch` | 项目 Windows 修正：内部控制 socket 单独延后关闭通知，排空时继续遵守接收上限，避免通用 Windows 关闭处理临时取消上限。新增内部子类，不改变现有 Qt 类布局。 |

显式准备器按上述顺序检查并应用补丁，匹配原 SDK 的 Network 全部功能开关，只替换 Network 及 macOS SecureTransport 插件；原始文件、签名元数据和构建回执留在本项目缓存中。SDK 头文件、其余模块与上游 SPDX 原文不变。新文件有独立摘要及派生来源，不冒充供应方原始字节。恢复和重建方法见[开发构建](../../docs/engineering/developer-build.md)。

这些补丁与构建配方纳入项目源码版本管理，Qt 源码及补丁不加入 Release 产品载荷。`source-access.json` 记录固定源码、补丁、配方和实际二进制摘要，全部源码提供条件与最终交付仍须独立核验。OpenSSL 静态库仅用于 Qt 配置时的 DTLS／OCSP 编译链接探测，不复制进 SDK 或产品；实际运行仍使用各平台原有 TLS 后端。
