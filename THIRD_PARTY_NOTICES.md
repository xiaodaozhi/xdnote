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

macOS 产品使用 Sparkle 2.10.0 的派生 framework 和随包更新助手。[固定输入清单](tools/sparkle-dependency.json)分别记录官方 SDK、源码和本项目网络补丁。[版权及许可原文](LICENSES/third-party/Sparkle.txt)保留适用的 MIT、BSD-2-Clause 和 Zlib 文本。主库和 Ed25519 翻译单元由固定源码编译，随包助手和资源取自官方 SDK。实际 framework、助手、资源与封印记录见[部署材料](docs/engineering/distribution-audit.md)。

## Qt 与运行库

当前 CMake 入口使用 Qt 6.8.2 的 Widgets、QML、Quick、Quick Widgets、Quick Controls 2、Concurrent、Network、SVG 及其传递模块；本机 SDK 使用共享 Qt 库。这些构建依赖不等于未来某个平台的实际交付文件清单。Qt 模块可用许可和其中的第三方组件须按[Qt 许可说明](https://doc.qt.io/qt-6.8/licensing.html)、固定版本源码、构建配置及实际包逐项核对。

Linux 诊断候选另使用同版本 Qt Wayland 客户端平台插件及其传递库；SDK 准备显式包含 `qtwayland` 归档。诊断目录按实际部署文件匹配 Qt 模块 SBOM，系统 Wayland／X11 等动态库单独列为宿主依赖，不把它们标为已随包交付或已完成最低系统验收。

产品不链接或部署 Qt WebEngine／Chromium。本机 Qt 6.8.2 SDK 的 `sbom/` 包含各模块 SPDX 文档，其中部分包的许可字段为 `NOASSERTION`；这些文档有助于清点，但不能单独证明许可文本和随包义务已齐备。不同平台的插件、QML 模块、系统库和 MSVC 运行库也须根据实际交付内容核对。

xdnote 动态链接 Qt 6.8.2。随包提供 Qt 的 [LGPL-3.0 原文](LICENSES/third-party/Qt-LGPL-3.0-only.txt)及其引用的 [GPL-3.0 原文](LICENSES/third-party/Qt-GPL-3.0-only.txt)。Qt 源码获取、修改和在自有应用副本中替换库的说明见[Qt 来源与自行替换](docs/engineering/qt-library-replacement.md)。实际分发模块、SDK 的 SPDX、适用许可及最终发行条件按各平台载荷核对。[分发材料](docs/engineering/distribution-audit.md)

Qt Core 的内嵌 MIME 数据来自 Apache Tika 的 MIME Type Definitions，由 Qt 改编。Tika 上游 NOTICE 说明其中部分定义改编自英国国家档案馆的 PRONOM 技术登记库。随包的 `licenses/project-and-source-dependencies/third-party/` 保留[原始 Tika NOTICE](LICENSES/third-party/Qt-Tika-NOTICE.txt)及 [Open Government Licence v3.0](LICENSES/third-party/Qt-Tika-OGL-UK-3.0.txt)。完整 NOTICE 也记录 Apache Tika 其他组件的署名。改编前的 MIME 文件见[固定上游源码](https://github.com/apache/tika/blob/5101bc7fb090ed7deffe56837d7633c9485a1e5d/tika-core/src/main/resources/org/apache/tika/mime/tika-mimetypes.xml)。

Contains public sector information licensed under the Open Government Licence v3.0.

同一目录保留 Qt Network 内嵌 Public Suffix List 的 [MPL-2.0 许可原文](LICENSES/third-party/Qt-PSL-MPL-2.0.txt)。可编辑列表与重建入口见[Qt 来源与自行替换说明](docs/engineering/qt-library-replacement.md)。

Qt Gui 的平滑缩放使用源自 Imlib2 的实现，随包保留 [BSD-2-Clause 与 Imlib2 原始许可及署名](LICENSES/third-party/QtGui-QIMAGETRANSFORM-LICENSE.txt)。Qt SVG 的路径弧线实现含 XSVG 代码，随包保留 [HPND-sell-variant 原始许可及署名](LICENSES/third-party/QtSvg-XSVG-LICENSE.txt)。

Qt 的图像与字形处理材料包括 [LibJPEG-turbo 许可](LICENSES/third-party/QtGui-LibJPEG-turbo-LICENSE.txt)、[IJG 原始条款](LICENSES/third-party/QtGui-LibJPEG-turbo-IJG.txt)、[LibJPEG-turbo 版权](LICENSES/third-party/QtGui-LibJPEG-turbo-COPYRIGHT.txt)、[LibPNG 原始许可](LICENSES/third-party/QtGui-LibPNG-LICENSE.txt)及 [HarfBuzz 原始许可](LICENSES/third-party/QtGui-HarfBuzz-LICENSE.txt)。

This software is based in part on the work of the Independent JPEG Group.

对随 Qt 交付的 FreeType，选用 [FreeType License（FTL）](LICENSES/third-party/QtGui-FreeType-LICENSE.txt)，并保留其 [BDF](LICENSES/third-party/QtGui-FreeType-BDF-LICENSE.txt)、[PCF](LICENSES/third-party/QtGui-FreeType-PCF-LICENSE.txt) 与 [zlib](LICENSES/third-party/QtGui-FreeType-ZLIB-LICENSE.txt) 组件的原始条款。

This software is based in part on the work of the FreeType Team (www.freetype.org).

Qt Core 含 TinyCBOR 0.6.0，随包保留其 [MIT 原始许可及版权声明](LICENSES/third-party/QtCore-TinyCBOR-LICENSE.txt)。

Qt Core 的数字转换、Unix 子进程和 macOS 事件分发实现分别保留 [double-conversion](LICENSES/third-party/QtCore-DoubleConversion-LICENSE.txt)、[forkfd](LICENSES/third-party/QtCore-ForkFD-LICENSE.txt) 和 [CoreFoundation 事件分发](LICENSES/third-party/QtCore-CoreFoundationEventDispatcher-LICENSE.txt)的原始条款。macOS 平台插件另附 [Cocoa 插件原始条款](LICENSES/third-party/QtCocoa-PlatformPlugin-LICENSE.txt)。

Qt Gui 的字形名、Markdown 导入、渐变预设与区域运算分别保留 [Adobe Glyph List For New Fonts](LICENSES/third-party/QtGui-AGLFN-LICENSE.txt)、[MD4C](LICENSES/third-party/QtGui-MD4C-LICENSE.txt)、[WebGradients](LICENSES/third-party/QtGui-WebGradients-LICENSE.txt)和 [X Consortium 区域运算](LICENSES/third-party/QtGui-XRegion-LICENSE.txt)的原始条款。Qt Network 的内置公共后缀查找另附 [libpsl DAFSA 实现条款](LICENSES/third-party/QtNetwork-LibPSL-DAFSA-LICENSE.txt)。

Qt Core 的缓动曲线包括 Robert Penner 的实现，随包保留其 [BSD-3-Clause 原始许可及版权声明](LICENSES/third-party/QtCore-Easing-LICENSE.txt)。Qt Core 自带的摘要实现另附 [RFC 6234 SHA-2 条款](LICENSES/third-party/QtCore-RFC6234-LICENSE.txt)、[Keccak CC0](LICENSES/third-party/QtCore-SHA3-Keccak-CC0.txt)、[brg_endian BSD-2-Clause](LICENSES/third-party/QtCore-SHA3-BrgEndian-LICENSE.txt)及 [BLAKE2 原始 COPYING](LICENSES/third-party/QtCore-BLAKE2-COPYING.txt)。Qt 的随包上游许可汇总已保留 SHA-1 公有领域声明。MD4 由 Alexander Peslyak 置于公有领域，MD5 来源声明记录 Colin Plumb 和 Ian Jackson。实际摘要实现范围随 Qt 构建选项而变化。

Qt5Compat 的 Big5、GB18030、EUC-JP、EUC-KR、ISO-2022-JP、Shift_JIS 和 TSCII 编码实现使用 BSD-2-Clause 条款。七份上游原始版权、条款及免责声明随包存放于 `licenses/project-and-source-dependencies/third-party/` 中，文件名以 `Qt5Compat-` 开头。

Qt Core 的供应方材料包含 PCRE2 10.44 及其可选的 SLJIT 实现。PCRE2 原始 BSD 三条款和二进制类包例外、SLJIT 原始 BSD 二条款分别以 `QtCore-PCRE2-LICENCE.txt` 和 `QtCore-PCRE2-LICENCE-SLJIT.txt` 随包保留；提供文本不表示各平台均启用了 SLJIT。

Qt Network 6.8.2 的 `xdnote-network.1` 使用显式源码构建的[固定补丁](tools/qt-network-patches/README.md)，包含两项 Qt 官方回移、SOCKS5 背压／EOF 和 macOS SecureTransport 缓存读取修正。补丁属于 Qt 派生修改，采用 LGPL-3.0-only，原文件版权及许可不变。部署材料分别保留供应方原始 SPDX、原始 SDK 和修改模块摘要及固定源码／补丁来源。版本号不变不表示二进制未修改，也不表示最终许可验收完成。

准备器显式下载并校验 OpenSSL 3.5.9（Apache-2.0），仅在本项目构建缓存中生成 Qt DTLS／OCSP 配置探测所需的静态输入；不复制 OpenSSL 库到 Qt SDK 或产品，不替换平台运行时 TLS 后端。来源及 SHA-256 见[固定清单](tools/qt-network-backports.json)，上游许可随下载源码保留。

## 字体、设计与研发工具

当前源码不包含 TTF、OTF、WOFF 或 WOFF2 字体文件；界面代码请求系统字体和回退字体。设计素材清单将复用图标记为项目原创几何，并注明没有导出字体；设计素材的项目许可与来源见[素材交付说明](docs/visual/asset-handoff.md)。这些记录不授予第三方字体或素材的额外权利。

`sharp`、Node、Python 构建工具、CMake、Ninja 和 SDK 准备工具用于设计检查或构建；当前尚无证据表明它们随产品发行。若后续交付包含这些工具、其他字体或素材，需重新核对其许可、版权和实际版本。

## 发行前仍需完成

针对每个实际包生成文件和依赖清单，核对 Qt 第三方声明、所选 Qt 许可及其源码提供和用户替换等适用条件、平台运行库、字体和设计素材署名；将所需文本与对应内容一并交付，并在最终构建配置或依赖变化时重新检查。具体交付义务以所选许可、实际内容及适用发行方式为准。
