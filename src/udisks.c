/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "udisks.h"

#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "log.h"

#define UD_NAME "org.freedesktop.UDisks2"
#define UD_PATH "/org/freedesktop/UDisks2"
#define IF_BLOCK UD_NAME ".Block"
#define IF_DRIVE UD_NAME ".Drive"
#define IF_ATA UD_NAME ".Drive.Ata"
#define IF_NVME UD_NAME ".NVMe.Controller"
#define IF_FS UD_NAME ".Filesystem"
#define IF_CRYPT UD_NAME ".Encrypted"

/* A temperature read from the SMART attributes themselves (see
 * ata_temperature), per drive object path, valid for one SmartUpdated. */
typedef struct {
    uint64_t updated;
    bool pending;
    bool has_temp;
    double temp_c;
} temp_entry;

struct kd_udisks {
    GDBusObjectManager *om;
    GCancellable *cancel;
    kd_udisks_changed_fn changed;
    void *ud;
    char *error;
    bool ready;
    GHashTable *temps; /* drive path -> temp_entry */
};

typedef struct {
    kd_udisks *u;
    char *path;
} temp_req;

/* ---- connection ----------------------------------------------------------- */

static void on_any_change(kd_udisks *u)
{
    if (u->changed)
        u->changed(u->ud);
}

static void on_object(GDBusObjectManager *om, GDBusObject *obj, gpointer data)
{
    (void)om;
    (void)obj;
    on_any_change(data);
}

static void on_interface(GDBusObjectManager *om, GDBusObject *obj, GDBusInterface *iface,
                         gpointer data)
{
    (void)om;
    (void)obj;
    (void)iface;
    on_any_change(data);
}

static void on_props(GDBusObjectManagerClient *om, GDBusObjectProxy *obj, GDBusProxy *proxy,
                     GVariant *changed, const gchar *const *invalidated, gpointer data)
{
    (void)om;
    (void)obj;
    (void)proxy;
    (void)changed;
    (void)invalidated;
    on_any_change(data);
}

static void on_connected(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    GError *err = NULL;
    GDBusObjectManager *om = g_dbus_object_manager_client_new_for_bus_finish(res, &err);
    if (!om && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err); /* kd_udisks_free: data is gone */
        return;
    }
    kd_udisks *u = data;
    if (!om) {
        u->error = g_strdup_printf(_("cannot talk to UDisks2: %s"), err->message);
        g_error_free(err);
    } else {
        char *owner = g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(om));
        if (!owner) {
            u->error = g_strdup(_("UDisks2 (udisksd) is not running"));
            g_object_unref(om);
            om = NULL;
        }
        g_free(owner);
    }
    if (om) {
        u->om = om;
        u->ready = true;
        g_signal_connect(om, "object-added", G_CALLBACK(on_object), u);
        g_signal_connect(om, "object-removed", G_CALLBACK(on_object), u);
        g_signal_connect(om, "interface-added", G_CALLBACK(on_interface), u);
        g_signal_connect(om, "interface-removed", G_CALLBACK(on_interface), u);
        g_signal_connect(om, "interface-proxy-properties-changed", G_CALLBACK(on_props), u);
    } else {
        kd_log_debug("udisks: %s", u->error);
    }
    on_any_change(u);
}

kd_udisks *kd_udisks_new(kd_udisks_changed_fn changed, void *ud)
{
    kd_udisks *u = g_new0(kd_udisks, 1);
    u->changed = changed;
    u->ud = ud;
    u->cancel = g_cancellable_new();
    u->temps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM,
                                             G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, UD_NAME,
                                             UD_PATH, NULL, NULL, NULL, u->cancel, on_connected, u);
    return u;
}

void kd_udisks_free(kd_udisks *u)
{
    if (!u)
        return;
    g_cancellable_cancel(u->cancel);
    g_object_unref(u->cancel);
    if (u->om) {
        g_signal_handlers_disconnect_by_data(u->om, u);
        g_object_unref(u->om);
    }
    g_free(u->error);
    g_hash_table_destroy(u->temps);
    g_free(u);
}

