/* SPDX-License-Identifier:  MIT
 * Copyright 2020-2024 Daniel Eklöf
 * Copyright 2024      Jorengarenar
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <locale.h>
#include <assert.h>

#include <sys/signalfd.h>

#include <wayland-client.h>
#include <wayland-cursor.h>

#include <wlr-layer-shell-unstable-v1.h>
#include <pixman.h>
#include <tllist.h>

#include "log.h"
#include "shm.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;

static pixman_color_t color = { 0, 0, 0, 0xffff };

static bool have_xrgb8888 = false;

struct output {
    struct wl_output *wl_output;
    uint32_t wl_name;

    char *make;
    char *model;

    int width;
    int height;

    int render_width;
    int render_height;

    struct wl_surface *surf;
    struct zwlr_layer_surface_v1 *layer;
    bool configured;
};
static tll(struct output) outputs;

static void render(struct output *output)
{
    const int width = output->render_width;
    const int height = output->render_height;

    struct buffer *buf = shm_get_buffer(
        shm, width, height, (uintptr_t)(void *)output);

    if (buf == NULL) {
        return;
    }

    pixman_image_t *fill = pixman_image_create_solid_fill(&color);

    pixman_image_composite(
        PIXMAN_OP_SRC,
        fill, NULL, buf->pix, 0, 0, 0, 0, 0, 0,
        width, height);

    pixman_image_unref(fill);

    wl_surface_attach(output->surf, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(output->surf, 0, 0, width, height);
    wl_surface_commit(output->surf);
}

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t w, uint32_t h)
{
    struct output *output = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    /* If the size of the last committed buffer has not change, do not
     * render a new buffer because it will be identical to the old one. */
    if (output->configured &&
        output->render_width == w &&
        output->render_height == h) {
        wl_surface_commit(output->surf);
        return;
    }

    output->render_width = w;
    output->render_height = h;
    output->configured = true;
    render(output);
}

static void output_layer_destroy(struct output *output)
{
    if (output->layer != NULL) {
        zwlr_layer_surface_v1_destroy(output->layer);
    }
    if (output->surf != NULL) {
        wl_surface_destroy(output->surf);
    }

    output->layer = NULL;
    output->surf = NULL;
    output->configured = false;
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    struct output *output = data;

    /* Don’t trust ‘output’ to be valid, in case compositor destroyed
     * if before calling closed() */
    tll_foreach(outputs, it) {
        if (&it->item == output) {
            output_layer_destroy(output);
            break;
        }
    }
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = &layer_surface_configure,
    .closed = &layer_surface_closed,
};

static void output_destroy(struct output *output)
{
    output_layer_destroy(output);

    if (output->wl_output != NULL) {
        wl_output_release(output->wl_output);
    }
    output->wl_output = NULL;

    free(output->make);
    free(output->model);
}

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                            int32_t physical_width, int32_t physical_height,
                            int32_t subpixel, const char *make, const char *model,
                            int32_t transform)
{
    struct output *output = data;

    free(output->make);
    free(output->model);

    output->make = (make != NULL) ? strdup(make) : NULL;
    output->model = (model != NULL) ? strdup(model) : NULL;
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) {
        return;
    }

    struct output *output = data;
    output->width = width;
    output->height = height;
}

static void output_done(void *data, struct wl_output *wl_output)
{
    struct output *output = data;
    const int width = output->width;
    const int height = output->height;

    LOG_INFO("output: %s %s (%dx%d)",
             output->make, output->model, width, height);
}

static void output_scale(void*, struct wl_output*, int32_t)
{
    // scale is hardcoded to 1
}

static const struct wl_output_listener output_listener = {
    .geometry = &output_geometry,
    .mode = &output_mode,
    .done = &output_done,
    .scale = &output_scale,
};

static void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
    if (format == WL_SHM_FORMAT_XRGB8888) {
        have_xrgb8888 = true;
    }
}

static const struct wl_shm_listener shm_listener = {
    .format = &shm_format,
};

static void add_surface_to_output(struct output *output)
{
    if (compositor == NULL || layer_shell == NULL) {
        return;
    }

    if (output->surf != NULL) {
        return;
    }

    struct wl_surface *surf = wl_compositor_create_surface(compositor);

    /* Default input region is 'infinite', while we want it to be empty */
    struct wl_region *empty_region = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(surf, empty_region);
    wl_region_destroy(empty_region);

    /* Surface is fully opaque (i.e. non-transparent) */
    struct wl_region *opaque_region = wl_compositor_create_region(compositor);
    wl_surface_set_opaque_region(surf, opaque_region);
    wl_region_destroy(opaque_region);

    struct zwlr_layer_surface_v1 *layer = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surf, output->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");

    zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
    zwlr_layer_surface_v1_set_anchor(layer,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);

    output->surf = surf;
    output->layer = layer;

    zwlr_layer_surface_v1_add_listener(layer, &layer_surface_listener, output);
    wl_surface_commit(surf);
}

static bool verify_iface_version(const char *iface, uint32_t version, uint32_t wanted)
{
    if (version >= wanted) {
        return true;
    }

    LOG_ERR("%s: need interface version %u, but compositor only implements %u",
            iface, wanted, version);
    return false;
}

