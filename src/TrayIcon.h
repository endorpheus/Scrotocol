#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>

// Minimal org.kde.StatusNotifierItem implementation over GDBus.
//
// GTK4 dropped GtkStatusIcon, and libappindicator links GTK3 -- mixing GTK3
// and GTK4 in one process risks symbol clashes, so this talks the
// StatusNotifierItem/StatusNotifierWatcher protocol directly. Works with any
// modern tray host (KDE, GNOME-shell extensions, waybar, or legacy XEmbed
// trays bridged via snixembed, which is what tint2 uses on this system).
class TrayIcon {
public:
    TrayIcon(const std::string &iconName, std::function<void()> onActivate,
              std::function<void()> onQuit);
    ~TrayIcon();

    TrayIcon(const TrayIcon &) = delete;
    TrayIcon &operator=(const TrayIcon &) = delete;

private:
    std::string icon_name_;
    std::function<void()> on_activate_;
    std::function<void()> on_quit_;

    GDBusConnection *connection_ = nullptr;
    guint registration_id_ = 0;
    guint bus_name_id_ = 0;
    std::string object_path_ = "/StatusNotifierItem";
    std::string bus_name_;
    GDBusNodeInfo *introspection_data_ = nullptr;

    static void onBusAcquired(GDBusConnection *connection, const char *name, gpointer userData);
    static void onNameAcquired(GDBusConnection *connection, const char *name, gpointer userData);
    static void onNameLost(GDBusConnection *connection, const char *name, gpointer userData);

    static void handleMethodCall(GDBusConnection *connection, const char *sender,
                                  const char *objectPath, const char *interfaceName,
                                  const char *methodName, GVariant *parameters,
                                  GDBusMethodInvocation *invocation, gpointer userData);
    static GVariant *handleGetProperty(GDBusConnection *connection, const char *sender,
                                        const char *objectPath, const char *interfaceName,
                                        const char *propertyName, GError **error,
                                        gpointer userData);

    void registerWithWatcher();
};
