#include "Config.h"

#include <algorithm>
#include <glib.h>
#include <glib/gstdio.h>

namespace {
constexpr const char *kGroup = "scrotocol";
constexpr const char *kKeyHistoryDir = "history_dir";
constexpr const char *kKeyDefaultDelay = "default_delay";
constexpr const char *kKeyMinimizeBeforeCapture = "minimize_before_capture";
constexpr const char *kKeyTaskbarIconUseCapture = "taskbar_icon_use_capture";
constexpr const char *kKeyCaptureScale          = "capture_scale";
} // namespace

Config &Config::instance() {
    static Config cfg;
    return cfg;
}

std::string Config::configFilePath() const {
    g_autofree gchar *dir = g_build_filename(g_get_user_config_dir(), "scrotocol", nullptr);
    g_mkdir_with_parents(dir, 0700);
    g_autofree gchar *path = g_build_filename(dir, "config.ini", nullptr);
    return std::string(path);
}

Config::Config() {
    g_autofree gchar *pictures = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));
    g_autofree gchar *fallback = g_build_filename(g_get_home_dir(), "Pictures", nullptr);
    g_autofree gchar *defaultHistory =
        g_build_filename(pictures ? pictures : fallback, "Scrotocol", nullptr);
    history_dir_ = defaultHistory;
    load();
    g_mkdir_with_parents(history_dir_.c_str(), 0755);
}

void Config::load() {
    g_autoptr(GKeyFile) keyfile = g_key_file_new();
    std::string path = configFilePath();
    GError *error = nullptr;
    if (!g_key_file_load_from_file(keyfile, path.c_str(), G_KEY_FILE_NONE, &error)) {
        g_clear_error(&error);
        return;
    }

    g_autofree gchar *dir =
        g_key_file_get_string(keyfile, kGroup, kKeyHistoryDir, nullptr);
    if (dir && *dir)
        history_dir_ = dir;

    GError *delayError = nullptr;
    int delay = g_key_file_get_integer(keyfile, kGroup, kKeyDefaultDelay, &delayError);
    if (!delayError)
        default_delay_ = delay;
    else
        g_clear_error(&delayError);

    GError *minimizeError = nullptr;
    gboolean minimize = g_key_file_get_boolean(keyfile, kGroup, kKeyMinimizeBeforeCapture, &minimizeError);
    if (!minimizeError)
        minimize_before_capture_ = minimize;
    else
        g_clear_error(&minimizeError);

    GError *taskbarIconError = nullptr;
    gboolean taskbarIcon = g_key_file_get_boolean(keyfile, kGroup, kKeyTaskbarIconUseCapture, &taskbarIconError);
    if (!taskbarIconError)
        taskbar_icon_use_capture_ = taskbarIcon;
    else
        g_clear_error(&taskbarIconError);

    GError *scaleError = nullptr;
    int scale = g_key_file_get_integer(keyfile, kGroup, kKeyCaptureScale, &scaleError);
    if (!scaleError)
        capture_scale_ = std::clamp(scale, 1, 4);
    else
        g_clear_error(&scaleError);
}

void Config::save() const {
    g_autoptr(GKeyFile) keyfile = g_key_file_new();
    g_key_file_set_string(keyfile, kGroup, kKeyHistoryDir, history_dir_.c_str());
    g_key_file_set_integer(keyfile, kGroup, kKeyDefaultDelay, default_delay_);
    g_key_file_set_boolean(keyfile, kGroup, kKeyMinimizeBeforeCapture, minimize_before_capture_);
    g_key_file_set_boolean(keyfile, kGroup, kKeyTaskbarIconUseCapture, taskbar_icon_use_capture_);
    g_key_file_set_integer(keyfile, kGroup, kKeyCaptureScale, capture_scale_);

    std::string path = configFilePath();
    g_autoptr(GError) error = nullptr;
    g_key_file_save_to_file(keyfile, path.c_str(), &error);
}

void Config::setHistoryDir(const std::string &path) {
    history_dir_ = path;
    g_mkdir_with_parents(history_dir_.c_str(), 0755);
    save();
}

void Config::setDefaultDelaySeconds(int seconds) {
    default_delay_ = seconds;
    save();
}

void Config::setMinimizeBeforeCapture(bool value) {
    minimize_before_capture_ = value;
    save();
}

void Config::setTaskbarIconUseCapture(bool value) {
    taskbar_icon_use_capture_ = value;
    save();
}

void Config::setCaptureScale(int scale) {
    capture_scale_ = std::clamp(scale, 1, 4);
    save();
}
