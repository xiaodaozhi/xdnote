# 第三方许可与来源记录

本记录覆盖当前源码和桌面构建中已确认的直接及传递依赖，用于保留上游声明和后续核对依据。当前已有开发安装和归档入口，尚无正式发行；本文件不是完整部署清单，也不表示任何发行方式已完成许可审核。项目自有内容的许可见[许可范围与依赖](docs/engineering/licensing.md)。

## Rust Markdown 服务

Markdown 服务使用以下第三方包；`pulldown-cmark` 为直接依赖，其余为其锁定的传递依赖。本项目在下表所列的上游许可选项中记录 MIT 方案，保留各包自己的原始版权与许可文本；这不改变上游同时提供的其他许可选项。

| 包 | 版本 | 上游声明 | 保留的许可文本 |
| --- | --- | --- | --- |
| `pulldown-cmark` | 0.13.6 | MIT | [原文](LICENSES/third-party/pulldown-cmark.txt) |
| `pulldown-cmark-escape` | 0.11.0 | MIT | [原文](LICENSES/third-party/pulldown-cmark-escape.txt) |
| `bitflags` | 2.13.2 | MIT 或 Apache-2.0 | [MIT 原文](LICENSES/third-party/bitflags.txt) |
| `memchr` | 2.8.3 | Unlicense 或 MIT | [MIT 原文](LICENSES/third-party/memchr.txt) |
| `unicase` | 2.10.0 | MIT 或 Apache-2.0 | [MIT 原文](LICENSES/third-party/unicase.txt) |

记录建立时，五个缓存归档的 SHA-256 与 `Cargo.lock` 一致，所保留的文本与各归档中的许可文件逐字节一致。[机器可读清单](LICENSES/third-party/manifest.json)记录版本、归档摘要和许可文本摘要。日常仓库检查核对清单、锁文件与文本的对应关系；这不能替代升级依赖时重新核对上游声明。

## Rust 更新核心及工作区构建依赖

无 Qt 的 `xdnote-update` 直接依赖 `ed25519-dalek`、`semver`、`serde`、`serde_json` 和 `sha2`；通过 `xdnote-services` 与 Markdown 服务共同组成桌面 C ABI 静态库。独立发行签名工具启用 `release-tool`，使用已记录的 `base64ct` 和 `zeroize`。以下记录覆盖完整 Cargo 锁文件，包括传递、构建、目标条件及锁定的可选依赖；最终静态组件以实际构建核对。

