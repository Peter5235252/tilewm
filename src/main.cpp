// tilewm - Phase 1 bring-up: a minimal nested wlroots 0.20 compositor.
//
// What works in this phase:
//   * backend autocreate (nested Wayland/X11 window under WSLg, DRM on real hw)
//   * GLES2 renderer + allocator, scene-graph rendering with per-frame commit
//   * single-layout output handling, software cursor with xcursor theme
//   * xdg-shell toplevels shown floating, click-to-focus, Alt+Return spawns
//     a terminal, Alt+Q closes the focused window, Alt+Shift+E quits.
//
// Next phase: replace the floating placement with the master-stack layout
// from tiling.hpp and add workspaces.

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

// wlroots is a C library whose headers carry no `extern "C"` guards, so
// force C linkage here. They also use C-only `static` array bounds (e.g. in
// wlr_scene.h, wlr/render/color.h) which C++ rejects, so neutralize the
// keyword for these headers only. Benign: it just drops `static` from
// `static inline` helpers and `static const` constants, which remain valid.
#define static
extern "C" {
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
}
#undef static

#include <algorithm>
#include <vector>

namespace {

struct Server;
struct View;

struct Output {
    Server *server = nullptr;
    struct wlr_output *wlr_output = nullptr;
    struct wl_listener frame{};
    struct wl_listener destroy{};
};

struct View {
    Server *server = nullptr;
    struct wlr_xdg_toplevel *toplevel = nullptr;
    struct wlr_scene_tree *scene_tree = nullptr;
    // Floating position in layout coordinates (tiling phase will drive this).
    int x = 0;
    int y = 0;
    bool mapped = false;
    struct wl_listener map{};
    struct wl_listener unmap{};
    struct wl_listener commit{};
    struct wl_listener destroy{};
};

// Per-keyboard state: owns the listeners so wl_container_of can reach both
// the wlr_keyboard and our server from any keyboard event. The destroy
// listener hangs off the input device, which is what emits destroy.
struct Keyboard {
    Server *server = nullptr;
    struct wlr_keyboard *kbd = nullptr;
    struct wlr_input_device *device = nullptr;
    struct wl_listener key{};
    struct wl_listener modifiers{};
    struct wl_listener destroy{};
};

// Cursor event listeners; owned by the Server for its whole lifetime.
struct CursorEvents {
    Server *server = nullptr;
    struct wl_listener motion{};
    struct wl_listener motion_absolute{};
    struct wl_listener button{};
    struct wl_listener axis{};
    struct wl_listener frame{};
};

struct Server {
    struct wl_display *display = nullptr;
    struct wlr_backend *backend = nullptr;
    struct wlr_session *session = nullptr;
    struct wlr_renderer *renderer = nullptr;
    struct wlr_allocator *allocator = nullptr;
    struct wlr_scene *scene = nullptr;
    struct wlr_scene_output_layout *scene_layout = nullptr;
    struct wlr_output_layout *output_layout = nullptr;
    struct wlr_xdg_shell *xdg_shell = nullptr;
    struct wlr_cursor *cursor = nullptr;
    struct wlr_xcursor_manager *cursor_mgr = nullptr;
    struct wlr_seat *seat = nullptr;
    CursorEvents cursor_events{};

    struct wl_listener new_output{};
    struct wl_listener new_input{};
    struct wl_listener new_toplevel{};
    struct wl_listener request_cursor{};

    std::vector<Output *> outputs;
    // Front of the vector is topmost (most recently focused).
    std::vector<View *> views;

