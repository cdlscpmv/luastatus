#include <sys/select.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <lua.h>
#include <stdlib.h>
#include <libudev.h>
#include <pthread.h>

#include "include/plugin_v1.h"
#include "include/sayf_macros.h"
#include "include/plugin_utils.h"

#include "libls/alloc_utils.h"
#include "libls/time_utils.h"
#include "libls/cstring_utils.h"
#include "libls/wakeup_fifo.h"
#include "libls/sig_utils.h"
#include "libls/panic.h"

typedef struct {
    char *subsystem;
    char *devtype;
    char *tag;
    bool kernel_ev;
    bool greet;
    struct timespec timeout;

    struct timespec pushed_timeout;
    pthread_spinlock_t push_lock;
} Priv;

static
void
destroy(LuastatusPluginData *pd)
{
    Priv *p = pd->priv;
    free(p->subsystem);
    free(p->devtype);
    free(p->tag);
    pthread_spin_destroy(&p->push_lock);
    free(p);
}

static
int
init(LuastatusPluginData *pd, lua_State *L)
{
    Priv *p = pd->priv = LS_XNEW(Priv, 1);
    *p = (Priv) {
        .subsystem = NULL,
        .devtype = NULL,
        .tag = NULL,
        .kernel_ev = false,
        .greet = false,
        .timeout = ls_timespec_invalid,
        .pushed_timeout = ls_timespec_invalid,
    };

    if (pthread_spin_init(&p->push_lock, PTHREAD_PROCESS_PRIVATE) != 0) {
        LS_PANIC("pthread_spin_init() failed, which is impossible");
    }

    PU_MAYBE_VISIT_STR_FIELD(-1, "subsystem", "'subsystem'", s,
        p->subsystem = ls_xstrdup(s);
    );

    PU_MAYBE_VISIT_STR_FIELD(-1, "devtype", "'devtype'", s,
        p->devtype = ls_xstrdup(s);
    );

    PU_MAYBE_VISIT_STR_FIELD(-1, "tag", "'tag'", s,
        p->tag = ls_xstrdup(s);
    );

    PU_MAYBE_VISIT_BOOL_FIELD(-1, "kernel_events", "'kernel_events'", b,
        p->kernel_ev = b;
    );

    PU_MAYBE_VISIT_NUM_FIELD(-1, "timeout", "'timeout'", n,
        if (n >= 0 && ls_timespec_is_invalid(p->timeout = ls_timespec_from_seconds(n))) {
            LS_FATALF(pd, "invalid 'timeout' value");
            goto error;
        }
    );

    PU_MAYBE_VISIT_BOOL_FIELD(-1, "greet", "'greet'", b,
        p->greet = b;
    );

    return LUASTATUS_OK;

error:
    destroy(pd);
    return LUASTATUS_ERR;
}

static
int
l_push_timeout(lua_State *L)
{
    LuastatusPluginData *pd = lua_touserdata(L, lua_upvalueindex(1));
    Priv *p = pd->priv;

    struct timespec timeout = ls_timespec_from_seconds(luaL_checknumber(L, 1));
    if (ls_timespec_is_invalid(timeout)) {
        return luaL_error(L, "invalid timeout");
    }

    pthread_spin_lock(&p->push_lock);
    p->pushed_timeout = timeout;
    pthread_spin_unlock(&p->push_lock);

    return 0;
}

static
void
register_funcs(LuastatusPluginData *pd, lua_State *L)
{
    // L: table
    lua_pushlightuserdata(L, pd); // L: table pd
    lua_pushcclosure(L, l_push_timeout, 1); // L: table pd l_push_timeout
    ls_lua_rawsetf(L, "push_timeout"); // L: table
}

static inline
void
report_status(LuastatusPluginData *pd, LuastatusPluginRunFuncs funcs, const char *status)
{
    lua_State *L = funcs.call_begin(pd->userdata);
    lua_createtable(L, 0, 1); // L: table
    lua_pushstring(L, status); // L: table string
    lua_setfield(L, -2, "what"); // L: table
    funcs.call_end(pd->userdata);
}

static
void
report_event(LuastatusPluginData *pd, LuastatusPluginRunFuncs funcs, struct udev_device *dev)
{
    lua_State *L = funcs.call_begin(pd->userdata);
    lua_createtable(L, 0, 4); // L: table

    lua_pushstring(L, "event"); // L: table string
    lua_setfield(L, -2, "what"); // L: table

#define PROP(Key_, Func_) \
    do { \
        /* L: table */ \
        const char *r_ = Func_(dev); \
        if (r_) { \
            lua_pushstring(L, r_);     /* L: table string */ \
            lua_setfield(L, -2, Key_); /* L: table */ \
        } \
    } while (0)

    PROP("syspath", udev_device_get_syspath);
    PROP("sysname", udev_device_get_sysname);
    PROP("sysnum", udev_device_get_sysnum);
    PROP("devpath", udev_device_get_devpath);
    PROP("devnode", udev_device_get_devnode);
    PROP("devtype", udev_device_get_devtype);
    PROP("subsystem", udev_device_get_subsystem);
    PROP("driver", udev_device_get_driver);
    PROP("action", udev_device_get_action);

#undef PROP

    funcs.call_end(pd->userdata);
}

static
void
run(LuastatusPluginData *pd, LuastatusPluginRunFuncs funcs)
{
    Priv *p = pd->priv;

    struct udev *udev = udev_new();
    if (!udev) {
        LS_FATALF(pd, "udev_new() failed");
        return;
    }

    struct udev_monitor *mon = udev_monitor_new_from_netlink(
        udev, p->kernel_ev ? "kernel" : "udev");
    udev_monitor_filter_add_match_subsystem_devtype(mon, p->subsystem, p->devtype);
    udev_monitor_filter_add_match_tag(mon, p->tag);
    udev_monitor_enable_receiving(mon);
    const int fd = udev_monitor_get_fd(mon);

    fd_set fds;
    FD_ZERO(&fds);

    sigset_t allsigs;
    ls_xsigfillset(&allsigs);

    if (p->greet) {
        report_status(pd, funcs, "hello");
    }

    while (1) {
        struct timespec timeout;
        pthread_spin_lock(&p->push_lock);
        if (ls_timespec_is_invalid(p->pushed_timeout)) {
            timeout = p->timeout;
        } else {
            timeout = p->pushed_timeout;
            p->pushed_timeout = ls_timespec_invalid;
        }
        pthread_spin_unlock(&p->push_lock);

        FD_SET(fd, &fds);
        const int r = pselect(
            fd + 1,
            &fds, NULL, NULL,
            ls_timespec_is_invalid(timeout) ? NULL : &timeout,
            &allsigs);

        if (r < 0) {
            LS_FATALF(pd, "pselect: %s", ls_strerror_onstack(errno));
            break;
        } else if (r == 0) {
            report_status(pd, funcs, "timeout");
        } else {
            struct udev_device *dev = udev_monitor_receive_device(mon);
            if (!dev) {
                // what the...?
                continue;
            }
            report_event(pd, funcs, dev);
            udev_device_unref(dev);
        }
    }

    udev_unref(udev);
}

LuastatusPluginIface luastatus_plugin_iface_v1 = {
    .init = init,
    .register_funcs = register_funcs,
    .run = run,
    .destroy = destroy,
};
