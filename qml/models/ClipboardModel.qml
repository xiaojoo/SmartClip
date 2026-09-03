pragma ComponentBehavior: Bound

import QtQuick
import "../../js/ClipboardManager.js" as Store

// Data model of the clipboard items (feed from the C++ clipboardStore).
QtObject {
    id: root

    property var entries: []
    signal changed()

    function reload(query) {
        entries = Store.items(query || "")
        changed()
    }
}
