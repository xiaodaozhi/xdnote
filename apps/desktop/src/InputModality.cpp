#include "InputModality.h"
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QPointer>

namespace xdnote {
InputModality::InputModality(QObject *parent) : QObject(parent) {
    QCoreApplication::instance()->installEventFilter(this);
}
InputModality *InputModality::forApplication() {
    static QPointer<InputModality> mode;
    if (!mode) mode = new InputModality(QCoreApplication::instance());
    return mode;
}
bool InputModality::eventFilter(QObject *target, QEvent *event) {
    bool next = m_keyboardNavigation;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::TouchBegin:
    case QEvent::Wheel:
    case QEvent::WindowDeactivate:
    case QEvent::ApplicationDeactivate:
        next = false;
        break;
    case QEvent::KeyPress: {
        const auto key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Tab || key == Qt::Key_Backtab) next = true;
        else if (key == Qt::Key_Escape) next = false;
        break;
    }
    default: break;
    }
    if (next != m_keyboardNavigation) { m_keyboardNavigation = next; emit changed(); }
    return QObject::eventFilter(target, event);
}
void InputModality::clear() { if (m_keyboardNavigation) { m_keyboardNavigation = false; emit changed(); } }
}
