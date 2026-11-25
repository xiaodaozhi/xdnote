#include "windows_uia_fixture.h"

#include <QFont>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QTextLayout>
#include <QtQuick/private/qquickflickable_p.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

namespace FixtureUia {
namespace {
struct Span {
    int start;
    int end;
};

struct Text {
    GuardedFixtureTextEdit &editor;
    QTextDocument &document;
    QAccessibleTextInterface &access;
    int length() const { return access.characterCount(); }
    QString value() const { return access.text(0, length()); }
};

struct Source {
    std::shared_ptr<Window> window;
    ComPtr<IRawElementProviderSimple> element;

    template<class Function> HRESULT read(Function function) const
    {
        return window->onGui([&](QQuickWindow &) -> HRESULT {
            const auto life = window->editor.lock();
            if (!life || !life->editor || !life->document || life->closing)
                return UIA_E_ELEMENTNOTAVAILABLE;
            if (life->busy)
                return UIA_E_INVALIDOPERATION;
            auto *accessible = QAccessible::queryAccessibleInterface(life->editor);
            auto *textAccess = accessible ? accessible->textInterface() : nullptr;
            if (!textAccess)
                return UIA_E_ELEMENTNOTAVAILABLE;
            return function(Text{*life->editor, *life->document, *textAccess}, life->document);
        });
    }
};

bool validEndpoint(TextPatternRangeEndpoint endpoint)
{
    return endpoint == TextPatternRangeEndpoint_Start || endpoint == TextPatternRangeEndpoint_End;
}

QList<int> boundaries(const Text &text, TextUnit unit)
{
    QList<int> result{0};
    const auto append = [&](qsizetype position) {
        if (position > result.last() && position <= text.length())
            result.append(int(position));
    };
    if (unit == TextUnit_Character || unit == TextUnit_Word) {
        const QString value = text.value();
        QTextBoundaryFinder finder(unit == TextUnit_Character ? QTextBoundaryFinder::Grapheme
                                                              : QTextBoundaryFinder::Word, value);
        for (qsizetype position = finder.toNextBoundary(); position != -1; position = finder.toNextBoundary()) {
            if (unit == TextUnit_Character || finder.boundaryReasons().testFlag(QTextBoundaryFinder::StartOfItem))
                append(position);
        }
    }
    if (unit == TextUnit_Line || unit == TextUnit_Paragraph) {
        for (auto block = text.document.begin(); block.isValid(); block = block.next()) {
            append(block.position());
            if (unit == TextUnit_Line) {
                const QTextLayout *layout = block.layout();
                for (int line = 0; layout && line < layout->lineCount(); ++line)
                    append(block.position() + layout->lineAt(line).textStart());
            }
        }
    }
    append(text.length());
    return result;
}

qsizetype unitAt(const QList<int> &points, int offset)
{
    const auto index = std::upper_bound(points.cbegin(), points.cend(), offset) - points.cbegin();
    return qMax<qsizetype>(0, index - 1);
}

QRectF caret(const Text &text, int position)
{
    return text.editor.mapRectToScene(text.editor.positionToRectangle(position));
}

QRectF viewport(const Text &text)
{
    const auto *window = text.editor.window();
    if (!window)
        return {};
    QRectF area(0, 0, window->width(), window->height());
    area = area.intersected(text.editor.mapRectToScene(text.editor.boundingRect()));
    for (auto *parent = text.editor.parentItem(); parent; parent = parent->parentItem()) {
        if (parent->clip())
            area = area.intersected(parent->mapRectToScene(parent->boundingRect()));
    }
    return area;
}

QRectF characterRect(const Text &text, int start, int end)
{
    const QRectF first = caret(text, start);
    const QRectF last = caret(text, end);
    if (qFuzzyCompare(first.top() + 1, last.top() + 1))
        return first.united(last);
    return first;
}

HRESULT copyArray(SAFEARRAY *array, SAFEARRAY **result)
{
    if (!array) return E_OUTOFMEMORY;
    *result = array;
    return S_OK;
}

HRESULT rangeArray(ITextRangeProvider *range, SAFEARRAY **result)
{
    auto *array = SafeArrayCreateVector(VT_UNKNOWN, 0, range ? 1 : 0);
    if (!array) return E_OUTOFMEMORY;
    if (range) {
        LONG index = 0;
        const HRESULT status = SafeArrayPutElement(array, &index, range);
        if (FAILED(status)) {
            SafeArrayDestroy(array);
            return status;
        }
    }
    return copyArray(array, result);
}

// Peers are checked by COM identity and document before using their endpoints.
struct __declspec(uuid("057D36EA-A6B6-46F8-A7B0-05E7EF5D105B")) RangeIdentity : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE ReadSpan(const Window *window, const QTextDocument *document, Span *span) = 0;
};

class Range final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ITextRangeProvider, RangeIdentity>
{
public:
    Range(Source source, std::weak_ptr<QTextDocument> document, Span span)
        : m_source(std::move(source)), m_document(std::move(document)), m_span(span) {}

