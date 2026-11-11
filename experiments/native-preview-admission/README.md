# Qt 原生阅读控件准入

此入口构建实际 `NativePreviewContent`／`NativePreviewView`，复用本项目共享外观、控件和资源。仅查找固定 Qt 6.8.2 Widgets／Svg／Test，不链接或启动产品旧预览。它与[语义模型验证](../native-model-admission/README.md)和旧的[受控分节试验](../native-preview-spike/README.md)范围不同；模型、缓存和准入缺口见[原生控件说明](../../docs/engineering/native-preview-view.md)。

准备本项目工具、Qt 和离线 Cargo 缓存后，在仓库根运行：

```text
python scripts/cleanup_cycles.py --init cycle-104-native-layout
python experiments/native-preview-admission/verify.py --qt-prefix build/Qt/6.8.2/msvc2022_64 --vcvars "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat" --full --measure
```

已有周期目录时只运行第二行，`--build-dir` 可显式选择其他本项目输出。`--full` 包含 Rust fmt／clippy／测试；纯 C++ 迭代可省略该项，仍构建当前 Rust 库及原生目标并运行模型和控件测试。`--measure` 仅在 Windows 对自有的空闲、密集语法、最大单段／代码／表格和无标题多段样本采样，不读取用户文档。其他平台可提供本项目实际 `--qt-prefix`、省略 `--vcvars` 和 `--measure`；入口存在不代表已取得对应平台结果。

运行临时设置当前 Qt 的 DLL／插件／字体路径和 `offscreen` 平台，不修改系统 PATH。Windows 递归核对 Qt DLL 导入，排除 Chromium；离屏 PNG、QtTest、源码和工具摘要及测量进入忽略的构建目录。所有测试报告必须完整，Qt critical／fatal 或断言即使进程退出成功也判为失败。新预览不调用旧产品运行环境；启动实际产品仍遵守统一 `scripts/desktop.py run` 入口。

诊断控件使用 800×600 逻辑像素观测视口，不是新增产品窗口规格。截图只表明当前控件出现了绘制，合成输入只验证 Qt 路径；不表示工作区／独立窗口已经迁移、视觉已经验收或真实读屏已经通过。

`native-verification.json` 记录当前输入与二进制身份、实际 Qt 导入和结果。每约 50ms 统计自有诊断进程树的私有提交与工作集；可能错过更短的峰值，工作集合计也可能重复共享页。私有提交可能换出，既不是常驻物理内存，也不是整应用或发行包预算。首个观测帧包含解析、派生内容、几何和强制抓图／编码成本，不等同于输入延迟或完整应用启动。

`complete=true` 只表示本次定义范围的测试和样本执行完毕；已明确返回失败的超长双向段落和大字素簇仍是准入缺口，不能按“预期失败测试通过”关闭对应能力要求。默认产品没有调用此控件，旧 WebEngine 清除另行实施。