bool kd_udisks_ready(const kd_udisks *u)
{
    return u->ready;
}

const char *kd_udisks_error(const kd_udisks *u)
{
    return u->error;
}

static GVariant *no_options(void);

/* ---- lookups ---------------------------------------------------------------- */

/* A cached property, or NULL. Caller unrefs. */
static GVariant *prop(GDBusInterface *iface, const char *name)
{
    return iface ? g_dbus_proxy_get_cached_property(G_DBUS_PROXY(iface), name) : NULL;
}

static bool prop_bool(GDBusInterface *iface, const char *name, bool def)
{
    GVariant *v = prop(iface, name);
    if (!v)
        return def;
    bool r = g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) ? g_variant_get_boolean(v) : def;
    g_variant_unref(v);
    return r;
}

/* Any integer or double type, as a double. */
static bool prop_num(GDBusInterface *iface, const char *name, double *out)
{
    GVariant *v = prop(iface, name);
    if (!v)
        return false;
    bool ok = true;
    const GVariantType *t = g_variant_get_type(v);
    if (g_variant_type_equal(t, G_VARIANT_TYPE_DOUBLE))
        *out = g_variant_get_double(v);
    else if (g_variant_type_equal(t, G_VARIANT_TYPE_UINT64))
        *out = (double)g_variant_get_uint64(v);
    else if (g_variant_type_equal(t, G_VARIANT_TYPE_INT64))
        *out = (double)g_variant_get_int64(v);
    else if (g_variant_type_equal(t, G_VARIANT_TYPE_INT32))
        *out = g_variant_get_int32(v);
    else if (g_variant_type_equal(t, G_VARIANT_TYPE_UINT32))
        *out = g_variant_get_uint32(v);
    else if (g_variant_type_equal(t, G_VARIANT_TYPE_UINT16))
        *out = g_variant_get_uint16(v);
    else
        ok = false;
    g_variant_unref(v);
    return ok;
}

/* The object path in an "o" property; "" for "/" or absent. Caller frees. */
static char *prop_path(GDBusInterface *iface, const char *name)
{
    GVariant *v = prop(iface, name);
    char *r = g_strdup(v && g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH) &&
                               strcmp(g_variant_get_string(v, NULL), "/") != 0
                           ? g_variant_get_string(v, NULL)
                           : "");
    if (v)
        g_variant_unref(v);
    return r;
}

static bool block_is(GDBusObject *obj, const char *dev)
{
    GDBusInterface *b = g_dbus_object_get_interface(obj, IF_BLOCK);
    if (!b)
        return false;
    GVariant *v = prop(b, "Device");
    bool r = false;
    if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING)) {
        const char *path = g_variant_get_bytestring(v);
        r = strncmp(path, "/dev/", 5) == 0 && strcmp(path + 5, dev) == 0;
    }
    if (v)
        g_variant_unref(v);
    g_object_unref(b);
    return r;
}

/* The object of /dev/<dev>, or NULL. Caller unrefs. */
static GDBusObject *find_block(kd_udisks *u, const char *dev)
{
    if (!u->om)
        return NULL;
    GList *objs = g_dbus_object_manager_get_objects(u->om);
    GDBusObject *found = NULL;
    for (GList *l = objs; l; l = l->next)
        if (!found && block_is(l->data, dev))
            found = g_object_ref(l->data);
    g_list_free_full(objs, g_object_unref);
    return found;
}

/* The Drive object behind a disk, or NULL. Caller unrefs. */
static GDBusObject *find_drive(kd_udisks *u, const char *disk)
{
    GDBusObject *blk = find_block(u, disk);
    if (!blk)
        return NULL;
    GDBusInterface *b = g_dbus_object_get_interface(blk, IF_BLOCK);
    char *path = prop_path(b, "Drive");
    GDBusObject *drive = *path ? g_dbus_object_manager_get_object(u->om, path) : NULL;
    g_free(path);
    if (b)
        g_object_unref(b);
    g_object_unref(blk);
    return drive;
}

