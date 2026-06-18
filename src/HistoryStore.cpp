#include "HistoryStore.h"

#include "Config.h"

#include <glib/gstdio.h>

#include <algorithm>

std::vector<HistoryEntry> HistoryStore::list() const {
    std::vector<HistoryEntry> entries;

    const std::string &dir = Config::instance().historyDir();
    GError *error = nullptr;
    GDir *gdir = g_dir_open(dir.c_str(), 0, &error);
    if (!gdir) {
        g_clear_error(&error);
        return entries;
    }

    const char *name;
    while ((name = g_dir_read_name(gdir)) != nullptr) {
        if (!g_str_has_suffix(name, ".png") && !g_str_has_suffix(name, ".jpg"))
            continue;

        g_autofree gchar *fullPath = g_build_filename(dir.c_str(), name, nullptr);
        GStatBuf statBuf;
        if (g_stat(fullPath, &statBuf) != 0)
            continue;

        entries.push_back({std::string(fullPath), static_cast<int64_t>(statBuf.st_mtime)});
    }
    g_dir_close(gdir);

    std::sort(entries.begin(), entries.end(),
              [](const HistoryEntry &a, const HistoryEntry &b) { return a.mtime > b.mtime; });
    return entries;
}

std::string HistoryStore::saveCapture(GdkPixbuf *pixbuf) const {
    if (!pixbuf)
        return "";

    const std::string &dir = Config::instance().historyDir();
    g_mkdir_with_parents(dir.c_str(), 0755);

    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *stamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
    g_autofree gchar *filename = g_strdup_printf("Scrotocol_%s.png", stamp);
    g_autofree gchar *fullPath = g_build_filename(dir.c_str(), filename, nullptr);

    GError *error = nullptr;
    if (!gdk_pixbuf_save(pixbuf, fullPath, "png", &error, nullptr)) {
        g_clear_error(&error);
        return "";
    }

    return std::string(fullPath);
}

bool HistoryStore::remove(const std::string &path) const {
    return g_remove(path.c_str()) == 0;
}

GdkPixbuf *HistoryStore::loadThumbnail(const std::string &path, int maxSize) const {
    GError *error = nullptr;
    GdkPixbuf *pixbuf =
        gdk_pixbuf_new_from_file_at_scale(path.c_str(), maxSize, maxSize, TRUE, &error);
    if (!pixbuf)
        g_clear_error(&error);
    return pixbuf;
}
