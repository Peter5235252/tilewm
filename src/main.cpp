// tilewm - Phase 3a: Lua-configured master-stack tiling compositor.
// Settings (gaps, mfact, nmaster, workspaces) and all keybindings come from
// ~/.config/tilewm/init.lua (see examples/init.lua), reloadable via
// Alt+Shift+R or SIGHUP; built-in defaults apply when missing or broken.
// Pointer: click focuses, Alt+Left-drag moves (floating tiled windows
// first), Alt+Right-drag resizes, with a default xcursor otherwise.
//
// What works in this phase:
//   * backend autocreate (nested Wayland/X11 window under WSLg, DRM on real hw)
//   * GLES2 renderer + allocator, scene-graph rendering with per-frame commit
//   * single-layout output handling, software cursor with xcursor theme
//   * xdg-shell toplevels arranged in a master-stack layout, click-to-focus,
//     Alt+Return spawns a terminal, Alt+J/K cycles focus, Alt+Space toggles
//     floating, Alt+1..4 switches between 4 workspaces, Alt+Shift+1..4 moves
//     the focused window, Alt+Q closes, Alt+Shift+E quits.

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <linux/input-event-codes.h>
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
#include <wlr/util/box.h>
#include <wlr/util/log.h>
}
#undef static

#include <algorithm>
#include <vector>

#include <drm_fourcc.h>

#include "config.hpp"
#include "tiling.hpp"
#include "wallpaper.hpp"

namespace {

struct Server;
struct View;

struct Output {
    Server *server = nullptr;
    struct wlr_output *wlr_output = nullptr;
    struct wlr_scene_buffer *bg = nullptr; // wallpaper node (bottom layer)
    int bg_w = -1;
    int bg_h = -1;
    struct wl_listener frame{};
    struct wl_listener destroy{};
};

struct View {
    Server *server = nullptr;
    struct wlr_xdg_toplevel *toplevel = nullptr;
    struct wlr_scene_tree *scene_tree = nullptr;
    // Tiling state: position in layout coordinates, floating override,
    // workspace index, and last size we configured (to avoid loops).
    int x = 0;
    int y = 0;
    bool floating = false;
    int workspace = 0;
    int applied_w = 0;
    int applied_h = 0;
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

// What the pointer is currently doing: passing events through, or
// dragging a grabbed view.
enum class CursorMode {
    Passthrough,
    Move,
    Resize,
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

// Shared wallpaper image uploaded once into an allocator buffer.
struct Wallpaper {
    struct wlr_buffer *buffer = nullptr;
    int img_w = 0;
    int img_h = 0;
    std::string tried_path; // last path we attempted (avoid open() per frame)
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
    CursorMode cursor_mode = CursorMode::Passthrough;
    View *grabbed_view = nullptr;
    uint32_t grab_button = 0;
    double grab_lx = 0;
    double grab_ly = 0;
    int grab_vx = 0;
    int grab_vy = 0;
    int grab_vw = 0;
    int grab_vh = 0;
    bool cursor_is_default = true;

    struct wl_listener new_output{};
    struct wl_listener new_input{};
    struct wl_listener new_toplevel{};
    struct wl_listener request_cursor{};
    struct wl_listener backend_destroy{};

    std::vector<Output *> outputs;
    // Front of the vector is topmost (most recently focused).
    std::vector<View *> views;

    int active_workspace = 0;
    tilewm::Config config;
    std::string config_path;
    Wallpaper wallpaper;

