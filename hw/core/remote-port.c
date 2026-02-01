// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * QEMU remote attach
 *
 * Copyright (c) 2013 Xilinx Inc
 * Written by Edgar E. Iglesias <edgar.iglesias@xilinx.com>
 *
 * This code is licensed under the GNU GPL.
 */

#include "qemu/osdep.h"
#include "system/system.h"
#include "chardev/char.h"
#include "system/cpus.h"
#include "system/cpu-timers.h"
#include "system/reset.h"
#include "hw/core/sysbus.h"
#include "hw/core/hw-error.h"
#include "qemu/sockets.h"
#include "qemu/thread.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "qemu/cutils.h"

#ifndef _WIN32
#include <sys/mman.h>
#endif

#include "hw/core/remote-port-proto.h"
#include "hw/core/remote-port.h"

#define D(x)
#define SYNCD(x)

#ifndef REMOTE_PORT_ERR_DEBUG
#define REMOTE_PORT_DEBUG_LEVEL 0
#else
#define REMOTE_PORT_DEBUG_LEVEL 1
#endif

#define DB_PRINT_L(level, ...) do { \
    if (REMOTE_PORT_DEBUG_LEVEL > level) { \
        fprintf(stderr,  ": %s: ", __func__); \
        fprintf(stderr, ## __VA_ARGS__); \
    } \
} while (0)

#define REMOTE_PORT_CLASS(klass)    \
     OBJECT_CLASS_CHECK(RemotePortClass, (klass), TYPE_REMOTE_PORT)

static char *rp_sanitize_prefix(RemotePort *s)
{
    char *sanitized_name;
    char *c;

    sanitized_name = g_strdup(s->prefix);
    for (c = sanitized_name; *c != '\0'; c++) {
        if (*c == '/') {
            *c = '_';
        }
    }
    return sanitized_name;
}

static char *rp_autocreate_chardesc(RemotePort *s, bool server)
{
    char *prefix;
    char *chardesc;
    int r;

    prefix = rp_sanitize_prefix(s);
    r = asprintf(&chardesc, "unix:%s/qemu-rport-%s%s",
                 machine_path, prefix, server ? ",wait,server" : "");
    assert(r > 0);
    free(prefix);
    return chardesc;
}

static Chardev *rp_autocreate_chardev(RemotePort *s, char *name)
{
    Chardev *chr = NULL;
    char *chardesc;
    char *s_path;
    int r;

    r = asprintf(&s_path, "%s/qemu-rport-%s", machine_path,
                 rp_sanitize_prefix(s));
    assert(r > 0);
    if (g_file_test(s_path, G_FILE_TEST_EXISTS)) {
        chardesc = rp_autocreate_chardesc(s, false);
        chr = qemu_chr_new_noreplay(name, chardesc, false, NULL);
        free(chardesc);
    }
    free(s_path);

    if (!chr) {
        chardesc = rp_autocreate_chardesc(s, true);
        chr = qemu_chr_new_noreplay(name, chardesc, false, NULL);
        free(chardesc);
    }
    return chr;
}

static void rp_reset(DeviceState *dev)
{
    RemotePort *s = REMOTE_PORT(dev);

    if (s->reset_done) {
        return;
    }

    s->reset_done = true;
}

