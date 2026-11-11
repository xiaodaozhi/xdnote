#include "NativePreviewModel.h"
#include "MarkdownBridge.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QtTest>

using xdnote::NativePreviewModel;
using Kind = NativePreviewModel::Kind;

namespace {
QByteArray owned(XdnoteRenderBuffer buffer) {
    const auto result = buffer.status == 0
        ? QByteArray(reinterpret_cast<const char *>(buffer.data), qsizetype(buffer.length)) : QByteArray();
    xdnote_markdown_free(&buffer); return result;
}
QByteArray encoded(const QByteArray &source) {
    return owned(xdnote_markdown_native_model(reinterpret_cast<const unsigned char *>(source.constData()), std::size_t(source.size())));
}
void put(QByteArray &bytes, qsizetype at, quint32 value) { qToLittleEndian(value, bytes.data() + at); }
QList<NativePreviewModel::Node> nodes(const NativePreviewModel &model, Kind kind) {
    QList<NativePreviewModel::Node> result;
    for (quint32 index = 0; index < model.nodeCount(); ++index) {
        const auto value = model.node(index); if (value.kind == kind) result.append(value);
    }
    return result;
}
}

class NativePreviewModelTests final : public QObject {
    Q_OBJECT
private slots:
    void globalReferencesAndHeadingIdsRetainExistingContracts() {
        const auto source = QStringLiteral("# 中文 **加粗**\n# 中文 加粗\n# <b>标签</b>\n#\n\n> - [x] ![[alt][ref]](assets/a.png \"图片\")\n>   - ~~子项~~\n\n[正文][ref] ![图][image]\n\n[ref]: nested/doc.md#title \"标题\"\n[image]: assets/a.png\n").toUtf8();
        QString error = "stale error";
        const auto model = NativePreviewModel::parse(source, &error);
        QVERIFY2(model, qPrintable(error)); QVERIFY(error.isEmpty()); QVERIFY(model->referencesComplete());
        QCOMPARE(model->sourceDigest(), QCryptographicHash::hash(source, QCryptographicHash::Sha256));
        QCOMPARE(model->sourceBytes(), quint32(source.size()));
        const auto headingBytes = owned(xdnote_markdown_headings(reinterpret_cast<const unsigned char *>(source.constData()), std::size_t(source.size())));
        const auto expectedHeadings = QJsonDocument::fromJson(headingBytes).object()["headings"].toArray();
        const auto actualHeadings = nodes(*model, Kind::Heading);
        QCOMPARE(actualHeadings.size(), expectedHeadings.size());
        for (qsizetype index = 0; index < actualHeadings.size(); ++index) {
            const auto heading = expectedHeadings[index].toObject(); const auto value = actualHeadings[index];
            QCOMPARE(QString::fromUtf8(model->text(value)), heading["id"].toString());
            QCOMPARE(value.detail, quint32(heading["level"].toInt()));
            QCOMPARE(value.sourceStart, quint32(heading["start"].toInt()));
            QCOMPARE(value.sourceEnd, quint32(heading["end"].toInt()));
        }
        const auto referenceBytes = owned(xdnote_markdown_references(reinterpret_cast<const unsigned char *>(source.constData()), std::size_t(source.size())));
        const auto references = QJsonDocument::fromJson(referenceBytes).object()["references"].toArray();
        QList<NativePreviewModel::Node> actualReferences;
        for (quint32 index = 0; index < model->nodeCount(); ++index) {
            const auto value = model->node(index);
            if (value.kind == Kind::Link || value.kind == Kind::Image) actualReferences.append(value);
        }
        QCOMPARE(actualReferences.size(), references.size());
        quint32 links = 0, images = 0;
        for (qsizetype index = 0; index < references.size(); ++index) {
            const auto reference = references[index].toObject(); const auto value = actualReferences[index];
            QCOMPARE(value.detail, value.kind == Kind::Link ? links++ : images++);
            QCOMPARE(value.kind == Kind::Link ? "link" : "image", reference["kind"].toString());
            QCOMPARE(QString::fromUtf8(model->text(value)), reference["target"].toString());
            QCOMPARE(QString::fromUtf8(model->auxiliary(value)), reference["title"].toString());
            QCOMPARE(value.sourceStart, quint32(reference["start"].toInt()));
            QCOMPARE(value.sourceEnd, quint32(reference["end"].toInt()));
        }
        QCOMPARE(nodes(*model, Kind::BlockQuote).size(), 1);
        QCOMPARE(nodes(*model, Kind::TaskMarker).first().detail, quint32(1));
        QCOMPARE(nodes(*model, Kind::BulletList).size(), 2);
    }

