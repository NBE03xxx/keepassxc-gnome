// Integrates the real Window code without production test switches/RPCs.
#define main keepassxc_gnome_product_main
#include "../src/main.cpp"
#undef main
#include <QTemporaryDir>
#include <QFile>
#include "gnome/SafeSave.h"
#include <cstring>
#include <atomic>
#include "SoakMetrics.h"
#ifdef __SANITIZE_ADDRESS__
#include "FontOffsetAudit.h"
#endif

namespace {
struct LifetimeUiProbe {
    AdwApplication* app;
    std::unique_ptr<Window> ui;
    QTemporaryDir files;
    std::string source, confirmedCopy, entryId;
    QByteArray original;
    AdwDialog* form = nullptr;
    GtkEditable* password = nullptr;
    GtkWidget* previousRow = nullptr;
    GdkContentProvider* lease = nullptr;
    char* delayedSecret = nullptr;
    static constexpr auto Canary = "synthetic-ready-reply-memory-canary-that-must-be-overwritten";
    unsigned stage = 0, ticks = 0;
    gpointer weakWindow = nullptr, weakApp = nullptr;
    bool deliveredStale = false;
    bool clipboardRead = false, clipboardCorrect = false, clipboardCancelled = false;
    GtkWidget* focusBeforeTrim = nullptr;
    bool trimIssued = false;
    gulong clipboardObserver = 0;
    unsigned idleSeconds = 0, completedCycles = 0, soakSeconds = 0;
    gint64 idleEnd = 0, idleStarted = 0, soakEnd = 0, cycleStarted = 0, nextCycle = 0;
    unsigned paceMs = 5000;
    long activeSeconds = 0, idleElapsedSeconds = 0;
    SoakMetrics* metrics = nullptr;
    explicit LifetimeUiProbe(AdwApplication* app) : app(app), files(QString::fromLocal8Bit(g_getenv("KPXC_UI_ROOT")) + "/lock.XXXXXX") {
        weakApp = app; g_object_add_weak_pointer(G_OBJECT(app), &weakApp);
        check(files.isValid(), "private test directory"); source = files.filePath("source.kdbx").toStdString();
        check(CoreSession::createFixture(source, FixtureFormat::Kdbx3), "synthetic fixture"); original = load(source);
    }
    static void check(bool ok, const char* label) { if (!ok) { std::fprintf(stderr, "FAIL lifetime UI: %s\n", label); std::exit(2); } }
    void activate() {
        ui = std::make_unique<Window>(app); weakWindow = ui->window;
        g_object_add_weak_pointer(G_OBJECT(ui->window), &weakWindow);
        ui->present(); cycleStarted = g_get_monotonic_time(); g_timeout_add(25, +[](gpointer data) { return static_cast<LifetimeUiProbe*>(data)->tick(); }, this);
    }
    void releaseUi() {
        cacheStats();
        ui.reset(); g_object_unref(app);
        for (unsigned n = 0; n < 2000 && (weakWindow || weakApp); ++n) { g_main_context_iteration(nullptr, FALSE); g_usleep(1000); }
        check(!weakWindow && !weakApp, "application and window finalized after each cycle");
    }
    void cacheStats() {
        if (metrics && ui) {
            metrics->fontCacheClears = ui->fontCache->clearCount();
            metrics->longestFontClearUs = ui->fontCache->longestClearUs();
        }
    }
    void sample(const char* phase, bool force = true) {
        cacheStats(); metrics->sample(phase, completedCycles, force);
    }
    static QByteArray load(const std::string& path) { QFile file(QString::fromStdString(path)); check(file.open(QIODevice::ReadOnly), "read test data"); return file.readAll(); }
    static GtkWidget* find(GtkWidget* root, GType type, const char* text = nullptr) {
        if (G_TYPE_CHECK_INSTANCE_TYPE(root, type)) {
            const char* label = GTK_IS_BUTTON(root) ? gtk_button_get_label(GTK_BUTTON(root)) : nullptr;
            if (!text || (label && !std::strcmp(label, text))) return root;
        }
        for (auto* child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) if (auto* match = find(child, type, text)) return match;
        return nullptr;
    }
    AdwDialog* dialog() { return adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(ui->window)); }
    bool click(const char* label) {
        if (!dialog()) return false;
        auto* button = find(GTK_WIDGET(dialog()), GTK_TYPE_BUTTON, label); if (!button) return false;
        g_signal_emit_by_name(button, "clicked"); return true;
    }
    gboolean tick() {
        check(stage >= 12 || ++ticks < 2000, "bounded GUI test"); auto& w = *ui;
        if (w.busy) return G_SOURCE_CONTINUE;
        switch (stage) {
        case 0: {
            if (w.guard && !w.guard->settled()) return G_SOURCE_CONTINUE;
            const auto name = "automatic-" + std::to_string(completedCycles);
            auto* root = g_build_filename(files.path().toUtf8().constData(), name.c_str(), nullptr); g_mkdir_with_parents(root, 0700); w.checkpointFolder = root; g_free(root);
            w.submit([this, folder = w.checkpointFolder](RemoteSession& session) {
                return session.unlock(source, CoreSession::FixturePassword) && session.enableRecovery(folder);
            }, [this](bool ok) { check(ok, "protected session initialization"); ui->unlocked = ui->sourceKnown = true; gtk_stack_set_visible_child_name(ui->stack, "unlocked"); ui->refresh(); });
            break;
        }
        case 1:
            entryId = w.entryRows.front().id;
            w.submit([this](RemoteSession& session) { check(session.rename(entryId, "Confirmed pre-lock title"), "protected edit"); return session.lastCheckpoint(); }, [this](std::string& copy) { confirmedCopy = copy; ui->dirty = true; ui->refresh(); }); break;
        case 2: {
            w.entries->select(0);
            const auto boot = IdleLock::bootTime(), monotonic = g_get_monotonic_time();
            w.idle.activity(boot - IdleLock::Timeout + 10000000);
            // A compositor may take ownership before the next test tick.
            // Observe installation synchronously instead of requiring continued
            // application ownership after a perfectly valid external takeover.
            clipboardObserver = g_signal_connect(w.clipboard, "changed", G_CALLBACK(+[](GdkClipboard* clipboard, gpointer data) {
                auto& probe = *static_cast<LifetimeUiProbe*>(data);
                if (probe.lease || !probe.ui->copied) return;
                probe.lease = GDK_CONTENT_PROVIDER(g_object_ref(probe.ui->copied));
                gdk_clipboard_read_text_async(clipboard, nullptr, +[](GObject* source, GAsyncResult* result, gpointer data) {
                    auto& probe = *static_cast<LifetimeUiProbe*>(data); GError* error = nullptr;
                    auto* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
                    probe.clipboardCorrect = text && !std::strcmp(text, "synthetic-entry-password");
                    probe.clipboardCancelled = error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
                    if (text) { Sensitive::erase(text, std::strlen(text)); g_free(text); }
                    g_clear_error(&error); probe.clipboardRead = true;
                }, &probe);
            }), this);
            g_signal_emit_by_name(w.copyPass, "clicked");
            check(!w.idle.poll(boot + 11000000, monotonic + 11000000), "assistive button action resets inactivity without bypassing expiry");
            break;
        }
        case 3:
            // Consent persists in the same window; later copies are immediate.
            if (!lease && !click(tr("Copy"))) return G_SOURCE_CONTINUE;
            break;
        case 4:
            check(w.clipboardAccepted && lease, "copy consent precedes observed provider installation");
            if (!clipboardRead) return G_SOURCE_CONTINUE;
            check(clipboardCorrect || (clipboardCancelled && !secret_clipboard_live(lease)), "GTK consumer gets synthetic text or ownership-loss cancellation");
            if (!w.copied) check(!secret_clipboard_live(lease), "external takeover revokes the old provider");
            g_signal_handler_disconnect(w.clipboard, clipboardObserver); clipboardObserver = 0;
            w.editSelected(); break;
        case 5: {
            if (!dialog()) return G_SOURCE_CONTINUE;
            std::function<GtkWidget*(GtkWidget*)> findRow = [&](GtkWidget* widget) -> GtkWidget* {
                if (ADW_IS_ACTION_ROW(widget)) return widget;
                for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) if (auto* found = findRow(child)) return found;
                return nullptr;
            };
            auto* listRow = findRow(w.entries->widget());
            if (!listRow) return G_SOURCE_CONTINUE;
            previousRow = GTK_WIDGET(g_object_ref(listRow));
            check(!adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(previousRow)), "account metadata is never parsed as markup or emitted in markup errors");
            form = ADW_DIALOG(g_object_ref(dialog())); auto* row = find(GTK_WIDGET(form), ADW_TYPE_PASSWORD_ENTRY_ROW);
            check(row, "password row present"); password = GTK_EDITABLE(row); gtk_editable_set_text(password, "Unconfirmed synthetic draft");
            auto* delegate = gtk_editable_get_delegate(password);
            for (unsigned n = 0; delegate && !GTK_IS_TEXT(delegate) && n < 8; ++n) delegate = gtk_editable_get_delegate(delegate);
            check(delegate && GTK_IS_TEXT(delegate) && GTK_IS_PASSWORD_ENTRY_BUFFER(gtk_text_get_buffer(GTK_TEXT(delegate))), "editor uses secure entry buffer");
            gboolean undo = TRUE; g_object_get(delegate, "enable-undo", &undo, nullptr); check(!undo, "entry undo disabled");
            auto* notes = GTK_TEXT_VIEW(find(GTK_WIDGET(form), GTK_TYPE_TEXT_VIEW)); check(notes, "notes field present");
            check(!gtk_text_buffer_get_enable_undo(gtk_text_view_get_buffer(notes)), "notes undo disabled");
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(notes), "Unconfirmed synthetic notes", -1);
            if (!soakEnd) {
                gtk_widget_grab_focus(GTK_WIDGET(password));
                focusBeforeTrim = gtk_root_get_focus(GTK_ROOT(w.window));
                check(focusBeforeTrim, "password editor focused before font maintenance");
            }
            // Isolate the lock-revocation contract from compositor ownership.
            // The actual copy/read path above and the real-desktop copy test
            // cover publication. This second lease is private to this test.
            w.clearClipboard(); g_object_unref(lease);
            w.copied = secret_clipboard_new("synthetic-entry-password");
            lease = GDK_CONTENT_PROVIDER(g_object_ref(w.copied));
            check(secret_clipboard_live(lease), "fresh private lease starts live before automatic lock");
            break;
        }
        case 6: {
            if (focusBeforeTrim && !trimIssued) {
                // Let GtkText's initial select-on-focus idle run before setting
                // the position whose preservation we actually want to test.
                check(gtk_root_get_focus(GTK_ROOT(w.window)) == focusBeforeTrim, "editor focus settled before trim");
                gtk_editable_set_position(password, 5);
                check(gtk_editable_get_position(password) == 5, "test cursor established");
                check(!w.fontCache->maintain(), "font maintenance does not run before the deadline");
                const auto due = g_get_monotonic_time() + FontCache::Interval;
                check(w.fontCache->maintain(due), "real widget font map cache maintained while editing");
                check(!w.fontCache->maintain(due), "font maintenance is rate limited");
                trimIssued = true; return G_SOURCE_CONTINUE;
            }
            if (focusBeforeTrim) {
                check(!std::strcmp(gtk_editable_get_text(password), "Unconfirmed synthetic draft"), "font maintenance preserves editable text");
                check(gtk_editable_get_position(password) == 5, "font maintenance preserves cursor after event dispatch");
                check(gtk_root_get_focus(GTK_ROOT(w.window)) == focusBeforeTrim, "font maintenance preserves focus after event dispatch");
                auto* notes = GTK_TEXT_VIEW(find(GTK_WIDGET(form), GTK_TYPE_TEXT_VIEW));
                GtkTextIter first, last; gtk_text_buffer_get_bounds(gtk_text_view_get_buffer(notes), &first, &last);
                auto* text = gtk_text_buffer_get_text(gtk_text_view_get_buffer(notes), &first, &last, FALSE);
                check(!std::strcmp(text, "Unconfirmed synthetic notes"), "font maintenance preserves notes");
                Sensitive::erase(text, std::strlen(text)); g_free(text); focusBeforeTrim = nullptr;
            }
            w.submit([this](RemoteSession&) { std::string result(Canary); delayedSecret = result.data(); return result; }, [this](std::string&) { deliveredStale = true; });
            // Keep GTK from dispatching this reply until cancellation has
            // explicitly erased it. The worker does not touch it after enqueue.
            bool ready = false;
            for (unsigned n = 0; n < 100 && !ready; ++n) {
                { std::lock_guard lock(w.worker.resultsMutex); for (auto& p : w.worker.pendingResults) if (!p.expired()) ready = true; }
                if (!ready) QThread::msleep(2);
            }
            check(ready && delayedSecret, "sensitive reply queued before lock");
            auto* dialogs = adw_application_window_get_dialogs(ADW_APPLICATION_WINDOW(w.window));
            auto* sameModel = adw_application_window_get_dialogs(ADW_APPLICATION_WINDOW(w.window));
            check(dialogs == sameModel, "dialog model is shared for ownership regression"); g_object_unref(sameModel);
            const auto references = G_OBJECT(dialogs)->ref_count;
            w.automaticLock();
            check(G_OBJECT(dialogs)->ref_count == references, "lock releases its full dialog-model reference"); g_object_unref(dialogs);
            check(!w.unlocked && !w.loading && w.entryRows.empty() && w.groupRows.empty() && !std::strcmp(gtk_editable_get_text(password), ""), "automatic lock conceals model and form immediately");
            check(std::all_of(delayedSecret, delayedSecret + std::strlen(Canary), [](char c) { return !c; }), "queued plaintext erased before stale callback runs");
            check(!secret_clipboard_live(lease), "automatic lock revokes clipboard provider");
            check(!std::strcmp(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(previousRow)), "")
                && !std::strcmp(adw_action_row_get_subtitle(ADW_ACTION_ROW(previousRow)), ""), "retained accessible row no longer exposes account metadata");
            g_object_unref(previousRow); previousRow = nullptr;
            break;
        }
        case 7: {
            check(!deliveredStale && !w.unlocked, "stale operation never reaches UI");
            check(!std::strcmp(gtk_editable_get_text(password), ""), "closed form cannot recover old input");
            CoreSession recovered; EntryDetails details;
            check(recovered.unlock(confirmedCopy, CoreSession::FixturePassword) && recovered.entry(entryId, details)
                && details.title == "Confirmed pre-lock title" && details.password != "Unconfirmed synthetic draft", "confirmed edit recovered, unconfirmed form excluded");
            check(load(source) == original, "autolock never saved original");
            bool complete; auto records = SafeSave::recoveries(QString::fromStdString(w.checkpointFolder), complete);
            check(complete && records.size() == 2, "locked worker releases two protected recovery copies");
            g_object_unref(form); form = nullptr; g_object_unref(lease); lease = nullptr;
            w.credentials(false, confirmedCopy); break;
        }
        case 8: {
            if (!dialog()) return G_SOURCE_CONTINUE;
            auto* row = find(GTK_WIDGET(dialog()), GTK_TYPE_PASSWORD_ENTRY); if (!row) return G_SOURCE_CONTINUE;
            gtk_editable_set_text(GTK_EDITABLE(row), CoreSession::FixturePassword);
            if (!click(tr("Unlock"))) return G_SOURCE_CONTINUE;
            break;
        }
        case 9:
            check(w.unlocked && w.dirty && !w.sourceKnown, "recovery requires explicit reauthentication and Save As");
            w.idle.activity(IdleLock::bootTime() - IdleLock::Timeout - 1); w.checkIdle();
            check(!w.unlocked, "idle deadline locks the real Window"); break;
        case 10: {
            w.credentials(true, {});
            if (!dialog()) return G_SOURCE_CONTINUE;
            form = ADW_DIALOG(g_object_ref(dialog())); auto* row = find(GTK_WIDGET(form), GTK_TYPE_PASSWORD_ENTRY);
            check(row, "new credentials form"); password = GTK_EDITABLE(row); gtk_editable_set_text(password, "synthetic-master-not-submitted");
            w.automaticLock(); check(!std::strcmp(gtk_editable_get_text(password), ""), "autolock clears unsubmitted master password");
            break;
        }
        case 11:
            check(!w.unlocked && !w.loading, "resume never unlocks model");
            g_object_unref(form); form = nullptr;
            std::puts("PASS lifetime GUI: protected edits, immediate autolock, queued secret erasure, secure form/undo clearing, clipboard revocation, recovery reauthentication, inactivity lock and master-form clearing");
            std::fflush(stdout);
            if (soakEnd) {
                ++completedCycles;
                nextCycle = cycleStarted + gint64(paceMs)*1000;
                stage = 13; return G_SOURCE_CONTINUE;
            }
            [[fallthrough]];
        case 12:
            if (idleSeconds && g_get_monotonic_time() < idleEnd) {
                check(!w.unlocked && !w.loading && w.entryRows.empty() && w.groupRows.empty(), "soak idle remains locked and concealed");
                sample("idle", false);
                return G_SOURCE_CONTINUE;
            }
            if (idleSeconds) { idleElapsedSeconds = (g_get_monotonic_time()-idleStarted)/G_USEC_PER_SEC; sample("idle-end"); }
            w.closing = true; w.worker.cancel(); w.conceal(); gtk_window_destroy(w.window); g_application_quit(G_APPLICATION(app)); return G_SOURCE_REMOVE;
        case 13:
            if (g_get_monotonic_time() < nextCycle) return G_SOURCE_CONTINUE;
            sample("cycle-locked");
            std::printf("PASS lifetime GUI soak cycle %u: database locked, window retained\n", completedCycles); std::fflush(stdout);
            if (g_get_monotonic_time() < soakEnd) {
                clipboardRead = clipboardCorrect = clipboardCancelled = deliveredStale = false;
                cycleStarted = g_get_monotonic_time(); stage = ticks = 0;
                return G_SOURCE_CONTINUE;
            }
            activeSeconds = (g_get_monotonic_time() - (soakEnd - gint64(soakSeconds)*G_USEC_PER_SEC))/G_USEC_PER_SEC;
            idleStarted = g_get_monotonic_time(); idleEnd = idleStarted + gint64(idleSeconds)*G_USEC_PER_SEC;
            sample("idle");
            std::printf("SOAK IDLE: seconds=%u, database locked\n", idleSeconds); std::fflush(stdout);
            stage = 12; return G_SOURCE_CONTINUE;
        }
        ++stage; return G_SOURCE_CONTINUE;
    }
};
}
int main(int argc, char** argv) {
    initializeUiLocale();
    const auto locale = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() / "share/locale";
    bindtextdomain("keepassxc-gnome", locale.c_str()); bind_textdomain_codeset("keepassxc-gnome", "UTF-8"); textdomain("keepassxc-gnome");
    QCoreApplication qt(argc, argv); LifetimeUiProbe::check(CoreSession::initialize(), "crypto");
    unsigned cycles = 1;
    if (const auto* count = g_getenv("KPXC_GUI_CYCLES")) {
        char* end = nullptr; const auto parsed = std::strtoul(count, &end, 10);
        LifetimeUiProbe::check(end && !*end && parsed >= 1 && parsed <= 20, "bounded test cycle count"); cycles = unsigned(parsed);
    }
    int result = 0;
    auto boundedEnv = [](const char* name, unsigned fallback, unsigned low, unsigned high) {
        const auto* value = g_getenv(name); if (!value) return fallback;
        char* end = nullptr; const auto parsed = std::strtoul(value, &end, 10);
        LifetimeUiProbe::check(end && !*end && parsed >= low && parsed <= high, name);
        return unsigned(parsed);
    };
    const auto soak = boundedEnv("KPXC_GUI_SOAK_SECONDS", 0, 60, 7200);
    const auto idle = soak ? boundedEnv("KPXC_GUI_IDLE_SECONDS", 900, 15, 1800) : 0;
    const auto pace = soak ? boundedEnv("KPXC_GUI_PACE_MS", 5000, 1000, 60000) : 0;
    if (soak) {
        SoakMetrics metrics(g_getenv("KPXC_UI_ROOT"));
        auto* app = adw_application_new("org.keepassxc.Gnome.LifetimeTest", G_APPLICATION_DEFAULT_FLAGS);
        unsigned completed = 0; long activeSeconds = 0, idleElapsedSeconds = 0;
        {
            LifetimeUiProbe probe(app); probe.metrics = &metrics;
            probe.soakSeconds = soak; probe.idleSeconds = idle; probe.paceMs = pace;
            probe.soakEnd = g_get_monotonic_time() + gint64(soak)*G_USEC_PER_SEC;
            g_signal_connect(app, "activate", G_CALLBACK(+[](AdwApplication*, gpointer data) { static_cast<LifetimeUiProbe*>(data)->activate(); }), &probe);
            result = g_application_run(G_APPLICATION(app), 1, argv); probe.releaseUi();
            completed = probe.completedCycles; activeSeconds = probe.activeSeconds; idleElapsedSeconds = probe.idleElapsedSeconds;
        }
        metrics.sample("finalized", completed);
        std::printf("PASS GUI SOAK: active_seconds=%ld idle_seconds=%ld cycles=%u requested_active=%u requested_idle=%u\n", activeSeconds, idleElapsedSeconds, completed, soak, idle);
        std::fflush(stdout);
    } else for (unsigned cycle = 0; cycle < cycles && !result; ++cycle) {
        auto* app = adw_application_new("org.keepassxc.Gnome.LifetimeTest", G_APPLICATION_DEFAULT_FLAGS); LifetimeUiProbe probe(app);
        g_signal_connect(app, "activate", G_CALLBACK(+[](AdwApplication*, gpointer data) { static_cast<LifetimeUiProbe*>(data)->activate(); }), &probe);
        result = g_application_run(G_APPLICATION(app), 1, argv); probe.releaseUi();
        std::printf("PASS lifetime GUI cycle %u/%u: application and window finalized\n", cycle+1, cycles); std::fflush(stdout);
    }
#ifdef __SANITIZE_ADDRESS__
    if (g_getenv("KPXC_GUI_OFFSET_AUDIT")) {
        const auto log=std::string(g_getenv("KPXC_UI_ROOT"))+"/app.log";
        return FontOffsetAudit::run(log);
    }
#endif
    return result;
}
