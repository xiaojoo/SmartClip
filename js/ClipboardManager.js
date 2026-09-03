// ---- thin wrapper around the clipboardStore context property ----
// NOTE: this is a normal (non-library) JS module so it can see the
// clipboardStore context property of the QML scope that imports it.

function items(query) {
    return clipboardStore.items(query || "")
}

function copyItem(id) {
    clipboardStore.copyItem(id)
}