    IFACEMETHODIMP ReadSpan(const Window *window, const QTextDocument *document, Span *span) override
    {
        if (!span || m_source.window.get() != window || m_document.lock().get() != document)
            return E_INVALIDARG;
        *span = m_span;
        return S_OK;
    }

    IFACEMETHODIMP Clone(ITextRangeProvider **result) override
    {
        if (!result) return E_INVALIDARG;
        *result = nullptr;
        return read([&](const Text &) { return makeRange(m_span, result); });
    }

    IFACEMETHODIMP Compare(ITextRangeProvider *other, BOOL *result) override
    {
        if (!result) return E_INVALIDARG;
        *result = FALSE;
        return read([&](const Text &text) -> HRESULT {
            Span span{};
            const HRESULT status = peer(other, text, &span);
            if (SUCCEEDED(status)) *result = span.start == m_span.start && span.end == m_span.end;
            return status;
        });
    }

    IFACEMETHODIMP CompareEndpoints(TextPatternRangeEndpoint endpoint, ITextRangeProvider *other,
                                   TextPatternRangeEndpoint otherEndpoint, int *result) override
    {
        if (!result || !validEndpoint(endpoint) || !validEndpoint(otherEndpoint)) return E_INVALIDARG;
        *result = 0;
        return read([&](const Text &text) -> HRESULT {
            Span span{};
            const HRESULT status = peer(other, text, &span);
            if (SUCCEEDED(status)) {
                const int a = offset(m_span, endpoint);
                const int b = offset(span, otherEndpoint);
                *result = (a > b) - (a < b);
            }
            return status;
        });
    }

    IFACEMETHODIMP ExpandToEnclosingUnit(TextUnit unit) override
    {
        if (unit < TextUnit_Character || unit > TextUnit_Document) return E_INVALIDARG;
        return read([&](const Text &text) -> HRESULT {
            const auto points = boundaries(text, unit);
            const auto index = qMin(unitAt(points, m_span.start), points.size() - 2);
            if (index >= 0) m_span = {points[index], points[index + 1]};
            return S_OK;
        });
    }

    IFACEMETHODIMP FindAttribute(TEXTATTRIBUTEID id, VARIANT expected, BOOL, ITextRangeProvider **result) override
    {
        if (!result) return E_INVALIDARG;
        *result = nullptr;
        return read([&](const Text &text) -> HRESULT {
            VARIANT actual;
            VariantInit(&actual);
            const HRESULT status = attribute(text, id, &actual);
            const bool matches = status == S_OK && VarCmp(&actual, &expected, LOCALE_INVARIANT, 0) == VARCMP_EQ;
            VariantClear(&actual);
            if (FAILED(status)) return status;
            return matches ? makeRange(m_span, result) : S_OK;
        });
    }

    IFACEMETHODIMP FindText(BSTR value, BOOL backward, BOOL ignoreCase, ITextRangeProvider **result) override
    {
        if (!result || !value || !SysStringLen(value)) return E_INVALIDARG;
        *result = nullptr;
        return read([&](const Text &text) -> HRESULT {
            const QString needle = QString::fromWCharArray(value, SysStringLen(value));
            const QString source = text.access.text(m_span.start, m_span.end);
            const auto sensitivity = ignoreCase ? Qt::CaseInsensitive : Qt::CaseSensitive;
            const qsizetype index = backward ? source.lastIndexOf(needle, -1, sensitivity)
                                             : source.indexOf(needle, 0, sensitivity);
            return index < 0 ? S_OK : makeRange({m_span.start + int(index),
                                                 m_span.start + int(index + needle.size())}, result);
        });
    }

