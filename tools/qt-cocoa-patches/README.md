# macOS Qt 6.8.2 辅助功能回移植

`xdnote-cocoa.1` 将 [QTBUG-134784 的引用计数修正](https://github.com/qt/qtbase/commit/b1ed5f656f064e553b33752f8e87d2f5b9553e38)应用到 Qt Base 6.8.2。项目补丁仅删除由实际清理对象拥有的缓存记录，保留 [QTBUG-149612](https://qt-project.atlassian.net/browse/QTBUG-149612) 中占位单元借用的父项记录。补丁来源和摘要见 [锁定配置](../qt-cocoa-backports.json)。

[准备入口](../../scripts/prepare_qt.py)从固定原厂归档编译 arm64 QtGui 和 Cocoa 平台插件，目标 macOS 14。QtGui 的公开与私有功能开关、两份库的供应方导出符号集合逐一比较。源码构建关闭 Cocoa 不使用的 D-Bus 运行依赖。若选定 Apple SDK 缺少 AGL stub，构建移除两份库的 AGL 链接项并保留 OpenGL。其余新依赖或意外缺失依赖会拒绝准备，允许的差异写入回执。

只在应用及构建任务均已退出时操作项目内 SDK。以下命令校验当前派生文件、封印与备份，再成对恢复两份原件。

```text
python scripts/qt_cocoa_backports.py --qt-prefix build/Qt/6.8.2/macos --restore-original
```

恢复后重跑 [项目准备入口](../../docs/engineering/developer-build.md)再执行常规构建。实际部署及替换说明见 [Qt 来源与自行替换](../../docs/engineering/qt-library-replacement.md)。
