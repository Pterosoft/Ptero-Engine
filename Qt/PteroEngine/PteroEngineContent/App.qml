import QtQuick
import PteroEngine

Window {
    width: mainScreen.width
    height: mainScreen.height

    visible: true
    title: "PteroEngine"

    Screen01 {
        id: mainScreen

        anchors.centerIn: parent
    }

}

