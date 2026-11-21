#pragma once

#include "AutoTypeSequence.h"
#include <gtk/gtk.h>
#include <functional>
#include <memory>

class NativeAutoType
{
public:
    enum class Event { Ready, ReadyWithIme, Triggered, Done, PermissionOrShortcut, UnsupportedLayout,
                       Interrupted, ModifiersHeld, ImeUnavailable, ImeRestoreFailed };
    using Notify = std::function<void(Event)>;

    ~NativeAutoType();
    bool start(GtkWindow* parent, const char* shortcutDescription, Notify notify);
    bool send(const AutoTypeSequence& sequence);
    bool active() const;
    bool triggered() const;
    void cancel();

private:
    struct Request;
    std::shared_ptr<Request> request;
};
