#include <QCoreApplication>
#include <QSvgRenderer>
#include <QImage>
#include <QPainter>
#include <QDir>
#include <cstdio>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3 || !QDir().mkpath(app.arguments()[2])) return 1;
    QSvgRenderer svg(app.arguments()[1]); if (!svg.isValid()) return 2;
    for (int size : {44, 50, 150}) {
        QImage image(size, size, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        QPainter painter(&image); svg.render(&painter); painter.end();
        if (!image.save(app.arguments()[2] + QString("/logo%1.png").arg(size))) return 3;
    }
    return 0;
}
