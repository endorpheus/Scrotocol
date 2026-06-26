#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>

enum class TrayAction { Show, FullScreen, Region, Window, Timer, Settings, Quit };

// Minimal org.kde.StatusNotifierItem + com.canonical.dbusmenu implementation over GDBus.
//
// GTK4 dropped GtkStatusIcon, and libappindicator links GTK3 -- mixing GTK3
// and GTK4 in one process risks symbol clashes, so this talks the
// StatusNotifierItem/StatusNotifierWatcher protocol directly. Works with any
// modern tray host (KDE, GNOME-shell extensions, waybar, or legacy XEmbed
// trays bridged via snixembed, which is what tint2 uses on this system).
class TrayIcon {
public:
    TrayIcon(const std::string &iconName, std::function<void(TrayAction)> onAction);
    ~TrayIcon();

    TrayIcon(const TrayIcon &) = delete;
    TrayIcon &operator=(const TrayIcon &) = delete;

private:
    std::string icon_name_;
    std::function<void(TrayAction)> on_action_;

    GDBusConnection *connection_ = nullptr;
    guint registration_id_ = 0;
    guint menu_registration_id_ = 0;
    guint bus_name_id_ = 0;
    guint32 menu_revision_ = 1;
    std::string object_path_ = "/StatusNotifierItem";
    std::string menu_object_path_ = "/MenuBar";
    std::string bus_name_;
    GDBusNodeInfo *introspection_data_ = nullptr;
    GDBusNodeInfo *menu_introspection_data_ = nullptr;

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

    static void handleMenuMethodCall(GDBusConnection *connection, const char *sender,
                                      const char *objectPath, const char *interfaceName,
                                      const char *methodName, GVariant *parameters,
                                      GDBusMethodInvocation *invocation, gpointer userData);
    static GVariant *handleMenuGetProperty(GDBusConnection *connection, const char *sender,
                                            const char *objectPath, const char *interfaceName,
                                            const char *propertyName, GError **error,
                                            gpointer userData);

    GVariant *buildMenuLayout() const;

    void registerWithWatcher();
};
