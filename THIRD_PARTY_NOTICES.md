# 第三方许可与来源记录

本记录覆盖当前源码和桌面构建中已确认的直接及传递依赖，用于保留上游声明和后续核对依据。此仓库尚无安装包或发行包；本文件不是完整部署清单，也不表示任何发行方式已完成许可审核。项目自有内容的许可见[许可范围与依赖](docs/engineering/licensing.md)。

## Rust Markdown 服务

`Cargo.lock` 当前锁定以下第三方包；`pulldown-cmark` 为直接依赖，其余为锁定的传递依赖。本项目在下表所列的上游许可选项中记录 MIT 方案，保留各包自己的原始版权与许可文本；这不改变上游同时提供的其他许可选项。

| 包 | 版本 | 上游声明 | 保留的许可文本 |
| --- | --- | --- | --- |
| `pulldown-cmark` | 0.13.6 | MIT | [原文](LICENSES/third-party/pulldown-cmark.txt) |
| `pulldown-cmark-escape` | 0.11.0 | MIT | [原文](LICENSES/third-party/pulldown-cmark-escape.txt) |
| `bitflags` | 2.13.2 | MIT 或 Apache-2.0 | [MIT 原文](LICENSES/third-party/bitflags.txt) |
| `memchr` | 2.8.3 | Unlicense 或 MIT | [MIT 原文](LICENSES/third-party/memchr.txt) |
| `unicase` | 2.10.0 | MIT 或 Apache-2.0 | [MIT 原文](LICENSES/third-party/unicase.txt) |

记录建立时，五个缓存归档的 SHA-256 与 `Cargo.lock` 一致，所保留的文本与各归档中的许可文件逐字节一致。[机器可读清单](LICENSES/third-party/manifest.json)记录版本、归档摘要和许可文本摘要。日常仓库检查核对清单、锁文件与文本的对应关系；这不能替代升级依赖时重新核对上游声明。

## Scintilla

桌面构建从项目准备目录中的 Scintilla 5.6.7 源码构建静态库，版本和原始下载摘要锁定在[桌面依赖清单](tools/desktop-dependencies.json)。其[许可及版权原文](LICENSES/third-party/scintilla.txt)来自所用源码目录，并与 [Scintilla 官方许可](https://www.scintilla.org/License.txt)的内容核对。该许可要求保留上游版权与许可声明，具体条件以原文为准。`xdnote` 没有将其改标为项目自有代码。

## Qt 与运行库

当前 CMake 入口使用 Qt 6.8.2 的 Widgets、QML、Quick、Quick Widgets、Quick Controls 2、Concurrent、Network、SVG 及其传递模块；本机 SDK 使用共享 Qt 库。这些构建依赖不等于未来某个平台的实际交付文件清单。Qt 模块可用许可和其中的第三方组件须按[Qt 许可说明](https://doc.qt.io/qt-6.8/licensing.html)、固定版本源码、构建配置及实际包逐项核对。

产品不链接或部署 Qt WebEngine／Chromium。本机 Qt 6.8.2 SDK 的 `sbom/` 包含各模块 SPDX 文档，其中部分包的许可字段为 `NOASSERTION`；这些文档有助于清点，但不能单独证明许可文本和随包义务已齐备。不同平台的插件、QML 模块、系统库和 MSVC 运行库也须根据实际交付内容核对。

Qt 6.8.2 的 [LGPL-3.0 原文](LICENSES/third-party/Qt-LGPL-3.0-only.txt)及其引用的 [GPL-3.0 原文](LICENSES/third-party/Qt-GPL-3.0-only.txt)以[来源和摘要](tools/deployment-notices.json)固定并由日常检查核对。实际 Windows 诊断目录生成与匹配 Qt 模块 SPDX／声明的收集见[部署诊断](docs/engineering/deployment.md)；模块 SBOM 的未确定字段、源码提供／用户替换等要求及最终发行核验仍须独立完成。

## 字体、设计与研发工具

当前源码不包含 TTF、OTF、WOFF 或 WOFF2 字体文件；界面代码请求系统字体和回退字体。设计素材清单将复用图标记为项目原创几何，并注明没有导出字体；设计素材的项目许可与来源见[素材交付说明](docs/visual/asset-handoff.md)。这些记录不授予第三方字体或素材的额外权利。

`sharp`、Node、Python 构建工具、CMake、Ninja 和 SDK 准备工具用于设计检查或构建；当前尚无证据表明它们随产品发行。若后续交付包含这些工具、其他字体或素材，需重新核对其许可、版权和实际版本。

## 发行前仍需完成

针对每个实际包生成文件和依赖清单，核对 Qt 第三方声明、所选 Qt 许可及其源码提供和用户替换等适用条件、平台运行库、字体和设计素材署名；将所需文本与对应内容一并交付，并在最终构建配置或依赖变化时重新检查。具体交付义务以所选许可、实际内容及适用发行方式为准。