    void literalTextAndTableMetadataAreInert() {
        const auto source = QStringLiteral("中文😀é &amp; \\*\r\n下一行  \n硬换行   <b>text</b>\n\n<script>fetch('file:///private')</script>\n\n|左|中|右|默认|\n|:--|:--:|--:|--|\n|a|b|c|d|\n\n```cpp ignored\n<script>literal</script>\n```\n").toUtf8();
        const auto original = source;
        QString error; const auto model = NativePreviewModel::parse(source, &error);
        QVERIFY2(model, qPrintable(error)); QCOMPARE(source, original);
        QString literal;
        for (const auto &value : nodes(*model, Kind::LiteralHtml)) literal += QString::fromUtf8(model->text(value));
        QVERIFY(literal.contains("<script>fetch('file:///private')</script>"));
        QCOMPARE(nodes(*model, Kind::SoftBreak).size(), 1); QCOMPARE(nodes(*model, Kind::HardBreak).size(), 1);
        const auto tables = nodes(*model, Kind::Table); QCOMPARE(tables.size(), 1);
        QCOMPARE(model->text(tables.first()).toByteArray(), QByteArray("LCRN"));
        QCOMPARE(tables.first().detail, quint32(4));
        QCOMPARE(model->text(nodes(*model, Kind::CodeBlock).first()).toByteArray(), QByteArray("cpp"));
        QVERIFY(nodes(*model, Kind::Link).isEmpty()); QVERIFY(nodes(*model, Kind::Image).isEmpty());
    }

    void maximumSingleBlocks_data() {
        QTest::addColumn<QByteArray>("flavor");
        QTest::newRow("single-paragraph-2MiB") << QByteArray("paragraph");
        QTest::newRow("single-code-2MiB") << QByteArray("code");
        QTest::newRow("dense-table-2MiB") << QByteArray("table");
        QTest::newRow("many-paragraphs-2MiB") << QByteArray("many");
    }
    void maximumSingleBlocks() {
        QFETCH(QByteArray, flavor);
        constexpr qsizetype size = 2 * 1024 * 1024;
        QByteArray source;
        if (flavor == "paragraph") source = QByteArray(size, 'x');
        else if (flavor == "code") source = "```\n" + QByteArray(size - 8, 'x') + "\n```";
        else if (flavor == "table") {
            source = "|a|b|\n|--|--|\n";
            while (source.size() + 6 <= size) source += "|x|y|\n";
        } else {
            while (source.size() + 5 <= size) source += "a b\n\n";
        }
        const auto digest = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
        QString error; const auto model = NativePreviewModel::parse(source, &error);
        QVERIFY2(model, qPrintable(error)); QVERIFY(model->nodeCount() > 0);
        QCOMPARE(model->sourceDigest(), digest); QVERIFY(model->encodedBytes() <= NativePreviewModel::MaximumBytes);
        // Storage guards for these fixed diagnostic fixtures, not a product
        // process-memory promise. Catch a return to expanded per-node objects.
        if (flavor == "table") QVERIFY(model->encodedBytes() <= source.size() * 16);
        else if (flavor == "many") QVERIFY(model->encodedBytes() <= source.size() * 8);
        else QVERIFY(model->encodedBytes() <= source.size() + 1024);
        quint32 roots = 0, rows = 0; qsizetype textBytes = 0;
        for (quint32 index = 0; index < model->nodeCount(); ++index) {
            const auto value = model->node(index);
            if (value.parent == NativePreviewModel::NoParent) ++roots;
            if (value.kind == Kind::TableRow) ++rows;
            if (value.kind == Kind::Text) textBytes += model->text(value).size();
            QVERIFY(value.kind != Kind::Heading);
        }
        if (flavor == "many") QCOMPARE(roots, quint32(source.size() / 5));
        else {
            QCOMPARE(roots, quint32(1)); QCOMPARE(model->node(0).subtreeEnd, model->nodeCount());
            if (flavor == "table") QCOMPARE(rows, quint32((source.size() - 14) / 6));
            else QVERIFY(textBytes >= size - 8);
        }
        qInfo("native semantic model: fixture=%s source=%lld encoded=%lld nodes=%u", flavor.constData(),
              qlonglong(source.size()), qlonglong(model->encodedBytes()), model->nodeCount());
    }