    const char *socket = nullptr;
};

void focus_view(Server *server, View *view);

void spawn_terminal() {
    if (fork() == 0) {
        setsid();
        execlp("foot", "foot", nullptr);
        execlp("weston-terminal", "weston-terminal", nullptr);
        _exit(EXIT_FAILURE);
    }
}

View *view_at(Server *server, double lx, double ly) {
    for (View *view : server->views) {
        if (!view->mapped) {
            continue;
        }
        struct wlr_box geom = view->toplevel->base->geometry;
        const int x = view->x + geom.x;
        const int y = view->y + geom.y;
        if (lx >= x && lx < x + geom.width && ly >= y && ly < y + geom.height) {
            return view;
        }
    }
    return nullptr;
}

void focus_view(Server *server, View *view) {
    struct wlr_surface *prev_surface = server->seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = view != nullptr ? view->toplevel->base->surface : nullptr;
    if (prev_surface == surface) {
        return;
    }
    if (prev_surface != nullptr) {
        struct wlr_xdg_toplevel *prev_top =
            wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_top != nullptr) {
            wlr_xdg_toplevel_set_activated(prev_top, false);
        }
    }
    if (view == nullptr) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
        return;
    }
    // Raise to the top both in the scene and in our focus order.
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    auto it = std::find(server->views.begin(), server->views.end(), view);
    if (it != server->views.end()) {
        server->views.erase(it);
        server->views.insert(server->views.begin(), view);
    }
    wlr_xdg_toplevel_set_activated(view->toplevel, true);
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    if (keyboard != nullptr) {
        wlr_seat_keyboard_notify_enter(server->seat, surface, keyboard->keycodes,
            keyboard->num_keycodes, &keyboard->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(server->seat, surface, nullptr, 0, nullptr);
    }
}

void on_view_map(struct wl_listener *listener, void * /*data*/) {
    View *view = wl_container_of(listener, view, map);
    Server *server = view->server;
    view->mapped = true;
    // Phase 1: cascade floating placement so new windows don't fully overlap.
    static int cascade = 0;
    view->x = 40 + (cascade % 8) * 32;
    view->y = 40 + (cascade % 8) * 24;
    ++cascade;
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    wlr_scene_node_set_enabled(&view->scene_tree->node, true);
    focus_view(server, view);
}

void on_view_unmap(struct wl_listener *listener, void * /*data*/) {
    View *view = wl_container_of(listener, view, unmap);
    view->mapped = false;
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);
}

void on_view_commit(struct wl_listener *listener, void * /*data*/) {
    View *view = wl_container_of(listener, view, commit);
    if (view->toplevel->base->initial_commit) {
        // Suggest a default size; the tiling phase will set this per layout.
        wlr_xdg_toplevel_set_size(view->toplevel, 640, 480);
    }
}

void on_view_destroy(struct wl_listener *listener, void * /*data*/) {
    View *view = wl_container_of(listener, view, destroy);
    Server *server = view->server;
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->destroy.link);
    auto it = std::find(server->views.begin(), server->views.end(), view);
    if (it != server->views.end()) {
        server->views.erase(it);
    }
    if (!server->views.empty()) {
        focus_view(server, server->views.front());
    }
    delete view;
}

void on_new_toplevel(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_toplevel);
    struct wlr_xdg_toplevel *toplevel = static_cast<struct wlr_xdg_toplevel *>(data);

    View *view = new View();
    view->server = server;
    view->toplevel = toplevel;
    view->scene_tree =
        wlr_scene_xdg_surface_create(&server->scene->tree, toplevel->base);
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);

    view->map.notify = on_view_map;
    wl_signal_add(&toplevel->base->surface->events.map, &view->map);
    view->unmap.notify = on_view_unmap;
    wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
    view->commit.notify = on_view_commit;
    wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
    view->destroy.notify = on_view_destroy;
    wl_signal_add(&toplevel->events.destroy, &view->destroy);

    server->views.push_back(view);
}

bool handle_keybinding(Server *server, xkb_keysym_t sym, uint32_t modifiers) {
    const bool alt = (modifiers & WLR_MODIFIER_ALT) != 0;
    const bool shift = (modifiers & WLR_MODIFIER_SHIFT) != 0;
    const bool ctrl = (modifiers & WLR_MODIFIER_CTRL) != 0;

    if (alt && sym == XKB_KEY_Return) {
        spawn_terminal();
        return true;
    }
    if (alt && !shift && (sym == XKB_KEY_q || sym == XKB_KEY_Q)) {
        if (!server->views.empty() && server->views.front()->mapped) {
            wlr_xdg_toplevel_send_close(server->views.front()->toplevel);
        }
        return true;
    }
    if (alt && shift && (sym == XKB_KEY_E || sym == XKB_KEY_e)) {
        wl_display_terminate(server->display);
        return true;
    }
    if (ctrl && alt && (sym == XKB_KEY_q || sym == XKB_KEY_Q)) {
        wl_display_terminate(server->display);
        return true;
    }
    return false;
}

