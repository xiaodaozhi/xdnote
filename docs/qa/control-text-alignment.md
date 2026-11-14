# 控件文字垂直对齐的核查与复验

核查区分文字行框、可见字形、图标画布与多行内容组。字体中下伸字母的形状差异、字体回退和离屏后端差异不直接认定为控件下沉，也不对所有 UI 统一增加 Y 偏移。共享逻辑参数见[视觉规范](../visual/design-system.md)与[tokens.json](../visual/tokens.json)。

## 当前绘制口径

| 控件或区域 | 当前算法与核查重点 |
| --- | --- |
| 按钮、输入框、复选框、标签和状态文字 | 保持 Qt 行框垂直居中；以同字体、同字号、完全相同字符串的 QLabel 为参考。状态和焦点不能改变文字 Y 偏移。 |
| 设置 24 像素页标题与搜索标题 | 用 QTextLayout 的实际字形范围光学居中，只作用于这一标题变体；不改变普通标签、字体基线及页面上边距。对应原审查 T01。 |
| 工作区面包屑 | 各路径段与分隔符均使用 Qt 单行矩形垂直居中，保持同一基线和现有水平省略。使用同文案参考标签复验，不再混用不同字符串推算固定补偿。对应 T02。 |
| 菜单、文件树与图标槽 | 槽位起点为行顶加 `(行高−图标高)/2`，避免偶数 QRect 的整数 center 偏差；可见图形的透明留白另行核对。对应 T03、T04。 |
| 最近／收藏／文件搜索双行项 | 标题 22、副标题 18、间隔 2，整个文字组在内缩后的行内居中，与文档及收藏图标使用相同行中线；保持已确认的行宽和左右边界。对应 T05。 |
| 快捷键字段 | 空闲时在实际输入框中绘制键帽，聚焦后保留原生编辑；两段组合均显示，未知候选回退为可见文本，空绑定为「未分配」。输入框的逻辑焦点、选区、文本和撤销不由绘制改写。 |
| 多行说明与文档正文 | 按内容块和段落基线排版，不按按钮的单行规则强行居中。字体回退和自然下伸字形单列观察。 |

## 自动化入口

`apps/desktop/tests/control_consistency_tests.cpp` 的 `uiTextAlignmentSurvey` 对普通／主要／禁用按钮、输入框、复选框和普通标签采样；`uiTextAlignmentSurfaces` 覆盖设置六页、工作区和独立窗口的三种视图、最近页、查找栏及菜单。`breadcrumbMatchesSameTextLabelBaseline` 使用完全相同的中文、拉丁、数字路径及字号对照面包屑和 QLabel，检查额外基线偏移。

这些函数的原生采样窗口均使用 `WA_DontShowOnScreen`，不向用户桌面窗口注入输入。Qt DLL／插件路径与构建身份必须通过[项目构建入口](../engineering/developer-build.md)准备，不裸启动构建目录中的产品 exe。

`settings_tests.cpp` 的 `shortcutCapsKeepNativeEditingAndEmptyBinding` 检查两段组合的编辑、应用、清空和恢复默认，以及空闲绘制不破坏值或撤销。`constrainedSettingsKeepsNavigationAndActionsReachable` 在离屏屏幕配置中检查六页导航、表单动作与固定完成按钮，临时收敛不覆盖正常尺寸偏好；`licenseDialogFitsAvailableArea` 对共享许可弹窗核对工作区边界与滚动后的动作可达性。

## 证据与边界

设置 `XDNOTE_TEST_EVIDENCE` 后保存原始 PNG、字形字体身份、实际 DPR、墨迹与目标矩形。截图的逻辑尺寸、DPR 和透明画布分别记录；参考断言检查同文本相对于原生标签的额外位移，不把所有字形的墨迹要求为严格对称。

Windows 原生隐藏取样、Qt 离屏绘制、合成事件、真实鼠标及系统输入分别记账。macOS／Linux 字体后端、系统原生选择器、输入法、真实辅助技术和物理跨屏仍需对应环境；不能从 Windows 截图推定通过。当前结果与剩余实施记录在 [Issue #137](https://github.com/xiaodaozhi/xdnote/issues/137)，控件范围见 [Issue #126](https://github.com/xiaodaozhi/xdnote/issues/126)。