/* Temperatures are plausible between these (°C); outside, the drive or the
 * attribute database got it wrong. */
#define TEMP_MIN -20.0
#define TEMP_MAX 110.0

/* SMART attribute "pretty" values in millikelvin (libatasmart's unit 4). */
#define UNIT_MKELVIN 4

static void on_attributes(GObject *src, GAsyncResult *res, gpointer data)
{
    temp_req *r = data;
    GError *err = NULL;
    GVariant *ret = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
    if (!ret && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err); /* kd_udisks_free: r->u is gone */
        g_free(r->path);
        g_free(r);
        return;
    }
    temp_entry *e = g_hash_table_lookup(r->u->temps, r->path);
    if (e) {
        e->pending = false;
        e->has_temp = false;
    }
    if (ret && e) {
        /* 194 is the temperature; 190 the airflow one, close enough. */
        GVariant *attrs = g_variant_get_child_value(ret, 0);
        double best[2] = {0, 0};
        bool have[2] = {false, false};
        GVariantIter it;
        g_variant_iter_init(&it, attrs);
        guchar id;
        gint32 unit;
        guint64 pretty;
        const char *name;
        GVariant *exp;
        guint16 flags;
        gint32 value, worst, threshold;
        while (g_variant_iter_next(&it, "(y&sqiiixi@a{sv})", &id, &name, &flags, &value, &worst,
                                   &threshold, &pretty, &unit, &exp)) {
            g_variant_unref(exp);
            int slot = id == 194 ? 0 : id == 190 ? 1 : -1;
            if (slot >= 0 && unit == UNIT_MKELVIN) {
                double c = (double)pretty / 1000.0 - 273.15;
                if (c > TEMP_MIN && c < TEMP_MAX) {
                    best[slot] = c;
                    have[slot] = true;
                }
            }
        }
        g_variant_unref(attrs);
        if (have[0] || have[1]) {
            e->has_temp = true;
            e->temp_c = have[0] ? best[0] : best[1];
        }
        g_variant_unref(ret);
        on_any_change(r->u);
    }
    if (err)
        g_error_free(err);
    g_free(r->path);
    g_free(r);
}

/* udisks takes the temperature from libatasmart, which misreads some
 * drives: Kingston/Phison SSDs keep "life left" in attribute 231 and get
 * 96 °C. The attributes themselves say better. Asks for them once per SMART
 * reading; the answer triggers a change. true: *temp_c is that answer. */
static bool ata_temperature(kd_udisks *u, GDBusObject *drive, GDBusInterface *ata,
                            uint64_t updated, double *temp_c, bool *has)
{
    const char *path = g_dbus_object_get_object_path(drive);
    temp_entry *e = g_hash_table_lookup(u->temps, path);
    if (e && e->updated == updated) {
        if (e->pending)
            return false;
        *has = e->has_temp;
        *temp_c = e->temp_c;
        return e->has_temp;
    }
    if (!e) {
        e = g_new0(temp_entry, 1);
        g_hash_table_insert(u->temps, g_strdup(path), e);
    }
    e->updated = updated;
    e->pending = true;
    temp_req *r = g_new0(temp_req, 1);
    r->u = u;
    r->path = g_strdup(path);
    g_dbus_proxy_call(G_DBUS_PROXY(ata), "SmartGetAttributes", no_options(),
                      G_DBUS_CALL_FLAGS_NONE, -1, u->cancel, on_attributes, r);
    return false;
}

