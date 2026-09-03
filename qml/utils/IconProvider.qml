pragma ComponentBehavior: Bound

import QtQuick

// Central registry for the built-in icon set (resources/icons/*.svg).
QtObject {
    id: root

    function path(kind) {
        return "qrc:/icons/" + kind + ".svg"
    }
}