static void rp_realize(DeviceState *dev, Error **errp)
{
    RemotePort *s = REMOTE_PORT(dev);
    int r;
    Error *err = NULL;

    s->prefix = object_get_canonical_path(OBJECT(dev));

    if (!qemu_chr_fe_get_driver(&s->chr)) {
        char *name;
        Chardev *chr = NULL;
        static int nr;

        r = asprintf(&name, "rport%d", nr);
        nr++;
        assert(r > 0);

        if (s->chrdev_id) {
            chr = qemu_chr_find(s->chrdev_id);
        }

        if (chr) {
            /* Found the chardev via commandline */
        } else if (s->chardesc) {
            chr = qemu_chr_new(name, s->chardesc, NULL);
        } else {
            if (!machine_path) {
                error_report("%s: Missing chardesc prop."
                             " Forgot -machine-path?",
                             s->prefix);
                exit(EXIT_FAILURE);
            }
            chr = rp_autocreate_chardev(s, name);
        }

        free(name);
        if (!chr) {
            error_report("%s: Unable to create remort-port channel %s",
                         s->prefix, s->chardesc);
            exit(EXIT_FAILURE);
        }

        qdev_prop_set_chr(dev, "chardev", chr);
        s->chrdev = chr;
    }

#ifdef _WIN32
    /*
     * Create a socket connection between two sockets. We auto-bind
     * and read out the port selected by the kernel.
     */
    {
        char *name;
        SocketAddress *sock;
        int port;
        int listen_sk;

        sock = socket_parse("127.0.0.1:0", &error_abort);
        listen_sk = socket_listen(sock, 1, &error_abort);

        if (s->event.pipe.read < 0) {
            perror("socket read");
            exit(EXIT_FAILURE);
        }

        {
            struct sockaddr_in saddr;
            socklen_t slen = sizeof saddr;
            int r;

            r = getsockname(listen_sk, (struct sockaddr *) &saddr, &slen);
            if (r < 0) {
                perror("getsockname");
                exit(EXIT_FAILURE);
            }
            port = htons(saddr.sin_port);
        }

        name = g_strdup_printf("127.0.0.1:%d", port);
        s->event.pipe.write = inet_connect(name, &error_abort);
        g_free(name);
        if (s->event.pipe.write < 0) {
            perror("socket write");
            exit(EXIT_FAILURE);
        }

        for (;;) {
            struct sockaddr_in saddr;
            socklen_t slen = sizeof saddr;
            int fd;

            slen = sizeof(saddr);
            fd = qemu_accept(listen_sk, (struct sockaddr *)&saddr, &slen);
            if (fd < 0 && errno != EINTR) {
                close(listen_sk);
                return;
            } else if (fd >= 0) {
                close(listen_sk);
                s->event.pipe.read = fd;
                break;
            }
        }

        if (!qemu_set_blocking(s->event.pipe.read, false, &err)) {
            error_report("%s: Unable to set non-block for internal pipes",
                        s->prefix);
            exit(EXIT_FAILURE);
        }
    }
#else
    if (!g_unix_open_pipe(s->event.pipes, FD_CLOEXEC, NULL)) {
        error_report("%s: Unable to create remort-port internal pipes",
                    s->prefix);
        exit(EXIT_FAILURE);
    }

    if (!qemu_set_blocking(s->event.pipe.read, false, &err)) {
        error_report("%s: Unable to set non-block for internal pipes",
                    s->prefix);
        exit(EXIT_FAILURE);
    }

#endif
}

static void rp_unrealize(DeviceState *dev)
{
    RemotePort *s = REMOTE_PORT(dev);

    s->finalizing = true;

    info_report("%s: Wait for remote-port to disconnect", s->prefix);
    qemu_chr_fe_disconnect(&s->chr);

    close(s->event.pipe.read);
    close(s->event.pipe.write);
    object_unparent(OBJECT(s->chrdev));
}

static const VMStateDescription vmstate_rp = {
    .name = TYPE_REMOTE_PORT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_END_OF_LIST(),
    }
};

static Property rp_properties[] = {
    DEFINE_PROP_CHR("chardev", RemotePort, chr),
    DEFINE_PROP_STRING("chardesc", RemotePort, chardesc),
    DEFINE_PROP_STRING("chrdev-id", RemotePort, chrdev_id),
};

static void rp_prop_allow_set_link(const Object *obj, const char *name,
                                   Object *val, Error **errp)
{
}

static void rp_init(Object *obj)
{
    RemotePort *s = REMOTE_PORT(obj);
    int i;

    for (i = 0; i < REMOTE_PORT_MAX_DEVS; ++i) {
        char *name = g_strdup_printf("remote-port-dev%d", i);
        object_property_add_link(obj, name, TYPE_REMOTE_PORT_DEVICE,
                             (Object **)&s->devs[i],
                             rp_prop_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
        g_free(name);
    }
}

static void rp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->legacy_reset = rp_reset;
    dc->realize = rp_realize;
    dc->unrealize = rp_unrealize;
    dc->vmsd = &vmstate_rp;
    device_class_set_props_n(dc, rp_properties, ARRAY_SIZE(rp_properties));
}

static const TypeInfo rp_info = {
    .name          = TYPE_REMOTE_PORT,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(RemotePort),
    .instance_init = rp_init,
    .class_init    = rp_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { },
    },
};

static const TypeInfo rp_device_info = {
    .name          = TYPE_REMOTE_PORT_DEVICE,
    .parent        = TYPE_INTERFACE,
    .class_size    = sizeof(RemotePortDeviceClass),
};

static void rp_register_types(void)
{
    type_register_static(&rp_info);
    type_register_static(&rp_device_info);
}

type_init(rp_register_types)