    IFACEMETHODIMP GetAttributeValue(TEXTATTRIBUTEID id, VARIANT *result) override
    {
        if (!result) return E_INVALIDARG;
        VariantInit(result);
        return read([&](const Text &text) -> HRESULT {
            const HRESULT status = attribute(text, id, result);
            if (status != S_FALSE) return status;
            result->vt = VT_UNKNOWN;
            return UiaGetReservedNotSupportedValue(&result->punkVal);
        });
    }

    IFACEMETHODIMP GetBoundingRectangles(SAFEARRAY **result) override
    {
        if (!result) return E_INVALIDARG;
        *result = nullptr;
        return read([&](const Text &text) -> HRESULT {
            QList<QRectF> rectangles;
            const QRectF clip = viewport(text);
            const auto points = boundaries(text, TextUnit_Character);
            qsizetype index = unitAt(points, m_span.start);
            for (; index + 1 < points.size() && points[index] < m_span.end; ++index) {
                const QRectF rect = characterRect(text, qMax(points[index], m_span.start),
                    qMin(points[index + 1], m_span.end)).intersected(clip);
                if (rect.isEmpty()) continue;
                if (!rectangles.isEmpty() && rectangles.last().intersects(rect))
                    rectangles.last() = rectangles.last().united(rect);
                else
                    rectangles.append(rect);
            }
            auto *array = SafeArrayCreateVector(VT_R8, 0, ULONG(rectangles.size() * 4));
            if (!array) return E_OUTOFMEMORY;
            LONG indexOut = 0;
            for (const QRectF &rect : rectangles) {
                const auto native = nativeRect(*m_source.window, rect);
                for (double number : {native.x(), native.y(), native.width(), native.height()}) {
                    const HRESULT status = SafeArrayPutElement(array, &indexOut, &number);
                    if (FAILED(status)) {
                        SafeArrayDestroy(array);
                        return status;
                    }
                    ++indexOut;
                }
            }
            return copyArray(array, result);
        });
    }

    IFACEMETHODIMP GetEnclosingElement(IRawElementProviderSimple **result) override
    {
        if (!result) return E_INVALIDARG;
        *result = nullptr;
        return read([&](const Text &) { return m_source.element.CopyTo(result); });
    }

    IFACEMETHODIMP GetText(int limit, BSTR *result) override
    {
        if (!result || limit < -1) return E_INVALIDARG;
        *result = nullptr;
        return read([&](const Text &text) -> HRESULT {
            QString value = text.access.text(m_span.start, m_span.end);
            if (limit >= 0 && value.size() > limit) {
                if (limit && value.at(limit).isLowSurrogate()) --limit;
                value.truncate(limit);
            }
            *result = toBstr(value);
            return *result ? S_OK : E_OUTOFMEMORY;
        });
    }

    IFACEMETHODIMP Move(TextUnit unit, int count, int *result) override
    {
        if (!result || unit < TextUnit_Character || unit > TextUnit_Document) return E_INVALIDARG;
        *result = 0;
        return read([&](const Text &text) -> HRESULT {
            if (count == 0) return S_OK;
            const auto points = boundaries(text, unit);
            const bool degenerate = m_span.start == m_span.end;
            const qsizetype last = points.size() - (degenerate ? 1 : 2);
            if (last < 0) return S_OK;
            const auto start = qMin(unitAt(points, m_span.start), last);
            const auto next = qBound<qint64>(0, qint64(start) + count, last);
            m_span = {points[next], points[degenerate ? next : next + 1]};
            *result = int(next - start);
            return S_OK;
        });
    }

    IFACEMETHODIMP MoveEndpointByUnit(TextPatternRangeEndpoint endpoint, TextUnit unit, int count, int *result) override
    {
        if (!result || !validEndpoint(endpoint) || unit < TextUnit_Character || unit > TextUnit_Document)
            return E_INVALIDARG;
        *result = 0;
        return read([&](const Text &text) -> HRESULT {
            if (count == 0) return S_OK;
            const auto points = boundaries(text, unit);
            const int current = offset(m_span, endpoint);
            auto origin = unitAt(points, current);
            // A backward move from inside a unit first reaches its leading boundary.
            if (count < 0 && points[origin] != current) ++origin;
            const auto target = qBound<qint64>(0, qint64(origin) + count, points.size() - 1);
            moveEndpoint(endpoint, points[target]);
            *result = int(target - origin);
            return S_OK;
        });
    }

