pragma ComponentBehavior: Bound

import QtQuick

// Central icon generator: returns a tinted inline-SVG data URL for a glyph.
QtObject {
    id: root

    function svg(kind, color) {
        var c = color || "#9aa0a8"
        function p(d) { return '<path d="' + d + '" fill="none" stroke="' + c + '" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>' }
        function f(d) { return '<path d="' + d + '" fill="' + c + '"/>' }
        function ci(x, y, r) { return '<circle cx="' + x + '" cy="' + y + '" r="' + r + '" fill="' + c + '"/>' }
        var s = ""
        if (kind === "chevron-down")   s = p('M4.4 6.4 L8 10 L11.6 6.4')
        else if (kind === "chevron-right") s = p('M6.4 4.4 L10 8 L6.4 11.6')
        else if (kind === "chevron-left")  s = p('M9.6 4.4 L6 8 L9.6 11.6')
        else if (kind === "chevron-up")    s = p('M4.4 9.6 L8 6 L11.6 9.6')
        else if (kind === "play")          s = f('M5.8 3.4 L13.4 8 L5.8 12.6 Z')
        else if (kind === "close")         s = p('M4.6 4.6 L11.4 11.4 M11.4 4.6 L4.6 11.4')
        else if (kind === "plus")          s = p('M8 3.6 L8 12.4 M3.6 8 L12.4 8')
        else if (kind === "refresh")       s = p('M14.4 9.4 A6.4 6.4 0 1 1 12.9 4.6') + p('M15.2 3.4 L15.2 6.8 L11.7 6.3')
        else if (kind === "search")        s = '<circle cx="6.2" cy="6.2" r="4.1" fill="none" stroke="' + c + '" stroke-width="1.7"/>' + p('M9.4 9.4 L13.8 13.8')
        else if (kind === "gear")          s = p('M8 4.9 A3.1 3.1 0 1 0 8 11.1 A3.1 3.1 0 1 0 8 4.9') + (function(){ var t = ""; for (var a = 0; a < 360; a += 45) t += '<line x1="13.2" y1="8" x2="15.4" y2="8" stroke="' + c + '" stroke-width="1.6" stroke-linecap="round" transform="rotate(' + a + ' 8 8)"/>'; return t })()
        else if (kind === "more")          s = ci(8, 3.2, 1.5) + ci(8, 8, 1.5) + ci(8, 12.8, 1.5)
        else if (kind === "grid")          s = p('M3 3 H7 V7 H3 Z M9 3 H13 V7 H9 Z M3 9 H7 V13 H3 Z M9 9 H13 V13 H9 Z')
        else if (kind === "branch")        s = ci(5.2, 4.8, 1.7) + ci(5.2, 11.2, 1.7) + ci(11.6, 8, 1.7) + p('M5.2 6.5 L5.2 9.5') + p('M5.2 9.5 C5.2 12 11.6 10.4 11.6 8.4')
        else if (kind === "folder")        s = f('M2.4 4.4 A1.6 1.6 0 0 1 4 2.8 H6.2 L7.6 4.6 H11.8 A1.6 1.6 0 0 1 13.4 6.2 V11 A1.6 1.6 0 0 1 11.8 12.6 H4 A1.6 1.6 0 0 1 2.4 11 Z')
        else if (kind === "file")          s = p('M4 2.6 H8.9 L13.2 6.9 V12.8 A1.2 1.2 0 0 1 12 14 H4 A1.2 1.2 0 0 1 2.8 12.8 V3.8 A1.2 1.2 0 0 1 4 2.6 Z') + p('M8.9 2.6 V6.9 H13.2') + p('M5.8 10.1 H10.2 M5.8 12.1 H10.2')
        else if (kind === "image")         s = p('M2.6 3.6 H13.4 A1.2 1.2 0 0 1 14.6 4.8 V11.2 A1.2 1.2 0 0 1 13.4 12.4 H2.6 A1.2 1.2 0 0 1 1.4 11.2 V4.8 A1.2 1.2 0 0 1 2.6 3.6 Z') + ci(6, 7.1, 1.2) + p('M2 12.3 L6.3 8 L8.8 10.2 L11 8.4 L14 11.2')
        else if (kind === "trash")         s = p('M2.8 4.6 H13.2') + p('M5.3 4.6 V3.4 A1.2 1.2 0 0 1 6.5 2.2 H9.5 A1.2 1.2 0 0 1 10.7 3.4 V4.6') + p('M4.2 4.6 L4.9 12.9 A1.2 1.2 0 0 0 6.1 14 H9.9 A1.2 1.2 0 0 0 11.1 12.9 L11.8 4.6') + p('M6.6 7.6 V11.4 M9.4 7.6 V11.4')
        return "data:image/svg+xml;charset=utf-8," + encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">' + s + '</svg>')
    }
}
