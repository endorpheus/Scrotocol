#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <string>
#include <vector>

struct HistoryEntry {
    std::string path;
    int64_t mtime = 0;
};

// Manages the on-disk screenshot history directory (see Config::historyDir).
class HistoryStore {
public:
    // Lists entries sorted newest-first by mtime.
    std::vector<HistoryEntry> list() const;

    // Writes `pixbuf` as a new timestamped PNG into the history directory.
    // Returns the path written, or empty string on failure.
    std::string saveCapture(GdkPixbuf *pixbuf) const;

    bool remove(const std::string &path) const;

    // Loads a scaled-down copy of the image at `path` for sidebar display.
    // Returns nullptr on failure. Caller owns the returned pixbuf.
    GdkPixbuf *loadThumbnail(const std::string &path, int maxSize) const;
};
