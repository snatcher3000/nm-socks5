/* SPDX-License-Identifier: MIT
 *
 * nm-socks5-editor.c - NetworkManager VPN editor plugin for SOCKS5 proxies.
 *
 * Implements both NMVpnEditorPlugin (registration/metadata, so the entry
 * shows up in nm-connection-editor's "Choose a Connection Type" list) and
 * NMVpnEditor (the actual GTK3 properties widget) in a single shared object.
 *
 * Connection data items used (NMSettingVpn):
 *   server      SOCKS5 server hostname or IPv4 address (required)
 *   port        TCP port, default 1080
 *   username    optional
 *   dns-through "yes"/"no" - push DNS through the tunnel (default "yes")
 *   dns-server  DNS server(s) pushed when dns-through=yes, default 1.1.1.1
 * Secrets:
 *   password    optional, stored system-wide (secret flags NONE)
 */

#include <gmodule.h>
#include <gtk/gtk.h>
#include <NetworkManager.h>
#include <stdlib.h>
#include <string.h>

#define SOCKS5_DBUS_SERVICE   "org.freedesktop.NetworkManager.socks5"
#define SOCKS5_PLUGIN_NAME    "SOCKS5 Proxy (tun2socks)"
#define SOCKS5_PLUGIN_DESC    "Route all traffic through a SOCKS5 proxy."

#define KEY_SERVER      "server"
#define KEY_PORT        "port"
#define KEY_USERNAME    "username"
#define KEY_PASSWORD    "password"
#define KEY_DNS_THROUGH "dns-through"
#define KEY_DNS_SERVER  "dns-server"

#define DEFAULT_PORT 1080
#define DEFAULT_DNS  "1.1.1.1"

/*****************************************************************************
 * Editor (the GTK widget shown in nm-connection-editor)
 *****************************************************************************/

typedef struct {
    GObject parent;

    GtkWidget *widget;         /* toplevel grid, owned */
    GtkWidget *entry_server;
    GtkWidget *spin_port;
    GtkWidget *entry_user;
    GtkWidget *entry_pass;
    GtkWidget *check_show_pass;
    GtkWidget *check_dns;
    GtkWidget *entry_dns;
} Socks5Editor;

typedef struct {
    GObjectClass parent;
} Socks5EditorClass;

static GType socks5_editor_get_type (void);
static void socks5_editor_interface_init (NMVpnEditorInterface *iface);

G_DEFINE_TYPE_EXTENDED (Socks5Editor, socks5_editor, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_EDITOR,
                                               socks5_editor_interface_init))

static void
stuff_changed_cb (GtkWidget *widget, gpointer user_data)
{
    (void) widget;
    g_signal_emit_by_name (user_data, "changed");
}

static void
show_password_toggled_cb (GtkToggleButton *button, gpointer user_data)
{
    Socks5Editor *self = (Socks5Editor *) user_data;

    gtk_entry_set_visibility (GTK_ENTRY (self->entry_pass),
                              gtk_toggle_button_get_active (button));
}

static void
dns_check_toggled_cb (GtkToggleButton *button, gpointer user_data)
{
    Socks5Editor *self = (Socks5Editor *) user_data;

    gtk_widget_set_sensitive (self->entry_dns,
                              gtk_toggle_button_get_active (button));
    g_signal_emit_by_name (self, "changed");
}

static GtkWidget *
add_row (GtkWidget *grid, int row, const char *label_text, GtkWidget *field)
{
    GtkWidget *label;

    label = gtk_label_new (label_text);
    gtk_widget_set_halign (label, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);

    gtk_widget_set_hexpand (field, TRUE);
    gtk_grid_attach (GTK_GRID (grid), field, 1, row, 1, 1);
    return field;
}

static void
init_from_connection (Socks5Editor *self, NMConnection *connection)
{
    NMSettingVpn *s_vpn = NULL;
    const char *value;
    long port = DEFAULT_PORT;
    gboolean dns_through = TRUE;
    const char *dns_server = DEFAULT_DNS;

    if (connection)
        s_vpn = nm_connection_get_setting_vpn (connection);

    if (s_vpn) {
        value = nm_setting_vpn_get_data_item (s_vpn, KEY_SERVER);
        if (value)
            gtk_entry_set_text (GTK_ENTRY (self->entry_server), value);

        value = nm_setting_vpn_get_data_item (s_vpn, KEY_PORT);
        if (value) {
            port = strtol (value, NULL, 10);
            if (port < 1 || port > 65535)
                port = DEFAULT_PORT;
        }

        value = nm_setting_vpn_get_data_item (s_vpn, KEY_USERNAME);
        if (value)
            gtk_entry_set_text (GTK_ENTRY (self->entry_user), value);

        value = nm_setting_vpn_get_secret (s_vpn, KEY_PASSWORD);
        if (value)
            gtk_entry_set_text (GTK_ENTRY (self->entry_pass), value);

        /* absent means "yes" so that DNS goes through the proxy by default */
        value = nm_setting_vpn_get_data_item (s_vpn, KEY_DNS_THROUGH);
        dns_through = !value || strcmp (value, "no") != 0;

        value = nm_setting_vpn_get_data_item (s_vpn, KEY_DNS_SERVER);
        if (value && *value)
            dns_server = value;
    }

    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->spin_port), (gdouble) port);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->check_dns), dns_through);
    gtk_entry_set_text (GTK_ENTRY (self->entry_dns), dns_server);
    gtk_widget_set_sensitive (self->entry_dns, dns_through);
}

