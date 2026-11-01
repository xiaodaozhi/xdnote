#pragma once
#include <QObject>

namespace xdnote {
// One modality source for all shared controls in this application instance.
class InputModality final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool keyboardNavigation READ keyboardNavigation NOTIFY changed)
public:
    static InputModality *forApplication();
    bool keyboardNavigation() const { return m_keyboardNavigation; }
signals:
    void changed();
protected:
    bool eventFilter(QObject *target, QEvent *event) override;
private:
    explicit InputModality(QObject *parent);
    bool m_keyboardNavigation = false;
};
}
