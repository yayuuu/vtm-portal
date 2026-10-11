/* SPDX-License-Identifier: MIT */
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "paths.h"

#define BUS_NAME "org.freedesktop.impl.portal.desktop.vtm"
#define OBJECT_PATH "/org/freedesktop/portal/desktop"
#define FILE_IFACE "org.freedesktop.impl.portal.FileChooser"
#define REQUEST_IFACE "org.freedesktop.impl.portal.Request"
#define MAX_PACKET (1024 * 1024)

static const char xml[] =
  "<node><interface name='" FILE_IFACE "'>"
  "<method name='OpenFile'>"
  "<arg type='o' direction='in'/><arg type='s' direction='in'/>"
  "<arg type='s' direction='in'/><arg type='s' direction='in'/>"
  "<arg type='a{sv}' direction='in'/><arg type='u' direction='out'/>"
  "<arg type='a{sv}' direction='out'/></method>"
  "<method name='SaveFile'>"
  "<arg type='o' direction='in'/><arg type='s' direction='in'/>"
  "<arg type='s' direction='in'/><arg type='s' direction='in'/>"
  "<arg type='a{sv}' direction='in'/><arg type='u' direction='out'/>"
  "<arg type='a{sv}' direction='out'/></method>"
  "<method name='SaveFiles'>"
  "<arg type='o' direction='in'/><arg type='s' direction='in'/>"
  "<arg type='s' direction='in'/><arg type='s' direction='in'/>"
  "<arg type='a{sv}' direction='in'/><arg type='u' direction='out'/>"
  "<arg type='a{sv}' direction='out'/></method>"
  "<property name='version' type='u' access='read'/></interface>"
  "<interface name='" REQUEST_IFACE "'><method name='Close'/></interface></node>";

typedef struct {
  char *handle, *sender, *job;
  GDBusMethodInvocation *invocation;
  guint registration, timer;
  GPid launcher;
  gboolean launcher_done, completed, preserve_job;
  gint64 created, closed;
} Request;

static GMainLoop *loop;
static GDBusConnection *bus;
static GDBusNodeInfo *info;
static GHashTable *requests;
static char *runtime;
static const char *helper;
static gboolean acquired;

static void request_free(gpointer data)
{
  Request *r = data;
  if (r->timer) g_source_remove(r->timer);
  if (r->registration) g_dbus_connection_unregister_object(bus, r->registration);
  g_clear_object(&r->invocation);
  if (r->preserve_job) goto free_fields;
  /* Jobs are private and contain only regular files created by this service. */
  GDir *dir = g_dir_open(r->job, 0, NULL);
  if (dir) {
    const char *name;
    while ((name = g_dir_read_name(dir))) {
      g_autofree char *path = g_build_filename(r->job, name, NULL);
      g_unlink(path);
    }
    g_dir_close(dir);
  }
  g_rmdir(r->job);
free_fields:
  g_free(r->job);
  g_free(r->sender);
  g_free(r->handle);
  g_free(r);
}

static void complete(Request *r, guint code, GVariant *results)
{
  if (r->completed) return;
  if (!results) results = g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0);
  g_dbus_method_invocation_return_value(r->invocation,
                                       g_variant_new("(u@a{sv})", code, results));
  g_clear_object(&r->invocation);
  r->completed = TRUE;
  r->closed = g_get_monotonic_time();
  if (r->registration) {
    g_dbus_connection_unregister_object(bus, r->registration);
    r->registration = 0;
  }
}

static void cancel(Request *r, guint code)
{
  if (r->completed) return;
  g_autofree char *file = g_build_filename(r->job, "cancel", NULL);
  g_file_set_contents(file, "", 0, NULL);
  /* Keep the job until the picker acknowledges cancellation, including when
   * it starts late. Never signal a PID read from a result file. */
  complete(r, code, NULL);
}

