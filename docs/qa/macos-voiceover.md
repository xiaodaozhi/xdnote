# macOS VoiceOver 静音验收

在已解锁的 Apple Silicon Mac 上，用[开发部署](../engineering/developer-delivery.md)流程生成的封存 `xdnote.app` 准备独立测试副本。记录宿主版本、产品构建身份、旁白开关和系统输出原来的静音状态。

```sh
python scripts/reader-acceptance/voiceover-macos.py prepare \
  --source-app build/macos-candidate/staging/xdnote.app \
  --work-dir build/voiceover-acceptance
```

准备脚本只复制应用、隔离设置和生成样本。朗读结果用原生旁白字幕窗判读。

1. 将系统输出静音。用 Finder 启动脚本输出的副本，打开同目录的 `VoiceOver manual.md`，保持阅读模式。开启旁白，并显示字幕窗。字幕窗隐藏时可在旁白实用工具的「视觉 → 面板和菜单」启用。[Apple 字幕窗说明](https://support.apple.com/en-in/guide/voiceover/unac078/10/mac/27)
2. 让旁白光标进入正文，以 `VO+U` 打开转子，进入「链接菜单」，选中 `Jump to Amber`。按空格定位链接，再用 `VO+空格` 激活。`VO` 指当前旁白修饰键，默认可用 `Control+Option`。
3. 确认字幕出现 `Distant Amber Heading`。保持跳转后的旁白光标，直接用 `VO+A` 继续朗读。字幕应依次出现 `Amber target reading sample continues here.`、`The amber lantern lights the quiet path.`，再进入下一段 `The sentence after the jump is readable from start to finish.`。若从开头或原链接重新朗读，此项记为失败。
4. 保留链接、目标标题及后续段落的字幕窗截图和结果，证据放在测试目录。关闭测试副本，恢复旁白、字幕窗和系统输出原来的状态。核对样本摘要后，可清理该目录内的 `xdnote.app` 和 `home/`，保留样本与证据。

```sh
python scripts/reader-acceptance/voiceover-macos.py check --work-dir build/voiceover-acceptance
```

2026-11-23 在 macOS 27.2.1 arm64 对私有 `0.0.0-alpha.302` 实包走通此流程，字幕显示目标正文和下一段，样本摘要一致。[验收记录 #208](https://github.com/xiaodaozhi/xdnote/issues/208)
