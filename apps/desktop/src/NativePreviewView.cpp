#include "NativePreviewView.h"
#include "Appearance.h"
#include "InputModality.h"
#include "MarkdownBridge.h"
#include "SharedWidgets.h"
#include "UiFonts.h"
#include "Utf8Text.h"
#include "VisualTokens.h"
#include <QAbstractTextDocumentLayout>
#include <QAccessibleWidget>
#include <QBuffer>
#include <QCache>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

namespace xdnote {
namespace {
using Kind = NativePreviewModel::Kind;
using Content = NativePreviewContent;
constexpr int FragmentCharacters = 4096;
constexpr int MaximumFragmentCharacters = 16384;
constexpr int CacheKiB = 8 * 1024, MaximumDocuments = 48;
constexpr qsizetype MaximumDecodedPerDocument = 16 * 1024 * 1024;
bool boundary(const QString &text, int at) {
    return at >= 0 && at <= text.size() && !(at > 0 && at < text.size() && text[at - 1].isHighSurrogate() && text[at].isLowSurrogate());
}
bool separator(QChar ch) { return ch == '\n' || ch == QChar(0x2028) || ch == QChar(0x2029); }
struct NativeDocument final : QTextDocument {
    QVector<int> positions;
    qsizetype decodedBytes = 0;
    std::function<QVariant(const QUrl &, NativeDocument *)> loader;
    std::function<void(qsizetype)> released;
    ~NativeDocument() override { if (released) released(decodedBytes); }
    int global(int local) const { return positions[qBound(0, local, int(positions.size()) - 1)]; }
    int local(int global, bool ceil = false) const {
        auto it = std::lower_bound(positions.cbegin(), positions.cend(), global);
        if (it == positions.cend()) return int(positions.size()) - 1;
        const int index = int(it - positions.cbegin());
        return !ceil && *it > global ? qMax(0, index - 1) : index;
    }
protected:
    QVariant loadResource(int type, const QUrl &url) override {
        return type == ImageResource && loader ? loader(url, this) : QVariant();
    }
};
struct Chunk {
    int flow = 0, start = 0, end = 0, probeEnd = 0;
    qreal y = 0, height = 0;
};
struct Geometry { qreal y = 0, height = 0; int first = 0, count = 0; };
struct GroupGeometry {
    qreal y = 0, height = 0, width = 0;
    int firstFlow = -1;
    QVector<qreal> columns;
};
struct ImageInfo { QSize pixels; int start = 0, length = 0; };
struct Measurement { QString text; quint32 font = 0; qreal width = 0; };
}

struct NativePreviewView::State {
    NativePreviewView *q;
    Appearance *appearance;
    int padding;
    qreal zoom = 1, left = 24, width = 760, totalHeight = 0;
    NativePreviewStamp expected, shown;
    std::shared_ptr<const Content> content;
    QHash<int, QByteArray> png;
    QHash<int, ImageInfo> imageInfo;
    QVector<Geometry> geometry;
    QVector<GroupGeometry> groups;
    QVector<Chunk> chunks;
    QHash<int, qreal> horizontal;
    QHash<int, SharedButton *> buttons;
    QHash<int, QScrollBar *> scrollbars;
    QString error;
    int anchor = 0, cursor = 0, pressedAction = -1, focusedAction = -1;
    bool dragging = false, keyboardOutline = false;
    bool localDocumentsAdmitted = false;
    std::unique_ptr<QTextBoundaryFinder> graphemes;
    mutable QHash<quint32, QFont> fonts;
    mutable QCache<quint64, Measurement> measurements{1024};
    Statistics stats;
    QCache<quint64, NativeDocument> cache{CacheKiB};
    std::unique_ptr<NativeDocument> transient;

