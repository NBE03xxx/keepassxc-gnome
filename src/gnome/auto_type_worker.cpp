#include "gnome/AutoTypePortal.h"
#include "gnome/AutoTypeWire.h"

#include <glib-unix.h>
#include <libei.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <optional>

namespace
{
using Kind = AutoTypeStep::Kind;
using namespace AutoTypeWire;

struct Key
{
    std::uint32_t code = 0;
    unsigned delayMs = 0;
    bool shift = false;
};

class Keyboard
{
public:
    explicit Keyboard(AutoTypePortal& portal) : portal(portal) {}
    ~Keyboard()
    {
        if (watch) g_source_remove(watch);
        if (device) {
            if (emulating) ei_device_stop_emulating(device);
            ei_device_unref(device);
        }
        if (context) ei_unref(context);
    }

    bool connect(int fd)
    {
        if (fd < 0) return false;
        context = ei_new_sender(nullptr);
        if (!context) {
            close(fd);
            return false;
        }
        ei_configure_name(context, "KeePassXC GNOME Auto-Type");
        if (ei_setup_backend_fd(context, fd) < 0) return false;
        watch = g_unix_fd_add(ei_get_fd(context), GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR),
            +[](gint, GIOCondition conditions, gpointer data) -> gboolean {
                auto& self = *static_cast<Keyboard*>(data);
                if (conditions & (G_IO_HUP | G_IO_ERR)) self.portal.cancel();
                else self.dispatch();
                return G_SOURCE_CONTINUE;
            }, this);
        return portal.waitFor(4500, [&] { return device != nullptr; });
    }

    void dispatch()
    {
        ei_dispatch(context);
        while (auto* event = ei_get_event(context)) {
            switch (ei_event_get_type(event)) {
            case EI_EVENT_SEAT_ADDED:
                if (auto* seat = ei_event_get_seat(event); ei_seat_has_capability(seat, EI_DEVICE_CAP_KEYBOARD)) {
                    ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_KEYBOARD, nullptr);
                }
                break;
            case EI_EVENT_DEVICE_RESUMED:
                if (auto* next = ei_event_get_device(event); !device && ei_device_has_capability(next, EI_DEVICE_CAP_KEYBOARD)) {
                    device = ei_device_ref(next);
                }
                break;
            case EI_EVENT_DEVICE_PAUSED:
            case EI_EVENT_DEVICE_REMOVED:
                if (device && device == ei_event_get_device(event)) portal.cancel();
                break;
            case EI_EVENT_KEYBOARD_MODIFIERS:
                if (device && device == ei_event_get_device(event)) {
                    modifiersKnown = true;
                    group = ei_event_keyboard_get_xkb_group(event);
                    depressed = ei_event_keyboard_get_xkb_mods_depressed(event);
                    latched = ei_event_keyboard_get_xkb_mods_latched(event);
                    locked = ei_event_keyboard_get_xkb_mods_locked(event);
                }
                break;
            case EI_EVENT_DISCONNECT:
                portal.cancel();
                break;
            default:
                break;
            }
            ei_event_unref(event);
        }
    }

    bool waitForReleasedModifiers()
    {
        // The shortcut can deactivate when its letter is released, before Ctrl or Alt.
        // The compositor sends the initial state separately from device resume.
        dispatch();
        return portal.waitFor(1500, [this] { return modifiersKnown && !depressed && !latched; });
    }

    bool prepare(const std::uint32_t* steps, std::size_t count, Key* keys) const
    {
        if (!device || depressed || latched) return false;
        auto* map = ei_device_keyboard_get_keymap(device);
        if (!map || ei_keymap_get_type(map) != EI_KEYMAP_TYPE_XKB) return false;
        const auto length = ei_keymap_get_size(map);
        if (!length || length > (1U << 20)) return false;
        auto* bytes = mmap(nullptr, length, PROT_READ, MAP_PRIVATE, ei_keymap_get_fd(map), 0);
        if (bytes == MAP_FAILED) return false;
        auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        auto* keymap = context ? xkb_keymap_new_from_buffer(context, static_cast<const char*>(bytes),
            length, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
        munmap(bytes, length);
        if (!keymap) {
            if (context) xkb_context_unref(context);
            return false;
        }
        auto* state = xkb_state_new(keymap);
        auto* shifted = xkb_state_new(keymap);
        const auto shift = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
        bool ok = state && shifted && group < xkb_keymap_num_layouts(keymap)
            && shift < 8 * sizeof(xkb_mod_mask_t);
        if (ok) {
            xkb_state_update_mask(state, 0, 0, locked, 0, 0, group);
            xkb_state_update_mask(shifted, xkb_mod_mask_t(1) << shift, 0, locked, 0, 0, group);
            ok = translate(steps, count, keys, keymap, state, shifted);
        }
        if (state) xkb_state_unref(state);
        if (shifted) xkb_state_unref(shifted);
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);
        return ok;
    }

    bool send(const Key& key)
    {
        if (key.delayMs) {
            portal.waitFor(key.delayMs, [] { return false; });
            return !portal.cancelled() && sameModifiers();
        }
        if (!key.code) return true; // zero-length delay
        if (portal.cancelled() || !sameModifiers()) return false;
        if (!emulating) {
            ei_device_start_emulating(device, 1);
            emulating = true;
        }
        if (key.shift) emit(KEY_LEFTSHIFT, true);
        emit(key.code, true);
        emit(key.code, false);
        if (key.shift) emit(KEY_LEFTSHIFT, false);
        portal.waitFor(20, [] { return false; });
        return !portal.cancelled();
    }

    void pinModifiers() { pinnedGroup = group; pinnedLocked = locked; }

