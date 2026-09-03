.pragma library

// ---- time helpers for clipboard entries ----

function periodFor(timestamp) {
    var then = new Date(timestamp), today = new Date()
    today.setHours(0, 0, 0, 0); then.setHours(0, 0, 0, 0)
    var days = Math.floor((today.getTime() - then.getTime()) / 86400000)
    if (days <= 0) return "today"
    if (days === 1) return "yesterday"
    if (days <= 7) return "week"
    return "older"
}

function periodLabel(key) {
    if (key === "today") return "今天"
    if (key === "yesterday") return "昨天"
    if (key === "week") return "近 7 天"
    return "更早"
}

function displayTime(timestamp) {
    return Qt.formatDateTime(new Date(timestamp), "yyyy-MM-dd HH:mm")
}

function itemKind(item) {
    return item.type === "image" ? "IMAGE" : "TEXT"
}