    explicit State(NativePreviewView *view, Appearance *theme, int inset) : q(view), appearance(theme), padding(inset) {}
    bool current() const { return content && shown == expected && error.isEmpty(); }
    const Content::Flow &flow(int index) const { return content->flows[index]; }
    qreal indent(const Content::Flow &value) const {
        return qMin(width * .65, (qreal(value.indent) * 20 + qreal(value.quote) * 19) * zoom);
    }
    qreal available(const Content::Flow &value) const { return qMax(48.0, width - indent(value)); }
    quint32 fontKey(const Content::Flow &value, quint32 style) const {
        return (style & 31) | (quint32(value.heading) << 8) | (quint32(value.header) << 16)
            | (quint32(value.kind == Content::FlowKind::CodeLine) << 17);
    }
    QFont font(const Content::Flow &value, quint32 style = 0) const {
        const quint32 key = fontKey(value, style);
        const auto found = fonts.constFind(key); if (found != fonts.cend()) return *found;
        const auto typography = visualTokens()["typography"].toObject();
        int size = typography[value.kind == Content::FlowKind::CodeLine || (style & Content::Mono) ? "source" : "reader"].toObject()["size"].toInt();
        if (value.heading) size = typography["headings"].toObject()[value.heading == 1 ? "documentH1" : value.heading == 2 ? "documentH2" : value.heading == 3 ? "documentH3" : ""].toInt(16);
        auto result = value.kind == Content::FlowKind::CodeLine || (style & Content::Mono) ? sourceFont(qMax(1, qRound(size * zoom))) : uiFont(qMax(1, qRound(size * zoom)));
        result.setBold(value.heading || value.header || (style & Content::Bold));
        result.setItalic(style & Content::Italic); result.setStrikeOut(style & Content::Strike);
        if (value.kind == Content::FlowKind::CodeLine) result.setKerning(false);
        fonts.insert(key, result);
        return result;
    }
    qreal lineHeight(const Content::Flow &value) const {
        return qMax(QFontMetricsF(font(value)).height(), font(value).pixelSize() * (value.heading ? 1.4 : 1.8));
    }
    int safeEnd(int start, int limit, int end) const {
        int result = qMin(end, start + limit);
        if (result < end && !boundary(content->text, result)) --result;
        // Boundary state belongs to the whole derived text, including regional
        // indicator parity and long ZWJ sequences across a proposed fragment.
        graphemes->setPosition(result);
        if (!graphemes->isAtBoundary()) {
            const int previous = graphemes->toPreviousBoundary();
            if (previous > start) result = previous;
            else {
                graphemes->setPosition(result); const int next = graphemes->toNextBoundary();
                if (next <= start) throw std::runtime_error("grapheme-layout-budget");
                result = next;
            }
        }
        for (const auto &image : imageInfo) {
            if (image.start < result && image.start + image.length > result)
                result = image.start > start ? image.start : image.start + image.length;
        }
        return qMax(start + 1, qMin(end, result));
    }
    qreal measure(const Content::Flow &value, int start, int length, bool images = true) const {
        qreal result = 0;
        auto it = std::lower_bound(content->runs.cbegin(), content->runs.cend(), start,
            [](const Content::Run &run, int at) { return run.start + run.length <= at; });
        const int end = start + length;
        for (; it != content->runs.cend() && it->start < end; ++it) {
            int from = qMax(start, it->start), stop = qMin(end, it->start + it->length);
            const QFontMetricsF metrics(font(value, it->style));
            while (from < stop) {
                int to = qMin(stop, from + FragmentCharacters);
                if (!boundary(content->text, to)) --to;
                const auto text = QStringView(content->text).mid(from, to - from);
                const auto shape = fontKey(value, it->style); const quint64 key = qHash(text, shape);
                const auto found = measurements.object(key);
                if (found && found->font == shape && found->text == text) result += found->width;
                else {
                    const QString owned = text.toString(); const auto width = metrics.horizontalAdvance(owned);
                    result += width; measurements.insert(key, new Measurement{owned, shape, width}, qMax(1, int((128 + owned.size() * 2 + 1023) / 1024)));
                }
                from = to;
            }
        }
        if (images) {
            auto image = std::lower_bound(content->images.cbegin(), content->images.cend(), start,
                [](const Content::Image &entry, int position) { return entry.start < position; });
            for (; image != content->images.cend() && image->start < end; ++image) {
                const auto info = imageInfo.constFind(int(image->index));
                if (info != imageInfo.cend() && image->start + image->length <= end)
                    result += qMin(qreal(info->pixels.width()), available(value) - 24 * zoom)
                        - measure(value, image->start, image->length, false);
            }
        }
        return result;
    }
    std::unique_ptr<NativeDocument> document(int flowIndex, int start, int end, int activeEnd, qreal textWidth, bool probe) {
        const auto &value = flow(flowIndex);
        auto doc = std::make_unique<NativeDocument>();
        doc->setLayoutEnabled(false);
        doc->setUndoRedoEnabled(false); doc->setDocumentMargin(0); doc->setDefaultFont(font(value));
        QString text;
        QVector<QPair<int, int>> objects;
        int at = start;
        auto image = std::lower_bound(content->images.cbegin(), content->images.cend(), start,
            [](const Content::Image &entry, int position) { return entry.start < position; });
        while (at < end) {
            if (image != content->images.cend() && image->start == at && image->start + image->length <= end
                && imageInfo.contains(int(image->index))) {
                doc->positions.append(at); objects.append({int(text.size()), int(image->index)});
                text += QChar::ObjectReplacementCharacter; at += image->length; ++image;
            } else {
                doc->positions.append(at); text += content->text[at++];
                while (image != content->images.cend() && image->start < at) ++image;
            }
        }
        doc->positions.append(end);
        if (text.size() > MaximumFragmentCharacters) throw std::runtime_error("fragment-layout-budget");
        QTextOption option; option.setWrapMode(value.kind == Content::FlowKind::Text ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
        doc->setDefaultTextOption(option); doc->setPlainText(text);
        QTextCursor cursor(doc.get()); cursor.select(QTextCursor::Document);
        QTextBlockFormat block; block.setTopMargin(0); block.setBottomMargin(0);
        block.setLineHeight(lineHeight(value), QTextBlockFormat::MinimumHeight); cursor.setBlockFormat(block);
        QTextCharFormat base; base.setForeground(appearance->color("text")); cursor.setCharFormat(base);
        auto run = std::lower_bound(content->runs.cbegin(), content->runs.cend(), start,
            [](const Content::Run &entry, int position) { return entry.start + entry.length <= position; });
        for (; run != content->runs.cend() && run->start < end; ++run) {
            const int from = doc->local(qMax(start, run->start)), to = doc->local(qMin(end, run->start + run->length), true);
            cursor.setPosition(from); cursor.setPosition(to, QTextCursor::KeepAnchor);
            QTextCharFormat format; format.setFont(font(value, run->style));
            if (run->link != NativePreviewModel::NoParent) {
                format.setForeground(appearance->color("textSecondary")); format.setFontUnderline(true);
            }
            cursor.mergeCharFormat(format);
            if (run->style & Content::Task) {
                QImage icon(QSize(16, 16) * q->devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
                icon.setDevicePixelRatio(q->devicePixelRatioF()); icon.fill(Qt::transparent);
                QPainter painter(&icon); const bool checked = content->text[run->start] == QChar(0x2611);
                painter.setRenderHint(QPainter::Antialiasing); painter.setPen(QPen(checked ? appearance->accentColor() : appearance->color("textSecondary"), 1.5));
                painter.setBrush(checked ? appearance->accentColor() : Qt::transparent); painter.drawRoundedRect(QRectF(1, 1, 14, 14), 3, 3);
                if (checked) { painter.setPen(QPen(appearance->accentColor("foreground"), 2)); painter.drawLine(QPointF(4, 8), QPointF(7, 11)); painter.drawLine(QPointF(7, 11), QPointF(12, 5)); }
                painter.end(); const QUrl url(QStringLiteral("xdnote-native:task/%1").arg(run->start));
                doc->addResource(QTextDocument::ImageResource, url, icon);
                QTextImageFormat format; format.setName(url.toString()); format.setWidth(16 * zoom); format.setHeight(16 * zoom);
                cursor.setPosition(from); cursor.setPosition(qMin(from + 1, to), QTextCursor::KeepAnchor); cursor.insertImage(format);
            }
        }
        const qreal availableWidth = qMax(1.0, textWidth);
        for (const auto &object : objects) {
            const auto size = imageInfo.value(object.second).pixels;
            const qreal imageWidth = qMin(availableWidth, value.kind == Content::FlowKind::TableRow ? available(value) - 24 * zoom : availableWidth);
            const qreal scale = qMin(1.0, imageWidth / size.width());
            QTextImageFormat format; format.setName(QStringLiteral("xdnote-native:image/%1").arg(object.second));
            format.setWidth(size.width() * scale); format.setHeight(size.height() * scale);
            cursor.setPosition(object.first); cursor.setPosition(object.first + 1, QTextCursor::KeepAnchor); cursor.setCharFormat(format);
        }
        doc->loader = [this, probe, activeEnd, start, availableWidth](const QUrl &url, NativeDocument *owner) -> QVariant {
            const QString name = url.toString(); const QString prefix = QStringLiteral("xdnote-native:image/");
            bool valid = false; const int index = name.startsWith(prefix) ? name.mid(prefix.size()).toInt(&valid) : -1;
            if (!valid || name != prefix + QString::number(index) || !imageInfo.contains(index) || !png.contains(index)) { ++stats.deniedResources; return {}; }
            if (probe) return QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
            const auto &info = imageInfo[index]; const bool active = info.start >= start && info.start < activeEnd;
            if (!active) return QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
            QBuffer input; input.setData(png.value(index)); input.open(QIODevice::ReadOnly);
            QImageReader reader(&input, "png"); const auto original = imageInfo.value(index).pixels;
            qreal scale = qMin(1.0, availableWidth * q->devicePixelRatioF() / original.width());
            if (original.width() * qreal(original.height()) * scale * scale > 2'000'000)
                scale = std::sqrt(2'000'000.0 / (original.width() * qreal(original.height())));
            reader.setScaledSize(QSize(qMax(1, qFloor(original.width() * scale)), qMax(1, qFloor(original.height() * scale))));
            auto image = reader.read(); ++stats.resourceLoads;
            if (image.isNull() || image.sizeInBytes() > MaximumDecodedPerDocument - owner->decodedBytes) {
                ++stats.deniedResources; return {};
            }
            owner->decodedBytes += image.sizeInBytes(); stats.decodedImageBytes += image.sizeInBytes();
            stats.peakDecodedImageBytes = qMax(stats.peakDecodedImageBytes, stats.decodedImageBytes);
            return image;
        };
        doc->released = [this](qsizetype bytes) { stats.decodedImageBytes -= bytes; };
        doc->setTextWidth(value.kind == Content::FlowKind::Text ? availableWidth : -1);
        doc->setLayoutEnabled(true);
        doc->documentLayout()->documentSize(); ++stats.layouts;
        return doc;
    }
    NativeDocument *cachedDocument(int flowIndex, int start, int probeEnd, int end, qreal textWidth) {
        const quint64 key = (quint64(quint32(start)) << 32) | quint32(probeEnd);
        if (auto *found = cache.object(key)) return found;
        auto doc = document(flowIndex, start, probeEnd, end, textWidth, false);
        // Load only admitted objects in this consumed fragment, with dimensions
        // already fixed by the header-only planning pass.
        for (auto it = imageInfo.cbegin(); it != imageInfo.cend(); ++it) {
            const bool active = it->start >= start && it->start < end;
            if (active) doc->resource(QTextDocument::ImageResource, QUrl(QStringLiteral("xdnote-native:image/%1").arg(it.key())));
        }
        const auto cost = 4096 + doc->positions.capacity() * qsizetype(sizeof(int))
            + doc->positions.size() * 64 + doc->decodedBytes;
        if (cost > CacheKiB * 1024) { transient = std::move(doc); return transient.get(); }
        while (cache.size() >= MaximumDocuments) cache.remove(cache.keys().first());
        auto *result = doc.release(); cache.insert(key, result, int((cost + 1023) / 1024));
        stats.cachedDocuments = int(cache.size()); stats.cachedEstimatedBytes = cache.totalCost() * 1024;
        stats.peakCachedDocuments = qMax(stats.peakCachedDocuments, stats.cachedDocuments);
        stats.peakCachedEstimatedBytes = qMax(stats.peakCachedEstimatedBytes, stats.cachedEstimatedBytes);
        return result;
    }
    void clearCache() { cache.clear(); transient.reset(); stats.cachedDocuments = 0; stats.cachedEstimatedBytes = 0; }
    int flowAt(int offset) const {
        auto it = std::upper_bound(content->flows.cbegin(), content->flows.cend(), offset,
            [](int position, const Content::Flow &entry) { return position < entry.start; });
        return qMax(0, int(it - content->flows.cbegin()) - 1);
    }
    int flowAtY(qreal y) const {
        auto it = std::upper_bound(geometry.cbegin(), geometry.cend(), y,
            [](qreal at, const Geometry &entry) { return at < entry.y; });
        return qMax(0, int(it - geometry.cbegin()) - 1);
    }
    qreal x(int flowIndex) const { return left + indent(flow(flowIndex)); }
    qreal offsetX(int owner) const { return horizontal.value(owner, 0); }
    qreal cellX(int flowIndex, int column, qreal left) const {
        const auto &value = flow(flowIndex); const auto &cell = content->cells[value.firstCell + column];
        const auto alignments = content->model.text(content->model.node(content->owners[value.owner].node));
        const qreal width = groups[value.owner].columns[column], measured = measure(value, cell.start, cell.length);
        if (column < alignments.size() && alignments[column] == 'R') return left + width - measured - 12 * zoom;
        if (column < alignments.size() && alignments[column] == 'C') return left + (width - measured) / 2;
        return left + 12 * zoom;
    }
    void eachTile(int flowIndex, int start, int end, qreal baseX, qreal baseY,
                  const std::function<void(int, int, qreal, qreal, qreal)> &visit) const {
        qreal y = baseY, x = baseX;
        while (start < end) {
            int lineEnd = start;
            while (lineEnd < end && !separator(content->text[lineEnd])) ++lineEnd;
            int from = start;
            while (from < lineEnd) {
                const int to = safeEnd(from, FragmentCharacters, lineEnd);
                const qreal width = measure(flow(flowIndex), from, to - from);
                visit(from, to, x, y, width); x += width; from = to;
            }
            start = lineEnd + 1; x = baseX; y += lineHeight(flow(flowIndex));
        }
    }
    QRectF positionRect(NativeDocument *doc, int offset, qreal x, qreal y) const {
        const int local = doc->local(offset); const auto block = doc->findBlock(local);
        if (!block.isValid()) return {};
        const auto layout = block.layout(); const auto line = layout->lineForTextPosition(local - block.position());
        if (!line.isValid()) return {};
        const qreal at = line.cursorToX(local - block.position());
        int next = qMin(local + 1, doc->characterCount() - 1);
        if (next < doc->positions.size() - 1 && !boundary(content->text, doc->global(next))) ++next;
        const qreal to = line.cursorToX(next - block.position());
        const auto rect = doc->documentLayout()->blockBoundingRect(block);
        return {x + qMin(at, to), y + rect.y() + line.y(), qMax(1.0, qAbs(to - at)), line.height()};
    }
};

class AccessibleNativePreview final : public QAccessibleWidget, public QAccessibleTextInterface {
public:
    explicit AccessibleNativePreview(NativePreviewView *view) : QAccessibleWidget(view, QAccessible::Document) {}
    NativePreviewView *view() const { return qobject_cast<NativePreviewView *>(object()); }
    void *interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::TextInterface) return static_cast<QAccessibleTextInterface *>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    QString contents() const { return view() && view()->content() ? view()->content()->text : QString(); }
    QAccessible::State state() const override {
        auto value = QAccessibleWidget::state(); value.readOnly = true; value.editable = false;
        value.multiLine = true; value.selectableText = true; value.focusable = true; return value;
    }
    QString text(QAccessible::Text type) const override {
        if (type == QAccessible::Value) return contents();
        if (type == QAccessible::Description && view()) return view()->error();
        return QAccessibleWidget::text(type);
    }
    void setText(QAccessible::Text type, const QString &value) override {
        if (type != QAccessible::Value) QAccessibleWidget::setText(type, value);
    }
    int characterCount() const override { return int(contents().size()); }
    QString text(int start, int end) const override {
        const auto value = contents(); if (end == -1) end = int(value.size());
        return start <= end && boundary(value, start) && boundary(value, end) ? value.mid(start, end - start) : QString();
    }
    int selectionCount() const override { return view() && view()->selection().first != view()->selection().second ? 1 : 0; }
    void selection(int index, int *start, int *end) const override {
        *start = *end = -1; if (index != 0 || !view()) return;
        const auto value = view()->selection(); *start = qMin(value.first, value.second); *end = qMax(value.first, value.second);
    }
    void addSelection(int start, int end) override { setSelection(0, start, end); }
    void setSelection(int index, int start, int end) override { if (index == 0 && view()) view()->setSelection(start, end); }
    void removeSelection(int index) override { if (index == 0 && view()) view()->setSelection(cursorPosition(), cursorPosition()); }
    int cursorPosition() const override { return view() ? view()->selection().second : -1; }
    void setCursorPosition(int position) override { if (view()) view()->setSelection(position, position); }
    QRect characterRect(int offset) const override { return view() ? view()->characterRect(offset) : QRect(); }
    int offsetAtPoint(const QPoint &point) const override {
        if (!view() || !view()->viewport()->rect().contains(view()->viewport()->mapFromGlobal(point))) return -1;
        return view()->offsetAtPoint(view()->viewport()->mapFromGlobal(point));
    }
    void scrollToSubstring(int start, int end) override {
        const auto value = contents(); if (view() && boundary(value, start) && boundary(value, end) && start <= end) view()->scrollToText(start);
    }
    QString attributes(int offset, int *start, int *end) const override {
        if (!boundary(contents(), offset)) { *start = *end = -1; return {}; }
        *start = 0; *end = characterCount(); return QStringLiteral("readonly:true;");
    }
};

static void installNativeAccessibility() {
    static const bool installed = [] {
        QAccessible::installFactory([](const QString &, QObject *object) -> QAccessibleInterface * {
            if (auto *view = qobject_cast<NativePreviewView *>(object)) return new AccessibleNativePreview(view);
            return nullptr;
        });
        return true;
    }();
    Q_UNUSED(installed);
}

NativePreviewView::NativePreviewView(Appearance *appearance, QWidget *parent, int padding)
    : QAbstractScrollArea(parent), d(std::make_unique<State>(this, appearance ? appearance : new Appearance(this), padding)) {
    setObjectName("nativeMarkdownPreview"); setAccessibleName(QStringLiteral("Markdown 阅读内容"));
    installNativeAccessibility();
    setFocusPolicy(Qt::StrongFocus); setFrameShape(QFrame::NoFrame); viewport()->setMouseTracking(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(d->appearance, &Appearance::changed, this, [this] { rebuild(true); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { updateControls(); viewport()->update(); });
}
NativePreviewView::~NativePreviewView() = default;
void NativePreviewView::expect(const NativePreviewStamp &stamp) {
    for (auto *button : d->buttons) { button->setEnabled(false); button->hide(); button->deleteLater(); }
    for (auto *bar : d->scrollbars) { bar->setEnabled(false); bar->hide(); bar->deleteLater(); }
    d->buttons.clear(); d->scrollbars.clear();
    d->expected = stamp; d->content.reset(); d->clearCache(); d->png.clear(); d->imageInfo.clear();
    d->geometry.clear(); d->chunks.clear(); d->groups.clear(); d->horizontal.clear(); d->error.clear();
    d->anchor = d->cursor = 0; d->focusedAction = -1; d->keyboardOutline = false;
    d->dragging = false; d->pressedAction = -1;
    d->graphemes.reset(); d->shown = {}; d->stats = {};
    d->localDocumentsAdmitted = false;
    verticalScrollBar()->setRange(0, 0); updateControls(); viewport()->update();
}
bool NativePreviewView::present(NativePreviewStamp stamp, std::shared_ptr<const Content> content, QHash<int, QByteArray> png, bool localDocumentsAdmitted) {
    if (stamp != d->expected || stamp.document.isNull() || !content) return false;
    d->clearCache(); d->content = std::move(content); d->shown = stamp; d->png.clear(); d->imageInfo.clear();
    qsizetype encoded = 0; qint64 pixels = 0;
    for (auto it = png.cbegin(); it != png.cend(); ++it) {
        if (d->imageInfo.size() >= 32 || it.key() < 0 || it.value().size() > 32 * 1024 * 1024 - encoded) continue;
        if (it.key() >= d->content->images.size() || d->content->images[it.key()].index != quint32(it.key()) || d->content->images[it.key()].state != 1) continue;
        const auto &entry = d->content->images[it.key()];
        QBuffer input; input.setData(it.value()); input.open(QIODevice::ReadOnly);
        QImageReader reader(&input, "png"); const auto size = reader.size();
        const qint64 count = qint64(size.width()) * size.height();
        if (reader.format() != "png" || !size.isValid() || size.width() > 8192 || size.height() > 8192
            || count > 12'000'000 || count > 24'000'000 - pixels) continue;
        encoded += it.value().size(); pixels += count;
        d->png.insert(it.key(), it.value()); d->imageInfo.insert(it.key(), {size, entry.start, entry.length});
    }
    d->stats = {}; d->stats.retainedContentBytes = d->content->estimatedBytes();
    d->localDocumentsAdmitted = localDocumentsAdmitted;
    d->graphemes = std::make_unique<QTextBoundaryFinder>(QTextBoundaryFinder::Grapheme, d->content->text);
    d->anchor = d->cursor = 0; d->focusedAction = -1; d->keyboardOutline = false; d->horizontal.clear();
    d->dragging = false; d->pressedAction = -1;
    const QPointer<NativePreviewView> alive(this);
    rebuild(false); return alive && d->error.isEmpty();
}
void NativePreviewView::setZoomFactor(qreal factor) { factor = qBound(.5, factor, 2.0); if (qFuzzyCompare(factor, d->zoom)) return; d->zoom = factor; rebuild(true); }
qreal NativePreviewView::zoomFactor() const { return d->zoom; }
const Content *NativePreviewView::content() const { return d->content.get(); }
NativePreviewStamp NativePreviewView::displayedStamp() const { return d->shown; }
NativePreviewView::Statistics NativePreviewView::statistics() const { return d->stats; }
QString NativePreviewView::error() const { return d->error; }
bool NativePreviewView::keyboardOutlineVisible() const {
    return d->keyboardOutline && hasFocus() && window()->isActiveWindow()
        && d->focusedAction >= 0 && InputModality::forApplication()->keyboardNavigation();
}

void NativePreviewView::rebuild(bool keepPosition) {
    const int oldOffset = keepPosition && d->content && !d->geometry.isEmpty()
        ? offsetAtPoint(QPoint(qRound(d->left + 1), 1)) : 0;
    d->clearCache(); d->geometry.clear(); d->chunks.clear(); d->groups.clear(); d->error.clear();
    d->fonts.clear(); d->measurements.clear();
    setStyleSheet(themedScrollBars(d->appearance));
    if (!d->content) return;
    try {
        const auto &content = *d->content;
        d->geometry.reserve(content.flows.size());
        const qreal inset = d->padding * d->zoom;
        const int maximum = visualTokens()["typography"].toObject()["reader"].toObject()["maxContentWidth"].toInt();
        d->width = qMax(48.0, qMin(viewport()->width() - inset * 2, maximum * d->zoom));
        d->left = qMax(inset, (viewport()->width() - d->width) / 2);
        d->groups.resize(content.owners.size());
        // Widths are measured in bounded text runs. Table rows need no retained
        // QTextDocument or per-cell layout object during this global pass.
        for (int index = 0; index < content.flows.size(); ++index) {
            const auto &flow = content.flows[index];
            if (flow.kind != Content::FlowKind::TableRow) continue;
            auto &group = d->groups[flow.owner]; group.columns.resize(qMax(group.columns.size(), qsizetype(flow.cellCount)));
            if (group.firstFlow < 0) group.firstFlow = index;
            for (int column = 0; column < flow.cellCount; ++column) {
                const auto &cell = content.cells[flow.firstCell + column]; qreal maximum = 0;
                d->eachTile(index, cell.start, cell.start + cell.length, 0, 0,
                    [&](int, int, qreal x, qreal, qreal width) { maximum = qMax(maximum, x + width); });
                group.columns[column] = qMax(group.columns[column], maximum + 24 * d->zoom);
            }
        }
        for (auto &group : d->groups) {
            for (auto width : group.columns) group.width += width;
            if (!group.columns.isEmpty() && group.firstFlow >= 0 && group.width < d->available(d->flow(group.firstFlow))) {
                const qreal minimum = d->available(d->flow(group.firstFlow));
                const qreal extra = (minimum - group.width) / group.columns.size();
                for (auto &column : group.columns) column += extra;
                group.width = minimum;
            }
        }
        qreal y = inset; int priorOwner = -1;
        for (int index = 0; index < content.flows.size(); ++index) {
            const auto &flow = content.flows[index];
            if (priorOwner >= 0 && priorOwner != flow.owner) {
                auto &prior = d->groups[priorOwner];
                y += content.owners[priorOwner].code ? 12 * d->zoom : 0;
                y += 12; prior.height = y - prior.y; y += 16 * d->zoom;
            }
            if (flow.owner >= 0 && priorOwner != flow.owner) {
                d->groups[flow.owner].y = y;
                d->groups[flow.owner].firstFlow = index;
                if (content.owners[flow.owner].code) y += 44 * d->zoom;
            }
            priorOwner = flow.owner;
            Geometry geometry; geometry.y = y; geometry.first = int(d->chunks.size());
            if (flow.kind == Content::FlowKind::Text) {
                const int end = flow.start + flow.length; int start = flow.start;
                bool shortLine = flow.length <= 128;
                if (shortLine) {
                    for (int at = start; at < end; ++at) if (separator(content.text[at])) { shortLine = false; break; }
                    for (const auto &image : d->imageInfo) if (image.start >= start && image.start < end) { shortLine = false; break; }
                    shortLine = shortLine && d->measure(flow, start, flow.length) + 2 * d->lineHeight(flow) < d->available(flow);
                }
                if (shortLine) {
                    // A short single line needs neither a probe document nor a
                    // retained per-flow chunk. Its real document is built only
                    // when painted, hit-tested or queried by accessibility.
                    geometry.count = -1; geometry.height = d->lineHeight(flow); y += geometry.height;
                } else {
                bool bidi = false;
                for (int at = start; at < end; ++at) {
                    const auto direction = content.text[at].direction();
                    if (direction == QChar::DirR || direction == QChar::DirAL || direction == QChar::DirLRE
                        || direction == QChar::DirRLE || direction == QChar::DirLRO || direction == QChar::DirRLO
                        || direction == QChar::DirPDF || direction == QChar::DirLRI || direction == QChar::DirRLI
                        || direction == QChar::DirFSI || direction == QChar::DirPDI) { bidi = true; break; }
                }
                if (bidi && flow.length > FragmentCharacters) throw std::runtime_error("long-bidi-admission");
                while (start < end) {
                    const int probeEnd = bidi ? end : d->safeEnd(start, FragmentCharacters, end);
                    auto doc = d->document(index, start, probeEnd, start, d->available(flow), true);
                    int consume = probeEnd; qreal height = doc->size().height();
                    const qreal pageHeight = bidi ? std::numeric_limits<qreal>::max() : qMax(256, qMin(1024, viewport()->height() * 2));
                    int lastStart = -1; qreal lastY = 0;
                    for (auto block = doc->begin(); block.isValid(); block = block.next()) {
                        const auto layout = block.layout(); const auto rect = doc->documentLayout()->blockBoundingRect(block);
                        for (int lineIndex = 0; lineIndex < layout->lineCount(); ++lineIndex) {
                            const auto line = layout->lineAt(lineIndex); const int position = block.position() + line.textStart();
                            const qreal lineY = rect.y() + line.y();
                            if (position > 0 && lineY >= pageHeight) { consume = doc->global(position); height = lineY; break; }
                            lastStart = position; lastY = lineY;
                        }
                        if (consume != probeEnd) break;
                    }
                    if (consume == probeEnd && probeEnd < end && lastStart > 0) { consume = doc->global(lastStart); height = lastY; }
                    if (consume <= start) throw std::runtime_error("fragment-no-progress");
                    d->chunks.append({index, start, consume, probeEnd, y, height}); y += height; start = consume;
                }
                geometry.count = int(d->chunks.size()) - geometry.first; geometry.height = y - geometry.y;
                }
                y += flow.indent ? 4 * d->zoom : 16 * d->zoom;
            } else if (flow.kind == Content::FlowKind::Rule) { geometry.height = 16 * d->zoom; y += geometry.height; }
            else if (flow.kind == Content::FlowKind::CodeLine) {
                geometry.height = d->lineHeight(flow); y += geometry.height;
                qreal maximum = 0;
                d->eachTile(index, flow.start, flow.start + flow.length, 0, 0,
                    [&](int, int, qreal x, qreal, qreal width) { maximum = qMax(maximum, x + width); });
                d->groups[flow.owner].width = qMax(d->groups[flow.owner].width, maximum + 32 * d->zoom);
            } else {
                int lines = 1;
                for (int column = 0; column < flow.cellCount; ++column) {
                    const auto &cell = content.cells[flow.firstCell + column]; int count = 1;
                    for (int at = cell.start; at < cell.start + cell.length; ++at) if (separator(content.text[at])) ++count;
                    lines = qMax(lines, count);
                }
                qreal height = d->lineHeight(flow) * lines;
                for (const auto &image : d->imageInfo) if (image.start >= flow.start && image.start < flow.start + flow.length) {
                    const qreal scale = qMin(1.0, (d->available(flow) - 24 * d->zoom) / image.pixels.width());
                    height = qMax(height, image.pixels.height() * scale);
                }
                geometry.height = height + 12 * d->zoom; y += geometry.height;
            }
            d->geometry.append(geometry);
        }
        if (priorOwner >= 0) {
            y += content.owners[priorOwner].code ? 12 * d->zoom : 0;
            y += 12; d->groups[priorOwner].height = y - d->groups[priorOwner].y;
        }
        d->totalHeight = y + inset; d->stats.chunks = int(d->chunks.size()); d->stats.flows = int(content.flows.size());
        verticalScrollBar()->setPageStep(viewport()->height()); verticalScrollBar()->setSingleStep(qMax(1, qRound(28.8 * d->zoom)));
        verticalScrollBar()->setRange(0, qMax(0, qCeil(d->totalHeight) - viewport()->height()));
        if (keepPosition) scrollToText(oldOffset);
    } catch (const std::exception &error) {
        d->error = QByteArray(error.what()) == "long-bidi-admission"
            ? QStringLiteral("暂时无法排版这个超长的双向文字段落，原文保持不变。")
            : QStringLiteral("预览排版超出当前资源预算，原文保持不变。");
        d->clearCache(); d->geometry.clear(); d->chunks.clear(); d->groups.clear();
        verticalScrollBar()->setRange(0, 0);
        const QPointer<NativePreviewView> alive(this); const auto message = d->error;
        emit layoutFailed(message); if (!alive) return;
    }
    updateControls(); viewport()->update();
}

void NativePreviewView::paintEvent(QPaintEvent *) {
    QPainter painter(viewport()); painter.fillRect(viewport()->rect(), d->appearance->color("background"));
    if (!d->error.isEmpty()) {
        painter.setFont(uiFont()); painter.setPen(d->appearance->color("textSecondary"));
        painter.drawText(viewport()->rect().adjusted(24, 24, -24, -24), Qt::TextWordWrap, d->error); return;
    }
    if (!d->content || d->geometry.isEmpty()) return;
    const auto &content = *d->content; const qreal scroll = verticalScrollBar()->value();
    const auto selectionStart = qMin(d->anchor, d->cursor), selectionEnd = qMax(d->anchor, d->cursor);
    const auto draw = [&](int flowIndex, int start, int end, int probeEnd, qreal x, qreal y, qreal width, qreal height) {
        QRectF surface(x, y - scroll, width, height);
        if (!surface.intersects(viewport()->rect())) return;
        auto *doc = d->cachedDocument(flowIndex, start, probeEnd, end, width);
        QAbstractTextDocumentLayout::PaintContext context;
        context.clip = QRectF(qMax(0.0, -x), qMax(0.0, scroll - y), viewport()->width(), qMin(height, viewport()->height() + scroll - y));
        context.palette = palette(); context.palette.setColor(QPalette::Text, d->appearance->color("text"));
        if (selectionStart < end && selectionEnd > start) {
            QAbstractTextDocumentLayout::Selection selected;
            selected.cursor = QTextCursor(doc); selected.cursor.setPosition(doc->local(qMax(start, selectionStart)));
            selected.cursor.setPosition(doc->local(qMin(end, selectionEnd), true), QTextCursor::KeepAnchor);
            selected.format.setBackground(d->appearance->accentColor("soft")); selected.format.setForeground(d->appearance->accentColor("softForeground"));
            context.selections.append(selected);
        }
        painter.save(); painter.setClipRect(surface, Qt::IntersectClip); painter.translate(x, y - scroll); doc->documentLayout()->draw(&painter, context); painter.restore();
    };
    // Shared code surfaces and table borders are independent of cached text.
    for (int owner = 0; owner < d->groups.size(); ++owner) {
        const auto &group = d->groups[owner];
        if (group.firstFlow < 0 || group.y + group.height < scroll || group.y > scroll + viewport()->height()) continue;
        const auto &flow = d->flow(group.firstFlow); const qreal x = d->x(group.firstFlow), width = d->available(flow);
        if (content.owners[owner].code) {
            painter.setPen(QPen(d->appearance->color("border"), 1)); painter.setBrush(d->appearance->color("codeSurface"));
            painter.drawRoundedRect(QRectF(x, group.y - scroll, width, group.height), 8 * d->zoom, 8 * d->zoom);
            painter.drawLine(QPointF(x, group.y - scroll + 32 * d->zoom), QPointF(x + width, group.y - scroll + 32 * d->zoom));
            painter.setPen(d->appearance->color("textSecondary")); painter.setFont(uiFont(qMax(1, qRound(12 * d->zoom))));
            const auto node = content.model.node(content.owners[owner].node);
            painter.drawText(QRectF(x + 12 * d->zoom, group.y - scroll, width - 100 * d->zoom, 32 * d->zoom), Qt::AlignVCenter,
                             decodeUtf8Literal(content.model.text(node)));
        }
    }
    int index = d->flowAtY(scroll);
    for (; index < content.flows.size() && d->geometry[index].y <= scroll + viewport()->height(); ++index) {
        const auto &flow = content.flows[index]; const auto &geometry = d->geometry[index];
        const qreal x = d->x(index), width = d->available(flow);
        if (geometry.y + geometry.height < scroll) continue;
        if (flow.quote) {
            painter.setPen(QPen(d->appearance->color("border"), 3 * d->zoom));
            painter.drawLine(QPointF(x - 12 * d->zoom, geometry.y - scroll), QPointF(x - 12 * d->zoom, geometry.y + geometry.height - scroll));
        }
        if (flow.kind == Content::FlowKind::Rule) {
            painter.setPen(QPen(d->appearance->color("border"), 1)); painter.drawLine(QPointF(x, geometry.y - scroll + 8 * d->zoom), QPointF(x + width, geometry.y - scroll + 8 * d->zoom));
        } else if (flow.kind == Content::FlowKind::Text) {
            if (geometry.count < 0) draw(index, flow.start, flow.start + flow.length, flow.start + flow.length, x, geometry.y, width, geometry.height);
            for (int part = 0; part < geometry.count; ++part) {
                const auto &chunk = d->chunks[geometry.first + part];
                draw(index, chunk.start, chunk.end, chunk.probeEnd, x, chunk.y, width, chunk.height);
            }
        } else {
            const qreal offset = d->offsetX(flow.owner);
            painter.save(); painter.setClipRect(QRectF(x, geometry.y - scroll, width, geometry.height));
            if (flow.kind == Content::FlowKind::CodeLine) {
                d->eachTile(index, flow.start, flow.start + flow.length, x + 16 * d->zoom - offset, geometry.y,
                    [&](int from, int to, qreal atX, qreal atY, qreal tileWidth) { draw(index, from, to, to, atX, atY, qMax(1.0, tileWidth + 1), geometry.height); });
            } else {
                qreal atX = x - offset; const auto &columns = d->groups[flow.owner].columns;
                for (int column = 0; column < flow.cellCount; ++column) {
                    const qreal cellWidth = columns[column]; const auto &cell = content.cells[flow.firstCell + column];
                    if (atX + cellWidth >= x && atX <= x + width) {
                        painter.setPen(QPen(d->appearance->color("border"), 1));
                        painter.setBrush(flow.header ? d->appearance->color("codeSurface") : d->appearance->color("background"));
                        painter.fillRect(QRectF(atX, geometry.y - scroll, cellWidth, geometry.height), flow.header ? d->appearance->color("codeSurface") : d->appearance->color("background"));
                        painter.drawLine(QPointF(atX, geometry.y - scroll + .5), QPointF(atX + cellWidth, geometry.y - scroll + .5));
                        painter.drawLine(QPointF(atX + .5, geometry.y - scroll), QPointF(atX + .5, geometry.y + geometry.height - scroll));
                        if (column + 1 == flow.cellCount) painter.drawLine(QPointF(atX + cellWidth - .5, geometry.y - scroll), QPointF(atX + cellWidth - .5, geometry.y + geometry.height - scroll));
                        if (index + 1 == content.flows.size() || content.flows[index + 1].owner != flow.owner)
                            painter.drawLine(QPointF(atX, geometry.y + geometry.height - scroll - .5), QPointF(atX + cellWidth, geometry.y + geometry.height - scroll - .5));
                        const qreal contentX = d->cellX(index, column, atX);
                        painter.save(); painter.setClipRect(QRectF(atX + 1, geometry.y - scroll + 1, cellWidth - 2, geometry.height - 2), Qt::IntersectClip);
                        d->eachTile(index, cell.start, cell.start + cell.length, contentX, geometry.y + 6 * d->zoom,
                            [&](int from, int to, qreal partX, qreal partY, qreal tileWidth) { draw(index, from, to, to, partX, partY, qMax(1.0, tileWidth + 1), d->lineHeight(flow)); });
                        painter.restore();
                    }
                    atX += cellWidth;
                }
            }
            painter.restore();
        }
    }
    if (keyboardOutlineVisible()
        && d->focusedAction >= 0 && d->focusedAction < content.actions.size()) {
        const auto &action = content.actions[d->focusedAction]; QRect rect;
        if (action.copyCode) {
            const auto button = d->buttons.value(int(action.node)); if (button) rect = button->geometry().adjusted(-2, -2, 2, 2);
        } else {
            rect = characterRect(action.start).united(characterRect(qMax(action.start, action.start + action.length - 1)));
            rect.translate(-viewport()->mapToGlobal(QPoint())); rect.adjust(-2, -2, 2, 2);
        }
        if (rect.isValid()) { painter.setBrush(Qt::NoBrush); painter.setPen(QPen(d->appearance->accentColor(), 2)); painter.drawRoundedRect(rect, 3, 3); }
    }
}

void NativePreviewView::updateControls() {
    for (auto *button : d->buttons) button->hide();
    for (auto *bar : d->scrollbars) bar->hide();
    if (!d->current() || d->geometry.isEmpty()) return;
    const qreal scroll = verticalScrollBar()->value();
    for (int owner = 0; owner < d->groups.size(); ++owner) {
        const auto &group = d->groups[owner];
        if (group.firstFlow < 0 || group.y + group.height < scroll || group.y > scroll + viewport()->height()) continue;
        const qreal x = d->x(group.firstFlow), width = d->available(d->flow(group.firstFlow));
        const auto &record = d->content->owners[owner];
        if (record.code && group.y >= scroll - 32 * d->zoom && group.y <= scroll + viewport()->height()) {
            auto *button = d->buttons.value(int(record.node));
            if (!button) {
                button = new SharedButton(QStringLiteral("复制"), QStringLiteral("nativeCodeCopy"), d->appearance, viewport());
                button->setAccessibleName(QStringLiteral("复制代码")); button->setToolTip(QStringLiteral("复制代码"));
                button->setFocusPolicy(Qt::NoFocus); d->buttons.insert(int(record.node), button);
                connect(button, &QPushButton::clicked, this, [this, node = record.node, snapshot = d->shown] {
                    if (!d->current() || d->shown != snapshot) return;
                    for (int index = 0; index < d->content->actions.size(); ++index)
                        if (d->content->actions[index].node == node && d->content->actions[index].copyCode) { activateAction(index); break; }
                });
            }
            button->setFont(uiFont(qMax(1, qRound(14 * d->zoom)))); button->setFixedHeight(qRound(28 * d->zoom));
            button->setGeometry(qRound(x + width - 68 * d->zoom), qRound(group.y - scroll + 2 * d->zoom), qRound(60 * d->zoom), qRound(28 * d->zoom)); button->show();
        }
        if (group.width > width && group.y + group.height - 12 >= scroll && group.y + group.height <= scroll + viewport()->height() + 12) {
            auto *bar = d->scrollbars.value(owner);
            if (!bar) {
                bar = new QScrollBar(Qt::Horizontal, viewport()); bar->setAccessibleName(record.code ? QStringLiteral("代码横向滚动") : QStringLiteral("表格横向滚动"));
                d->scrollbars.insert(owner, bar);
                connect(bar, &QScrollBar::valueChanged, this, [this, owner](int value) { d->horizontal[owner] = value; viewport()->update(); });
            }
            bar->setGeometry(qRound(x), qRound(group.y + group.height - scroll - 12), qRound(width), 12);
            bar->setRange(0, qMax(0, qCeil(group.width - width))); bar->setPageStep(qRound(width)); bar->setValue(qRound(d->horizontal.value(owner))); bar->show();
        }
    }
    // Only visible controls survive. These objects never accumulate while a
    // long document is toured; selection and logical actions live elsewhere.
    for (auto it = d->buttons.begin(); it != d->buttons.end();) {
        if (it.value()->isHidden()) { it.value()->setEnabled(false); it.value()->deleteLater(); it = d->buttons.erase(it); } else ++it;
    }
    for (auto it = d->scrollbars.begin(); it != d->scrollbars.end();) {
        if (it.value()->isHidden()) { it.value()->setEnabled(false); it.value()->deleteLater(); it = d->scrollbars.erase(it); } else ++it;
    }
}

QString NativePreviewView::selectedText() const {
    return d->content ? d->content->text.mid(qMin(d->anchor, d->cursor), qAbs(d->anchor - d->cursor)) : QString();
}
void NativePreviewView::setSelection(int anchor, int cursor) {
    if (!d->content || !boundary(d->content->text, anchor) || !boundary(d->content->text, cursor)) return;
    if (anchor == d->anchor && cursor == d->cursor) return;
    d->anchor = anchor; d->cursor = cursor;
    const QPointer<NativePreviewView> alive(this); const auto snapshot = d->shown;
    emit selectionChanged(); if (!alive || d->shown != snapshot) return;
    viewport()->update();
    QAccessibleTextSelectionEvent event(this, qMin(anchor, cursor), qMax(anchor, cursor)); QAccessible::updateAccessibility(&event);
}
QPair<int, int> NativePreviewView::selection() const { return {d->anchor, d->cursor}; }
void NativePreviewView::copy() { if (d->current() && d->anchor != d->cursor) emit copyTextRequested(selectedText()); }
void NativePreviewView::selectAll() { if (d->content) setSelection(0, int(d->content->text.size())); }

bool NativePreviewView::activateAction(int index) {
    d->keyboardOutline = false; InputModality::forApplication()->clear(); viewport()->update();
    if (!d->current() || index < 0 || index >= d->content->actions.size()) return false;
    const auto action = d->content->actions[index];
    if (action.copyCode) { emit copyTextRequested(d->content->text.mid(action.start, action.length)); return true; }
    if (!d->content->model.referencesComplete()) { emit navigationMessage(QStringLiteral("引用分析不完整，暂不能打开链接。")); return false; }
    const auto node = d->content->model.node(action.node); const auto target = decodeUtf8Literal(d->content->model.text(node));
    if (target.startsWith('#')) {
        const auto bytes = target.toUtf8(); const QByteArray document("document.md");
        auto buffer = xdnote_workspace_resolve(reinterpret_cast<const unsigned char *>(document.constData()), std::size_t(document.size()), reinterpret_cast<const unsigned char *>(bytes.constData()), std::size_t(bytes.size()));
        const auto object = buffer.status == 0 ? QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(buffer.data), qsizetype(buffer.length))).object() : QJsonObject();
        xdnote_markdown_free(&buffer);
        if (object.value("ok").toBool()) return navigateToHeading(object.value("fragment").toString(), d->shown);
    } else if (d->localDocumentsAdmitted && !target.isEmpty() && !target.startsWith('/') && !target.contains(':') && !target.contains('\\')) {
        const auto snapshot = d->shown; emit documentLinkRequested(target, snapshot); return true;
    }
    emit navigationMessage(QStringLiteral("此链接不属于当前允许的本地文档导航。")); return false;
}
bool NativePreviewView::navigateToHeading(const QString &fragment, const NativePreviewStamp &stamp) {
    if (!d->current() || stamp != d->shown) return false;
    const QString requested = fragment; const auto encoded = requested.toUtf8();
    for (const auto &heading : d->content->headings)
        if (d->content->model.text(d->content->model.node(heading.node)) == QByteArrayView(encoded)) {
            scrollToText(heading.position); emit fragmentNavigated(requested); return true;
        }
    emit navigationMessage(QStringLiteral("当前版本没有找到此标题。")); return false;
}

QRect NativePreviewView::characterRect(int offset) {
    if (!d->content || d->geometry.isEmpty() || !boundary(d->content->text, offset)) return {};
    const int index = d->flowAt(offset); const auto &flow = d->flow(index); const auto &geometry = d->geometry[index];
    QRectF result; const qreal x = d->x(index), scroll = verticalScrollBar()->value();
    if (flow.kind == Content::FlowKind::Text) {
        if (geometry.count < 0) {
            auto *doc = d->cachedDocument(index, flow.start, flow.start + flow.length, flow.start + flow.length, d->available(flow));
            result = d->positionRect(doc, qMin(offset, flow.start + flow.length), x, geometry.y - scroll);
        }
        const Chunk *found = nullptr;
        for (int part = 0; part < geometry.count; ++part) {
            const auto &chunk = d->chunks[geometry.first + part];
            if (offset >= chunk.start && offset <= chunk.end) { found = &chunk; if (offset < chunk.end) break; }
        }
        if (found) {
            auto *doc = d->cachedDocument(index, found->start, found->probeEnd, found->end, d->available(flow));
            result = d->positionRect(doc, qMin(offset, found->end), x, found->y - scroll);
        }
    } else if (flow.kind == Content::FlowKind::CodeLine || flow.kind == Content::FlowKind::TableRow) {
        int start = flow.start, end = flow.start + flow.length;
        qreal atX = x + 16 * d->zoom - d->offsetX(flow.owner), atY = geometry.y;
        if (flow.kind == Content::FlowKind::TableRow) {
            atX = x - d->offsetX(flow.owner); atY += 6 * d->zoom;
            for (int column = 0; column < flow.cellCount; ++column) {
                const auto &cell = d->content->cells[flow.firstCell + column];
                if (offset >= cell.start && offset <= cell.start + cell.length) { start = cell.start; end = cell.start + cell.length; atX = d->cellX(index, column, atX); break; }
                atX += d->groups[flow.owner].columns[column];
            }
        }
        d->eachTile(index, start, end, atX, atY, [&](int from, int to, qreal tileX, qreal tileY, qreal width) {
            if (offset >= from && offset <= to && result.isEmpty()) {
                auto *doc = d->cachedDocument(index, from, to, to, qMax(1.0, width + 1));
                result = d->positionRect(doc, offset, tileX, tileY - scroll);
            }
        });
    }
    if (!result.isValid()) result = QRectF(x, geometry.y - scroll, 1, d->lineHeight(flow));
    auto rect = result.toAlignedRect(); rect.translate(viewport()->mapToGlobal(QPoint())); return rect;
}

int NativePreviewView::offsetAtPoint(const QPoint &point) {
    if (!d->content || d->geometry.isEmpty()) return 0;
    const qreal y = point.y() + verticalScrollBar()->value(); const int index = d->flowAtY(y);
    const auto &flow = d->flow(index); const auto &geometry = d->geometry[index]; const qreal x = d->x(index);
    if (flow.kind == Content::FlowKind::Text) {
        if (geometry.count < 0) {
            auto *doc = d->cachedDocument(index, flow.start, flow.start + flow.length, flow.start + flow.length, d->available(flow));
            const int local = doc->documentLayout()->hitTest(QPointF(point.x() - x, y - geometry.y), Qt::FuzzyHit);
            return qBound(flow.start, doc->global(qMax(0, local)), flow.start + flow.length);
        }
        for (int part = 0; part < geometry.count; ++part) {
            const auto &chunk = d->chunks[geometry.first + part];
            if (y <= chunk.y + chunk.height || part + 1 == geometry.count) {
                auto *doc = d->cachedDocument(index, chunk.start, chunk.probeEnd, chunk.end, d->available(flow));
                const int local = doc->documentLayout()->hitTest(QPointF(point.x() - x, y - chunk.y), Qt::FuzzyHit);
                return qBound(chunk.start, doc->global(qMax(0, local)), chunk.end);
            }
        }
    } else {
        int start = flow.start, end = flow.start + flow.length;
        qreal atX = x + 16 * d->zoom - d->offsetX(flow.owner), atY = geometry.y;
        if (flow.kind == Content::FlowKind::TableRow) {
            atX = x - d->offsetX(flow.owner); atY += 6 * d->zoom;
            for (int column = 0; column < flow.cellCount; ++column) {
                const auto &cell = d->content->cells[flow.firstCell + column]; const auto width = d->groups[flow.owner].columns[column];
                if (point.x() <= atX + width || column + 1 == flow.cellCount) { start = cell.start; end = cell.start + cell.length; atX = d->cellX(index, column, atX); break; }
                atX += width;
            }
        }
        int result = end; bool found = false;
        d->eachTile(index, start, end, atX, atY, [&](int from, int to, qreal tileX, qreal tileY, qreal width) {
            if (!found && point.x() <= tileX + width && y >= tileY && y <= tileY + d->lineHeight(flow)) {
                auto *doc = d->cachedDocument(index, from, to, to, qMax(1.0, width + 1));
                result = doc->global(qMax(0, doc->documentLayout()->hitTest(QPointF(point.x() - tileX, y - tileY), Qt::FuzzyHit))); found = true;
            }
        });
        return result;
    }
    return flow.start;
}
void NativePreviewView::scrollToText(int offset) {
    if (!d->content || d->geometry.isEmpty() || !boundary(d->content->text, offset)) return;
    const auto rectangle = characterRect(offset); const int relative = rectangle.y() - viewport()->mapToGlobal(QPoint()).y();
    verticalScrollBar()->setValue(verticalScrollBar()->value() + relative);
}
QPointF NativePreviewView::readingPosition() const { return {0, qreal(verticalScrollBar()->value())}; }
void NativePreviewView::restoreReadingPosition(const QPointF &position) { verticalScrollBar()->setValue(qRound(position.y())); }
void NativePreviewView::resizeEvent(QResizeEvent *event) { QAbstractScrollArea::resizeEvent(event); rebuild(true); }
void NativePreviewView::scrollContentsBy(int, int) { updateControls(); viewport()->update(); }

void NativePreviewView::mousePressEvent(QMouseEvent *event) {
    d->keyboardOutline = false; InputModality::forApplication()->clear();
    if (event->button() != Qt::LeftButton || !d->current()) { QAbstractScrollArea::mousePressEvent(event); return; }
    setFocus(Qt::MouseFocusReason); const int offset = offsetAtPoint(event->position().toPoint());
    d->pressedAction = actionAtPoint(event->position().toPoint());
    d->dragging = true;
    setSelection(event->modifiers() & Qt::ShiftModifier ? d->anchor : offset, offset); event->accept();
}
void NativePreviewView::mouseMoveEvent(QMouseEvent *event) {
    if (!d->current()) return;
    if (d->dragging && (event->buttons() & Qt::LeftButton)) {
        if (event->position().y() < 0) verticalScrollBar()->setValue(verticalScrollBar()->value() - verticalScrollBar()->singleStep());
        else if (event->position().y() > viewport()->height()) verticalScrollBar()->setValue(verticalScrollBar()->value() + verticalScrollBar()->singleStep());
        setSelection(d->anchor, offsetAtPoint(event->position().toPoint()));
    } else {
        viewport()->setCursor(actionAtPoint(event->position().toPoint()) >= 0 ? Qt::PointingHandCursor : Qt::IBeamCursor);
    }
}
void NativePreviewView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        const int action = d->pressedAction; d->dragging = false; d->pressedAction = -1;
        if (action >= 0 && d->anchor == d->cursor && d->current()) activateAction(action);
    }
    event->accept();
}
void NativePreviewView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (!d->current()) return;
    const int offset = offsetAtPoint(event->position().toPoint());
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, d->content->text); finder.setPosition(offset);
    const int start = finder.toPreviousBoundary(); finder.setPosition(offset); const int end = finder.toNextBoundary();
    d->pressedAction = -1; setSelection(qMax(0, start), end < 0 ? int(d->content->text.size()) : end);
}
int NativePreviewView::actionAtPoint(const QPoint &point) {
    if (!d->current()) return -1;
    const int offset = offsetAtPoint(point);
    auto it = std::upper_bound(d->content->actions.cbegin(), d->content->actions.cend(), offset,
        [](int at, const Content::Action &action) { return at < action.start; });
    const auto global = viewport()->mapToGlobal(point);
    while (it != d->content->actions.cbegin()) {
        --it;
        if (it->start + it->length < offset) break;
        if (it->copyCode || it->length == 0) continue;
        const int at = qBound(it->start, offset, it->start + it->length - 1);
        if (boundary(d->content->text, at) && characterRect(at).contains(global)) return int(it - d->content->actions.cbegin());
        int previous = qMin(offset - 1, it->start + it->length - 1);
        if (previous > it->start && !boundary(d->content->text, previous)) --previous;
        if (previous >= it->start && characterRect(previous).contains(global)) return int(it - d->content->actions.cbegin());
    }
    return -1;
}
bool NativePreviewView::focusNextPrevChild(bool next) {
    if (!d->current() || d->content->actions.isEmpty()) return QAbstractScrollArea::focusNextPrevChild(next);
    const int target = d->focusedAction < 0 ? (next ? 0 : int(d->content->actions.size()) - 1) : d->focusedAction + (next ? 1 : -1);
    if (target < 0 || target >= d->content->actions.size()) { d->focusedAction = -1; d->keyboardOutline = false; return QAbstractScrollArea::focusNextPrevChild(next); }
    d->focusedAction = target; d->keyboardOutline = InputModality::forApplication()->keyboardNavigation();
    const auto &action = d->content->actions[target];
    if (action.copyCode) {
        for (int owner = 0; owner < d->content->owners.size(); ++owner) if (d->content->owners[owner].node == action.node) {
            verticalScrollBar()->setValue(qRound(d->groups[owner].y)); break;
        }
    } else scrollToText(action.start);
    setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason); viewport()->update(); return true;
}
void NativePreviewView::keyPressEvent(QKeyEvent *event) {
    if (!d->current()) { QAbstractScrollArea::keyPressEvent(event); return; }
    if (event->matches(QKeySequence::Copy)) { copy(); event->accept(); return; }
    if (event->matches(QKeySequence::SelectAll)) { selectAll(); event->accept(); return; }
    if (event->key() == Qt::Key_Escape) { d->keyboardOutline = false; d->dragging = false; d->pressedAction = -1; InputModality::forApplication()->clear(); viewport()->update(); event->accept(); return; }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) && d->focusedAction >= 0) {
        activateAction(d->focusedAction); event->accept(); return;
    }
    const bool shift = event->modifiers() & Qt::ShiftModifier, control = event->modifiers() & Qt::ControlModifier;
    int position = d->cursor;
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        QTextBoundaryFinder finder(control ? QTextBoundaryFinder::Word : QTextBoundaryFinder::Grapheme, d->content->text); finder.setPosition(position);
        position = event->key() == Qt::Key_Left ? finder.toPreviousBoundary() : finder.toNextBoundary();
        position = qBound(0, position < 0 && event->key() == Qt::Key_Right ? int(d->content->text.size()) : position, int(d->content->text.size()));
    } else if (event->key() == Qt::Key_Home) position = control ? 0 : d->flow(d->flowAt(position)).start;
    else if (event->key() == Qt::Key_End) { const auto &flow = d->flow(d->flowAt(position)); position = control ? int(d->content->text.size()) : flow.start + flow.length; }
    else { QAbstractScrollArea::keyPressEvent(event); return; }
    const QPointer<NativePreviewView> alive(this);
    setSelection(shift ? d->anchor : position, position); if (alive) scrollToText(position); event->accept();
}
void NativePreviewView::focusOutEvent(QFocusEvent *event) { d->keyboardOutline = false; d->dragging = false; d->pressedAction = -1; viewport()->update(); QAbstractScrollArea::focusOutEvent(event); }
void NativePreviewView::wheelEvent(QWheelEvent *event) {
    d->keyboardOutline = false; InputModality::forApplication()->clear();
    if (d->current() && !d->geometry.isEmpty() && ((event->modifiers() & Qt::ShiftModifier) || event->angleDelta().x() != 0 || event->pixelDelta().x() != 0)) {
        const int index = d->flowAtY(event->position().y() + verticalScrollBar()->value()); const auto &flow = d->flow(index);
        if (flow.owner >= 0) {
            const qreal maximum = qMax(0.0, d->groups[flow.owner].width - d->available(flow));
            const qreal delta = !event->pixelDelta().isNull() ? event->pixelDelta().x() : event->angleDelta().x() != 0 ? event->angleDelta().x() / 2.0 : event->angleDelta().y() / 2.0;
            d->horizontal[flow.owner] = qBound(0.0, d->offsetX(flow.owner) - delta, maximum);
            updateControls(); viewport()->update(); event->accept(); return;
        }
    }
    QAbstractScrollArea::wheelEvent(event);
}
void NativePreviewView::contextMenuEvent(QContextMenuEvent *event) {
    const QPointer<NativePreviewView> alive(this);
    QPointer<SharedMenu> menu = new SharedMenu(d->appearance, this);
    auto *copy = menu->addAction(themedIcon("copy", d->appearance->color("text")), QStringLiteral("复制")); copy->setEnabled(d->anchor != d->cursor);
    connect(copy, &QAction::triggered, this, &NativePreviewView::copy);
    auto *all = menu->addAction(themedIcon("select-all", d->appearance->color("text")), QStringLiteral("全选"));
    connect(all, &QAction::triggered, this, &NativePreviewView::selectAll); menu->exec(event->globalPos());
    if (menu) menu->deleteLater();
    if (!alive) return;
    d->keyboardOutline = false; InputModality::forApplication()->clear(); viewport()->update();
}
}
