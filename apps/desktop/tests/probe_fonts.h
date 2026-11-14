#pragma once
#include <QFontDatabase>
#include <QDebug>
#include <cstdio>

// Explicit fixture input for diagnostic executables only. The product entry
// point does not include this header, read this variable or bundle this font.
inline bool installProbeFont() {
    const auto path = qEnvironmentVariable("XDNOTE_TEST_FONT_FILE");
    if (path.isEmpty()) return true;
    const auto id = QFontDatabase::addApplicationFont(path);
    const auto families = QFontDatabase::applicationFontFamilies(id);
    if (id < 0 || families.isEmpty()) {
        std::fprintf(stderr, "Unable to load application-local diagnostic font\n");
        return false;
    }
    QFontDatabase::setApplicationFallbackFontFamilies(QChar::Script_Han, families);
    QFontDatabase::setApplicationFallbackFontFamilies(QChar::Script_Latin, families);
    qInfo() << "Application-local diagnostic font:" << families;
    return true;
}