| 包 | 版本 | 上游声明／本项目保留方案 | 原始文本 |
| --- | --- | --- | --- |
| `base64ct` | 1.8.3 | `Apache-2.0 OR MIT`；保留 `MIT` | [许可](LICENSES/third-party/base64ct.txt) |
| `block-buffer` | 0.10.4 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/block-buffer.txt) |
| `cfg-if` | 1.0.5 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/cfg-if.txt) |
| `const-oid` | 0.9.6 | `Apache-2.0 OR MIT`；保留 `MIT` | [许可](LICENSES/third-party/const-oid.txt) |
| `cpufeatures` | 0.2.17 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/cpufeatures.txt) |
| `crypto-common` | 0.1.7 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/crypto-common.txt) |
| `curve25519-dalek` | 4.1.3 | `BSD-3-Clause`；保留 `BSD-3-Clause` | [许可](LICENSES/third-party/curve25519-dalek.txt) |
| `curve25519-dalek-derive` | 0.1.1 | `MIT/Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/curve25519-dalek-derive.txt) |
| `der` | 0.7.10 | `Apache-2.0 OR MIT`；保留 `MIT` | [许可](LICENSES/third-party/der.txt) |
| `digest` | 0.10.7 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/digest.txt) |
| `ed25519` | 2.2.3 | `Apache-2.0 OR MIT`；保留 `MIT` | [许可](LICENSES/third-party/ed25519.txt) |
| `ed25519-dalek` | 2.2.0 | `BSD-3-Clause`；保留 `BSD-3-Clause` | [许可](LICENSES/third-party/ed25519-dalek.txt) |
| `fiat-crypto` | 0.2.9 | `MIT OR Apache-2.0 OR BSD-1-Clause`；保留 `MIT` | [许可](LICENSES/third-party/fiat-crypto.txt)、[原始署名](LICENSES/third-party/fiat-crypto-COPYRIGHT.txt)、[原始署名](LICENSES/third-party/fiat-crypto-AUTHORS.txt) |
| `generic-array` | 0.14.7 | `MIT`；保留 `MIT` | [许可](LICENSES/third-party/generic-array.txt) |
| `getrandom` | 0.2.17 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/getrandom.txt) |
| `itoa` | 1.0.19 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/itoa.txt) |
| `libc` | 0.2.191 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/libc.txt) |
| `pkcs8` | 0.10.2 | `Apache-2.0 OR MIT`；保留 `MIT` | [许可](LICENSES/third-party/pkcs8.txt) |
| `proc-macro2` | 1.0.107 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/proc-macro2.txt) |
| `quote` | 1.0.47 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/quote.txt) |
| `rand_core` | 0.6.4 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/rand_core.txt)、[原始署名](LICENSES/third-party/rand_core-COPYRIGHT.txt) |
| `rustc_version` | 0.4.1 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/rustc_version.txt) |
| `semver` | 1.0.28 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/semver.txt) |
| `serde` | 1.0.229 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/serde.txt) |
| `serde_core` | 1.0.229 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/serde_core.txt) |
| `serde_derive` | 1.0.229 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/serde_derive.txt) |
| `serde_json` | 1.0.154 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/serde_json.txt) |
| `sha2` | 0.10.9 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/sha2.txt) |
| `signature` | 2.2.0 | `Apache-2.0 OR MIT`；保留 `MIT` | [许可](LICENSES/third-party/signature.txt) |
| `spki` | 0.7.3 | `Apache-2.0 OR MIT`；保留 `MIT` | [许可](LICENSES/third-party/spki.txt) |
| `subtle` | 2.6.1 | `BSD-3-Clause`；保留 `BSD-3-Clause` | [许可](LICENSES/third-party/subtle.txt) |
| `syn` | 2.0.119 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/syn.txt) |
| `syn` | 3.0.10 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/syn.txt) |
| `typenum` | 1.20.1 | `MIT OR Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/typenum.txt) |
| `unicode-ident` | 1.0.27 | `(MIT OR Apache-2.0) AND Unicode-3.0`；保留 `MIT AND Unicode-3.0` | [许可](LICENSES/third-party/unicode-ident.txt)、[Unicode 条款](LICENSES/third-party/unicode-ident-Unicode-3.0.txt) |
| `version_check` | 0.9.5 | `MIT/Apache-2.0`；保留 `MIT` | [许可](LICENSES/third-party/version_check.txt) |
| `wasi` | 0.11.1+wasi-snapshot-preview1 | `Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT`；保留 `MIT` | [许可](LICENSES/third-party/wasi.txt) |
| `zeroize` | 1.9.1 | `Apache-2.0 OR MIT`；保留 `MIT` | [许可](LICENSES/third-party/zeroize.txt) |
| `zmij` | 1.0.23 | `MIT`；保留 `MIT` | [许可](LICENSES/third-party/zmij.txt) |

所有新增文本取自本项目 Cargo 缓存中摘要与 `Cargo.lock` 匹配的归档，保持原始字节。`unicode-ident` 同时保留 MIT 与 Unicode-3.0，不将 AND 条件当作二选一；使用的版权／作者补充文本单独记录摘要。机器清单的 `additionalTexts` 同样参与源材料检查和 Windows 分发必需材料检查。清单完整性不等于已完成最终包的许可验收。

原生更新助手的 Windows 条件依赖另包含 `windows-sys` 0.61.2 和 `windows-link` 0.2.1（上游 MIT OR Apache-2.0，本项目保留 [MIT](LICENSES/third-party/windows-sys.txt)／[MIT](LICENSES/third-party/windows-link.txt) 原文）；Linux 使用同一锁文件中的 `libc`。助手独立链接，不将平台条件依赖误记为所有桌面配置均静态编入。

## Scintilla

