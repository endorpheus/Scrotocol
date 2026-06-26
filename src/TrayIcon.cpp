#include "TrayIcon.h"

#include "Config.h"

#include <unistd.h>

namespace {

const char *kSniXml = R"xml(
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
    <property name="Menu" type="o" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
  </interface>
</node>
)xml";

const char *kMenuXml = R"xml(
<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg type="i" direction="in" name="parentId"/>
      <arg type="i" direction="in" name="recursionDepth"/>
      <arg type="as" direction="in" name="propertyNames"/>
      <arg type="u" direction="out" name="revision"/>
      <arg type="(ia{sv}av)" direction="out" name="layout"/>
    </method>
    <method name="GetGroupProperties">
      <arg type="ai" direction="in" name="ids"/>
      <arg type="as" direction="in" name="propertyNames"/>
      <arg type="a(ia{sv})" direction="out" name="properties"/>
    </method>
    <method name="Event">
      <arg type="i" direction="in" name="id"/>
      <arg type="s" direction="in" name="eventId"/>
      <arg type="v" direction="in" name="data"/>
      <arg type="u" direction="in" name="timestamp"/>
    </method>
    <method name="EventGroup">
      <arg type="a(isvu)" direction="in" name="events"/>
      <arg type="ai" direction="out" name="idErrors"/>
    </method>
    <method name="AboutToShow">
      <arg type="i" direction="in" name="id"/>
      <arg type="b" direction="out" name="needUpdate"/>
    </method>
    <method name="AboutToShowGroup">
      <arg type="ai" direction="in" name="ids"/>
      <arg type="ai" direction="out" name="updatesNeeded"/>
      <arg type="ai" direction="out" name="idErrors"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg type="a(ia{sv})" name="updatedProps"/>
      <arg type="a(ias)" name="removedProps"/>
    </signal>
    <signal name="LayoutUpdated">
      <arg type="u" name="revision"/>
      <arg type="i" name="parent"/>
    </signal>
    <signal name="ItemActivationRequested">
      <arg type="i" name="id"/>
      <arg type="u" name="timestamp"/>
    </signal>
  </interface>
</node>
)xml";

// Returns a{sv} properties for a given menu item ID.
// Menu layout: 0=root, 1=Full Screen, 2=Region, 3=Window, 4=Timer,
//              5=separator, 6=Settings, 7=Show, 8=separator, 9=Quit
GVariant *propsForId(gint32 id) {
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

    if (id == 0) {
        g_variant_builder_add(&props, "{sv}", "children-display", g_variant_new_string("submenu"));
    } else if (id == 5 || id == 8) {
        g_variant_builder_add(&props, "{sv}", "type", g_variant_new_string("separator"));
        g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
    } else if (id == 4) {
        int delay = Config::instance().defaultDelaySeconds();
        g_autofree gchar *lbl = g_strdup_printf("Timer (%ds)", delay);
        g_variant_builder_add(&props, "{sv}", "label", g_variant_new_string(lbl));
        g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
    } else {
        static const struct { gint32 id; const char *label; } kLabels[] = {
            {1, "Full Screen"}, {2, "Region"}, {3, "Window"},
            {6, "Settings"},    {7, "Show"},   {9, "Quit"},
        };
        for (const auto &e : kLabels) {
            if (e.id == id) {
                g_variant_builder_add(&props, "{sv}", "label", g_variant_new_string(e.label));
                g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(TRUE));
                g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
                break;
            }
        }
    }
    return g_variant_builder_end(&props);
}

// Build a single menu item as (ia{sv}av).
GVariant *makeMenuItem(gint32 id) {
    GVariantBuilder kids;
    g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
    return g_variant_new("(i@a{sv}@av)", id, propsForId(id), g_variant_builder_end(&kids));
}

} // namespace

TrayIcon::TrayIcon(const std::string &iconName, std::function<void(TrayAction)> onAction)
    : icon_name_(iconName), on_action_(std::move(onAction)) {
    bus_name_ = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";

    bus_name_id_ =
        g_bus_own_name(G_BUS_TYPE_SESSION, bus_name_.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
                        onBusAcquired, onNameAcquired, onNameLost, this, nullptr);
}