bool kd_udisks_drive_info(kd_udisks *u, const char *disk, kd_drive_info *out)
{
    memset(out, 0, sizeof *out);
    out->rotation_rate = -1;
    out->bad_sectors = -1;
    out->bus = "";
    GDBusObject *drive = find_drive(u, disk);
    if (!drive)
        return false;
    out->found = true;

    GDBusInterface *d = g_dbus_object_get_interface(drive, IF_DRIVE);
    double n;
    out->removable = prop_bool(d, "Removable", false);
    out->ejectable = prop_bool(d, "Ejectable", false);
    out->can_power_off = prop_bool(d, "CanPowerOff", false);
    if (prop_num(d, "RotationRate", &n))
        out->rotation_rate = (int)n;
    GVariant *bus = prop(d, "ConnectionBus");
    if (bus) {
        const char *s = g_variant_get_string(bus, NULL);
        out->bus = strcmp(s, "usb") == 0 ? "usb" : strcmp(s, "sdio") == 0 ? "sdio" : "";
        g_variant_unref(bus);
    }
    if (d)
        g_object_unref(d);

    GDBusInterface *ata = g_dbus_object_get_interface(drive, IF_ATA);
    if (ata) {
        out->smart_supported = prop_bool(ata, "SmartSupported", false);
        out->smart_enabled = prop_bool(ata, "SmartEnabled", false);
        out->smart_failing = prop_bool(ata, "SmartFailing", false);
        if (prop_num(ata, "SmartUpdated", &n))
            out->smart_updated = (uint64_t)n;
        /* Kelvin; 0 when the drive doesn't say. */
        if (prop_num(ata, "SmartTemperature", &n) && n > 0) {
            out->has_temp = true;
            out->temp_c = n - 273.15;
        }
        if (out->smart_updated) {
            bool has = false;
            double c;
            if (ata_temperature(u, drive, ata, out->smart_updated, &c, &has))
                out->temp_c = c;
        }
        if (out->has_temp && (out->temp_c <= TEMP_MIN || out->temp_c >= TEMP_MAX))
            out->has_temp = false;
        if (prop_num(ata, "SmartPowerOnSeconds", &n))
            out->power_on_s = (uint64_t)n;
        if (prop_num(ata, "SmartNumBadSectors", &n))
            out->bad_sectors = (int64_t)n;
        if (prop_num(ata, "SmartNumAttributesFailing", &n))
            out->attrs_failing = (int)n;
        if (prop_num(ata, "SmartNumAttributesFailedInThePast", &n))
            out->attrs_failed_past = (int)n;
        g_object_unref(ata);
    }

    /* UDisks 2.10+. */
    GDBusInterface *nvme = g_dbus_object_get_interface(drive, IF_NVME);
    if (nvme) {
        out->smart_supported = out->smart_enabled = true;
        if (prop_num(nvme, "SmartUpdated", &n))
            out->smart_updated = (uint64_t)n;
        if (prop_num(nvme, "SmartTemperature", &n) && n > 0) {
            out->has_temp = true;
            out->temp_c = n - 273.15;
        }
        if (prop_num(nvme, "SmartPowerOnHours", &n))
            out->power_on_s = (uint64_t)n * 3600;
        GVariant *warn = prop(nvme, "SmartCriticalWarning");
        if (warn) {
            out->smart_failing = g_variant_n_children(warn) > 0;
            g_variant_unref(warn);
        }
        g_object_unref(nvme);
    }
    g_object_unref(drive);
    return true;
}

/* ---- actions ------------------------------------------------------------------ */

bool kd_udisks_allowed(const char *dev)
{
    const char *only = getenv("KIDISKS_ONLY_LOOP");
    return !only || !*only || strncmp(dev, "loop", 4) == 0;
}

typedef struct {
    GDBusProxy *proxy;
    char *method;
    GVariant *args; /* floating until sent */
} step;

typedef struct {
    GQueue steps;
    GCancellable *cancel;
    kd_udisks_done_fn done;
    void *ud;
    char *result;
} op;

static void step_free(gpointer p)
{
    step *s = p;
    g_object_unref(s->proxy);
    g_free(s->method);
    if (s->args)
        g_variant_unref(g_variant_ref_sink(s->args));
    g_free(s);
}

