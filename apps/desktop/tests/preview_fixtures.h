#pragma once
#include <QByteArray>
#include <QString>
namespace previewFixtures {
inline QByteArray fixture(int bytes, const QString &flavor) {
    if (flavor == "idle") return {};
    if (flavor == "bidi") { const auto unit = QStringLiteral("العربية abc 123 ").toUtf8(); QByteArray source; while (source.size() + unit.size() <= bytes) source += unit; return source + QByteArray(bytes - source.size(), 'x'); }
    if (flavor == "cluster") { const QByteArray accent("\xcc\x81", 2); const auto source = QByteArray("a") + accent.repeated((bytes - 1) / 2); return source + QByteArray(bytes - source.size(), 'x'); }
    if (flavor == "paragraph") return QByteArray(bytes, 'x');
    if (flavor == "code") return "```cpp\n" + QByteArray(bytes - 11, 'x') + "\n```";
    QByteArray source;
    if (flavor == "table") {
        source = "|a|b|\n|--|--|\n";
        while (source.size() + 6 <= bytes) source += "|x|y|\n";
        return source;
    }
    const auto unit = flavor == "many" ? QByteArray("a b\n\n") :
        QStringLiteral("# 中文标题\n\n中文与 Latin 😀，**粗体** *斜体* ~~删除~~。\n\n[文档引用][shared]\n\n- [x] 完成\n- [ ] 待办\n\n|列|状态|\n|:--|--:|\n|甲|乙|\n\n```cpp\nconst char *text = \"<script>literal</script>\";\n```\n\n").toUtf8();
    const QByteArray definition("\n\n[shared]: nested/target.md\n");
    while (source.size() + unit.size() + definition.size() <= bytes) source += unit;
    source += QByteArray(bytes - source.size() - definition.size(), 'x'); return source + definition;
}
}
