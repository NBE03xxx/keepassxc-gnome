#pragma once
#include <gdk/gdk.h>
#include <string>

// Revocation covers this provider, not copies already read by Mutter, history
// managers or another application. Call on the GTK main context only.
GdkContentProvider* secret_clipboard_new(const std::string& text, gint64 lifetimeMicroseconds = 10000000);
void secret_clipboard_revoke(GdkContentProvider* provider);
bool secret_clipboard_live(GdkContentProvider* provider);
unsigned secret_clipboard_pending(GdkContentProvider* provider);
