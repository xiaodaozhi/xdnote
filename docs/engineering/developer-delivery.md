# 开发交付与兼容审计

开发候选将实际产品清单、构建身份、源码提交、第三方材料和归档摘要绑定。生成归档不等于正式发行；当前报告保留 `releaseReady=false` 与 `publicationAllowed=false`，不创建 GitHub Release，也不上传产品二进制。[发行决策](release-and-updates.md)已确定 Windows 不自行签名、后续 MSIX 使用 Store；macOS 保留 ad-hoc、不做 Developer ID 或 Apple 公证。归档报告记录所选策略，不把这些不采用的身份服务列为未完成门槛；独立源码渠道、实际静态依赖范围、最低系统／真实桌面和具体发布授权仍分别跟踪。

## 静态兼容范围

`scripts/compatibility_audit.py --root <build 下的纯产品目录> --report <新报告.json>` 逐个读取实际原生文件，记录摘要并使用 `tools/delivery-policy.json` 的已定目标。

| 平台 | 检查 | 独立保留的未知项 |
| --- | --- | --- |
| Windows x86_64 | PE32+ 架构、OS／子系统版本声明、普通与延迟导入 | 头部声明不能证明每个 API 在 Windows 10 1809 都存在，仍需最低系统运行 |
| Linux x86_64 | ELF64 架构、导入、加载路径与所需符号版本；只读取 version needs，不把库自身导出的版本当成需求 | Ubuntu 22.04 参考 ABI 上限为 GLIBC 2.35、GLIBCXX 3.4.30、CXXABI 1.3.13；真实桌面、运行时动态加载及硬件能力另验 |
| macOS Intel／ARM64 | 检查所有 Mach-O 切片、目标架构和最低部署声明，拒绝超过 macOS 13 的随包文件 | 当前 host 的架构必须存在；双架构库与两个原生候选分别核对，最低系统运行与签名身份另验 |

未知符号版本、损坏或截断的头部、错误架构、缺失最低声明及超出目标要求会使检查失败。原有传递依赖闭包继续由部署入口核对，不以本项代替。依据见 [PE 格式](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)、[GCC ABI 表](https://gcc.gnu.org/onlinedocs/libstdc++/manual/abi.html)与 [Apple Mach-O 定义](https://github.com/apple-oss-distributions/cctools/blob/main/include/mach-o/loader.h)。

## Rust 与 Qt 材料

`runtime_materials.py` 从锁定 Rust 工具链收集标准库原始版权清单及所记录的许可文本，核对实际编译器提交、版本和每份文本 SHA-256。只收集材料，不复制编译器／标准库二进制；原文可能包含未编入当前程序的目标／组件，因此保留 `actualStaticComponentReviewComplete=false`，不能将工具链包级材料直接当成精确静态链接清单。

`qt_source_access.py` 按实际 Qt 模块生成官方独立源码地址、固定摘要和[自行替换说明](qt-library-replacement.md)。显式选择 `--download-cache` 时，只下载到本项目独立构建缓存进行字节核验；Qt 源码不进入产品目录、安装包或 Release 附件。可访问的归档和一份说明不等于全部源码／替换条件已满足，继续保留 `sourceAccessFulfillmentReviewComplete=false`。

## 归档与门槛

```text
python scripts/developer_archive.py --root <纯产品目录> --audit <封存清单.json> --compatibility <兼容报告.json> --output <build 下的开发包.zip 或 .tar.xz>
```

Windows 使用 ZIP，含 framework／SONAME 相对链接的 Unix 产品使用 tar.xz。输入必须与已封存清单逐项一致，兼容报告必须覆盖所有原生文件。源码／SDK 目录、编译头文件、静态库及嵌套压缩包被拒绝；许可文本与源码获取元数据继续保留。归档采用固定时间和规范化权限，重复生成核对 SHA-256；在自有临时目录解包后复核内容、链接与执行权限，macOS 再核对 ad-hoc 完整性。

归档旁保存 `.manifest.json` 和 `.sha256`，记录提交、dirty 标志、实际文件摘要与开发限制。修改、遗漏或额外文件均失败；旧目标不覆盖。现有 Windows 产品安装专项及 Unix 部署专项生成并验证候选，仅上传清单、摘要和原始证据，完整许可与发行门槛满足前不上传归档本体，不自动发布。日常 main 提交仍只运行材料 CI。

`deployment-unix` 覆盖三套 Unix 配置；`deployment-linux` 仅补验 Linux，`deployment-macos` 仅补验 macOS Intel／ARM。平台专属修正优先使用相应范围，不重复无影响的平台。临时目录按真实路径核对，支持系统目录别名，同时继续拒绝越界成员。
