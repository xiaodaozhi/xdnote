# D1：pulldown-cmark 解析候选试验

本目录比较 Markdown 解析事件与源文位置，不是产品渲染器，不决定采用 Rust 或 pulldown-cmark。它使用与 Qt 试验相同的自有语料，开启表格、任务列表和删除线扩展，检查常用块、原始 HTML、链接与图片地址，以及中文和表情的 UTF-8 字节范围。

解析出的 HTML、链接和图片地址都不可信。此试验既不生成／显示 HTML，也不会按文档中的图片或链接地址访问本地或网络资源；没有实现脚本过滤、协议允许列表、本地图片授权、渲染布局或可访问性。字节偏移不是 Qt 的 UTF-16 位置，完整转换、增量更新及边界处理仍需另行验证。

偏移对照增加了仅针对同一份有效 UTF-8 源文的字节／UTF-16 线性转换：检查中文、表情的代理对、组合字符、CRLF、越界和无效边界，并把解析器事件的范围映射回原文。实体解码后显示的文字不等于原文片段；Qt 文档的换行和文本变换、编辑后的增量映射、无效 UTF-16、字素簇与真实预览定位均未验证。该参考实现每次从源文开头扫描，不适合作为大量定位请求的性能方案。

依赖固定为 [pulldown-cmark 0.13.6](https://github.com/pulldown-cmark/pulldown-cmark/tree/v0.13.6)，关闭默认特性，使用仓库的 `Cargo.lock` 固定解析结果。上游使用 [MIT 许可](https://github.com/pulldown-cmark/pulldown-cmark/blob/v0.13.6/LICENSE)；实际部署依赖及其许可义务仍需按最终选型核验。

## 显式准备与测试

以下在仓库根目录执行；先显式下载锁定依赖，测试和基准使用离线模式。需要已安装的 Rust 和 Cargo，CI 所用版本只是实验输入，不是产品版本决定。

```sh
export CARGO_HOME="$PWD/build/pulldown-cargo-home"
export CARGO_TARGET_DIR="$PWD/build/pulldown-markdown-spike"
cargo fetch --locked --manifest-path experiments/pulldown-markdown-spike/Cargo.toml
cargo test --locked --offline --manifest-path experiments/pulldown-markdown-spike/Cargo.toml
cargo run --release --locked --offline --manifest-path experiments/pulldown-markdown-spike/Cargo.toml
```

Windows PowerShell 可将前两行替换为 `$env:CARGO_HOME = "$PWD/build/pulldown-cargo-home"` 和 `$env:CARGO_TARGET_DIR = "$PWD/build/pulldown-markdown-spike"`。基准只解析自生成的 10 KiB、1 MiB、10 MiB 重复样本并输出事件数与耗时，不测量布局、输入延迟或内存；不能与 Qt 的完整预览排版耗时直接比较，也不是产品性能上限。技术研究和剩余边界见 [Issue #10](https://github.com/xiaodaozhi/xdnote/issues/10)。