static void handle_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        const uint32_t required = 4;
        if (!verify_iface_version(interface, version, required)) {
            return;
        }

        compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, required);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required)) {
            return;
        }

        shm = wl_registry_bind(
            registry, name, &wl_shm_interface, required);
        wl_shm_add_listener(shm, &shm_listener, NULL);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        const uint32_t required = 3;
        if (!verify_iface_version(interface, version, required)) {
            return;
        }

        struct wl_output *wl_output = wl_registry_bind(
            registry, name, &wl_output_interface, required);

        tll_push_back(
            outputs, ((struct output){
            .wl_output = wl_output, .wl_name = name,
            .surf = NULL, .layer = NULL
        }));

        struct output *output = &tll_back(outputs);
        wl_output_add_listener(wl_output, &output_listener, output);
        add_surface_to_output(output);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        const uint32_t required = 2;
        if (!verify_iface_version(interface, version, required)) {
            return;
        }

        layer_shell = wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, required);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    tll_foreach(outputs, it) {
        if (it->item.wl_name == name) {
            LOG_DEBUG("destroyed: %s %s", it->item.make, it->item.model);
            output_destroy(&it->item);
            tll_remove(outputs, it);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = &handle_global,
    .global_remove = &handle_global_remove,
};

static pixman_color_t parse_color(const char *hex_color)
{
    if (strlen(hex_color) != 7 || hex_color[0] != '#') {
        LOG_ERR("Invalid input format");
        return (pixman_color_t) { 0, 0, 0, 0xffff };
    }

    unsigned int r;
    unsigned int g;
    unsigned int b;

    sscanf(hex_color + 1, "%02x%02x%02x", &r, &g, &b);

    return (pixman_color_t){
               .red   = (uint16_t)(r * 0x0101),
               .green = (uint16_t)(g * 0x0101),
               .blue  = (uint16_t)(b * 0x0101),
               .alpha = 0xffff,
    };
}

int main(int argc, const char *const *argv)
{
    if (argc > 1) {
        color = parse_color(argv[1]);
    }

    setlocale(LC_CTYPE, "");

    LOG_INFO("%s v%s", argv[0], WBG_VERSION);

    int exit_code = EXIT_FAILURE;
    int sig_fd = -1;

    struct wl_display *display = wl_display_connect(NULL);
    if (display == NULL) {
        LOG_ERR("failed to connect to wayland; no compositor running?");
        goto out;
    }

    static struct wl_registry *registry; /* for some reason, without this
                                          *   static, compiler warns about
                                          *   uninitialized value...
                                          */
    registry = wl_display_get_registry(display);
    if (registry == NULL) {
        LOG_ERR("failed to get wayland registry");
        goto out;
    }

    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (compositor == NULL) {
        LOG_ERR("no compositor");
        goto out;
    }
    if (shm == NULL) {
        LOG_ERR("no shared memory buffers interface");
        goto out;
    }
    if (layer_shell == NULL) {
        LOG_ERR("no layer shell interface");
        goto out;
    }

    tll_foreach(outputs, it)
    add_surface_to_output(&it->item);

    wl_display_roundtrip(display);

    if (!have_xrgb8888) {
        LOG_ERR("shm: XRGB image format not available");
        goto out;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGQUIT);

    sigprocmask(SIG_BLOCK, &mask, NULL);

    if ((sig_fd = signalfd(-1, &mask, 0)) < 0) {
        LOG_ERRNO("failed to create signal FD");
        goto out;
    }

    while (true) {
        wl_display_flush(display);

        struct pollfd fds[] = {
            { .fd = wl_display_get_fd(display), .events = POLLIN },
            { .fd = sig_fd, .events = POLLIN },
        };
        int ret = poll(fds, sizeof (fds) / sizeof (fds[0]), -1);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            LOG_ERRNO("failed to poll");
            break;
        }

        if (fds[0].revents & POLLHUP) {
            LOG_WARN("disconnected by compositor");
            break;
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_dispatch(display) < 0) {
                LOG_ERRNO("failed to dispatch Wayland events");
                break;
            }
        }

        if (fds[1].revents & POLLHUP) {
            abort();
        }

        if (fds[1].revents & POLLIN) {
            struct signalfd_siginfo info;
            ssize_t count = read(sig_fd, &info, sizeof (info));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }

                LOG_ERRNO("failed to read from signal FD");
                break;
            }

            assert(count == sizeof (info));
            assert(info.ssi_signo == SIGINT || info.ssi_signo == SIGQUIT);

            LOG_INFO("goodbye");
            exit_code = EXIT_SUCCESS;
            break;
        }
    }

out:

    if (sig_fd >= 0) {
        close(sig_fd);
    }

    tll_foreach(outputs, it)
    output_destroy(&it->item);
    tll_free(outputs);

    if (layer_shell != NULL) {
        zwlr_layer_shell_v1_destroy(layer_shell);
    }
    if (shm != NULL) {
        wl_shm_destroy(shm);
    }
    if (compositor != NULL) {
        wl_compositor_destroy(compositor);
    }
    if (registry != NULL) {
        wl_registry_destroy(registry);
    }
    if (display != NULL) {
        wl_display_disconnect(display);
    }

    return exit_code;
}