    void rejectsMalformedWireDataWithoutPublishingPartialModel() {
        const auto source = QStringLiteral("# 中文\n\n[a](b.md)\n").toUtf8();
        const auto valid = encoded(source);
        QVERIFY(!valid.isEmpty()); QVERIFY(NativePreviewModel::decode(valid, source));
        QString error;
        for (qsizetype length = 0; length < valid.size(); ++length) {
            const auto result = NativePreviewModel::decode(valid.first(length), source, &error);
            QVERIFY2(!result, qPrintable(QString::number(length))); QVERIFY(!error.isEmpty());
        }
        // Header version, source size, count, arena size, unknown flags and
        // record bytes, followed by the first heading's fixed string fields.
        for (const auto &mutation : QList<QPair<qsizetype, quint32>>{
                 {8, 2}, {12, 0}, {16, 0xffffffffu}, {20, 0xffffffffu}, {24, 2}, {28, 1},
                 {34, 0}, {41, 0xffffffffu}, {45, 0xffffffffu}}) {
            auto broken = valid; put(broken, mutation.first, mutation.second);
            QVERIFY2(!NativePreviewModel::decode(broken, source, &error), qPrintable(QString::number(mutation.first)));
        }
        auto broken = valid; broken[0] = '!'; QVERIFY(!NativePreviewModel::decode(broken, source));
        for (const auto &mutation : QList<QPair<qsizetype, char>>{{32, 99}, {33, 8}, {38, 1}, {38, char(128)}, {40, 127}, {49, 7}, {49, 0}}) {
            broken = valid; broken[mutation.first] = mutation.second;
            QVERIFY(!NativePreviewModel::decode(broken, source));
        }
        const auto indexAt = 32 + qFromLittleEndian<quint32>(valid.constData() + 28);
        broken = valid; put(broken, indexAt, 1); QVERIFY(!NativePreviewModel::decode(broken, source));
        broken = valid; put(broken, indexAt + 4, 0xffffffffu); QVERIFY(!NativePreviewModel::decode(broken, source));
        broken = valid; broken.append('x'); QVERIFY(!NativePreviewModel::decode(broken, source));
        broken = valid; broken[broken.size() - 1] = char(0xff); QVERIFY(!NativePreviewModel::decode(broken, source));
        // UTF-8 source offsets cannot land inside the first Chinese character.
        broken = valid; broken[39] = 3; QVERIFY(!NativePreviewModel::decode(broken, source));
        broken = valid; put(broken, 41, qFromLittleEndian<quint32>(valid.constData() + 41) + 1);
        QVERIFY(!NativePreviewModel::decode(broken, source));
        auto badSource = source; badSource[0] = char(0xff); QVERIFY(!NativePreviewModel::decode(valid, badSource));
    }

    void inputErrorsEmptyInputAndReferenceExpansionAreExplicit() {
        QString error;
        QVERIFY(!NativePreviewModel::parse(QByteArray(1, char(0xff)), &error)); QVERIFY(error.contains("UTF-8"));
        QVERIFY(!NativePreviewModel::parse(QByteArray(2 * 1024 * 1024 + 1, 'x'), &error)); QVERIFY(error.contains(QStringLiteral("预算")));
        const auto empty = NativePreviewModel::parse({}, &error);
        QVERIFY2(empty, qPrintable(error)); QCOMPARE(empty->nodeCount(), quint32(0)); QVERIFY(error.isEmpty());
        const auto guardedSource = QByteArray("[x] ").repeated(1000) + "\n\n[x]: " + QByteArray(2000, 'a') + "\n";
        const auto guarded = NativePreviewModel::parse(guardedSource, &error);
        QVERIFY2(guarded, qPrintable(error)); QVERIFY(!guarded->referencesComplete());
    }

    void deepTreeAndCopiedModelRetainIndependentReadOnlyLifetime() {
        const auto source = QByteArray("> ").repeated(4096) + "text\n";
        auto model = NativePreviewModel::parse(source);
        QVERIFY(model); QCOMPARE(nodes(*model, Kind::BlockQuote).size(), 4096);
        auto copy = *model;
        model.reset();
        QCOMPARE(copy.node(4095).parent, quint32(4094));
        QCOMPARE(copy.node(0).subtreeEnd, copy.nodeCount());
        const auto literal = nodes(copy, Kind::Text).first();
        QCOMPARE(copy.text(literal).toByteArray(), QByteArray("text"));
        NativePreviewModel::Node forged;
        forged.textOffset = 0xffffffffu; forged.textLength = 9;
        QVERIFY(copy.text(forged).isEmpty());
    }
};
QTEST_GUILESS_MAIN(NativePreviewModelTests)
#include "native_preview_model_tests.moc"