static void op_free(op *o)
{
    g_queue_clear_full(&o->steps, step_free);
    g_object_unref(o->cancel);
    g_free(o->result);
    g_free(o);
}

static void op_add(op *o, GDBusInterface *iface, const char *method, GVariant *args)
{
    step *s = g_new0(step, 1);
    s->proxy = G_DBUS_PROXY(g_object_ref(iface));
    s->method = g_strdup(method);
    s->args = args;
    g_queue_push_tail(&o->steps, s);
}

static GVariant *no_options(void)
{
    return g_variant_new("(a{sv})", NULL);
}

/* UDisks2 errors in words a person can act on. NULL: the person cancelled
 * the password dialog, say nothing. */
static char *describe_error(GError *err)
{
    char *remote = g_dbus_error_get_remote_error(err);
    char *msg = NULL;
    if (!remote) {
        msg = g_strdup(err->message);
    } else if (strcmp(remote, UD_NAME ".Error.NotAuthorizedDismissed") == 0) {
        msg = NULL;
    } else if (g_str_has_prefix(remote, UD_NAME ".Error.NotAuthorized")) {
        msg = g_strdup(_("Not allowed: the administrator password is needed for this."));
    } else if (strcmp(remote, UD_NAME ".Error.DeviceBusy") == 0) {
        msg = g_strdup(_("The device is busy: close the programs and terminals using it "
                         "and try again."));
    } else {
        g_dbus_error_strip_remote_error(err);
        msg = g_strdup(err->message);
    }
    g_free(remote);
    return msg;
}

static void op_next(op *o);

static void on_step_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    GVariant *ret = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
    op *o = data;
    if (!ret) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            char *msg = describe_error(err);
            o->done(false, msg, NULL, o->ud);
            g_free(msg);
        }
        g_error_free(err);
        op_free(o);
        return;
    }
    /* Mount answers with where it mounted. */
    if (g_variant_is_of_type(ret, G_VARIANT_TYPE("(s)"))) {
        g_free(o->result);
        g_variant_get(ret, "(s)", &o->result);
    }
    g_variant_unref(ret);
    op_next(o);
}

static void op_next(op *o)
{
    step *s = g_queue_pop_head(&o->steps);
    if (!s) {
        o->done(true, NULL, o->result, o->ud);
        op_free(o);
        return;
    }
    GVariant *args = s->args;
    s->args = NULL;
    /* No timeout: the password dialog waits for the person. */
    g_dbus_proxy_call(s->proxy, s->method, args, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                      G_MAXINT, o->cancel, on_step_done, o);
    step_free(s);
}

static op *op_new(kd_udisks *u, kd_udisks_done_fn done, void *ud)
{
    op *o = g_new0(op, 1);
    g_queue_init(&o->steps);
    o->cancel = g_object_ref(u->cancel);
    o->done = done;
    o->ud = ud;
    return o;
}

/* Runs o, or fails it at once with msg. */
static void op_start(op *o, const char *msg)
{
    if (msg) {
        o->done(false, msg, NULL, o->ud);
        op_free(o);
    } else {
        op_next(o);
    }
}

static const char *check_dev(kd_udisks *u, const char *dev)
{
    if (!kd_udisks_allowed(dev))
        return _("KIDISKS_ONLY_LOOP is set: only loop devices may be changed.");
    if (!u->ready)
        return u->error ? u->error : _("UDisks2 is not ready yet.");
    return NULL;
}

static void simple_call(kd_udisks *u, const char *dev, const char *iface_name, bool drive,
                        const char *method, kd_udisks_done_fn done, void *ud)
{
    op *o = op_new(u, done, ud);
    const char *msg = check_dev(u, dev);
    GDBusObject *obj = NULL;
    if (!msg) {
        obj = drive ? find_drive(u, dev) : find_block(u, dev);
        if (!obj)
            msg = _("UDisks2 does not know this device.");
    }
    GDBusInterface *iface = obj ? g_dbus_object_get_interface(obj, iface_name) : NULL;
    if (!msg && !iface)
        msg = strcmp(iface_name, IF_FS) == 0 ? _("There is no filesystem to mount here.")
                                             : _("This device does not support that.");
    if (iface) {
        op_add(o, iface, method, no_options());
        g_object_unref(iface);
    }
    if (obj)
        g_object_unref(obj);
    op_start(o, msg);
}