static NMVpnEditor *
socks5_editor_new (NMConnection *connection, GError **error)
{
    Socks5Editor *self;
    GtkWidget *grid;

    (void) error;

    self = g_object_new (socks5_editor_get_type (), NULL);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    g_object_set (grid,
                  "margin-start", 12, "margin-end", 12,
                  "margin-top", 12, "margin-bottom", 12,
                  NULL);

    self->entry_server = add_row (grid, 0, "Server:", gtk_entry_new ());
    gtk_widget_set_tooltip_text (self->entry_server,
                                 "Hostname or IPv4 address of the SOCKS5 server");

    self->spin_port = add_row (grid, 1, "Port:",
                               gtk_spin_button_new_with_range (1, 65535, 1));
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->spin_port), DEFAULT_PORT);

    self->entry_user = add_row (grid, 2, "Username:", gtk_entry_new ());

    self->entry_pass = add_row (grid, 3, "Password:", gtk_entry_new ());
    gtk_entry_set_visibility (GTK_ENTRY (self->entry_pass), FALSE);
    gtk_entry_set_input_purpose (GTK_ENTRY (self->entry_pass),
                                 GTK_INPUT_PURPOSE_PASSWORD);

    self->check_show_pass = gtk_check_button_new_with_label ("Show password");
    gtk_grid_attach (GTK_GRID (grid), self->check_show_pass, 1, 4, 1, 1);

    self->check_dns = gtk_check_button_new_with_label ("Route DNS through the proxy");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->check_dns), TRUE);
    gtk_widget_set_tooltip_text (self->check_dns,
                                 "Push the DNS server below into the tunnel so DNS "
                                 "queries also go through the SOCKS5 proxy. Requires "
                                 "a proxy with UDP ASSOCIATE support.");
    gtk_grid_attach (GTK_GRID (grid), self->check_dns, 0, 5, 2, 1);

    self->entry_dns = add_row (grid, 6, "DNS server:", gtk_entry_new ());
    gtk_entry_set_text (GTK_ENTRY (self->entry_dns), DEFAULT_DNS);
    gtk_widget_set_tooltip_text (self->entry_dns,
                                 "One or more IPv4 addresses, comma separated");

    init_from_connection (self, connection);

    g_signal_connect (self->entry_server, "changed", G_CALLBACK (stuff_changed_cb), self);
    g_signal_connect (self->spin_port, "value-changed", G_CALLBACK (stuff_changed_cb), self);
    g_signal_connect (self->entry_user, "changed", G_CALLBACK (stuff_changed_cb), self);
    g_signal_connect (self->entry_pass, "changed", G_CALLBACK (stuff_changed_cb), self);
    g_signal_connect (self->entry_dns, "changed", G_CALLBACK (stuff_changed_cb), self);
    g_signal_connect (self->check_show_pass, "toggled",
                      G_CALLBACK (show_password_toggled_cb), self);
    g_signal_connect (self->check_dns, "toggled",
                      G_CALLBACK (dns_check_toggled_cb), self);

    gtk_widget_show_all (grid);
    self->widget = g_object_ref_sink (grid);

    return NM_VPN_EDITOR (self);
}

static GObject *
get_widget (NMVpnEditor *editor)
{
    return G_OBJECT (((Socks5Editor *) editor)->widget);
}

