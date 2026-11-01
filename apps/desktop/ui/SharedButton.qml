import QtQuick
import QtQuick.Controls

Button {
    id: control
    property bool keyboardOutline: false
    function updateOutline() {
        keyboardOutline = activeFocus && (focusReason === Qt.TabFocusReason || focusReason === Qt.BacktabFocusReason)
    }
    onActiveFocusChanged: updateOutline()
    onFocusReasonChanged: updateOutline()
    onPressed: keyboardOutline = false
    onCanceled: keyboardOutline = false
    onClicked: keyboardOutline = false
    focusPolicy: Qt.TabFocus
    implicitHeight: visual.controls.height
    implicitWidth: label.implicitWidth + visual.spacing[2] * 2
    Accessible.name: text
    contentItem: Text {
        id: label
        text: control.text
        color: visual.themes.light.text
        font.pixelSize: visual.typography.ui.size
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: visual.radius.control
        color: control.down ? visual.themes.light.border : control.hovered ? visual.accents.graphite.light.soft : "transparent"
        border.width: control.keyboardOutline ? visual.controls.focusRingWidth : 0
        border.color: visual.accents.graphite.base
    }
    Keys.onEscapePressed: keyboardOutline = false
}