void on_keyboard_key(struct wl_listener *listener, void *data) {
    Keyboard *kb = wl_container_of(listener, kb, key);
    Server *server = kb->server;
    struct wlr_keyboard_key_event *event =
        static_cast<struct wlr_keyboard_key_event *>(data);

    const uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms = nullptr;
    int nsyms = xkb_state_key_get_syms(kb->kbd->xkb_state, keycode, &syms);

    bool handled = false;
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        const uint32_t modifiers = wlr_keyboard_get_modifiers(kb->kbd);
        for (int i = 0; i < nsyms && !handled; ++i) {
            handled = handle_keybinding(server, syms[i], modifiers);
        }
    }
    if (!handled) {
        wlr_seat_set_keyboard(server->seat, kb->kbd);
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode,
            event->state);
    }
}

void on_keyboard_modifiers(struct wl_listener *listener, void * /*data*/) {
    Keyboard *kb = wl_container_of(listener, kb, modifiers);
    wlr_seat_set_keyboard(kb->server->seat, kb->kbd);
    wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->kbd->modifiers);
}

void on_keyboard_destroy(struct wl_listener *listener, void * /*data*/) {
    Keyboard *kb = wl_container_of(listener, kb, destroy);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->destroy.link);
    delete kb;
}

void setup_keyboard(Server *server, struct wlr_input_device *device) {
    struct wlr_keyboard *kbd = wlr_keyboard_from_input_device(device);

    struct xkb_rule_names rules{};
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(kbd, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(kbd, 25, 600);

    Keyboard *kb = new Keyboard();
    kb->server = server;
    kb->kbd = kbd;
    kb->device = device;
    kb->key.notify = on_keyboard_key;
    wl_signal_add(&kbd->events.key, &kb->key);
    kb->modifiers.notify = on_keyboard_modifiers;
    wl_signal_add(&kbd->events.modifiers, &kb->modifiers);
    kb->destroy.notify = on_keyboard_destroy;
    wl_signal_add(&device->events.destroy, &kb->destroy);

    wlr_seat_set_keyboard(server->seat, kbd);
}

void on_cursor_motion(struct wl_listener *listener, void *data) {
    CursorEvents *ce = wl_container_of(listener, ce, motion);
    Server *server = ce->server;
    auto *event = static_cast<struct wlr_pointer_motion_event *>(data);
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    wlr_seat_pointer_notify_motion(server->seat, event->time_msec, server->cursor->x,
        server->cursor->y);
}

void on_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    CursorEvents *ce = wl_container_of(listener, ce, motion_absolute);
    Server *server = ce->server;
    auto *event = static_cast<struct wlr_pointer_motion_absolute_event *>(data);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    wlr_seat_pointer_notify_motion(server->seat, event->time_msec, server->cursor->x,
        server->cursor->y);
}

void on_cursor_button(struct wl_listener *listener, void *data) {
    CursorEvents *ce = wl_container_of(listener, ce, button);
    Server *server = ce->server;
    auto *event = static_cast<struct wlr_pointer_button_event *>(data);
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button,
        event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        View *view = view_at(server, server->cursor->x, server->cursor->y);
        focus_view(server, view);
    }
}

void on_cursor_axis(struct wl_listener *listener, void *data) {
    CursorEvents *ce = wl_container_of(listener, ce, axis);
    Server *server = ce->server;
    auto *event = static_cast<struct wlr_pointer_axis_event *>(data);
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
        event->delta, event->delta_discrete, event->source,
        event->relative_direction);
}

void on_cursor_frame(struct wl_listener *listener, void * /*data*/) {
    CursorEvents *ce = wl_container_of(listener, ce, frame);
    wlr_seat_pointer_notify_frame(ce->server->seat);
}

void on_request_cursor(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, request_cursor);
    auto *event = static_cast<struct wlr_seat_pointer_request_set_cursor_event *>(data);
    if (event->seat_client == server->seat->pointer_state.focused_client) {
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x,
            event->hotspot_y);
    }
}

void on_new_input(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = static_cast<struct wlr_input_device *>(data);
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        setup_keyboard(server, device);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
        wlr_cursor_attach_input_device(server->cursor, device);
    }
}