TrayIcon::~TrayIcon() {
    if (menu_registration_id_ && connection_)
        g_dbus_connection_unregister_object(connection_, menu_registration_id_);
    if (registration_id_ && connection_)
        g_dbus_connection_unregister_object(connection_, registration_id_);
    if (bus_name_id_)
        g_bus_unown_name(bus_name_id_);
    if (introspection_data_)
        g_dbus_node_info_unref(introspection_data_);
    if (menu_introspection_data_)
        g_dbus_node_info_unref(menu_introspection_data_);
}

void TrayIcon::onBusAcquired(GDBusConnection *connection, const char *, gpointer userData) {
    auto *self = static_cast<TrayIcon *>(userData);
    self->connection_ = connection;

    GError *error = nullptr;

    self->introspection_data_ = g_dbus_node_info_new_for_xml(kSniXml, &error);
    if (!self->introspection_data_) {
        g_warning("Scrotocol: failed to parse SNI introspection XML: %s", error->message);
        g_clear_error(&error);
        return;
    }

    GDBusInterfaceInfo *sniIface =
        g_dbus_node_info_lookup_interface(self->introspection_data_, "org.kde.StatusNotifierItem");

    static const GDBusInterfaceVTable sniVtable = {handleMethodCall, handleGetProperty, nullptr,
                                                    {nullptr}};
    self->registration_id_ = g_dbus_connection_register_object(
        connection, self->object_path_.c_str(), sniIface, &sniVtable, self, nullptr, &error);
    if (self->registration_id_ == 0) {
        g_warning("Scrotocol: failed to register StatusNotifierItem object: %s", error->message);
        g_clear_error(&error);
    }

    self->menu_introspection_data_ = g_dbus_node_info_new_for_xml(kMenuXml, &error);
    if (!self->menu_introspection_data_) {
        g_warning("Scrotocol: failed to parse DBusMenu introspection XML: %s", error->message);
        g_clear_error(&error);
        return;
    }

    GDBusInterfaceInfo *menuIface = g_dbus_node_info_lookup_interface(
        self->menu_introspection_data_, "com.canonical.dbusmenu");

    static const GDBusInterfaceVTable menuVtable = {handleMenuMethodCall, handleMenuGetProperty,
                                                     nullptr, {nullptr}};
    self->menu_registration_id_ = g_dbus_connection_register_object(
        connection, self->menu_object_path_.c_str(), menuIface, &menuVtable, self, nullptr, &error);
    if (self->menu_registration_id_ == 0) {
        g_warning("Scrotocol: failed to register DBusMenu object: %s", error->message);
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
        if (self->on_action_)
            self->on_action_(TrayAction::Show);
    }
    // ContextMenu: tray host renders the DBusMenu.

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
    if (g_strcmp0(propertyName, "Menu") == 0)
        return g_variant_new_object_path(self->menu_object_path_.c_str());
    if (g_strcmp0(propertyName, "ItemIsMenu") == 0)
        return g_variant_new_boolean(FALSE);

    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property %s",
                propertyName);
    return nullptr;
}