private:
    static bool translate(const std::uint32_t* steps, std::size_t count, Key* keys,
                          xkb_keymap* map, xkb_state* plain, xkb_state* shifted)
    {
        unsigned totalDelay = 0;
        bool character = false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto word = GUINT32_FROM_LE(steps[i]);
            const auto kind = Kind(word >> KindShift);
            const auto value = word & ValueMask;
            xkb_keysym_t symbol = XKB_KEY_NoSymbol;
            switch (kind) {
            case Kind::Delay:
                if (value > AutoTypeSequence::MaximumTotalDelayMs - totalDelay) return false;
                totalDelay += value;
                keys[i].delayMs = value;
                continue;
            case Kind::Character:
                if (!g_unichar_isprint(value)) return false;
                character = true;
                symbol = xkb_utf32_to_keysym(value);
                break;
            case Kind::Tab:
            case Kind::Enter:
                if (value) return false;
                symbol = kind == Kind::Tab ? XKB_KEY_Tab : XKB_KEY_Return;
                break;
            default:
                return false;
            }
            if (symbol == XKB_KEY_NoSymbol) return false;
            for (auto* state : {plain, kind == Kind::Character ? shifted : nullptr}) {
                if (!state) break;
                for (auto code = xkb_keymap_min_keycode(map); code <= xkb_keymap_max_keycode(map) && code <= KEY_MAX + 8; ++code) {
                    if (code > 8 && xkb_state_key_get_one_sym(state, code) == symbol) {
                        keys[i] = {code - 8, 0, state == shifted};
                        break;
                    }
                }
                if (keys[i].code) break;
            }
            if (!keys[i].code) return false;
        }
        return character;
    }

    bool sameModifiers() const { return !depressed && !latched && group == pinnedGroup && locked == pinnedLocked; }

    void emit(std::uint32_t code, bool down)
    {
        ei_device_keyboard_key(device, code, down);
        ei_device_frame(device, ei_now(context));
    }

    AutoTypePortal& portal;
    ei* context = nullptr;
    ei_device* device = nullptr;
    guint watch = 0;
    bool emulating = false, modifiersKnown = false;
    std::uint32_t group = 0, depressed = 0, latched = 0, locked = 0;
    std::uint32_t pinnedGroup = 0, pinnedLocked = 0;
};