void kd_udisks_mount(kd_udisks *u, const char *dev, kd_udisks_done_fn done, void *ud)
{
    simple_call(u, dev, IF_FS, false, "Mount", done, ud);
}

void kd_udisks_unmount(kd_udisks *u, const char *dev, kd_udisks_done_fn done, void *ud)
{
    simple_call(u, dev, IF_FS, false, "Unmount", done, ud);
}

void kd_udisks_eject(kd_udisks *u, const char *disk, kd_udisks_done_fn done, void *ud)
{
    simple_call(u, disk, IF_DRIVE, true, "Eject", done, ud);
}

static bool fs_mounted(GDBusInterface *fs)
{
    GVariant *v = prop(fs, "MountPoints");
    bool r = v && g_variant_n_children(v) > 0;
    if (v)
        g_variant_unref(v);
    return r;
}

/* Unmount steps for obj and, if it is an open LUKS, for what is inside;
 * the Lock steps go to locks (run after every unmount). */
static void plan_release(kd_udisks *u, op *o, GQueue *locks, GDBusObject *obj)
{
    GDBusInterface *fs = g_dbus_object_get_interface(obj, IF_FS);
    if (fs) {
        if (fs_mounted(fs))
            op_add(o, fs, "Unmount", no_options());
        g_object_unref(fs);
    }
    GDBusInterface *crypt = g_dbus_object_get_interface(obj, IF_CRYPT);
    if (!crypt)
        return;
    char *clear = prop_path(crypt, "CleartextDevice");
    GDBusObject *inner = *clear ? g_dbus_object_manager_get_object(u->om, clear) : NULL;
    if (inner) {
        GDBusInterface *ifs = g_dbus_object_get_interface(inner, IF_FS);
        if (ifs) {
            if (fs_mounted(ifs))
                op_add(o, ifs, "Unmount", no_options());
            g_object_unref(ifs);
        }
        g_object_unref(inner);
        g_queue_push_tail(locks, g_object_ref(crypt));
    }
    g_free(clear);
    g_object_unref(crypt);
}

void kd_udisks_power_off(kd_udisks *u, const char *disk, kd_udisks_done_fn done, void *ud)
{
    op *o = op_new(u, done, ud);
    const char *msg = check_dev(u, disk);
    GDBusObject *drive = msg ? NULL : find_drive(u, disk);
    if (!msg && !drive)
        msg = _("UDisks2 does not know this device.");
    GDBusInterface *dif = drive ? g_dbus_object_get_interface(drive, IF_DRIVE) : NULL;
    if (!msg && !prop_bool(dif, "CanPowerOff", false))
        msg = _("This drive cannot be powered off.");

    if (!msg) {
        const char *drive_path = g_dbus_object_get_object_path(drive);
        GQueue locks = G_QUEUE_INIT;
        GList *objs = g_dbus_object_manager_get_objects(u->om);
        for (GList *l = objs; l; l = l->next) {
            GDBusInterface *b = g_dbus_object_get_interface(l->data, IF_BLOCK);
            char *dp = prop_path(b, "Drive");
            if (strcmp(dp, drive_path) == 0)
                plan_release(u, o, &locks, l->data);
            g_free(dp);
            if (b)
                g_object_unref(b);
        }
        g_list_free_full(objs, g_object_unref);
        for (GDBusInterface *c; (c = g_queue_pop_head(&locks));) {
            op_add(o, c, "Lock", no_options());
            g_object_unref(c);
        }
        op_add(o, dif, "PowerOff", no_options());
    }
    if (dif)
        g_object_unref(dif);
    if (drive)
        g_object_unref(drive);
    op_start(o, msg);
}
