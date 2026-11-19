# Qt 来源与自行替换说明

xdnote 使用动态链接的 Qt 6.8.2。实际模块、供应方构建配置、许可及文件摘要保存在随包 Qt SPDX 和分发清单中；平台、CPU、编译器 ABI、Release／Debug 配置和模块组合应匹配。独立源码地址及 SHA-256 见随包 `source-access.json`；Qt 源码不包含在 xdnote 的 Release 附件、安装包或产品归档中。

需要修改 Qt 的技术用户可以从所列官方独立归档取得对应源码，参考原始模块 SPDX 中的配置及 Qt 的[源码构建说明](https://doc.qt.io/qt-6.8/build-sources.html)构建匹配的库，再在自有应用副本中替换相应模块。项目自身的可恢复源码和构建入口见[开发构建](https://github.com/xiaodaozhi/xdnote/blob/main/docs/engineering/developer-build.md)。不要混用不同架构、不兼容版本、编译器 ABI 或调试配置的模块。

当前 Network 使用固定 6.8.2 的 `xdnote-network.1` 派生构建。随包 `source-access.json` 的 `networkBackports` 分别记录源码、补丁来源及 SHA-256、准备器摘要、原 SDK 和实际模块摘要；上游 SPDX 保持原文，新模块不声称与原始供应方字节相同。对应补丁和配方在 [项目源码](https://github.com/xiaodaozhi/xdnote/tree/main/tools/qt-network-patches)及 `scripts/qt_network_backports.py` 中维护，源码和补丁不进入产品 Release 载荷。恢复或重建应使用匹配该次构建身份的项目修订，全部对应源码和提供条件仍须按最终交付核验。

Qt Network 的内嵌 Public Suffix List 使用 MPL-2.0。可编辑原文见[固定的上游列表](https://github.com/publicsuffix/list/blob/903a83ff7bfc3148e3692e09396f9f3bdc9462ef/public_suffix_list.dat)，SHA-256 为 `2dcffaa353cc1eb13be5c03d238dca7194dd35f4729d54740d2b9b5ec11b8f1e`。原始许可见 Qt Base 源码归档中的 `src/3rdparty/libpsl/PSL-LICENSE.txt`。同目录的 `README.txt` 说明如何用归档内的生成器从该列表重建 `psl_data.cpp` 和测试用 DAFSA。

macOS 的 `xdnote-cocoa.1` 在同一 Qt Base 6.8.2 源码构建中生成 QtGui 和 Cocoa 平台插件。两者共享辅助功能对象缓存的私有接口，用户修改或恢复时应成对替换。随包 `cocoaBackports` 绑定这两份实际模块、原始 SDK、补丁来源、构建配方及兼容检查。对应[补丁与构建说明](https://github.com/xiaodaozhi/xdnote/tree/main/tools/qt-cocoa-patches)随项目源码独立维护。

Windows 使用相匹配的 DLL 和插件目录；Linux 保留 SONAME 相对链接及目录内加载路径，同时满足目标系统 ABI；macOS 保留 `.app` 的 framework 布局和加载路径，修改后按本机策略对自有副本恢复必要的 ad-hoc 完整性封印。本项目不采用 Developer ID 或 Apple 公证，新增更新 framework／XPC／helper 时仍须保留各自签名参数。Store 安装目录的维护边界与可构建的自有副本分别核对。Qt 库的部署重定位和代码节核对不等于已经重编并验证了用户修改的 Qt。

用户自行修改后的文件应得到保留。当前升级／卸载维护会因文件摘要变化拒绝覆盖或删除；应按提示另选安装目录或在备份后由用户处理自己的副本。该维护保护与运行时库加载分别核对，不将维护拒绝解释为禁止用户修改 Qt。

工程自动验证通过 `scripts/build_modified_qt_core.py` 从显式给出的固定 Qt Base 归档重建 Core，不安装到系统或覆盖 SDK；记录实际源码补丁、SDK 的 ICU／Zstd 开关、命令、指令节和输入／输出摘要。它添加仅供自有副本诊断的证明函数；macOS 另保留新 Apple 编译器所需的既定头文件适配。该 Core 构建不是供应方全部模块及配置的精确重现。

`scripts/verify_qt_replacement.py` 只操作本项目 `build/` 的安装副本。Windows／Linux／macOS 通过实际加载位置及该证明函数核对修改库，再运行明暗阅读、图片、编辑／撤销／保存和会话接管，并核对维护拒绝及原 SDK／输入不变。离屏后端、探针与字体不成为产品载荷。手动 `qt-replacement.yml`（或桌面入口 `scope=qt-replacement`）执行三平台同一范围；源码归档、源码树和二进制不作为其证据工件上传，单次结果留在 Issue／PR。

Qt LGPL 及其引用文本随产品保留。官方源码归档的可获取性、版本／模块匹配和摘要可以自动检查；全部实际模块、对应构建／源码提供条件及最终交付仍由 Issue #4 跟踪。Core 工程验证和这里的操作说明不单独构成完整许可结论。依据见 [Qt LGPL 使用条件](https://www.qt.io/development/open-source-lgpl-obligations)。
