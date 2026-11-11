#pragma once
#include "NativePreviewContent.h"
#include "SharedWidgets.h"
#include "UiFonts.h"
#include "VisualTokens.h"
#include <QFontMetricsF>
#include <QPainter>
#include <QtMath>
#include <cmath>

namespace xdnote {
inline QString previewImageLabel(const NativePreviewContent &content, const NativePreviewContent::Image &image) {
    auto text = content.text.mid(image.start, qMin(image.length, 256));
    const auto colon = text.indexOf(QChar(0xff1a));
    if (colon >= 0 && text.startsWith(QStringLiteral("图片"))) text = text.mid(colon + 1);
    return text.trimmed().isEmpty() ? QStringLiteral("图片") : text.trimmed();
}
inline QString previewImageReason(const NativePreviewContent::Image &image) {
    if (image.state == 2) return QStringLiteral("图片未获授权");
    if (image.state == 3) return QStringLiteral("图片超出资源限制");
    if (image.state == 4) return QStringLiteral("图片格式不支持或无法解码");
    if (image.state == 5) return QStringLiteral("图片不存在或不可读");
    return QStringLiteral("图片未加载");
}
inline bool previewBlockImage(const NativePreviewContent::Image &image, const NativePreviewContent::Flow &flow) {
    return flow.kind == NativePreviewContent::FlowKind::Text && flow.start == image.start && flow.length == image.length;
}
inline QSizeF previewPlaceholderSize(const NativePreviewContent &content, const NativePreviewContent::Image &image,
                                    const NativePreviewContent::Flow &flow, qreal room, qreal zoom) {
    const auto metrics = visualTokens()["typography"].toObject()["reader"].toObject();
    room = qMax(1.0, room);
    if (previewBlockImage(image, flow)) return {room, metrics["imageBlockHeight"].toInt() * zoom};
    const qreal label = QFontMetricsF(uiFont(qMax(1, qRound(visualTokens()["typography"].toObject()["reader"].toObject()["imageLabelSize"].toInt() * zoom)))).horizontalAdvance(previewImageLabel(content, image));
    return {qMin(room, qMin(metrics["imageInlineMaxWidth"].toInt() * zoom, label + metrics["imageLabelPadding"].toInt() * zoom)), metrics["imageInlineHeight"].toInt() * zoom};
}
inline QImage paintPreviewPlaceholder(const NativePreviewContent &content, const NativePreviewContent::Image &image,
                                     const NativePreviewContent::Flow &flow, qreal room, qreal zoom, qreal dpr,
                                     const QColor &surface, const QColor &border, const QColor &text) {
    const auto size = previewPlaceholderSize(content, image, flow, room, zoom);
    qreal ratio = dpr;
    if (size.width() * size.height() * ratio * ratio > 2'000'000)
        ratio = std::sqrt(2'000'000.0 / (size.width() * size.height()));
    QImage result(QSize(qMax(1, qCeil(size.width() * ratio)), qMax(1, qCeil(size.height() * ratio))), QImage::Format_ARGB32_Premultiplied);
    result.setDevicePixelRatio(ratio); result.fill(Qt::transparent);
    QPainter painter(&result); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(border, 1)); painter.setBrush(surface);
    painter.drawRoundedRect(QRectF(QPointF(), size).adjusted(.5, .5, -.5, -.5), 6 * zoom, 6 * zoom);
    const bool block = previewBlockImage(image, flow);
    const auto metrics = visualTokens()["typography"].toObject()["reader"].toObject();
    const int icon = qRound(metrics[block ? "imageBlockIconSize" : "imageInlineIconSize"].toInt() * zoom);
    themedIcon("image-off", text).paint(&painter, QRect(qRound(block ? (size.width() - icon) / 2 : 8 * zoom), qRound(block ? 18 * zoom : (size.height() - icon) / 2), icon, icon));
    painter.setFont(uiFont(qMax(1, qRound(metrics[block ? "imageBlockLabelSize" : "imageLabelSize"].toInt() * zoom)))); painter.setPen(text);
    const QRectF label = block ? QRectF(12 * zoom, 48 * zoom, qMax(1.0, size.width() - 24 * zoom), 24 * zoom)
                               : QRectF(28 * zoom, 0, qMax(1.0, size.width() - 36 * zoom), size.height());
    painter.drawText(label, Qt::AlignVCenter | (block ? Qt::AlignHCenter : Qt::AlignLeft), painter.fontMetrics().elidedText(previewImageLabel(content, image), Qt::ElideRight, qRound(label.width())));
    if (block) {
        painter.setFont(uiFont(qMax(1, qRound(visualTokens()["typography"].toObject()["reader"].toObject()["imageLabelSize"].toInt() * zoom))));
        painter.drawText(QRectF(12 * zoom, 76 * zoom, qMax(1.0, size.width() - 24 * zoom), 20 * zoom), Qt::AlignCenter,
                         painter.fontMetrics().elidedText(previewImageReason(image), Qt::ElideRight, qMax(1, qRound(size.width() - 24 * zoom))));
    }
    return result;
}
}