    IFACEMETHODIMP MoveEndpointByRange(TextPatternRangeEndpoint endpoint, ITextRangeProvider *other,
                                      TextPatternRangeEndpoint otherEndpoint) override
    {
        if (!validEndpoint(endpoint) || !validEndpoint(otherEndpoint)) return E_INVALIDARG;
        return read([&](const Text &text) -> HRESULT {
            Span span{};
            const HRESULT status = peer(other, text, &span);
            if (SUCCEEDED(status)) moveEndpoint(endpoint, offset(span, otherEndpoint));
            return status;
        });
    }

    IFACEMETHODIMP Select() override
    {
        return read([&](const Text &text) -> HRESULT {
            if (text.editor.isInputMethodComposing()) return UIA_E_INVALIDOPERATION;
            text.access.setSelection(0, m_span.start, m_span.end);
            return S_OK;
        });
    }

    IFACEMETHODIMP AddToSelection() override
    {
        // The editor supports only one selection.
        return read([](const Text &) { return HRESULT(UIA_E_INVALIDOPERATION); });
    }

    IFACEMETHODIMP RemoveFromSelection() override
    {
        return read([](const Text &) { return HRESULT(UIA_E_INVALIDOPERATION); });
    }

    IFACEMETHODIMP ScrollIntoView(BOOL alignToTop) override
    {
        return read([&](const Text &text) -> HRESULT {
            for (auto *item = text.editor.parentItem(); item; item = item->parentItem()) {
                if (auto *flickable = qobject_cast<QQuickFlickable *>(item)) {
                    const auto position = alignToTop ? m_span.start : m_span.end;
                    const QRectF rect = text.editor.mapRectToItem(flickable->contentItem(),
                        text.editor.positionToRectangle(position));
                    flickable->setContentY(alignToTop ? rect.top() : rect.bottom() - flickable->height());
                    flickable->returnToBounds();
                    break;
                }
            }
            return S_OK;
        });
    }

    IFACEMETHODIMP GetChildren(SAFEARRAY **result) override
    {
        if (!result) return E_INVALIDARG;
        *result = nullptr;
        return read([&](const Text &) { return rangeArray(nullptr, result); });
    }

private:
    template<class Function> HRESULT read(Function function)
    {
        return m_source.read([&](const Text &text, const std::shared_ptr<QTextDocument> &document) -> HRESULT {
            if (m_document.lock() != document) return UIA_E_ELEMENTNOTAVAILABLE;
            m_span.start = qBound(0, m_span.start, text.length());
            m_span.end = qBound(m_span.start, m_span.end, text.length());
            return function(text);
        });
    }

    HRESULT makeRange(Span span, ITextRangeProvider **result) const
    {
        auto range = Make<Range>(m_source, m_document, span);
        return range ? range.CopyTo(result) : E_OUTOFMEMORY;
    }

    HRESULT peer(ITextRangeProvider *other, const Text &text, Span *span) const
    {
        ComPtr<RangeIdentity> identity;
        if (!other || FAILED(other->QueryInterface(IID_PPV_ARGS(&identity)))) return E_INVALIDARG;
        const HRESULT status = identity->ReadSpan(m_source.window.get(), &text.document, span);
        if (SUCCEEDED(status)) {
            span->start = qBound(0, span->start, text.length());
            span->end = qBound(span->start, span->end, text.length());
        }
        return status;
    }

    static int offset(Span span, TextPatternRangeEndpoint endpoint)
    {
        return endpoint == TextPatternRangeEndpoint_Start ? span.start : span.end;
    }

    void moveEndpoint(TextPatternRangeEndpoint endpoint, int position)
    {
        if (endpoint == TextPatternRangeEndpoint_Start)
            m_span = {position, qMax(position, m_span.end)};
        else
            m_span = {qMin(m_span.start, position), position};
    }