bool signalParent(char signal)
{
    ssize_t written;
    do { written = write(STDOUT_FILENO, &signal, 1); } while (written < 0 && errno == EINTR);
    return written == 1;
}

int typeSequence(AutoTypePortal& portal)
{
    Keyboard keyboard(portal);
    if (!keyboard.connect(portal.connectKeyboard())) return Interrupted;
    if (!keyboard.waitForReleasedModifiers()) return portal.cancelled() ? Interrupted : ModifiersHeld;
    if (!signalParent(Triggered)) return Interrupted;

    bool readable = false;
    const auto readiness = g_unix_fd_add(STDIN_FILENO, G_IO_IN, +[](gint, GIOCondition, gpointer data) -> gboolean {
        *static_cast<bool*>(data) = true;
        return G_SOURCE_CONTINUE;
    }, &readable);
    const bool available = portal.waitFor(2500, [&] { return readable; });
    g_source_remove(readiness);
    if (!available) return Interrupted;
    std::array<std::uint32_t, AutoTypeSequence::MaximumSteps> data{};
    std::array<Key, AutoTypeSequence::MaximumSteps> keys{};
    // The controller writes one atomic record smaller than PIPE_BUF.
    const auto length = read(STDIN_FILENO, data.data(), sizeof(data));
    const auto count = length / ssize_t(sizeof(data.front()));
    if (!keyboard.waitForReleasedModifiers()) {
        explicit_bzero(data.data(), sizeof(data));
        return portal.cancelled() ? Interrupted : ModifiersHeld;
    }
    keyboard.pinModifiers();
    const bool valid = length > 0 && length % sizeof(data.front()) == 0
        && keyboard.prepare(data.data(), count, keys.data());
    explicit_bzero(data.data(), sizeof(data));
    if (!valid) {
        explicit_bzero(keys.data(), sizeof(keys));
        return UnsupportedLayout;
    }
    bool sent = true;
    for (ssize_t i = 0; i < count && sent; ++i) sent = keyboard.send(keys[i]);
    explicit_bzero(keys.data(), sizeof(keys));
    return sent ? 0 : Interrupted;
}
}

int main(int argc, char** argv)
{
    if (argc != 4 || std::strlen(argv[2]) > 4096 || (*argv[2] && !g_str_has_prefix(argv[2], "wayland:"))
        || std::strlen(argv[3]) > 256 || !g_utf8_validate(argv[3], -1, nullptr)) return Interrupted;
    char* end = nullptr;
    const auto parent = std::strtoul(argv[1], &end, 10);
    const rlimit core{0, 0};
    if (!parent || *end || setrlimit(RLIMIT_CORE, &core) || prctl(PR_SET_PDEATHSIG, SIGTERM)
        || unsigned(getppid()) != parent) return Interrupted;
    std::signal(SIGPIPE, SIG_IGN);

    AutoTypePortal portal;
    g_unix_signal_add(SIGTERM, +[](gpointer data) -> gboolean {
        static_cast<AutoTypePortal*>(data)->cancel();
        return G_SOURCE_CONTINUE;
    }, &portal);
    g_unix_fd_add(STDIN_FILENO, GIOCondition(G_IO_HUP | G_IO_ERR), +[](gint, GIOCondition, gpointer data) -> gboolean {
        static_cast<AutoTypePortal*>(data)->cancel();
        return G_SOURCE_REMOVE;
    }, &portal);
    if (!portal.connect()) return portal.imeFailed() ? ImeUnavailable : PermissionOrShortcut;
    if (!portal.prepare(argv[2], argv[3])) return PermissionOrShortcut;
    if (!signalParent(portal.imeAvailable() ? ReadyWithIme : Ready)) return Interrupted;
    if (!portal.waitForShortcut()) return PermissionOrShortcut;
    if (!portal.beginIme()) return portal.cancelled() ? Interrupted : ImeUnavailable;

    // Take the EIS keymap only after GNOME has applied the input source.
    const auto result = typeSequence(portal);
    const bool restored = portal.endIme();
    return !restored && result == 0 ? ImeRestoreFailed : result;
}