void on_output_frame(struct wl_listener *listener, void * /*data*/) {
    Output *output = wl_container_of(listener, output, frame);
    Server *server = output->server;

    struct wlr_scene_output *scene_output =
        wlr_scene_get_scene_output(server->scene, output->wlr_output);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    if (!wlr_scene_output_build_state(scene_output, &state, nullptr)) {
        wlr_output_state_finish(&state);
        return;
    }
    if (!wlr_output_commit_state(output->wlr_output, &state)) {
        wlr_log(WLR_ERROR, "output commit failed");
    }
    wlr_output_state_finish(&state);
}

void on_output_destroy(struct wl_listener *listener, void * /*data*/) {
    Output *output = wl_container_of(listener, output, destroy);
    Server *server = output->server;
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    auto it = std::find(server->outputs.begin(), server->outputs.end(), output);
    if (it != server->outputs.end()) {
        server->outputs.erase(it);
    }
    delete output;
}

void on_new_output(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = static_cast<struct wlr_output *>(data);

    // Since wlroots 0.19 outputs must be explicitly bound to the renderer
    // and allocator before anything (including the software cursor) can
    // submit buffers to them.
    if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
        wlr_log(WLR_ERROR, "failed to init output rendering");
        return;
    }

    // Give the host-side window (the nested output under WSLg/X11) real
    // metadata. Without a title/app_id, WSLg's RAIL shell only gets a bare
    // entry that shows on the taskbar but won't restore or focus properly.
    if (wlr_output_is_wl(wlr_output)) {
        wlr_wl_output_set_title(wlr_output, "tilewm");
        wlr_wl_output_set_app_id(wlr_output, "tilewm");
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != nullptr) {
        wlr_output_state_set_mode(&state, mode);
    }
    if (!wlr_output_commit_state(wlr_output, &state)) {
        wlr_log(WLR_ERROR, "initial output commit failed");
    }
    wlr_output_state_finish(&state);

    wlr_output_layout_add_auto(server->output_layout, wlr_output);

    Output *output = new Output();
    output->server = server;
    output->wlr_output = wlr_output;
    output->frame.notify = on_output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->destroy.notify = on_output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);
    server->outputs.push_back(output);

    wlr_scene_output_create(server->scene, wlr_output);
}

} // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    wlr_log_init(WLR_DEBUG, nullptr);

    Server server{};
    server.display = wl_display_create();
    assert(server.display && "wl_display_create failed");

    struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_backend_autocreate(loop, &server.session);
    if (server.backend == nullptr) {
        std::fprintf(stderr, "tilewm: failed to create backend\n");
        return 1;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (server.renderer == nullptr) {
        std::fprintf(stderr, "tilewm: failed to create renderer\n");
        return 1;
    }
    if (!wlr_renderer_init_wl_display(server.renderer, server.display)) {
        std::fprintf(stderr, "tilewm: failed to init renderer display\n");
        return 1;
    }

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (server.allocator == nullptr) {
        std::fprintf(stderr, "tilewm: failed to create allocator\n");
        return 1;
    }

    wlr_compositor_create(server.display, 6, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    server.output_layout = wlr_output_layout_create(server.display);
    server.scene = wlr_scene_create();
    server.scene_layout =
        wlr_scene_attach_output_layout(server.scene, server.output_layout);

    server.xdg_shell = wlr_xdg_shell_create(server.display, 6);
    server.new_toplevel.notify = on_new_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_toplevel);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    server.cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);

    server.cursor_events.server = &server;
    server.cursor_events.motion.notify = on_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_events.motion);
    server.cursor_events.motion_absolute.notify = on_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute,
        &server.cursor_events.motion_absolute);
    server.cursor_events.button.notify = on_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_events.button);
    server.cursor_events.axis.notify = on_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_events.axis);
    server.cursor_events.frame.notify = on_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_events.frame);

    server.seat = wlr_seat_create(server.display, "seat0");
    server.request_cursor.notify = on_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);

    server.new_input.notify = on_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.new_output.notify = on_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.socket = wl_display_add_socket_auto(server.display);
    if (server.socket == nullptr) {
        std::fprintf(stderr, "tilewm: failed to create Wayland socket\n");
        return 1;
    }
    std::fprintf(stderr, "tilewm: running on WAYLAND_DISPLAY=%s\n", server.socket);

    if (!wlr_backend_start(server.backend)) {
        std::fprintf(stderr, "tilewm: failed to start backend\n");
        return 1;
    }

    wl_display_run(server.display);

    wl_display_destroy_clients(server.display);
    wl_display_destroy(server.display);
    return 0;
}