static gboolean valid_results(guint code, GVariant *results)
{
  if (code > 2) return FALSE;
  if (code != 0) return TRUE;
  g_autoptr(GVariant) uris = g_variant_lookup_value(results, "uris", G_VARIANT_TYPE("as"));
  if (!uris || !g_variant_n_children(uris)) return FALSE;
  GVariantIter iter;
  const char *uri;
  g_variant_iter_init(&iter, uris);
  while (g_variant_iter_next(&iter, "&s", &uri)) {
    g_autofree char *host = NULL;
    g_autofree char *filename = g_filename_from_uri(uri, &host, NULL);
    if (!filename || (host && *host && strcmp(host, "localhost"))) return FALSE;
  }
  g_autoptr(GVariant) choices = g_variant_lookup_value(results, "choices", NULL);
  g_autoptr(GVariant) filter = g_variant_lookup_value(results, "current_filter", NULL);
  return (!choices || g_variant_is_of_type(choices, G_VARIANT_TYPE("a(ss)"))) &&
         (!filter || g_variant_is_of_type(filter, G_VARIANT_TYPE("(sa(us))")));
}

static gboolean poll_result(gpointer data)
{
  Request *r = data;
  g_autofree char *path = g_build_filename(r->job, "result.variant", NULL);
  g_autofree char *contents = NULL;
  gsize length = 0;
  gboolean done = FALSE;
  struct stat st;
  if (!g_stat(path, &st)) {
    done = TRUE;
    if (!r->completed) {
      g_autoptr(GVariant) packet = NULL;
      g_autoptr(GError) error = NULL;
      if (st.st_size <= MAX_PACKET && g_file_get_contents(path, &contents, &length, &error))
        packet = g_variant_parse(G_VARIANT_TYPE("(ua{sv})"), contents,
                                 contents + length, NULL, &error);
      if (packet) {
        guint code;
        GVariant *results;
        g_variant_get(packet, "(u@a{sv})", &code, &results);
        if (valid_results(code, results)) complete(r, code, results);
        else { g_variant_unref(results); complete(r, 2, NULL); }
      } else {
        g_warning("Invalid picker response: %s", error ? error->message : "packet too large");
        complete(r, 2, NULL);
      }
    }
  }
  gint64 now = g_get_monotonic_time();
  if (!r->completed) {
    g_autofree char *started = g_build_filename(r->job, "started", NULL);
    if (!g_file_test(started, G_FILE_TEST_EXISTS)) {
      double timeout = 30;
      g_autofree char *timeout_path = g_build_filename(r->job, "start-timeout", NULL);
      g_autofree char *value = NULL;
      if (g_file_get_contents(timeout_path, &value, NULL, NULL))
        timeout = CLAMP(g_ascii_strtod(value, NULL), 1, 300);
      if (now - r->created > timeout * G_USEC_PER_SEC) {
        g_warning("Picker did not start in time");
        cancel(r, 2);
      }
    }
  }
  /* Allow the worker to finish, but do not accumulate abandoned jobs forever. */
  if (r->completed && (done || now - r->closed > 60 * G_USEC_PER_SEC) && r->launcher_done) {
    r->timer = 0;
    g_hash_table_remove(requests, r->handle);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void launcher_done(GPid pid, gint status, gpointer data)
{
  char *handle = data;
  Request *r = g_hash_table_lookup(requests, handle);
  if (r) {
    r->launcher_done = TRUE;
    r->launcher = 0;
    if (!g_spawn_check_wait_status(status, NULL)) {
      g_warning("Failed to launch picker in VTM");
      cancel(r, 2);
    }
  }
  g_spawn_close_pid(pid);
  g_free(handle);
}

static void close_request(GDBusConnection *connection, const char *sender,
                          const char *path, const char *iface, const char *method,
                          GVariant *parameters, GDBusMethodInvocation *invocation,
                          gpointer data)
{
  (void)connection; (void)path; (void)iface; (void)method; (void)parameters;
  Request *r = data;
  if (strcmp(sender, r->sender)) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.freedesktop.DBus.Error.AccessDenied", "Only the requesting portal may close this request");
    return;
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
  cancel(r, 1);
}

static const GDBusInterfaceVTable request_vtable = { .method_call = close_request };

static void choose(GDBusConnection *connection, const char *sender, const char *path,
                   const char *iface, const char *method, GVariant *parameters,
                   GDBusMethodInvocation *invocation, gpointer data)
{
  (void)connection; (void)path; (void)iface; (void)data;
  const char *handle, *app, *parent, *title;
  g_autoptr(GVariant) options = NULL;
  g_variant_get(parameters, "(&o&s&s&s@a{sv})", &handle, &app, &parent, &title, &options);
  if (!g_str_has_prefix(handle, OBJECT_PATH "/request/") ||
      g_hash_table_contains(requests, handle) || g_hash_table_size(requests) >= 64 ||
      g_variant_get_size(parameters) > MAX_PACKET) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.freedesktop.DBus.Error.InvalidArgs", "Invalid, duplicate or excessive request");
    return;
  }
  g_autoptr(GError) error = NULL;
  g_autofree char *template = g_build_filename(runtime, "picker-XXXXXX", NULL);
  if (!g_mkdtemp_full(template, 0700)) {
    g_dbus_method_invocation_return_dbus_error(invocation,
      "org.freedesktop.DBus.Error.Failed", "Cannot create private picker directory");
    return;
  }
  Request *r = g_new0(Request, 1);
  r->handle = g_strdup(handle);
  r->sender = g_strdup(sender);
  r->job = g_steal_pointer(&template);
  r->invocation = g_object_ref(invocation);
  r->created = g_get_monotonic_time();
  r->launcher_done = TRUE;
  r->registration = g_dbus_connection_register_object(bus, handle, info->interfaces[1],
                                                       &request_vtable, r, NULL, &error);
  if (!r->registration) {
    g_dbus_method_invocation_return_gerror(invocation, error);
    request_free(r);
    return;
  }
  g_hash_table_insert(requests, r->handle, r);
  const char *mode = !strcmp(method, "SaveFile") ? "save" :
                     !strcmp(method, "SaveFiles") ? "savefiles" : "open";
  g_autoptr(GVariant) packet = g_variant_ref_sink(g_variant_new("(ssss@a{sv})", mode,
                                                  app, parent, title, g_variant_ref(options)));
  g_autofree char *text = g_variant_print(packet, TRUE);
  g_autofree char *file = g_build_filename(r->job, "request.variant", NULL);
  if (!g_file_set_contents(file, text, -1, &error)) {
    g_warning("Cannot write request: %s", error->message);
    cancel(r, 2);
  } else {
    char *argv[] = { (char *)helper, "launch-picker", r->job, NULL };
    if (g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                      &r->launcher, &error)) {
      r->launcher_done = FALSE;
      g_child_watch_add(r->launcher, launcher_done, g_strdup(handle));
    } else {
      g_warning("Cannot start picker helper: %s", error->message);
      cancel(r, 2);
    }
  }
  r->timer = g_timeout_add(100, poll_result, r);
}

