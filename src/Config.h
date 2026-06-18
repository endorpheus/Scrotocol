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

private:
    Config();
    void load();
    void save() const;
    std::string configFilePath() const;

    std::string history_dir_;
    int default_delay_ = 0;
};
