#pragma once

#include <string>

// Persists user-configurable settings to $XDG_CONFIG_HOME/scrotocol/config.ini.
class Config {
public:
    static Config &instance();

    const std::string &historyDir() const { return history_dir_; }
    void setHistoryDir(const std::string &path);

    int defaultDelaySeconds() const { return default_delay_; }
    void setDefaultDelaySeconds(int seconds);

    bool minimizeBeforeCapture() const { return minimize_before_capture_; }
    void setMinimizeBeforeCapture(bool value);

    bool taskbarIconUseCapture() const { return taskbar_icon_use_capture_; }
    void setTaskbarIconUseCapture(bool value);

    int captureScale() const { return capture_scale_; }
    void setCaptureScale(int scale);

private:
    Config();
    void load();
    void save() const;
    std::string configFilePath() const;

    std::string history_dir_;
    int default_delay_ = 0;
    bool minimize_before_capture_  = true;
    bool taskbar_icon_use_capture_ = false;
    int  capture_scale_            = 1;
};
