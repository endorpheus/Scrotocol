#include "TrayIcon.h"

#include <unistd.h>

namespace {

const char *kIntrospectionXml = R"xml(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <method name="Activate">
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
    </method>
    <method name="SecondaryActivate">
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
    </method>
    <method name="ContextMenu">
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
    </method>
    <method name="Scroll">
      <arg type="i" direction="in" name="delta"/>
      <arg type="s" direction="in" name="orientation"/>
    </method>
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
  </interface>
</node>
)xml";

} // namespace

TrayIcon::TrayIcon(const std::string &iconName, std::function<void()> onActivate,
                    std::function<void()> onQuit)
    : icon_name_(iconName), on_activate_(std::move(onActivate)), on_quit_(std::move(onQuit)) {
    bus_name_ = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";

    bus_name_id_ =
        g_bus_own_name(G_BUS_TYPE_SESSION, bus_name_.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
                        onBusAcquired, onNameAcquired, onNameLost, this, nullptr);
}

TrayIcon::~TrayIcon() {
    if (registration_id_ && connection_)
        g_dbus_connection_unregister_object(connection_, registration_id_);
    if (bus_name_id_)
        g_bus_unown_name(bus_name_id_);
    if (introspection_data_)
        g_dbus_node_info_unref(introspection_data_);
}

void TrayIcon::onBusAcquired(GDBusConnection *connection, const char *, gpointer userData) {
    auto *self = static_cast<TrayIcon *>(userData);
    self->connection_ = connection;

    GError *error = nullptr;
    self->introspection_data_ = g_dbus_node_info_new_for_xml(kIntrospectionXml, &error);
    if (!self->introspection_data_) {
        g_warning("Scrotocol: failed to parse SNI introspection XML: %s", error->message);
        g_clear_error(&error);
        return;
    }

    GDBusInterfaceInfo *ifaceInfo =
        g_dbus_node_info_lookup_interface(self->introspection_data_, "org.kde.StatusNotifierItem");

    static const GDBusInterfaceVTable vtable = {handleMethodCall, handleGetProperty, nullptr,
                                                 {nullptr}};

    self->registration_id_ = g_dbus_connection_register_object(
        connection, self->object_path_.c_str(), ifaceInfo, &vtable, self, nullptr, &error);
    if (self->registration_id_ == 0) {
        g_warning("Scrotocol: failed to register StatusNotifierItem object: %s", error->message);
        g_clear_error(&error);
    }
}

void TrayIcon::onNameAcquired(GDBusConnection *, const char *, gpointer userData) {
    auto *self = static_cast<TrayIcon *>(userData);
    self->registerWithWatcher();
}

void TrayIcon::onNameLost(GDBusConnection *, const char *, gpointer userData) {
    auto *self = static_cast<TrayIcon *>(userData);
    self->connection_ = nullptr;
}

void TrayIcon::registerWithWatcher() {
    if (!connection_)
        return;

    g_dbus_connection_call(connection_, "org.kde.StatusNotifierWatcher",
                            "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                            "RegisterStatusNotifierItem", g_variant_new("(s)", bus_name_.c_str()),
                            nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

void TrayIcon::handleMethodCall(GDBusConnection *, const char *, const char *, const char *,
                                 const char *methodName, GVariant *, GDBusMethodInvocation *invocation,
                                 gpointer userData) {
    auto *self = static_cast<TrayIcon *>(userData);

    if (g_strcmp0(methodName, "Activate") == 0 || g_strcmp0(methodName, "SecondaryActivate") == 0) {
        if (self->on_activate_)
            self->on_activate_();
    } else if (g_strcmp0(methodName, "ContextMenu") == 0) {
        if (self->on_quit_)
            self->on_quit_();
    } else if (g_strcmp0(methodName, "Scroll") == 0) {
        // no-op
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant *TrayIcon::handleGetProperty(GDBusConnection *, const char *, const char *, const char *,
                                       const char *propertyName, GError **error,
                                       gpointer userData) {
    auto *self = static_cast<TrayIcon *>(userData);

    if (g_strcmp0(propertyName, "Category") == 0)
        return g_variant_new_string("ApplicationStatus");
    if (g_strcmp0(propertyName, "Id") == 0)
        return g_variant_new_string("scrotocol");
    if (g_strcmp0(propertyName, "Title") == 0)
        return g_variant_new_string("Scrotocol");
    if (g_strcmp0(propertyName, "Status") == 0)
        return g_variant_new_string("Active");
    if (g_strcmp0(propertyName, "WindowId") == 0)
        return g_variant_new_int32(0);
    if (g_strcmp0(propertyName, "IconName") == 0)
        return g_variant_new_string(self->icon_name_.c_str());
    if (g_strcmp0(propertyName, "ItemIsMenu") == 0)
        return g_variant_new_boolean(FALSE);

    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property %s",
                propertyName);
    return nullptr;
}