    const char *socket = nullptr;
};

void focus_view(Server *server, View *view);
void drop_wallpaper(Server *server);

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
        if (!view->mapped || view->workspace != server->active_workspace) {
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

// Tile all mapped, non-floating views of the active workspace using the
// master-stack layout. Oldest window becomes master for a stable layout.
void arrange(Server *server) {
    if (server->outputs.empty()) {
        return;
    }
    struct wlr_box area{};
    wlr_output_layout_get_box(server->output_layout,
        server->outputs.front()->wlr_output, &area);
    const int gaps = server->config.gaps;
    area.x += gaps;
    area.y += gaps;
    area.width -= 2 * gaps;
    area.height -= 2 * gaps;
    if (area.width <= 0 || area.height <= 0) {
        return;
    }
    std::vector<View *> tiled;
    for (auto it = server->views.rbegin(); it != server->views.rend(); ++it) {
        View *v = *it;
        if (v->mapped && !v->floating && v->workspace == server->active_workspace) {
            tiled.push_back(v);
        }
    }
    auto boxes = tilewm::master_stack(static_cast<int>(tiled.size()),
        tilewm::Box{area.x, area.y, area.width, area.height},
        server->config.nmaster, server->config.mfact);
    for (std::size_t i = 0; i < tiled.size(); ++i) {
        View *v = tiled[i];
        int w = boxes[i].w - 2 * gaps;
        int h = boxes[i].h - 2 * gaps;
        if (w < 1) {
            w = 1;
        }
        if (h < 1) {
            h = 1;
        }
        v->x = boxes[i].x + gaps;
        v->y = boxes[i].y + gaps;
        wlr_scene_node_set_position(&v->scene_tree->node, v->x, v->y);
        if (v->applied_w != w || v->applied_h != h) {
            v->applied_w = w;
            v->applied_h = h;
            wlr_xdg_toplevel_set_size(v->toplevel, w, h);
        }
    }
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

// Most recently focused mapped view on the active workspace, if any.
View *top_visible(Server *server) {
    for (View *v : server->views) {
        if (v->mapped && v->workspace == server->active_workspace) {
            return v;
        }
    }
    return nullptr;
}

void switch_workspace(Server *server, int ws) {
    if (ws < 0 || ws >= server->config.workspaces ||
        ws == server->active_workspace) {
        return;
    }
    server->active_workspace = ws;
    for (View *v : server->views) {
        wlr_scene_node_set_enabled(&v->scene_tree->node,
            v->mapped && v->workspace == ws);
    }
    arrange(server);
    focus_view(server, top_visible(server));
}

void focus_cycle(Server *server, int dir) {
    std::vector<View *> vis;
    for (View *v : server->views) {
        if (v->mapped && v->workspace == server->active_workspace) {
            vis.push_back(v);
        }
    }
    if (vis.empty()) {
        return;
    }
    struct wlr_surface *cur = server->seat->keyboard_state.focused_surface;
    std::size_t idx = 0;
    bool found = false;
    for (std::size_t i = 0; i < vis.size(); ++i) {
        if (vis[i]->toplevel->base->surface == cur) {
            idx = i;
            found = true;
            break;
        }
    }
    if (!found) {
        focus_view(server, vis.front());
        return;
    }
    idx = (idx + static_cast<std::size_t>(dir) + vis.size()) % vis.size();
    focus_view(server, vis[idx]);
}

void on_view_map(struct wl_listener *listener, void * /*data*/) {
    View *view = wl_container_of(listener, view, map);
    Server *server = view->server;
    view->mapped = true;
    view->workspace = server->active_workspace;
    wlr_scene_node_set_enabled(&view->scene_tree->node, true);
    arrange(server);
    focus_view(server, view);
}

void on_view_unmap(struct wl_listener *listener, void * /*data*/) {
    View *view = wl_container_of(listener, view, unmap);
    Server *server = view->server;
    view->mapped = false;
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    arrange(server);
    if (server->seat->keyboard_state.focused_surface ==
        view->toplevel->base->surface) {
        focus_view(server, top_visible(server));
    }
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
    // Exclude the dying view from layout before arranging: configuring a
    // toplevel from inside its own destroy event would use-after-free.
    view->mapped = false;
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->destroy.link);
    auto it = std::find(server->views.begin(), server->views.end(), view);
    if (it != server->views.end()) {
        server->views.erase(it);
    }
    arrange(server);
    // The toplevel is going away: never let it keep keyboard focus, and
    // don't dereference its surface below (it may already be half-torn-down).
    if (server->seat->keyboard_state.focused_surface != nullptr) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
    focus_view(server, top_visible(server));
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

View *focused_view(Server *server) {
    struct wlr_surface *s = server->seat->keyboard_state.focused_surface;
    if (s == nullptr) {
        return nullptr;
    }
    for (View *v : server->views) {
        if (v->mapped && v->toplevel->base->surface == s) {
            return v;
        }
    }
    return nullptr;
}

uint32_t wlr_to_tile_mods(uint32_t wlr_mods) {
    uint32_t mods = 0;
    if (wlr_mods & WLR_MODIFIER_SHIFT) {
        mods |= tilewm::MOD_SHIFT;
    }
    if (wlr_mods & WLR_MODIFIER_CTRL) {
        mods |= tilewm::MOD_CTRL;
    }
    if (wlr_mods & WLR_MODIFIER_ALT) {
        mods |= tilewm::MOD_ALT;
    }
    if (wlr_mods & WLR_MODIFIER_LOGO) {
        mods |= tilewm::MOD_SUPER;
    }
    return mods;
}

// Re-read the config file and apply it: fix up workspace assignments,
// refresh visibility, re-tile, refocus. Keeps the old config on failure.
bool reload_config(Server *server) {
    tilewm::Config next = server->config;
    std::string error;
    if (!tilewm::load_config_file(server->config_path.c_str(), next, error)) {
        wlr_log(WLR_ERROR, "config reload failed (%s): %s",
            server->config_path.c_str(), error.c_str());
        return false;
    }
    server->config = std::move(next);
    if (server->active_workspace >= server->config.workspaces) {
        server->active_workspace = server->config.workspaces - 1;
    }
    for (View *v : server->views) {
        if (v->workspace >= server->config.workspaces) {
            v->workspace = 0;
        }
        wlr_scene_node_set_enabled(&v->scene_tree->node,
            v->mapped && v->workspace == server->active_workspace);
    }
    arrange(server);
    focus_view(server, top_visible(server));
    wlr_log(WLR_INFO, "config reloaded: gaps=%d mfact=%.2f nmaster=%d "
            "workspaces=%d binds=%zu",
        server->config.gaps, static_cast<double>(server->config.mfact),
        server->config.nmaster, server->config.workspaces,
        server->config.keys.size());
    // A changed wallpaper path rebuilds lazily on the next frame.
    drop_wallpaper(server);
    return true;
}

void run_action(Server *server, const tilewm::Keybind &bind) {
    const std::string &a = bind.action;
    if (a == "spawn-terminal") {
        spawn_terminal();
    } else if (a == "close") {
        View *focused = focused_view(server);
        if (focused != nullptr) {
            wlr_xdg_toplevel_send_close(focused->toplevel);
        }
    } else if (a == "quit") {
        wl_display_terminate(server->display);
    } else if (a == "focus-next") {
        focus_cycle(server, +1);
    } else if (a == "focus-prev") {
        focus_cycle(server, -1);
    } else if (a == "toggle-floating") {
        View *focused = focused_view(server);
        if (focused != nullptr) {
            focused->floating = !focused->floating;
            // Keep the window where it is and on top while floating.
            wlr_scene_node_raise_to_top(&focused->scene_tree->node);
            arrange(server);
        }
    } else if (a == "workspace") {
        switch_workspace(server, bind.arg - 1);
    } else if (a == "move-to-workspace") {
        View *focused = focused_view(server);
        int ws = bind.arg - 1;
        if (focused != nullptr && ws >= 0 && ws < server->config.workspaces) {
            focused->workspace = ws;
            wlr_scene_node_set_enabled(&focused->scene_tree->node,
                focused->mapped && ws == server->active_workspace);
            arrange(server);
            focus_view(server, top_visible(server));
        }
    } else if (a == "reload-config") {
        reload_config(server);
    }
}

bool handle_keybinding(Server *server, xkb_keysym_t sym, uint32_t modifiers) {
    const uint32_t mods = wlr_to_tile_mods(modifiers);
    for (const tilewm::Keybind &bind : server->config.keys) {
        if (bind.mods == mods && bind.keysym == sym) {
            run_action(server, bind);
            return true;
        }
    }
    return false;
}

int on_reload_signal(int /*signal_number*/, void *data) {
    Server *server = static_cast<Server *>(data);
    reload_config(server);
    return 0;
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

void set_default_cursor(Server *server) {
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
    server->cursor_is_default = true;
}

// Shared tail of both motion handlers: drive an active drag, otherwise
// forward to the seat and restore the default cursor off-client.
void cursor_process_position(Server *server, uint32_t time_msec) {
    if (server->cursor_mode != CursorMode::Passthrough &&
        server->grabbed_view != nullptr) {
        View *view = server->grabbed_view;
        const int dx =
            static_cast<int>(server->cursor->x - server->grab_lx);
        const int dy =
            static_cast<int>(server->cursor->y - server->grab_ly);
        if (server->cursor_mode == CursorMode::Move) {
            view->x = server->grab_vx + dx;
            view->y = server->grab_vy + dy;
            wlr_scene_node_set_position(&view->scene_tree->node, view->x,
                view->y);
        } else {
            int w = server->grab_vw + dx;
            int h = server->grab_vh + dy;
            if (w < 100) {
                w = 100;
            }
            if (h < 100) {
                h = 100;
            }
            wlr_xdg_toplevel_set_size(view->toplevel, w, h);
        }
        return;
    }
    wlr_seat_pointer_notify_motion(server->seat, time_msec, server->cursor->x,
        server->cursor->y);
    if (server->seat->pointer_state.focused_surface == nullptr &&
        !server->cursor_is_default) {
        set_default_cursor(server);
    }
}

void begin_grab(Server *server, View *view, CursorMode mode, uint32_t button) {
    if (!view->floating) {
        // Dragging floats the window first so the tiling layout reflows
        // around the gap it leaves behind.
        view->floating = true;
        arrange(server);
    }
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    server->grabbed_view = view;
    server->grab_button = button;
    server->cursor_mode = mode;
    server->grab_lx = server->cursor->x;
    server->grab_ly = server->cursor->y;
    server->grab_vx = view->x;
    server->grab_vy = view->y;
    server->grab_vw = view->applied_w > 0 ? view->applied_w : 640;
    server->grab_vh = view->applied_h > 0 ? view->applied_h : 480;
}

void on_cursor_motion(struct wl_listener *listener, void *data) {
    CursorEvents *ce = wl_container_of(listener, ce, motion);
    Server *server = ce->server;
    auto *event = static_cast<struct wlr_pointer_motion_event *>(data);
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    cursor_process_position(server, event->time_msec);
}

void on_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    CursorEvents *ce = wl_container_of(listener, ce, motion_absolute);
    Server *server = ce->server;
    auto *event = static_cast<struct wlr_pointer_motion_absolute_event *>(data);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    cursor_process_position(server, event->time_msec);
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
        struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
        const uint32_t mods =
            kbd != nullptr ? wlr_keyboard_get_modifiers(kbd) : 0;
        if (view != nullptr && (mods & WLR_MODIFIER_ALT) != 0) {
            if (event->button == BTN_LEFT) {
                begin_grab(server, view, CursorMode::Move, event->button);
            } else if (event->button == BTN_RIGHT) {
                begin_grab(server, view, CursorMode::Resize, event->button);
            }
        }
    } else if (event->button == server->grab_button) {
        server->cursor_mode = CursorMode::Passthrough;
        server->grabbed_view = nullptr;
        server->grab_button = 0;
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
        server->cursor_is_default = false;
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

// If the backend dies (e.g. the host disconnects our nested window),
// leave the event loop so main() can tear down in order instead of
// tripping wlroots' listener-list assertions during display destroy.
// NOTE: this listener removes itself: it fires from inside backend
// destruction, so it must already be detached when wlr_backend_finish
// runs its empty-list assertions afterwards.
void on_backend_destroy(struct wl_listener *listener, void * /*data*/) {
    Server *server = wl_container_of(listener, server, backend_destroy);
    wl_list_remove(&listener->link);
    wlr_log(WLR_ERROR, "backend destroyed; shutting down");
    wl_display_terminate(server->display);
}

// GPU-side wallpaper upload for buffers that refuse CPU mapping (typical
// for GBM/dmabuf on real hardware): push pixels into a texture, then blit
// it into the destination buffer with a throwaway render pass.
bool blit_wallpaper_gpu(Server *server, const uint8_t *rgba, int iw, int ih,
    struct wlr_buffer *buf) {
    struct wlr_texture *tex = wlr_texture_from_pixels(server->renderer,
        DRM_FORMAT_ABGR8888, static_cast<uint32_t>(iw * 4),
        static_cast<uint32_t>(iw), static_cast<uint32_t>(ih), rgba);
    if (tex == nullptr) {
        return false;
    }
    struct wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(server->renderer, buf, nullptr);
    if (pass == nullptr) {
        wlr_texture_destroy(tex);
        return false;
    }
    struct wlr_render_texture_options opts{};
    opts.texture = tex;
    opts.dst_box = {0, 0, iw, ih};
    wlr_render_pass_add_texture(pass, &opts);
    bool ok = wlr_render_pass_submit(pass);
    wlr_texture_destroy(tex);
    return ok;
}

// Decode the configured wallpaper and upload it once into a shared XRGB
// allocator buffer. Remembers the last attempted path so a missing file
// costs one open() instead of one per frame.
//
// Two upload paths, in order: a CPU memcpy when the buffer is mappable
// (shm/pixman/dumb allocators), else a GPU blit through the renderer
// (GBM/dmabuf buffers on real hardware generally refuse CPU mapping).
// Anything else degrades to no background instead of crashing.
bool upload_wallpaper(Server *server) {
    if (server->wallpaper.buffer != nullptr) {
        return true;
    }
    std::string path = server->config.wallpaper.empty()
        ? tilewm::default_wallpaper_path()
        : server->config.wallpaper;
    if (path == server->wallpaper.tried_path) {
        return false;
    }
    server->wallpaper.tried_path = path;

    std::vector<uint8_t> rgba;
    int iw = 0, ih = 0;
    std::string error;
    if (!tilewm::decode_image(path.c_str(), rgba, iw, ih, error)) {
        wlr_log(WLR_ERROR, "wallpaper: %s", error.c_str());
        return false;
    }
    static uint64_t mods[] = {DRM_FORMAT_MOD_LINEAR};
    static struct wlr_drm_format fmt = {DRM_FORMAT_XRGB8888, 1, 1, mods};
    struct wlr_buffer *buf =
        wlr_allocator_create_buffer(server->allocator, iw, ih, &fmt);
    if (buf == nullptr) {
        wlr_log(WLR_ERROR, "wallpaper: allocator refused %dx%d buffer", iw, ih);
        return false;
    }
    void *data = nullptr;
    uint32_t format = 0;
    size_t stride = 0;
    bool mapped = wlr_buffer_begin_data_ptr_access(buf,
        WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride);
    if (mapped && format != DRM_FORMAT_XRGB8888) {
        wlr_buffer_end_data_ptr_access(buf);
        mapped = false;
    }
    if (mapped) {
        auto *px = static_cast<uint8_t *>(data);
        for (int y = 0; y < ih; ++y) {
            uint8_t *row = px + static_cast<std::size_t>(y) * stride;
            const uint8_t *src =
                rgba.data() + static_cast<std::size_t>(y) * iw * 4;
            for (int x = 0; x < iw; ++x) {
                row[4 * x + 0] = src[4 * x + 2];
                row[4 * x + 1] = src[4 * x + 1];
                row[4 * x + 2] = src[4 * x + 0];
                row[4 * x + 3] = 0xFF;
            }
        }
        wlr_buffer_end_data_ptr_access(buf);
    } else if (!blit_wallpaper_gpu(server, rgba.data(), iw, ih, buf)) {
        wlr_buffer_drop(buf);
        wlr_log(WLR_ERROR,
            "wallpaper: buffer is neither CPU-mappable nor GPU-blittable; skipping background");
        return false;
    }
    server->wallpaper.buffer = buf;
    server->wallpaper.img_w = iw;
    server->wallpaper.img_h = ih;
    wlr_log(WLR_INFO, "wallpaper: %dx%d from %s", iw, ih, path.c_str());
    return true;
}

void drop_wallpaper(Server *server) {
    for (Output *o : server->outputs) {
        if (o->bg != nullptr) {
            wlr_scene_node_destroy(&o->bg->node);
            o->bg = nullptr;
            o->bg_w = o->bg_h = -1;
        }
    }
    if (server->wallpaper.buffer != nullptr) {
        wlr_buffer_drop(server->wallpaper.buffer);
        server->wallpaper.buffer = nullptr;
    }
    server->wallpaper.img_w = server->wallpaper.img_h = 0;
    server->wallpaper.tried_path.clear();
}

// Keep a cover-fit wallpaper behind everything on this output. Cheap
// per-frame checks; the image uploads once and node creation happens once.
void ensure_wallpaper(Server *server, Output *output) {
    if (!upload_wallpaper(server)) {
        return;
    }
    if (output->bg == nullptr) {
        output->bg = wlr_scene_buffer_create(&server->scene->tree,
            server->wallpaper.buffer);
        if (output->bg == nullptr) {
            return;
        }
    }
    // Always stay behind views, even ones mapped before we existed.
    wlr_scene_node_lower_to_bottom(&output->bg->node);
    int ow = 0, oh = 0;
    wlr_output_effective_resolution(output->wlr_output, &ow, &oh);
    if (ow == output->bg_w && oh == output->bg_h) {
        return;
    }
    const int iw = server->wallpaper.img_w;
    const int ih = server->wallpaper.img_h;
    const float scale =
        std::max(static_cast<float>(ow) / iw, static_cast<float>(oh) / ih);
    const int dw = static_cast<int>(iw * scale + 0.5f);
    const int dh = static_cast<int>(ih * scale + 0.5f);
    struct wlr_box obox{};
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &obox);
    wlr_scene_node_set_position(&output->bg->node, obox.x + (ow - dw) / 2,
        obox.y + (oh - dh) / 2);
    wlr_scene_buffer_set_dest_size(output->bg, dw, dh);
    output->bg_w = ow;
    output->bg_h = oh;
}

void on_output_frame(struct wl_listener *listener, void * /*data*/) {
    Output *output = wl_container_of(listener, output, frame);
    Server *server = output->server;

    ensure_wallpaper(server, output);

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
    if (output->bg != nullptr) {
        wlr_scene_node_destroy(&output->bg->node);
        output->bg = nullptr;
    }
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
    set_default_cursor(server);
    arrange(server);
}

} // namespace

int main(int argc, char **argv) {
    wlr_log_init(WLR_DEBUG, nullptr);

    Server server{};
    server.config = tilewm::default_config();
    server.config_path =
        argc > 1 ? argv[1] : tilewm::default_config_path();
    {
        std::string error;
        if (tilewm::load_config_file(server.config_path.c_str(), server.config,
                error)) {
            wlr_log(WLR_INFO, "loaded config %s", server.config_path.c_str());
        } else {
            wlr_log(WLR_ERROR, "using built-in defaults (%s: %s)",
                server.config_path.c_str(), error.c_str());
        }
        wlr_log(WLR_INFO, "config: gaps=%d mfact=%.2f nmaster=%d workspaces=%d "
                "binds=%zu",
            server.config.gaps, static_cast<double>(server.config.mfact),
            server.config.nmaster, server.config.workspaces,
            server.config.keys.size());
    }
    server.display = wl_display_create();
    assert(server.display && "wl_display_create failed");

    struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
    // SIGHUP reloads the config file without restarting the compositor
    // (Alt+Shift+R does the same from the keyboard).
    wl_event_loop_add_signal(loop, SIGHUP, on_reload_signal, &server);
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
    // A visible pointer from the start: client cursors take over on focus
    // via request_set_cursor, and cursor_process_position restores this
    // whenever the pointer rests on no client surface. Needs an xcursor
    // theme installed (e.g. Adwaita) or nothing shows.
    set_default_cursor(&server);

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
    server.backend_destroy.notify = on_backend_destroy;
    wl_signal_add(&server.backend->events.destroy, &server.backend_destroy);

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

    // Tear down in order: detach our listeners first so backend/display
    // destruction never trips wlroots' listener-list assertions.
    // (backend_destroy detaches itself if/when it fires, including during
    // display destroy on a normal exit, so it is deliberately not removed
    // here.)
    wl_list_remove(&server.new_output.link);
    wl_list_remove(&server.new_input.link);
    wl_list_remove(&server.new_toplevel.link);
    wl_list_remove(&server.request_cursor.link);
    wl_list_remove(&server.cursor_events.motion.link);
    wl_list_remove(&server.cursor_events.motion_absolute.link);
    wl_list_remove(&server.cursor_events.button.link);
    wl_list_remove(&server.cursor_events.axis.link);
    wl_list_remove(&server.cursor_events.frame.link);

    wl_display_destroy_clients(server.display);
    wl_display_destroy(server.display);
    return 0;
}