static gboolean
update_connection (NMVpnEditor *editor, NMConnection *connection, GError **error)
{
    Socks5Editor *self = (Socks5Editor *) editor;
    NMSettingVpn *s_vpn;
    const char *server, *username, *password, *dns_server;
    char *port_str;
    gboolean dns_through;

    server = gtk_entry_get_text (GTK_ENTRY (self->entry_server));
    if (!server || !*server) {
        g_set_error_literal (error,
                             NM_CONNECTION_ERROR,
                             NM_CONNECTION_ERROR_INVALID_PROPERTY,
                             "A SOCKS5 server address is required.");
        return FALSE;
    }

    s_vpn = NM_SETTING_VPN (nm_setting_vpn_new ());
    g_object_set (s_vpn, NM_SETTING_VPN_SERVICE_TYPE, SOCKS5_DBUS_SERVICE, NULL);

    nm_setting_vpn_add_data_item (s_vpn, KEY_SERVER, server);

    port_str = g_strdup_printf ("%d",
        gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (self->spin_port)));
    nm_setting_vpn_add_data_item (s_vpn, KEY_PORT, port_str);
    g_free (port_str);

    username = gtk_entry_get_text (GTK_ENTRY (self->entry_user));
    if (username && *username)
        nm_setting_vpn_add_data_item (s_vpn, KEY_USERNAME, username);

    password = gtk_entry_get_text (GTK_ENTRY (self->entry_pass));
    if (password && *password)
        nm_setting_vpn_add_secret (s_vpn, KEY_PASSWORD, password);
    /* store the password in the connection file (system scope) so activation
     * works headless without a secret agent / auth dialog */
    nm_setting_set_secret_flags (NM_SETTING (s_vpn), KEY_PASSWORD,
                                 NM_SETTING_SECRET_FLAG_NONE, NULL);

    dns_through = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->check_dns));
    nm_setting_vpn_add_data_item (s_vpn, KEY_DNS_THROUGH, dns_through ? "yes" : "no");
    dns_server = gtk_entry_get_text (GTK_ENTRY (self->entry_dns));
    if (dns_through && dns_server && *dns_server)
        nm_setting_vpn_add_data_item (s_vpn, KEY_DNS_SERVER, dns_server);

    nm_connection_add_setting (connection, NM_SETTING (s_vpn));
    return TRUE;
}

static void
socks5_editor_init (Socks5Editor *self)
{
    (void) self;
}

static void
socks5_editor_dispose (GObject *object)
{
    Socks5Editor *self = (Socks5Editor *) object;

    g_clear_object (&self->widget);
    G_OBJECT_CLASS (socks5_editor_parent_class)->dispose (object);
}

static void
socks5_editor_class_init (Socks5EditorClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = socks5_editor_dispose;
}

static void
socks5_editor_interface_init (NMVpnEditorInterface *iface)
{
    iface->get_widget = get_widget;
    iface->update_connection = update_connection;
}

/*****************************************************************************
 * Editor plugin (metadata + factory, what nm-connection-editor enumerates)
 *****************************************************************************/

typedef struct {
    GObject parent;
} Socks5EditorPlugin;

typedef struct {
    GObjectClass parent;
} Socks5EditorPluginClass;

enum {
    PROP_0,
    PROP_NAME,
    PROP_DESC,
    PROP_SERVICE
};

static GType socks5_editor_plugin_get_type (void);
static void socks5_editor_plugin_interface_init (NMVpnEditorPluginInterface *iface);

G_DEFINE_TYPE_EXTENDED (Socks5EditorPlugin, socks5_editor_plugin, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_EDITOR_PLUGIN,
                                               socks5_editor_plugin_interface_init))

static NMVpnEditor *
get_editor (NMVpnEditorPlugin *plugin, NMConnection *connection, GError **error)
{
    (void) plugin;
    return socks5_editor_new (connection, error);
}

static NMVpnEditorPluginCapability
get_capabilities (NMVpnEditorPlugin *plugin)
{
    (void) plugin;
    return NM_VPN_EDITOR_PLUGIN_CAPABILITY_NONE;
}

static void
plugin_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    switch (prop_id) {
    case PROP_NAME:
        g_value_set_string (value, SOCKS5_PLUGIN_NAME);
        break;
    case PROP_DESC:
        g_value_set_string (value, SOCKS5_PLUGIN_DESC);
        break;
    case PROP_SERVICE:
        g_value_set_string (value, SOCKS5_DBUS_SERVICE);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
socks5_editor_plugin_init (Socks5EditorPlugin *self)
{
    (void) self;
}

static void
socks5_editor_plugin_class_init (Socks5EditorPluginClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = plugin_get_property;
    g_object_class_override_property (object_class, PROP_NAME,
                                      NM_VPN_EDITOR_PLUGIN_NAME);
    g_object_class_override_property (object_class, PROP_DESC,
                                      NM_VPN_EDITOR_PLUGIN_DESCRIPTION);
    g_object_class_override_property (object_class, PROP_SERVICE,
                                      NM_VPN_EDITOR_PLUGIN_SERVICE);
}

static void
socks5_editor_plugin_interface_init (NMVpnEditorPluginInterface *iface)
{
    iface->get_editor = get_editor;
    iface->get_capabilities = get_capabilities;
}

G_MODULE_EXPORT NMVpnEditorPlugin *
nm_vpn_editor_plugin_factory (GError **error)
{
    g_return_val_if_fail (!error || !*error, NULL);

    return g_object_new (socks5_editor_plugin_get_type (), NULL);
}