static GVariant *get_property(GDBusConnection *connection, const char *sender,
                             const char *path, const char *iface, const char *property,
                             GError **error, gpointer data)
{
  (void)connection; (void)sender; (void)path; (void)iface; (void)error; (void)data;
  return !strcmp(property, "version") ? g_variant_new_uint32(4) : NULL;
}
static const GDBusInterfaceVTable portal_vtable = {
  .method_call = choose, .get_property = get_property
};

static void owner_changed(GDBusConnection *connection, const char *sender,
                          const char *path, const char *iface, const char *signal_name,
                          GVariant *parameters, gpointer data)
{
  (void)connection; (void)sender; (void)path; (void)iface; (void)signal_name; (void)data;
  const char *name, *old_owner, *new_owner;
  g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
  if (*new_owner || name[0] != ':') return;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, requests);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Request *r = value;
    if (!strcmp(name, r->sender)) cancel(r, 1);
  }
}

static void on_bus(GDBusConnection *connection, const char *name, gpointer data)
{
  (void)name; (void)data;
  bus = g_object_ref(connection);
  g_autoptr(GError) error = NULL;
  if (!g_dbus_connection_register_object(bus, OBJECT_PATH, info->interfaces[0],
                                         &portal_vtable, NULL, NULL, &error)) {
    g_printerr("%s\n", error->message);
    g_main_loop_quit(loop);
  }
  g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
    "NameOwnerChanged", "/org/freedesktop/DBus", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
    owner_changed, NULL, NULL);
}
static void on_acquired(GDBusConnection *connection, const char *name, gpointer data)
{
  (void)connection; (void)name; (void)data;
  acquired = TRUE;
  g_message("VTM FileChooser portal %s ready", PORTAL_VERSION);
}
static void on_lost(GDBusConnection *connection, const char *name, gpointer data)
{
  (void)connection; (void)name; (void)data;
  acquired = FALSE;
  g_printerr("Cannot own %s on the session bus\n", BUS_NAME);
  g_main_loop_quit(loop);
}
static gboolean stop(gpointer data)
{
  (void)data;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, requests);
  while (g_hash_table_iter_next(&iter, NULL, &value)) cancel(value, 1);
  g_main_loop_quit(loop);
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1], "--version")) {
    g_print("xdg-desktop-portal-vtm %s\n", PORTAL_VERSION);
    return 0;
  }
  if (argc != 1) { g_printerr("Usage: %s [--version]\n", argv[0]); return 2; }
  helper = g_getenv("VTM_PORTAL_HELPER");
  if (!helper) helper = PORTAL_HELPER;
  if (!g_getenv("VTM_PORTAL_SYSTEM_CONFIG"))
    g_setenv("VTM_PORTAL_SYSTEM_CONFIG", PORTAL_CONFIG, FALSE);
  /* mkdtemp is exclusive even if several isolated buses share a runtime dir. */
  g_autofree char *template = g_build_filename(g_get_user_runtime_dir(), "vtm-portal-XXXXXX", NULL);
  runtime = g_mkdtemp_full(template, 0700);
  if (!runtime) { g_printerr("Cannot create runtime directory\n"); return 1; }
  runtime = g_strdup(runtime);
  info = g_dbus_node_info_new_for_xml(xml, NULL);
  requests = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, request_free);
  loop = g_main_loop_new(NULL, FALSE);
  g_unix_signal_add(SIGTERM, stop, NULL);
  g_unix_signal_add(SIGINT, stop, NULL);
  guint owner = g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                              on_bus, on_acquired, on_lost, NULL, NULL);
  g_main_loop_run(loop);
  /* On shutdown leave cancellation markers for running pickers. Their jobs
   * are removed by the worker; inactive jobs may be removed immediately. */
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, requests);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    Request *r = value;
    cancel(r, 1);
    g_autofree char *started = g_build_filename(r->job, "started", NULL);
    if (g_file_test(started, G_FILE_TEST_EXISTS) || !r->launcher_done) {
      g_autofree char *orphan = g_build_filename(r->job, "orphan", NULL);
      g_file_set_contents(orphan, "", 0, NULL);
      /* Preserve the private job until the worker sees the cancel marker. */
      r->preserve_job = TRUE;
    }
  }
  g_bus_unown_name(owner);
  g_hash_table_destroy(requests);
  g_clear_object(&bus);
  g_dbus_node_info_unref(info);
  g_main_loop_unref(loop);
  g_rmdir(runtime);
  g_free(runtime);
  return acquired ? 0 : 1;
}
