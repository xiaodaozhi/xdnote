# Qt 来源与自行替换说明

xdnote 使用动态链接的 Qt 6.8.2。实际模块、供应方构建配置、许可及文件摘要保存在随包 Qt SPDX 和分发清单中；平台、CPU、编译器 ABI、Release／Debug 配置和模块组合应匹配。独立源码地址及 SHA-256 见随包 `source-access.json`；Qt 源码不包含在 xdnote 的 Release 附件、安装包或产品归档中。

需要修改 Qt 的技术用户可以从所列官方独立归档取得对应源码，参考原始模块 SPDX 中的配置及 Qt 的[源码构建说明](https://doc.qt.io/qt-6.8/build-sources.html)构建匹配的库，再在自有应用副本中替换相应模块。项目自身的可恢复源码和构建入口见[开发构建](https://github.com/xiaodaozhi/xdnote/blob/main/docs/engineering/developer-build.md)。不要混用不同架构、不兼容版本、编译器 ABI 或调试配置的模块。

Windows 使用相匹配的 DLL 和插件目录；Linux 保留 SONAME 相对链接及目录内加载路径，同时满足目标系统 ABI；macOS 保留 `.app` 的 framework 布局和加载路径，修改后按本机策略重新签名自有副本。现有开发候选的 ad-hoc 封印没有发布者身份，后续 Developer ID／公证后的操作还须单独验证。Qt 库的部署重定位和代码节核对不等于已经重编并验证了用户修改的 Qt。

用户自行修改后的文件应得到保留。当前升级／卸载维护会因文件摘要变化拒绝覆盖或删除；应按提示另选安装目录或在备份后由用户处理自己的副本。该维护保护与运行时库加载分别核对，不将维护拒绝解释为禁止用户修改 Qt。

Qt LGPL 及其引用文本随产品保留。官方源码归档的可获取性、版本／模块匹配和摘要可以自动检查；实际源码提供条件、全部对应构建材料及用户重编／替换后的运行核验仍由 Issue #4 跟踪。这里的操作说明不单独构成完整许可结论。依据见 [Qt LGPL 使用条件](https://www.qt.io/development/open-source-lgpl-obligations)。