GVariant *TrayIcon::buildMenuLayout() const {
    static const gint32 kOrder[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    for (gint32 id : kOrder)
        g_variant_builder_add(&children, "v", makeMenuItem(id));

    return g_variant_new("(i@a{sv}@av)", 0, propsForId(0), g_variant_builder_end(&children));
}

void TrayIcon::handleMenuMethodCall(GDBusConnection *, const char *, const char *, const char *,
                                     const char *methodName, GVariant *parameters,
                                     GDBusMethodInvocation *invocation, gpointer userData) {
    auto *self = static_cast<TrayIcon *>(userData);

    if (g_strcmp0(methodName, "GetLayout") == 0) {
        GVariant *layout = self->buildMenuLayout();
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(u@(ia{sv}av))", self->menu_revision_, layout));

    } else if (g_strcmp0(methodName, "GetGroupProperties") == 0) {
        GVariant *idsVariant = g_variant_get_child_value(parameters, 0);
        GVariantBuilder result;
        g_variant_builder_init(&result, G_VARIANT_TYPE("a(ia{sv})"));
        gsize n = g_variant_n_children(idsVariant);
        for (gsize i = 0; i < n; i++) {
            GVariant *idVal = g_variant_get_child_value(idsVariant, i);
            gint32 id = g_variant_get_int32(idVal);
            g_variant_unref(idVal);
            g_variant_builder_add(&result, "(i@a{sv})", id, propsForId(id));
        }
        g_variant_unref(idsVariant);
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&result)));

    } else if (g_strcmp0(methodName, "AboutToShow") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));

    } else if (g_strcmp0(methodName, "AboutToShowGroup") == 0) {
        GVariantBuilder empty1, empty2;
        g_variant_builder_init(&empty1, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&empty2, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@ai@ai)",
                                       g_variant_builder_end(&empty1),
                                       g_variant_builder_end(&empty2)));

    } else if (g_strcmp0(methodName, "EventGroup") == 0) {
        GVariant *eventsArr = g_variant_get_child_value(parameters, 0);
        gsize n = g_variant_n_children(eventsArr);
        for (gsize i = 0; i < n; i++) {
            GVariant *ev      = g_variant_get_child_value(eventsArr, i);
            GVariant *idVal   = g_variant_get_child_value(ev, 0);
            GVariant *evtVal  = g_variant_get_child_value(ev, 1);
            gint32 id         = g_variant_get_int32(idVal);
            const gchar *evtId = g_variant_get_string(evtVal, nullptr);
            if (g_strcmp0(evtId, "clicked") == 0 && self->on_action_) {
                switch (id) {
                    case 1: self->on_action_(TrayAction::FullScreen); break;
                    case 2: self->on_action_(TrayAction::Region);     break;
                    case 3: self->on_action_(TrayAction::Window);     break;
                    case 4: self->on_action_(TrayAction::Timer);      break;
                    case 6: self->on_action_(TrayAction::Settings);   break;
                    case 7: self->on_action_(TrayAction::Show);       break;
                    case 9: self->on_action_(TrayAction::Quit);       break;
                    default: break;
                }
            }
            g_variant_unref(evtVal);
            g_variant_unref(idVal);
            g_variant_unref(ev);
        }
        g_variant_unref(eventsArr);
        GVariantBuilder errors;
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@ai)", g_variant_builder_end(&errors)));

    } else if (g_strcmp0(methodName, "Event") == 0) {
        GVariant *idVal = g_variant_get_child_value(parameters, 0);
        GVariant *eventVal = g_variant_get_child_value(parameters, 1);
        gint32 id = g_variant_get_int32(idVal);
        const gchar *eventId = g_variant_get_string(eventVal, nullptr);

        if (g_strcmp0(eventId, "clicked") == 0 && self->on_action_) {
            switch (id) {
                case 1: self->on_action_(TrayAction::FullScreen); break;
                case 2: self->on_action_(TrayAction::Region);     break;
                case 3: self->on_action_(TrayAction::Window);     break;
                case 4: self->on_action_(TrayAction::Timer);      break;
                case 6: self->on_action_(TrayAction::Settings);   break;
                case 7: self->on_action_(TrayAction::Show);       break;
                case 9: self->on_action_(TrayAction::Quit);       break;
                default: break;
            }
        }

        g_variant_unref(idVal);
        g_variant_unref(eventVal);
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
}

GVariant *TrayIcon::handleMenuGetProperty(GDBusConnection *, const char *, const char *,
                                           const char *, const char *propertyName, GError **error,
                                           gpointer) {
    if (g_strcmp0(propertyName, "Version") == 0)
        return g_variant_new_uint32(3);
    if (g_strcmp0(propertyName, "TextDirection") == 0)
        return g_variant_new_string("ltr");
    if (g_strcmp0(propertyName, "Status") == 0)
        return g_variant_new_string("normal");
    if (g_strcmp0(propertyName, "IconThemePath") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        return g_variant_builder_end(&b);
    }

    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property %s",
                propertyName);
    return nullptr;
}