    static HRESULT attribute(const Text &text, TEXTATTRIBUTEID id, VARIANT *value)
    {
        switch (id) {
        case UIA_IsReadOnlyAttributeId: case UIA_IsHiddenAttributeId:
            value->vt = VT_BOOL;
            value->boolVal = (id == UIA_IsReadOnlyAttributeId ? text.editor.isReadOnly()
                                                            : !text.editor.isVisible()) ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;
        case UIA_FontNameAttributeId:
            value->vt = VT_BSTR;
            value->bstrVal = toBstr(text.editor.font().family());
            return value->bstrVal ? S_OK : E_OUTOFMEMORY;
        default:
            return S_FALSE;
        }
    }

    const Source m_source;
    const std::weak_ptr<QTextDocument> m_document;
    Span m_span; // Accessed on the GUI thread only.
};

class TextProvider final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ITextProvider>
{
public:
    explicit TextProvider(Source source) : m_source(std::move(source)) {}

    IFACEMETHODIMP GetSelection(SAFEARRAY **result) override
    {
        if (!result) return E_INVALIDARG;
        *result = nullptr;
        return m_source.read([&](const Text &text, const auto &document) -> HRESULT {
            Span span{text.access.cursorPosition(), text.access.cursorPosition()};
            if (text.access.selectionCount()) text.access.selection(0, &span.start, &span.end);
            auto range = Make<Range>(m_source, document, span);
            return range ? rangeArray(range.Get(), result) : E_OUTOFMEMORY;
        });
    }

    IFACEMETHODIMP GetVisibleRanges(SAFEARRAY **result) override
    {
        if (!result) return E_INVALIDARG;
        *result = nullptr;
        return m_source.read([&](const Text &text, const auto &document) -> HRESULT {
            const QRectF clip = viewport(text);
            const auto points = boundaries(text, TextUnit_Character);
            const int empty = text.length() + 1;
            Span span{empty, 0};
            for (qsizetype i = 1; i < points.size(); ++i) {
                if (!characterRect(text, points[i - 1], points[i]).intersects(clip)) continue;
                span.start = qMin(span.start, points[i - 1]);
                span.end = points[i];
            }
            if (span.start == empty) return rangeArray(nullptr, result);
            auto range = Make<Range>(m_source, document, span);
            return range ? rangeArray(range.Get(), result) : E_OUTOFMEMORY;
        });
    }

    IFACEMETHODIMP RangeFromChild(IRawElementProviderSimple *, ITextRangeProvider **result) override
    {
        if (!result) return E_INVALIDARG;
        *result = nullptr;
        return m_source.read([](const Text &, const auto &) { return E_INVALIDARG; });
    }

    IFACEMETHODIMP RangeFromPoint(UiaPoint point, ITextRangeProvider **result) override
    {
        if (!result || !std::isfinite(point.x) || !std::isfinite(point.y)) return E_INVALIDARG;
        *result = nullptr;
        return m_source.read([&](const Text &text, const auto &document) -> HRESULT {
            const auto &window = *m_source.window;
            const POINT origin = clientOrigin(window.handle);
            const qreal scale = window.window->devicePixelRatio();
            const QPointF scene((point.x - origin.x) / scale, (point.y - origin.y) / scale);
            const QPointF local = text.editor.mapFromScene(scene);
            const int position = text.editor.positionAt(local.x(), local.y());
            auto range = Make<Range>(m_source, document, Span{position, position});
            return range ? range.CopyTo(result) : E_OUTOFMEMORY;
        });
    }

    IFACEMETHODIMP get_DocumentRange(ITextRangeProvider **result) override
    {
        if (!result) return E_INVALIDARG;
        *result = nullptr;
        return m_source.read([&](const Text &text, const auto &document) -> HRESULT {
            auto range = Make<Range>(m_source, document, Span{0, text.length()});
            return range ? range.CopyTo(result) : E_OUTOFMEMORY;
        });
    }

    IFACEMETHODIMP get_SupportedTextSelection(SupportedTextSelection *result) override
    {
        if (!result) return E_INVALIDARG;
        *result = SupportedTextSelection_None;
        return m_source.read([&](const Text &, const auto &) {
            *result = SupportedTextSelection_Single;
            return S_OK;
        });
    }

private:
    const Source m_source;
};
}

HRESULT makeTextProvider(const std::shared_ptr<Window> &window, IRawElementProviderSimple *editor, IUnknown **result)
{
    auto provider = Make<TextProvider>(Source{window, editor});
    return provider ? provider.CopyTo(result) : E_OUTOFMEMORY;
}
}