桌面构建从项目准备目录中的 Scintilla 5.6.7 源码构建静态库，版本和原始下载摘要锁定在[桌面依赖清单](tools/desktop-dependencies.json)。其[许可及版权原文](LICENSES/third-party/scintilla.txt)来自所用源码目录，并与 [Scintilla 官方许可](https://www.scintilla.org/License.txt)的内容核对。该许可要求保留上游版权与许可声明，具体条件以原文为准。`xdnote` 没有将其改标为项目自有代码。

## Sparkle 独立准入依赖

原生更新准入工程显式准备 Sparkle 2.10.0；产品当前尚未链接或部署它。[固定输入清单](tools/sparkle-dependency.json)分别记录官方 SDK、源码和本项目网络补丁，[完整版权及许可原文](LICENSES/third-party/Sparkle.txt)保留 `MIT AND BSD-2-Clause AND Zlib` 的适用内容，不删减外部组件声明。主库和 Ed25519 翻译单元由固定源码编译，helper、资源和工具沿用官方 SDK 字节；不能把整个 SDK 记为源码重建或实际随产品交付。后续正式部署须按[准入方法](docs/engineering/macos-update-transport.md)和实际载荷核对；上游归属不改标为项目自有内容。

## Qt 与运行库

当前 CMake 入口使用 Qt 6.8.2 的 Widgets、QML、Quick、Quick Widgets、Quick Controls 2、Concurrent、Network、SVG 及其传递模块；本机 SDK 使用共享 Qt 库。这些构建依赖不等于未来某个平台的实际交付文件清单。Qt 模块可用许可和其中的第三方组件须按[Qt 许可说明](https://doc.qt.io/qt-6.8/licensing.html)、固定版本源码、构建配置及实际包逐项核对。

Linux 诊断候选另使用同版本 Qt Wayland 客户端平台插件及其传递库；SDK 准备显式包含 `qtwayland` 归档。诊断目录按实际部署文件匹配 Qt 模块 SBOM，系统 Wayland／X11 等动态库单独列为宿主依赖，不把它们标为已随包交付或已完成最低系统验收。

产品不链接或部署 Qt WebEngine／Chromium。本机 Qt 6.8.2 SDK 的 `sbom/` 包含各模块 SPDX 文档，其中部分包的许可字段为 `NOASSERTION`；这些文档有助于清点，但不能单独证明许可文本和随包义务已齐备。不同平台的插件、QML 模块、系统库和 MSVC 运行库也须根据实际交付内容核对。

Qt 6.8.2 的 [LGPL-3.0 原文](LICENSES/third-party/Qt-LGPL-3.0-only.txt)及其引用的 [GPL-3.0 原文](LICENSES/third-party/Qt-GPL-3.0-only.txt)以[来源和摘要](tools/deployment-notices.json)固定并由日常检查核对。各平台诊断目录生成与匹配 Qt 模块 SPDX／声明的收集见[部署诊断](docs/engineering/deployment.md)；模块 SBOM 的未确定字段、源码提供／用户替换等要求及最终发行核验仍须独立完成。

Qt Network 6.8.2 的 `xdnote-network.1` 使用显式源码构建的[固定补丁](tools/qt-network-patches/README.md)，包含两项 Qt 官方回移、SOCKS5 背压／EOF 和 macOS SecureTransport 缓存读取修正。补丁属于 Qt 派生修改，采用 LGPL-3.0-only，原文件版权及许可不变。部署材料分别保留供应方原始 SPDX、原始 SDK 和修改模块摘要及固定源码／补丁来源。版本号不变不表示二进制未修改，也不表示最终许可验收完成。

准备器显式下载并校验 OpenSSL 3.5.9（Apache-2.0），仅在本项目构建缓存中生成 Qt DTLS／OCSP 配置探测所需的静态输入；不复制 OpenSSL 库到 Qt SDK 或产品，不替换平台运行时 TLS 后端。来源及 SHA-256 见[固定清单](tools/qt-network-backports.json)，上游许可随下载源码保留。

## 字体、设计与研发工具

当前源码不包含 TTF、OTF、WOFF 或 WOFF2 字体文件；界面代码请求系统字体和回退字体。设计素材清单将复用图标记为项目原创几何，并注明没有导出字体；设计素材的项目许可与来源见[素材交付说明](docs/visual/asset-handoff.md)。这些记录不授予第三方字体或素材的额外权利。

`sharp`、Node、Python 构建工具、CMake、Ninja 和 SDK 准备工具用于设计检查或构建；当前尚无证据表明它们随产品发行。若后续交付包含这些工具、其他字体或素材，需重新核对其许可、版权和实际版本。

## 发行前仍需完成

针对每个实际包生成文件和依赖清单，核对 Qt 第三方声明、所选 Qt 许可及其源码提供和用户替换等适用条件、平台运行库、字体和设计素材署名；将所需文本与对应内容一并交付，并在最终构建配置或依赖变化时重新检查。具体交付义务以所选许可、实际内容及适用发行方式为准。
