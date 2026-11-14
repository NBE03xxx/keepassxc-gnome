#include "CoreSession.h"
#include "gnome/Sensitive.h"

#include <adwaita.h>
#include "gnome/RemoteSession.h"
#include "gnome/SessionGuard.h"
#include "gnome/SecretClipboard.h"
#include "gnome/NativePicker.h"
#include "gnome/Appearance.h"
#include "gnome/Locale.h"
#include "gnome/FontCache.h"
#include "gnome/RuntimePaths.h"
#include "gnome/RecentDatabase.h"

#ifndef KPXC_GNOME_DEVELOPMENT_UI
#define KPXC_GNOME_DEVELOPMENT_UI 0
#endif
#include "gnome/SummaryList.h"
#include "gnome/NavigationList.h"
#include "gnome/DatabaseIcons.h"
#include <glib/gstdio.h>
#include <QCoreApplication>
#include <QThread>

#include <clocale>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <libintl.h>
#include <memory>
#include <string>
#include <utility>
#include <tuple>
#include <mutex>
#include <optional>
#include <algorithm>

namespace
{
const char* tr(const char* message) { return gettext(message); }

class Worker
{
    friend struct LifetimeUiProbe;
public:
    Worker()
    {
        receiver->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, receiver, &QObject::deleteLater);
        thread.start();
    }

    void cancel() {
        cancellation->fetch_add(1);
        // An idle model must also die while GTK is concealing a large view.
        // Active RPCs already observe the cancellation counter in their poll.
        if (receiver) QMetaObject::invokeMethod(receiver, [this] { if (session) session->lock(); }, Qt::QueuedConnection);
        std::lock_guard lock(resultsMutex);
        for (auto& result : pendingResults) if (auto erase = result.lock()) (*erase)();
        pendingResults.clear();
    }

    ~Worker() { shutdown(); }
    void shutdown()
    {
        if (!receiver) return;
        cancel();
        QMetaObject::invokeMethod(receiver, [this] { session.reset(); }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
        receiver = nullptr;
    }

    template<typename Task, typename Done>
    void run(Task task, Done done)
    {
        const auto epoch = cancellation->load();
        using Result = decltype(task(std::declval<RemoteSession&>()));
        QMetaObject::invokeMethod(receiver, [this, epoch, operation = std::make_optional(std::move(task)), done = std::move(done)]() mutable {
            if (!session) {
                session = std::make_unique<RemoteSession>(QString{}, ProcessLimits{}, cancellation);
            }
            session->beginTask(epoch);
            auto result = Sensitive::owned(new Result(epoch == cancellation->load() ? (*operation)(*session) : Result{}));
            operation.reset(); // release/scrub input captures before any reply can run
            const auto failure = session->fatalFailure();
            auto erase = std::make_shared<std::function<void()>>([result] { Sensitive::wipe(*result); });
            {
                std::lock_guard lock(resultsMutex);
                if (epoch != cancellation->load()) (*erase)();
                pendingResults.erase(std::remove_if(pendingResults.begin(), pendingResults.end(), [](const auto& p) { return p.expired(); }), pendingResults.end());
                pendingResults.push_back(erase);
            }
            auto* reply = new std::function<void()>([done = std::move(done), result, erase, failure]() mutable {
                done(*result, failure);
            });
            auto* source = g_idle_source_new();
            g_source_set_callback(source, [](gpointer data) -> gboolean {
                (*static_cast<std::function<void()>*>(data))();
                return G_SOURCE_REMOVE;
            }, reply, [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
            g_source_attach(source, g_main_context_default());
            g_source_unref(source);
        }, Qt::QueuedConnection);
    }

private:
    RemoteSession::Cancellation cancellation = std::make_shared<std::atomic<std::uint64_t>>(0);
    QThread thread;
    QObject* receiver = new QObject;
    std::unique_ptr<RemoteSession> session;
    std::mutex resultsMutex;
    std::vector<std::weak_ptr<std::function<void()>>> pendingResults;
};

struct Snapshot {
    std::vector<GroupSummary> groups;
    std::vector<EntrySummary> entries;
    Snapshot() = default;
    Snapshot(std::vector<GroupSummary> g, std::vector<EntrySummary> e) : groups(std::move(g)), entries(std::move(e)) {}
    Snapshot(Snapshot&&) = default;
    Snapshot(const Snapshot&) = delete;
    ~Snapshot() { clearSensitive(); }
    void clearSensitive() { Sensitive::wipe(groups); Sensitive::wipe(entries); }
};

class Window
{
    friend struct LifetimeUiProbe;
    friend struct PolishUiProbe;
    friend struct WelcomeUiProbe;
    friend struct CloseUiProbe;
    friend struct DefaultKeysUiProbe;
    friend struct SaveUiProbe; // Separate GUI regression executable; no production runtime hook.
public:
    explicit Window(AdwApplication* application, std::string pickerExecutable = {})
        : application(application), picker(std::move(pickerExecutable))
    {
        g_weak_ref_init(&editor, nullptr);
        window = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(application)));
        fontCache = std::make_unique<FontCache>(GTK_WIDGET(window));
        gtk_window_set_title(window, KPXC_GNOME_DEVELOPMENT_UI ? tr("KeePassXC GNOME — Development") : "KeePassXC GNOME");
        gtk_window_set_default_size(window, 980, 660);
        g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
            auto& self = *static_cast<Window*>(data);
            self.requestClose();
            return TRUE;
        }), this);

        auto* toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
        auto* header = ADW_HEADER_BAR(adw_header_bar_new());
        windowTitle = ADW_WINDOW_TITLE(adw_window_title_new("KeePassXC GNOME", tr("No database is open")));
        adw_header_bar_set_title_widget(header, GTK_WIDGET(windowTitle));
        newButton = iconButton("document-new-symbolic", tr("New database"), [this] { newDatabase(); });
        openButton = iconButton("document-open-symbolic", tr("Open database"), [this] { chooseFile(false); });
        cancelButton = button(tr("Cancel"), [this] { lock(); });
        saveButton = button(tr("Save"), [this] { if (sourceKnown) saveDatabase(); else chooseFile(true, true); });
        dirtyIndicator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_box_append(GTK_BOX(dirtyIndicator), gtk_image_new_from_icon_name("document-edit-symbolic"));
        gtk_box_append(GTK_BOX(dirtyIndicator), gtk_label_new(tr("Unsaved")));
        gtk_widget_add_css_class(dirtyIndicator, "accent");
        gtk_widget_add_css_class(dirtyIndicator, "heading");
        gtk_widget_set_tooltip_text(dirtyIndicator, tr("Changes have not been saved to the database file."));
        gtk_accessible_update_property(GTK_ACCESSIBLE(dirtyIndicator), GTK_ACCESSIBLE_PROPERTY_LABEL, tr("Changes have not been saved to the database file."), -1);
        auto* menu = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "open-menu-symbolic");
        gtk_widget_set_tooltip_text(menu, tr("Database menu"));
        auto* popover = GTK_POPOVER(gtk_popover_new());
        auto* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        auto action = [&, this](const char* title, std::function<void()> run) {
            auto* item = button(title, [popover, run = std::move(run)] { gtk_popover_popdown(popover); run(); });
            gtk_widget_add_css_class(item, "flat");
            gtk_box_append(GTK_BOX(actions), item);
            return item;
        };
        copySaveButton = action(tr("Save a copy"), [this] { chooseFile(true); });
        saveAsButton = action(tr("Save as a new file"), [this] { chooseFile(true, true); });
        recoveryButton = action(tr("Recovery files"), [this] { chooseRecoveryFolder(); });
        automaticRecoveryButton = action(tr("Recover unsaved changes"), [this] { inspectRecovery(checkpointFolder); });
        action(tr("Opening KDBX files"), [this] {
            auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(tr("Opening KDBX files"),
                tr("In Files, right-click a KDBX file and choose Open With to select KeePassXC or KeePassXC GNOME. You can change the default application in the file's Properties. This app does not overwrite an explicitly selected default application.")));
            adw_alert_dialog_add_responses(dialog, "close", tr("Close"), nullptr);
            adw_alert_dialog_set_default_response(dialog, "close");
            adw_alert_dialog_set_close_response(dialog, "close");
            adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
        });
        action(tr("Keyboard shortcuts"), [this] {
            auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(tr("Keyboard shortcuts"),
                tr("Ctrl+O  Open database\nCtrl+S  Save\nCtrl+Shift+S  Save as a new file\nCtrl+L  Lock\nCtrl+F  Search entries\nCtrl+N  New entry\nCtrl+Shift+N  New database")));
            adw_alert_dialog_add_responses(dialog, "close", tr("Close"), nullptr);
            adw_alert_dialog_set_default_response(dialog, "close");
            adw_alert_dialog_set_close_response(dialog, "close");
            adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
        });
        forgetButton = action(tr("Forget recent database"), [this] {
            if (!recent.forget()) message(tr("Could not update recent database preferences. Check the application state folder permissions."));
            sensitivity();
        });
        gtk_popover_set_child(popover, actions);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), GTK_WIDGET(popover));
        lockButton = iconButton("system-lock-screen-symbolic", tr("Lock"), [this] { confirmDiscard([this] { lock(); }); });
        adw_header_bar_pack_start(header, newButton);
        adw_header_bar_pack_start(header, openButton);
        adw_header_bar_pack_end(header, lockButton);
        adw_header_bar_pack_end(header, cancelButton);
        adw_header_bar_pack_end(header, saveButton);
        adw_header_bar_pack_end(header, dirtyIndicator);
        adw_header_bar_pack_end(header, menu);
        adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));
        auto* banner = adw_banner_new(tr("Development build. Use synthetic test databases only."));
        adw_banner_set_revealed(ADW_BANNER(banner), TRUE);
        if (KPXC_GNOME_DEVELOPMENT_UI) adw_toolbar_view_add_top_bar(toolbar, banner);
        else { g_object_ref_sink(banner); g_object_unref(banner); }
        guardBanner = ADW_BANNER(adw_banner_new(tr("Some system lock notifications are unavailable. The five-minute inactivity lock remains enabled.")));
        adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(guardBanner));

        auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        stack = GTK_STACK(gtk_stack_new());
        gtk_widget_set_vexpand(GTK_WIDGET(stack), TRUE);
        welcome = adw_status_page_new();
        auto* welcomeActions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_halign(welcomeActions, GTK_ALIGN_CENTER);
        recentLabel = GTK_LABEL(gtk_label_new(nullptr));
        gtk_label_set_ellipsize(recentLabel, PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(recentLabel, 36);
        gtk_accessible_update_property(GTK_ACCESSIBLE(recentLabel), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, tr("Recent database"), -1);
        gtk_box_append(GTK_BOX(welcomeActions), GTK_WIDGET(recentLabel));
        recentButton = button(tr("Unlock database"), [this] {
            if (!recent.path().empty()) credentials(false, recent.path());
        });
        gtk_widget_add_css_class(recentButton, "suggested-action");
        gtk_widget_add_css_class(recentButton, "pill");
        gtk_box_append(GTK_BOX(welcomeActions), recentButton);
        welcomeNew = button(tr("New database"), [this] { newDatabase(); });
        welcomeOpen = button(tr("Open database"), [this] { chooseFile(false); });
        for (auto* item : {welcomeNew, welcomeOpen}) {
            gtk_widget_add_css_class(item, "pill");
            gtk_widget_set_size_request(item, 240, -1);
            gtk_box_append(GTK_BOX(welcomeActions), item);
        }
        adw_status_page_set_child(ADW_STATUS_PAGE(welcome), welcomeActions);
        gtk_stack_add_named(stack, welcome, "locked");

        split = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
        adw_overlay_split_view_set_min_sidebar_width(split, 220);
        adw_overlay_split_view_set_max_sidebar_width(split, 340);
        adw_overlay_split_view_set_sidebar_width_fraction(split, .27);
        sidebarButton = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(sidebarButton), "sidebar-show-symbolic");
        gtk_widget_set_tooltip_text(sidebarButton, tr("Groups"));
        gtk_accessible_update_property(GTK_ACCESSIBLE(sidebarButton), GTK_ACCESSIBLE_PROPERTY_LABEL, tr("Groups"), -1);
        gtk_widget_set_visible(sidebarButton, FALSE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sidebarButton), TRUE);
        g_object_bind_property(sidebarButton, "active", split, "show-sidebar", GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
        adw_header_bar_pack_start(header, sidebarButton);
        auto* breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 780sp"));
        adw_breakpoint_add_setters(breakpoint, G_OBJECT(split), "collapsed", TRUE, G_OBJECT(split), "show-sidebar", FALSE, nullptr);
        adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window), breakpoint);
        g_signal_connect(split, "notify::collapsed", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
            auto& self = *static_cast<Window*>(data);
            gtk_widget_set_visible(self.sidebarButton, self.unlocked && adw_overlay_split_view_get_collapsed(self.split));
        }), this);
        gtk_widget_set_size_request(GTK_WIDGET(window), 560, 480);
        auto* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_size_request(sidebar, 190, -1);
        auto* groupActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        groupAdd = iconButton("list-add-symbolic", tr("New group"), [this] { groupName(false); });
        gtk_widget_set_tooltip_text(groupAdd, tr("New group"));
        groupRename = iconButton("document-edit-symbolic", tr("Edit group"), [this] { groupName(true); });
        groupDelete = iconButton("user-trash-symbolic", tr("Delete"), [this] { deleteGroup(); });
        for (auto* item : {groupAdd, groupRename, groupDelete}) {
            gtk_box_append(GTK_BOX(groupActions), item);
        }
        gtk_box_append(GTK_BOX(sidebar), groupActions);
        groups = std::make_unique<NavigationList>([this](GtkWidget* row, NavigationList::Item node) {
            const auto i = node.index;
            if (i >= groupRows.size()) return;
            SummaryList::setTitle(row, groupRows[i].name.c_str());
            DatabaseIcons::set(GTK_IMAGE(g_object_get_data(G_OBJECT(row), "database-icon")), groupRows[i].icon, groupRows[i].customIcon);
        }, [this] {
            if (refreshing || !unlocked) return;
            const auto epoch = generation; checkIdle();
            if (!unlocked || epoch != generation) return;
            idle.activity(IdleLock::bootTime());
            const auto i = groups->selected().index;
            if (i < groupRows.size() && selectedGroup != groupRows[i].id) { selectedGroup = groupRows[i].id; clearPreview(); refresh(); }
        }, [this] {
            if (unlocked && !busy) groups->toggleSelected();
        });
        gtk_box_append(GTK_BOX(sidebar), scrolled(groups->widget()));
        adw_overlay_split_view_set_sidebar(split, sidebar);

        auto* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        auto* listHeader = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(right), listHeader);
        search = GTK_EDITABLE(gtk_search_entry_new());
        g_object_set(search, "placeholder-text", tr("Search entries"), nullptr);
        gtk_widget_set_tooltip_text(GTK_WIDGET(search), tr("Search entries"));
        gtk_widget_set_hexpand(GTK_WIDGET(search), TRUE);
        gtk_box_append(GTK_BOX(listHeader), GTK_WIDGET(search));
        g_signal_connect(search, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
            static_cast<Window*>(data)->refresh();
        }), this);
        auto* entryActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        entryAdd = button(tr("New entry"), [this] { edit({}); });
        entryEdit = iconButton("document-edit-symbolic", tr("Edit"), [this] { editSelected(); });
        entryDelete = iconButton("user-trash-symbolic", tr("Delete"), [this] { deleteEntry(); });
        copyUser = iconButton("avatar-default-symbolic", tr("Copy username"), [this] { copy(false); });
        copyPass = iconButton("edit-copy-symbolic", tr("Copy password"), [this] { copy(true); });
        for (auto* item : {entryAdd, entryEdit, entryDelete, copyUser, copyPass}) {
            gtk_box_append(GTK_BOX(entryActions), item);
        }
        gtk_box_append(GTK_BOX(listHeader), entryActions);
        entries = std::make_unique<SummaryList>(true, [this](GtkWidget* row, guint i) {
            if (i >= entryRows.size()) return;
            SummaryList::setTitle(row, entryRows[i].title.c_str());
            SummaryList::setSubtitle(row, entryRows[i].username.c_str());
            DatabaseIcons::set(GTK_IMAGE(g_object_get_data(G_OBJECT(row), "database-icon")), entryRows[i].icon, entryRows[i].customIcon);
        }, [this] {
            sensitivity();
            if (!refreshing && unlocked) {
                const auto epoch = generation; checkIdle();
                if (!unlocked || epoch != generation) return;
                idle.activity(IdleLock::bootTime()); previewSelected();
            }
        }, [this](guint i) { entries->select(i); editSelected(); });
        gtk_accessible_update_property(GTK_ACCESSIBLE(entries->widget()), GTK_ACCESSIBLE_PROPERTY_LABEL, tr("Entries"), -1);
        auto* detailSplit = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_VERTICAL));
        auto* entryScroll = scrolled(entries->widget()); gtk_widget_set_focusable(entryScroll, FALSE);
        gtk_paned_set_start_child(detailSplit, entryScroll);
        gtk_paned_set_resize_start_child(detailSplit, TRUE); gtk_paned_set_shrink_start_child(detailSplit, FALSE);
        gtk_paned_set_resize_end_child(detailSplit, TRUE); gtk_paned_set_shrink_end_child(detailSplit, FALSE);
        gtk_paned_set_position(detailSplit, 180); gtk_paned_set_wide_handle(detailSplit, TRUE);
        gtk_widget_set_size_request(entries->widget(), -1, 100);
        preview = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        auto* previewActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        // Keep actions beside the preview rather than duplicating account data.
        for (auto* item : {entryEdit, entryDelete, copyUser, copyPass}) {
            g_object_ref(item); gtk_box_remove(GTK_BOX(entryActions), item);
            gtk_box_append(GTK_BOX(previewActions), item); g_object_unref(item);
        }
        gtk_box_append(GTK_BOX(preview), previewActions);
        auto* previewFields = gtk_grid_new();
        gtk_grid_set_column_homogeneous(GTK_GRID(previewFields), TRUE);
        gtk_grid_set_column_spacing(GTK_GRID(previewFields), 18); gtk_grid_set_row_spacing(GTK_GRID(previewFields), 8);
        auto previewField = [previewFields](const char* title, int column, int row, int width = 1) {
            auto* cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3); gtk_widget_set_hexpand(cell, TRUE);
            auto* label = gtk_label_new(""); gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_wrap(GTK_LABEL(label), TRUE); gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(GTK_LABEL(label), 30);
            auto* caption = gtk_label_new(title); gtk_label_set_xalign(GTK_LABEL(caption), 0); gtk_widget_add_css_class(caption, "dim-label");
            gtk_box_append(GTK_BOX(cell), caption); gtk_box_append(GTK_BOX(cell), label);
            gtk_grid_attach(GTK_GRID(previewFields), cell, column, row, width, 1);
            gtk_accessible_update_property(GTK_ACCESSIBLE(label), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
            return GTK_LABEL(label);
        };
        previewTitle = previewField(tr("Title"), 0, 0); previewUsername = previewField(tr("Username"), 1, 0);
        auto* passwordCell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        auto* passwordCaption = gtk_label_new(tr("Password")); gtk_label_set_xalign(GTK_LABEL(passwordCaption), 0); gtk_widget_add_css_class(passwordCaption, "dim-label");
        gtk_box_append(GTK_BOX(passwordCell), passwordCaption);
        previewPassword = GTK_EDITABLE(gtk_password_entry_new());
        gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(previewPassword), TRUE);
        gtk_editable_set_editable(previewPassword, FALSE); secureEditable(GTK_WIDGET(previewPassword), true);
        gtk_accessible_update_property(GTK_ACCESSIBLE(previewPassword), GTK_ACCESSIBLE_PROPERTY_LABEL, tr("Password preview"), -1);
        gtk_box_append(GTK_BOX(passwordCell), GTK_WIDGET(previewPassword));
        gtk_grid_attach(GTK_GRID(previewFields), passwordCell, 0, 1, 1, 1);
        previewUrl = previewField(tr("Website"), 1, 1); previewNotes = previewField(tr("Notes"), 0, 2, 2);
        gtk_widget_set_margin_end(previewFields, 12);
        gtk_box_append(GTK_BOX(preview), previewFields);
        auto* previewScroll = scrolled(preview); gtk_widget_set_size_request(previewScroll, -1, 160);
        gtk_paned_set_end_child(detailSplit, previewScroll);
        gtk_box_append(GTK_BOX(right), GTK_WIDGET(detailSplit));
        gtk_widget_set_vexpand(GTK_WIDGET(detailSplit), TRUE);
        previewHint = gtk_label_new(tr("Select an entry to preview it. Double-click or press Enter to edit."));
        gtk_label_set_wrap(GTK_LABEL(previewHint), TRUE); gtk_box_append(GTK_BOX(right), previewHint);
        gtk_widget_set_visible(preview, FALSE);
        emptyLabel = gtk_label_new(tr("No entries. Create an entry or change your search."));
        gtk_label_set_wrap(GTK_LABEL(emptyLabel), TRUE);
        gtk_box_append(GTK_BOX(right), emptyLabel);
        adw_overlay_split_view_set_content(split, right);
        gtk_stack_add_named(stack, GTK_WIDGET(split), "unlocked");
        gtk_box_append(GTK_BOX(content), GTK_WIDGET(stack));
        status = GTK_LABEL(gtk_label_new(tr("Ready")));
        gtk_label_set_wrap(status, TRUE);
        gtk_label_set_xalign(status, 0);
        gtk_box_append(GTK_BOX(content), GTK_WIDGET(status));
        for (auto* item : {content, sidebar, right}) {
            gtk_widget_set_margin_start(item, 12);
            gtk_widget_set_margin_end(item, 12);
            gtk_widget_set_margin_top(item, 8);
            gtk_widget_set_margin_bottom(item, 8);
        }
        adw_toolbar_view_set_content(toolbar, content);
        adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), GTK_WIDGET(toolbar));
        appearance = std::make_unique<Appearance>(adw_application_get_style_manager(application), GTK_WIDGET(window));
        clipboard = GDK_CLIPBOARD(g_object_ref(gtk_widget_get_clipboard(GTK_WIDGET(window))));
        auto* primary = gdk_display_get_primary_clipboard(gtk_widget_get_display(GTK_WIDGET(window)));
        if (primary) primaryClipboard = GDK_CLIPBOARD(g_object_ref(primary));
        secureEditable(GTK_WIDGET(search));
        g_signal_connect(clipboard, "changed", G_CALLBACK(+[](GdkClipboard* clipboard, gpointer data) {
            auto& self = *static_cast<Window*>(data);
            if (self.copied && gdk_clipboard_get_content(clipboard) != self.copied) self.clearClipboard();
        }), this);
        auto* root = g_build_filename(g_get_user_state_dir(), "keepassxc-gnome", "recovery", nullptr);
        checkpointFolder = root; g_mkdir_with_parents(root, 0700); g_free(root);
        guard = std::make_unique<SessionGuard>([this] { automaticLock(); }, [this](bool available) { if (!closing) { adw_banner_set_revealed(guardBanner, !available); sensitivity(); } });
        idle.activity(IdleLock::bootTime());
        idleTimer = g_timeout_add_seconds(1, +[](gpointer data) -> gboolean {
            auto& self = *static_cast<Window*>(data); self.checkIdle();
            if (!self.closing) self.fontCache->maintain();
            return G_SOURCE_CONTINUE;
        }, this);
        auto* activity = gtk_event_controller_legacy_new();
        gtk_event_controller_set_propagation_phase(activity, GTK_PHASE_CAPTURE);
        g_signal_connect(activity, "event", G_CALLBACK(+[](GtkEventControllerLegacy*, GdkEvent* event, gpointer data) -> gboolean {
            auto& self = *static_cast<Window*>(data); const auto type = gdk_event_get_event_type(event);
            const auto generation = self.generation;
            self.checkIdle();
            if (generation != self.generation) return TRUE;
            if (type == GDK_KEY_PRESS || type == GDK_BUTTON_PRESS || type == GDK_TOUCH_BEGIN || type == GDK_SCROLL) self.idle.activity(IdleLock::bootTime());
            return FALSE;
        }), this);
        gtk_widget_add_controller(GTK_WIDGET(window), activity);
        auto* shortcuts = gtk_shortcut_controller_new();
        gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(shortcuts), GTK_SHORTCUT_SCOPE_MANAGED);
        for (const auto& [trigger, target] : std::vector<std::pair<const char*, GtkWidget*>>{
            {"<Control>o", openButton}, {"<Control>s", saveButton}, {"<Control><Shift>s", saveAsButton},
            {"<Control>l", lockButton}, {"<Control>n", entryAdd}, {"<Control><Shift>n", newButton}}) {
            auto* action = gtk_callback_action_new(+[](GtkWidget*, GVariant*, gpointer target) -> gboolean {
                auto* button = GTK_WIDGET(target);
                if (!gtk_widget_is_sensitive(button)) return FALSE;
                g_signal_emit_by_name(button, "clicked"); return TRUE;
            }, target, nullptr);
            gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), gtk_shortcut_new(gtk_shortcut_trigger_parse_string(trigger), action));
        }
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcuts), gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>f"),
            gtk_callback_action_new(+[](GtkWidget*, GVariant*, gpointer p) -> gboolean {
                auto& self = *static_cast<Window*>(p);
                if (!self.unlocked) return FALSE;
                if (adw_overlay_split_view_get_collapsed(self.split)) adw_overlay_split_view_set_show_sidebar(self.split, FALSE);
                return gtk_widget_grab_focus(GTK_WIDGET(self.search));
            }, this, nullptr)));
        gtk_widget_add_controller(GTK_WIDGET(window), shortcuts);
        sensitivity();
    }

    ~Window()
    {
        closing = true;
        worker.shutdown();
        if (idleTimer) g_source_remove(idleTimer);
        cancelOpenRequest();
        worker.cancel();
        alive.reset();
        picker.cancel();
        g_weak_ref_clear(&editor);
        clearClipboard();
        const auto deadline = g_get_monotonic_time() + 250000;
        while (!revokingProviders.empty() && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, FALSE); reapClipboard();
            if (!revokingProviders.empty()) g_usleep(1000);
        }
        if (clipboardCleanup) g_source_remove(clipboardCleanup);
        for (auto* provider : revokingProviders) g_object_unref(provider);
        revokingProviders.clear();
        guard.reset();
        g_signal_handlers_disconnect_by_data(clipboard, this);
        g_clear_object(&clipboard);
        if (primaryClipboard && gdk_clipboard_is_local(primaryClipboard)) gdk_clipboard_set_content(primaryClipboard, nullptr);
        g_clear_object(&primaryClipboard);
        Sensitive::wipe(groupRows); Sensitive::wipe(entryRows);
    }

    void present() { gtk_window_present(window); }

    // GApplication::open receives local file arguments from the desktop entry.
    // Authentication is deferred until the initial desktop protection queries
    // settle; the database is not read before the ordinary password response.
    void openFiles(GFile** files, int count) {
        if (count != 1) { message(tr("Open one database at a time.")); return; }
        auto* path = g_file_get_path(files[0]);
        if (!path) { message(tr("Only local database files can be opened. Choose a local KDBX file.")); return; }
        cancelOpenRequest(); requestedPath = path; g_free(path); openAttempts = 0;
        openRequestTimer = g_timeout_add(100, +[](gpointer data) -> gboolean {
            auto& self = *static_cast<Window*>(data);
            if (++self.openAttempts < 100 && (self.busy || (self.guard && !self.guard->settled()))) return G_SOURCE_CONTINUE;
            self.openRequestTimer = 0;
            const auto path = std::exchange(self.requestedPath, {});
            self.checkIdle();
            if (self.closing) return G_SOURCE_REMOVE;
            if (self.busy || (self.guard && self.guard->blocked())) {
                self.message(tr("Wait for desktop lock and sleep protection to be ready before opening a database."));
            } else if (adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self.window))) {
                self.message(tr("Close the current dialog before opening another database."));
            } else {
                self.confirmDiscard([&self, path] { self.credentials(false, path); });
            }
            return G_SOURCE_REMOVE;
        }, this);
    }

private:
    GtkWidget* button(const char* title, std::function<void()> clicked)
    {
        auto* result = title ? gtk_button_new_with_label(title) : gtk_button_new();
        auto* callback = new std::function<void()>([this, lifetime = std::weak_ptr<int>(alive), action = std::move(clicked)] {
            if (lifetime.expired() || closing) return;
            const auto epoch = generation;
            checkIdle(); // assistive actions must not bypass an expired lock
            if (closing || epoch != generation) return;
            idle.activity(IdleLock::bootTime()); // AT-SPI need not emit GDK input
            action();
        });
        g_signal_connect_data(result, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            (*static_cast<std::function<void()>*>(data))();
        }), callback, +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, G_CONNECT_DEFAULT);
        return result;
    }

    GtkWidget* iconButton(const char* icon, const char* title, std::function<void()> clicked) {
        auto* result = button(nullptr, std::move(clicked));
        auto* image = gtk_image_new_from_icon_name(icon);
        gtk_accessible_update_property(GTK_ACCESSIBLE(image), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
        gtk_button_set_child(GTK_BUTTON(result), image);
        gtk_widget_set_tooltip_text(result, title);
        gtk_accessible_update_property(GTK_ACCESSIBLE(result), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
        return result;
    }

    static GtkWidget* scrolled(GtkWidget* child)
    {
        auto* scroll = gtk_scrolled_window_new();
        gtk_widget_set_vexpand(scroll, TRUE);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), child);
        return scroll;
    }

    void sensitivity()
    {
        const bool ready = unlocked && !busy && !closeSavePending;
        const bool selected = ready && entries->selected() < entryRows.size();
        for (auto* item : {newButton, openButton, welcomeNew, welcomeOpen, recentButton}) {
            gtk_widget_set_sensitive(item, !busy && !closeSavePending && (!guard || !guard->blocked()));
        }
        for (auto* item : {saveButton, copySaveButton, saveAsButton, entryAdd, groupAdd, groupRename}) {
            gtk_widget_set_sensitive(item, ready);
        }
        gtk_widget_set_sensitive(groupDelete, ready && !groupRows.empty() && selectedGroup != groupRows.front().id);
        for (auto* item : {entryEdit, entryDelete, copyUser, copyPass}) {
            gtk_widget_set_sensitive(item, selected);
        }
        gtk_widget_set_sensitive(lockButton, unlocked || loading);
        gtk_widget_set_visible(cancelButton, loading);
        gtk_widget_set_sensitive(recoveryButton, !busy && !closeSavePending);
        gtk_widget_set_sensitive(automaticRecoveryButton, !busy && !closeSavePending);
        gtk_widget_set_visible(sidebarButton, unlocked && adw_overlay_split_view_get_collapsed(split));
        gtk_widget_set_sensitive(sidebarButton, ready);
        gtk_widget_set_sensitive(GTK_WIDGET(search), !closeSavePending);
        gtk_widget_set_sensitive(forgetButton, !busy && !closeSavePending && !recent.path().empty());
        updateTitle();
    }

    void updateTitle() {
        if (closing) return;
        const bool hasRecent = !recent.path().empty();
        const auto* idleTitle = hasRecent ? tr("Unlock your database") : hasOpenedDatabase ? tr("Your database is locked") : tr("Welcome to KeePassXC GNOME");
        adw_status_page_set_title(ADW_STATUS_PAGE(welcome), idleTitle);
        adw_status_page_set_description(ADW_STATUS_PAGE(welcome), hasRecent ? tr("Your passwords are ready when you are.") :
            hasOpenedDatabase ? tr("Open a database to continue.") : tr("Create a database or open one to get started."));
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(welcome), hasRecent || hasOpenedDatabase ? "system-lock-screen-symbolic" : "dialog-password-symbolic");
        gtk_widget_set_visible(GTK_WIDGET(recentLabel), hasRecent);
        gtk_widget_set_visible(recentButton, hasRecent);
        if (hasRecent) gtk_widget_remove_css_class(welcomeNew, "suggested-action");
        else gtk_widget_add_css_class(welcomeNew, "suggested-action");
        const auto recentName = displayFilename(recent.path());
        gtk_label_set_text(recentLabel, recentName.c_str());
        const auto filename = displayFilename(currentPath);
        std::string title = unlocked ? (filename.empty() ? tr("Unsaved database") : filename) : "KeePassXC GNOME";
        if (unlocked && dirty) title = "• " + title;
        adw_window_title_set_title(windowTitle, title.c_str());
        const auto applicationTitle = KPXC_GNOME_DEVELOPMENT_UI ? tr("KeePassXC GNOME — Development") : "KeePassXC GNOME";
        const auto nativeTitle = unlocked ? title + " — " + applicationTitle : std::string(applicationTitle);
        gtk_window_set_title(window, nativeTitle.c_str());
        gtk_widget_set_visible(dirtyIndicator, unlocked && dirty);
        if (unlocked && dirty) gtk_widget_add_css_class(saveButton, "suggested-action");
        else gtk_widget_remove_css_class(saveButton, "suggested-action");
        adw_window_title_set_subtitle(windowTitle, busy ? tr("Working…") : !unlocked ?
            (hasRecent || hasOpenedDatabase ? tr("Locked") : tr("Welcome")) :
            dirty ? tr("Unsaved changes") : tr("Saved"));
    }
    static std::string displayFilename(const std::string& path) {
        if (path.empty()) return {};
        auto* basename = g_filename_display_basename(path.c_str());
        std::string result(basename); g_free(basename);
        // Keep untrusted file names on one visual line. GtkLabel uses literal
        // text, never markup. The original filename is retained for file IO.
        for (auto& c : result) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
        return result;
    }
    void rememberDatabase(const std::string& path) {
        recentWarning = !path.empty() && !recent.remember(path);
        if (recentWarning) message(tr("The database is open, but its location could not be remembered."));
    }
    void message(const char* text) { gtk_label_set_text(status, text); updateTitle(); }

    void readFailure(ReadFailure failure)
    {
        switch (failure) {
        case ReadFailure::ResourceLimit:
            message(tr("This database exceeds the supported read limits. The file was not changed.")); break;
        case ReadFailure::UnsupportedKdf:
            message(tr("This database uses KDF settings this application cannot preserve or compute correctly. The file was not changed.")); break;
        case ReadFailure::IoError:
            message(tr("Could not read a local regular database file.")); break;
        case ReadFailure::UnsupportedData:
            message(tr("This database contains data this application cannot safely preserve. The file was not changed.")); break;
        case ReadFailure::Timeout:
            message(tr("The operation exceeded its time limit. The database is locked.")); break;
        case ReadFailure::Cancelled:
            message(tr("Cancelled. The database is locked.")); break;
        case ReadFailure::RecoveryFailure:
            message(tr("The change could not be protected. The database is locked. Recover earlier confirmed edits from Recover unsaved changes.")); break;
        case ReadFailure::WorkerFailure:
            message(tr("The database worker stopped unexpectedly. The database is locked.")); break;
        default:
            message(tr("Could not unlock the database. Check the password and file.")); break;
        }
    }

    template<typename Task, typename Done>
    void submit(Task task, Done done)
    {
        ++pending;
        busy = true;
        sensitivity();
        message(tr("Working…"));
        const auto epoch = generation;
        const std::weak_ptr<int> lifetime = alive;
        worker.run(std::move(task), [this, lifetime, epoch, done = std::move(done)](auto& result, ReadFailure transport) mutable {
            if (lifetime.expired()) return;
            --pending;
            busy = pending != 0;
            if (closing || epoch != generation) return;
            if (transport != ReadFailure::None) {
                const bool wasReading = loading;
                worker.cancel();
                conceal();
                if (wasReading || transport == ReadFailure::RecoveryFailure) readFailure(transport);
                else message(tr("Operation interrupted. The database is locked. Use Recover unsaved changes to check confirmed edits; the last operation may be incomplete. If saving, check the destination and Recovery files."));
                return;
            }
            sensitivity();
            done(result);
        });
    }

    void confirm(const char* title, const char* body, const char* accept, std::function<void()> action)
    {
        auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, body));
        adw_alert_dialog_add_responses(dialog, "cancel", tr("Cancel"), "accept", accept, nullptr);
        adw_alert_dialog_set_default_response(dialog, "cancel");
        adw_alert_dialog_set_close_response(dialog, "cancel");
        adw_alert_dialog_set_response_appearance(dialog, "accept", ADW_RESPONSE_DESTRUCTIVE);
        const auto epoch = generation;
        auto* callback = new std::function<void()>([this, epoch, action = std::move(action)] {
            if (!closing && epoch == generation) action();
        });
        g_signal_connect_data(dialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
            if (g_str_equal(response, "accept")) {
                (*static_cast<std::function<void()>*>(data))();
            }
        }), callback, +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, G_CONNECT_DEFAULT);
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
    }

    void confirmDiscard(std::function<void()> action)
    {
        if (!dirty) {
            action();
            return;
        }
        confirm(tr("Discard unsaved changes?"), tr("Save a copy first if you want to keep your changes. Confirmed edits also remain encrypted in Recover unsaved changes until you remove those records."),
                tr("Discard"), std::move(action));
    }

    void closeNow() {
        closing = true;
        worker.cancel(); conceal(); gtk_window_destroy(window);
    }

    void requestClose() {
        if (closing || closeSavePending) return;
        checkIdle();
        // A read may be cancelled immediately. A mutation/save already in
        // progress must finish before deciding which confirmed edits to save.
        if (busy && !loading) { message(tr("Wait for the current operation to finish before closing.")); return; }
        if (unlocked && (picker.active() || adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)))) {
            message(tr("Finish or cancel the current dialog before closing the application.")); return;
        }
        if (!dirty) { closeNow(); return; }
        auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(tr("Save changes before closing?"),
            tr("Your changes have not been saved to the database file. Save them before closing, or discard them. Confirmed edits remain in encrypted recovery records until you remove those records.")));
        adw_alert_dialog_add_responses(dialog, "cancel", tr("Cancel"), "discard", tr("Discard"),
            "save", sourceKnown ? tr("Save (overwrite)") : tr("Save as…"), nullptr);
        adw_alert_dialog_set_response_appearance(dialog, "discard", ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_response_appearance(dialog, "save", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_default_response(dialog, "save");
        adw_alert_dialog_set_close_response(dialog, "cancel");
        const auto epoch = generation;
        auto* callback = new std::function<void(const char*)>([this, epoch](const char* response) {
            if (closing || epoch != generation || g_str_equal(response, "cancel")) return;
            checkIdle();
            if (closing || epoch != generation || !unlocked || (guard && guard->blocked())) return;
            if (busy) { message(tr("Wait for the current operation to finish before closing.")); return; }
            idle.activity(IdleLock::bootTime());
            if (g_str_equal(response, "discard")) { closeNow(); return; }
            if (!g_str_equal(response, "save")) return;
            closeSavePending = true; sensitivity();
            auto saved = [this] { closeSavePending = false; closeNow(); };
            if (sourceKnown) saveDatabase({}, false, std::move(saved));
            else chooseFileImpl(true, true, std::move(saved));
        });
        g_signal_connect_data(dialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
            (*static_cast<std::function<void(const char*)>*>(data))(response);
        }), callback, +[](gpointer data, GClosure*) { delete static_cast<std::function<void(const char*)>*>(data); }, G_CONNECT_DEFAULT);
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
    }

    void conceal()
    {
        ++generation;
        lockReady = false;
        unlocked = false;
        gtk_stack_set_visible_child_name(stack, "locked");
        loading = false;
        saving = false;
        closeSavePending = false;
        sourceKnown = false;
        currentPath.clear();
        recentWarning = false;
        activeRecovery.clear();
        picker.cancel();
        auto* dialogs = adw_application_window_get_dialogs(ADW_APPLICATION_WINDOW(window));
        std::vector<AdwDialog*> closingDialogs;
        for (unsigned i = 0; i < g_list_model_get_n_items(dialogs); ++i) closingDialogs.push_back(ADW_DIALOG(g_list_model_get_item(dialogs, i)));
        g_object_unref(dialogs); // get_dialogs() transfers a full reference.
        for (auto* dialog : closingDialogs) {
            clearForm(GTK_WIDGET(dialog)); adw_dialog_force_close(dialog); g_object_unref(dialog);
        }
        clearClipboard();
        if (primaryClipboard && gdk_clipboard_is_local(primaryClipboard)) gdk_clipboard_set_content(primaryClipboard, nullptr);
        clearPreview();
        groups->clear();
        entries->clear();
        Sensitive::wipe(groupRows);
        Sensitive::wipe(entryRows);
        selectedGroup.clear();
        gtk_editable_set_text(search, "");
        unlocked = false;
        dirty = false;
        busy = pending != 0;
        gtk_stack_set_visible_child_name(stack, "locked");
        sensitivity();
    }

    void cancelOpenRequest() {
        if (openRequestTimer) g_source_remove(openRequestTimer);
        openRequestTimer = 0; requestedPath.clear();
    }

    void lock()
    {
        cancelOpenRequest();
        const bool wasSaving = saving;
        worker.cancel();
        conceal();
        submit([](RemoteSession& session) { session.lock(); return true; }, [this, wasSaving](bool) {
            message(wasSaving ? tr("Locked. If a save was in progress, check its destination and Recovery files.") : tr("Locked"));
        });
    }

    void checkIdle() {
        if (guard) guard->refresh();
        if (idle.poll(IdleLock::bootTime(), g_get_monotonic_time())) automaticLock();
    }
    void automaticLock() {
        if (closing) return;
        const bool hadConfirmedModel = unlocked;
        lockReady = false;
        const bool active = picker.active() || unlocked || loading || pending || adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
        idle.activity(IdleLock::bootTime());
        if (!active) { clearClipboard(); lockReady = true; finishLock(); return; }
        worker.cancel(); conceal();
        submit([](RemoteSession& session) { session.lock(); return session.fullyStopped(); }, [this, hadConfirmedModel](bool stopped) {
            lockReady = stopped; finishLock();
            message(hadConfirmedModel ? tr("Automatically locked. Confirmed edits are available in Recover unsaved changes. Unconfirmed form input was cleared.") : tr("Automatically locked. Input was cleared."));
        });
    }

    void credentials(bool create, std::string path)
    {
        if (guard && guard->blocked()) { message(tr("Wait for desktop lock and sleep protection to be ready before opening a database.")); return; }
        auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(create ? tr("New database") : tr("Unlock database"), nullptr));
        if (!create) adw_alert_dialog_set_body(dialog, displayFilename(path).c_str());
        auto* fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkEditable* name = nullptr;
        if (create) {
            name = GTK_EDITABLE(gtk_entry_new());
            gtk_entry_set_placeholder_text(GTK_ENTRY(name), tr("Database name"));
            gtk_entry_set_activates_default(GTK_ENTRY(name), TRUE);
            gtk_accessible_update_property(GTK_ACCESSIBLE(name), GTK_ACCESSIBLE_PROPERTY_LABEL, tr("Database name"), -1);
            gtk_editable_set_text(name, tr("Passwords"));
            gtk_box_append(GTK_BOX(fields), GTK_WIDGET(name));
        }
        auto* password = GTK_EDITABLE(gtk_password_entry_new());
        secureEditable(GTK_WIDGET(password), true);
        gtk_accessible_update_property(GTK_ACCESSIBLE(password), GTK_ACCESSIBLE_PROPERTY_LABEL, tr("Master password"), -1);
        g_object_set(password, "placeholder-text", tr("Master password"), "show-peek-icon", TRUE, "activates-default", TRUE, nullptr);
        gtk_box_append(GTK_BOX(fields), GTK_WIDGET(password));
        GtkEditable* repeated = nullptr;
        if (create) {
            repeated = GTK_EDITABLE(gtk_password_entry_new());
            secureEditable(GTK_WIDGET(repeated), true);
            gtk_accessible_update_property(GTK_ACCESSIBLE(repeated), GTK_ACCESSIBLE_PROPERTY_LABEL, tr("Repeat master password"), -1);
            g_object_set(repeated, "placeholder-text", tr("Repeat master password"), "activates-default", TRUE, nullptr);
            gtk_box_append(GTK_BOX(fields), GTK_WIDGET(repeated));
        }
        adw_alert_dialog_set_extra_child(dialog, fields);
        adw_alert_dialog_add_responses(dialog, "cancel", tr("Cancel"), "accept", create ? tr("Create") : tr("Unlock"), nullptr);
        adw_alert_dialog_set_response_appearance(dialog, "accept", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_default_response(dialog, "accept");
        adw_alert_dialog_set_close_response(dialog, "cancel");
        auto validCredentials = [dialog, name, password, repeated] {
            const char* secret = gtk_editable_get_text(password);
            const bool valid = *secret && (!name || *gtk_editable_get_text(name)) &&
                (!repeated || !std::strcmp(secret, gtk_editable_get_text(repeated)));
            adw_alert_dialog_set_response_enabled(dialog, "accept", valid);
        };
        for (auto* field : {name, password, repeated}) if (field) {
            auto* update = new std::function<void()>(validCredentials);
            g_signal_connect_data(field, "changed", G_CALLBACK(+[](GtkEditable*, gpointer p) {
                (*static_cast<std::function<void()>*>(p))();
            }), update, +[](gpointer p, GClosure*) { delete static_cast<std::function<void()>*>(p); }, G_CONNECT_DEFAULT);
        }
        validCredentials();
        const auto credentialsEpoch = generation;
        auto* callback = new std::function<void(const char*)>([this, credentialsEpoch, create, path = std::move(path), name, password, repeated](const char* response) {
            if (credentialsEpoch != generation || !g_str_equal(response, "accept")) {
                gtk_editable_set_text(password, "");
                if (repeated) gtk_editable_set_text(repeated, "");
                return;
            }
            if (guard && guard->blocked()) { clearForm(GTK_WIDGET(password)); if (repeated) clearForm(GTK_WIDGET(repeated)); return; }
            auto secret = Sensitive::owned(new std::string(gtk_editable_get_text(password)));
            const bool valid = !secret->empty() && (!repeated || *secret == gtk_editable_get_text(repeated));
            auto title = Sensitive::owned(new std::string(create ? gtk_editable_get_text(name) : ""));
            if (name) gtk_editable_set_text(name, "");
            gtk_editable_set_text(password, "");
            if (repeated) gtk_editable_set_text(repeated, "");
            if (!valid) {
                message(tr("Enter matching, nonempty passwords."));
                return;
            }
            if (create && title->empty()) {
                message(tr("Enter a nonempty name."));
                return;
            }
            worker.cancel();
            conceal();
            loading = true;
            const auto recoveryRoot = checkpointFolder;
            submit([create, path, title, secret, recoveryRoot](RemoteSession& session) {
                session.lock();
                bool ok = create ? session.create(*title, *secret) : session.unlock(path, *secret);
                Sensitive::wipe(*secret);
                if (ok) ok = session.enableRecovery(recoveryRoot);
                return std::tuple{ok, session.lastReadFailure(), !create && ok && session.hasSource(), session.recoveryDirectory()};
            }, [this, create, path](auto result) {
                const auto [ok, failure, source, recovery] = result;
                if (ok && guard && guard->blocked()) { automaticLock(); return; }
                loading = false;
                sensitivity();
                if (!ok) {
                    readFailure(failure);
                    return;
                }
                hasOpenedDatabase = true;
                unlocked = true;
                idle.activity(IdleLock::bootTime());
                dirty = create || !recovery.empty();
                activeRecovery = recovery;
                sourceKnown = source;
                currentPath = source ? path : std::string{};
                if (source) rememberDatabase(path);
                if (!path.empty()) recoveryFolder = std::filesystem::path(path).parent_path().string();
                gtk_stack_set_visible_child_name(stack, "unlocked");
                refresh();
            });
        });
        g_signal_connect_data(dialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
            (*static_cast<std::function<void(const char*)>*>(data))(response);
        }), callback, +[](gpointer data, GClosure*) { delete static_cast<std::function<void(const char*)>*>(data); }, G_CONNECT_DEFAULT);
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
    }

    void newDatabase() { cancelOpenRequest(); confirmDiscard([this] { credentials(true, {}); }); }

    void chooseFile(bool save, bool associate = false)
    {
        cancelOpenRequest();
        if (!save && dirty) {
            confirmDiscard([this] { chooseFileImpl(false); });
            return;
        }
        chooseFileImpl(save, associate);
    }

    void chooseFileImpl(bool save, bool associate = false, std::function<void()> saved = {})
    {
        if (picker.active()) return;
        const auto epoch = generation, serial = ++pickerSerial;
        const std::weak_ptr<int> lifetime = alive;
        const auto title = save ? (associate ? tr("Save as a new file") : tr("Save a database copy")) : tr("Open database");
        if (!picker.choose(window, {save ? NativePicker::Mode::Save : NativePicker::Mode::Open, title, recoveryFolder, save ? "Passwords.kdbx" : ""},
            [this, save, associate, epoch, serial, lifetime, saved = std::move(saved)](NativePicker::Status result, std::string path) {
                if (lifetime.expired() || closing || epoch != generation || serial != pickerSerial) return;
                if (result != NativePicker::Status::Selected) {
                    if (saved) { closeSavePending = false; sensitivity(); }
                    if (result == NativePicker::Status::Failed) message(tr("Could not open the file chooser. Try again."));
                    else if (saved) message(tr("Save cancelled. Your changes remain open."));
                    return;
                }
                if (save) saveDatabase(std::move(path), associate, std::move(saved));
                else credentials(false, std::move(path));
            })) {
                closeSavePending = false; sensitivity();
                message(tr("Could not open the file chooser. Try again."));
            }
    }

    void saveDatabase(std::string path = {}, bool associate = false, std::function<void()> saved = {})
    {
        saving = true;
        if (!path.empty()) recoveryFolder = std::filesystem::path(path).parent_path().string();
        submit([path, associate](RemoteSession& session) {
            if (path.empty()) session.save();
            else if (associate) session.saveAs(path);
            else session.saveCopy(path);
            return session.lastSaveResult();
        }, [this, associate, normal = path.empty(), saved = std::move(saved)](SaveResult result) {
            saving = false;
            if (result.committed()) {
                if (associate || normal) { dirty = false; sourceKnown = true; activeRecovery.clear(); }
                if (associate) currentPath = result.target;
                message(associate || normal ? tr("Database saved.") : tr("Copy saved. The current database still has its previous save state."));
                if (associate) rememberDatabase(currentPath);
                sensitivity();
                if (saved && (associate || normal) && !dirty) saved();
                return;
            }
            closeSavePending = false; sensitivity();
            const char* reason;
            switch (result.status) {
            case SaveStatus::Exists: reason = tr("A file already exists at that location. Choose a new filename."); break;
            case SaveStatus::Conflict: reason = tr("The destination changed outside this application. Review recovery files and save your changes under a new name."); break;
            case SaveStatus::UnsafePath: reason = tr("This path cannot be safely replaced. Choose a new local file without symbolic or hard links."); break;
            case SaveStatus::Busy: reason = tr("Another save is using this destination. Try again after it finishes."); break;
            case SaveStatus::RecoveryFull: reason = tr("The recovery limit for this file has been reached. Review and remove older recovery records before saving."); break;
            case SaveStatus::Uncertain: reason = tr("The save result is uncertain. Review the destination and recovery files before retrying."); break;
            case SaveStatus::VerificationError: reason = tr("The saved candidate could not be read back safely. The destination was not replaced."); break;
            default: reason = tr("Could not save. Check available space and permissions. Review recovery files before retrying."); break;
            }
            // After a publication-related error, the baseline is deliberately
            // not advanced. Further edits must be explicitly saved to a new name.
            if (result.published) sourceKnown = false;
            message(reason);
            if (!result.recovery.empty()) recoveryFolder = std::filesystem::path(result.recovery).parent_path().string();
        });
    }

    void chooseRecoveryFolder()
    {
        if (picker.active()) return;
        const auto epoch = generation, serial = ++pickerSerial;
        const std::weak_ptr<int> lifetime = alive;
        if (!picker.choose(window, {NativePicker::Mode::Folder, tr("Choose the database folder to inspect"), recoveryFolder, {}},
            [this, epoch, serial, lifetime](NativePicker::Status result, std::string path) {
                if (lifetime.expired() || closing || epoch != generation || serial != pickerSerial) return;
                if (result == NativePicker::Status::Failed) message(tr("Could not open the file chooser. Try again."));
                if (result == NativePicker::Status::Selected) { recoveryFolder = std::move(path); inspectRecovery(recoveryFolder); }
            })) message(tr("Could not open the file chooser. Try again."));
    }

    void inspectRecovery(const std::string& folder)
    {
        // Inspecting recovery data never terminates a live database worker.
        // A separate short-lived worker keeps its I/O/resource failures local.
        submit([folder](RemoteSession& session) {
            bool complete;
            auto records = session.recoveries(folder, complete);
            return std::pair{std::move(records), complete};
        }, [this, folder](auto result) {
            auto* dialog = ADW_DIALOG(adw_dialog_new());
            adw_dialog_set_title(dialog, tr("Recovery files"));
            adw_dialog_set_content_width(dialog, 720);
            adw_dialog_set_content_height(dialog, 520);
            auto* toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
            adw_toolbar_view_add_top_bar(toolbar, adw_header_bar_new());
            auto* scroll = gtk_scrolled_window_new();
            auto* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
            auto* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_description(group, result.second
                ? tr("Encrypted copies from this application's saves. Copies belonging to unlocked databases appear after locking. Open a copy, check its contents, then save under a new name.")
                : tr("The recovery scan was incomplete. Some records may be missing. Review the listed records or check folder permissions, then retry."));
            const auto epoch = generation;
            for (const auto& record : result.first) {
                auto* expander = ADW_EXPANDER_ROW(adw_expander_row_new());
                const auto filename = std::filesystem::path(record.target).filename().string();
                adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(expander), FALSE);
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander), filename.c_str());
                const char* state = record.state == "checkpoint" ? tr("Automatic recovery copy") : record.state == "committed" ? tr("Completed save") : record.state == "conflict" ? tr("External update detected") : tr("Interrupted or unfinished save");
                const auto subtitle = std::string(state) + " · " + record.created;
                adw_expander_row_set_subtitle(expander, subtitle.c_str());
                gtk_widget_set_tooltip_text(GTK_WIDGET(expander), record.directory.c_str());
                for (const auto& file : record.files) {
                    auto* row = ADW_ACTION_ROW(adw_action_row_new());
                    const char* kind = file.kind == "written.kdbx" ? tr("Saved candidate") : file.kind == "previous.kdbx" ? tr("Version before saving") : file.kind == "displaced.kdbx" ? tr("Displaced destination") : tr("Unfinished candidate");
                    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), kind);
                    adw_action_row_set_subtitle(row, file.hashMatches ? tr("Matches the recorded encrypted hash; contents still require unlocking.") : tr("Unverified copy; unlocking and data validation are required."));
                    auto* open = button(tr("Open copy"), [this, dialog, epoch, path = file.path] {
                        if (epoch != generation) return;
                        adw_dialog_close(dialog);
                        confirmDiscard([this, path] { credentials(false, path); });
                    });
                    gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
                    adw_action_row_add_suffix(row, open);
                    adw_expander_row_add_row(expander, GTK_WIDGET(row));
                }
                auto* remove = button(tr("Remove record"), [this, dialog, epoch, directory = record.directory, folder] {
                    if (epoch != generation) return;
                    adw_dialog_close(dialog);
                    confirm(tr("Remove recovery record?"), tr("This permanently deletes the encrypted copies in this record. Save recovered data under a new name first."), tr("Remove"), [this, epoch, directory, folder] {
                        if (epoch != generation) return;
                        submit([directory](RemoteSession& session) { return session.discardRecovery(directory); }, [this, folder](bool ok) {
                            if (ok) inspectRecovery(folder);
                            else message(tr("Could not confirm removal. Review recovery files again; the record may be active or contain unexpected files."));
                        });
                    });
                });
                gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
                gtk_widget_set_sensitive(remove, record.directory != activeRecovery);
                adw_expander_row_add_suffix(expander, remove);
                adw_preferences_group_add(group, GTK_WIDGET(expander));
            }
            if (result.first.empty()) adw_preferences_group_set_title(group, tr("No recovery files found"));
            adw_preferences_page_add(page, group);
            gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(page));
            adw_toolbar_view_set_content(toolbar, scroll);
            adw_dialog_set_child(dialog, GTK_WIDGET(toolbar));
            adw_dialog_present(dialog, GTK_WIDGET(window));
        });
    }

    void refresh()
    {
        if (!unlocked) return;
        const auto serial = ++refreshSerial;
        const auto group = selectedGroup;
        auto query = Sensitive::owned(new std::string(gtk_editable_get_text(search)));
        submit([group, query](RemoteSession& session) {
            auto allGroups = session.groups();
            const auto base = group.empty() && !allGroups.empty() ? allGroups.front().id : group;
            return Snapshot{std::move(allGroups), session.search(*query, base)};
        },
               [this, serial](Snapshot& snapshot) {
            if (serial != refreshSerial) return;
            const auto previousEntry = selectedEntry();
            std::set<std::string> expandedIds;
            for (auto index : groups->expanded()) if (index < groupRows.size()) expandedIds.insert(groupRows[index].id);
            refreshing = true;
            clearPreview();
            groups->clear();
            entries->clear();
            Sensitive::wipe(groupRows); Sensitive::wipe(entryRows);
            groupRows = std::move(snapshot.groups);
            entryRows = std::move(snapshot.entries);
            guint selected = 0, entrySelection = GTK_INVALID_LIST_POSITION;
            std::set<guint> expandedIndices;
            for (guint i = 0; i < groupRows.size(); ++i) {
                if (expandedIds.contains(groupRows[i].id)) expandedIndices.insert(i);
                if (groupRows[i].id == selectedGroup) selected = i;
            }
            groups->reset(groupRows, {}, false, expandedIndices);
            if (!groupRows.empty()) { selectedGroup = groupRows[selected].id; groups->select({true, selected}); }
            std::vector<std::string> ids; ids.reserve(entryRows.size());
            for (guint i = 0; i < entryRows.size(); ++i) {
                ids.push_back(entryRows[i].id);
                if (entryRows[i].id == previousEntry) entrySelection = i;
            }
            entries->append(ids); entries->select(entrySelection);
            gtk_widget_set_visible(emptyLabel, entryRows.empty());
            refreshing = false;
            sensitivity();
            previewSelected();
            message(recentWarning ? tr("The database is open, but its location could not be remembered.") : dirty ? tr("Unsaved changes") : tr("Ready"));
        });
    }

    std::string selectedEntry() const
    {
        const auto index = entries->selected();
        return index < entryRows.size() ? entryRows[index].id : std::string{};
    }

    void clearPreview() {
        ++previewSerial;
        for (auto* label : {previewTitle, previewUsername, previewUrl, previewNotes}) gtk_label_set_text(label, "");
        gtk_editable_set_text(previewPassword, "");
        auto* delegate = previewPassword;
        for (unsigned i = 0; i < 8 && delegate; ++i) {
            if (GTK_IS_TEXT(delegate)) { gtk_text_set_visibility(GTK_TEXT(delegate), FALSE); break; }
            delegate = gtk_editable_get_delegate(delegate);
        }
        gtk_widget_set_visible(preview, FALSE); gtk_widget_set_visible(previewHint, TRUE);
    }

    void previewSelected() {
        clearPreview();
        const auto id = selectedEntry();
        if (!unlocked || id.empty() || refreshing) return;
        const auto serial = previewSerial;
        submit([id](RemoteSession& session) { EntryDetails details; session.entry(id, details); return details; },
            [this, id, serial](EntryDetails& details) {
                if (serial != previewSerial || !unlocked || id != selectedEntry() || details.id != id) return;
                gtk_label_set_text(previewTitle, details.title.c_str());
                gtk_label_set_text(previewUsername, details.username.c_str());
                gtk_editable_set_text(previewPassword, details.password.c_str());
                gtk_label_set_text(previewUrl, details.url.c_str());
                gtk_label_set_text(previewNotes, details.notes.c_str());
                gtk_widget_set_visible(preview, TRUE); gtk_widget_set_visible(previewHint, FALSE);
                if (adw_overlay_split_view_get_collapsed(split)) adw_overlay_split_view_set_show_sidebar(split, FALSE);
                message(recentWarning ? tr("The database is open, but its location could not be remembered.") : dirty ? tr("Unsaved changes") : tr("Ready"));
            });
    }

    void groupName(bool rename)
    {
        if (!unlocked || busy) return;
        auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(rename ? tr("Edit group") : tr("New group"), nullptr));
        auto* input = GTK_EDITABLE(gtk_entry_new());
        secureEditable(GTK_WIDGET(input));
        gtk_entry_set_placeholder_text(GTK_ENTRY(input), tr("Group name"));
        gtk_entry_set_activates_default(GTK_ENTRY(input), TRUE);
        gtk_accessible_update_property(GTK_ACCESSIBLE(input), GTK_ACCESSIBLE_PROPERTY_LABEL, tr("Group name"), -1);
        auto iconChoice = std::make_shared<int>(-1);
        int currentIcon = 48; bool customIcon = false;
        if (rename) {
            for (const auto& group : groupRows) {
                if (group.id == selectedGroup) {
                    gtk_editable_set_text(input, group.name.c_str()); currentIcon = group.icon; customIcon = group.customIcon;
                }
            }
        }
        auto* fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_box_append(GTK_BOX(fields), GTK_WIDGET(input));
        if (rename) gtk_box_append(GTK_BOX(fields), DatabaseIcons::chooser(currentIcon, customIcon, iconChoice));
        adw_alert_dialog_set_extra_child(dialog, fields);
        adw_alert_dialog_add_responses(dialog, "cancel", tr("Cancel"), "save", tr("Save"), nullptr);
        adw_alert_dialog_set_default_response(dialog, "save");
        adw_alert_dialog_set_response_appearance(dialog, "save", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_close_response(dialog, "cancel");
        adw_alert_dialog_set_response_enabled(dialog, "save", *gtk_editable_get_text(input) != '\0');
        g_signal_connect(input, "changed", G_CALLBACK(+[](GtkEditable* field, gpointer p) {
            adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(p), "save", *gtk_editable_get_text(field) != '\0');
        }), dialog);
        const auto group = selectedGroup;
        const auto epoch = generation;
        auto* callback = new std::function<void(const char*)>([this, epoch, rename, group, input, iconChoice](const char* response) {
            auto name = Sensitive::owned(new std::string(gtk_editable_get_text(input)));
            gtk_editable_set_text(input, "");
            if (epoch != generation || !unlocked || !g_str_equal(response, "save")) return;
            submit([rename, group, name, icon = *iconChoice](RemoteSession& session) {
                return rename ? session.updateGroup(group, *name, icon) : !session.addGroup(group, *name).empty();
            }, [this](bool ok) {
                if (ok) {
                    for (guint i = 0; i < groupRows.size(); ++i) if (groupRows[i].id == selectedGroup) groups->expandGroup(i);
                    dirty = true; refresh();
                }
                else message(tr("Enter a nonempty name."));
            });
        });
        g_signal_connect_data(dialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
            (*static_cast<std::function<void(const char*)>*>(data))(response);
        }), callback, +[](gpointer data, GClosure*) { delete static_cast<std::function<void(const char*)>*>(data); }, G_CONNECT_DEFAULT);
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
    }

    void deleteGroup()
    {
        const auto id = selectedGroup;
        confirm(tr("Delete group?"), tr("The group and its entries will move to the recycle bin."), tr("Delete"), [this, id] {
            submit([id](RemoteSession& session) { return session.removeGroup(id); }, [this](bool ok) {
                if (ok) { dirty = true; selectedGroup.clear(); refresh(); }
            });
        });
    }

    void deleteEntry()
    {
        const auto id = selectedEntry();
        if (id.empty()) return;
        confirm(tr("Delete entry?"), tr("The entry will move to the recycle bin."), tr("Delete"), [this, id] {
            submit([id](RemoteSession& session) { return session.removeEntry(id); }, [this](bool ok) {
                if (ok) { dirty = true; refresh(); }
            });
        });
    }

    void editSelected()
    {
        const auto epoch = generation; checkIdle();
        if (!unlocked || epoch != generation) return;
        idle.activity(IdleLock::bootTime());
        const auto id = selectedEntry();
        if (id.empty()) return;
        submit([id](RemoteSession& session) { EntryDetails details; session.entry(id, details); return details; },
               [this](EntryDetails& details) { if (!details.id.empty()) edit(details); });
    }

    static void secureEditable(GtkWidget* widget, bool secret = false) {
        for (unsigned i = 0; i < 8 && GTK_IS_EDITABLE(widget); ++i) {
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(widget), "enable-undo")) g_object_set(widget, "enable-undo", FALSE, nullptr);
            if (GTK_IS_TEXT(widget)) {
                auto* buffer = gtk_password_entry_buffer_new(); gtk_text_set_buffer(GTK_TEXT(widget), buffer); g_object_unref(buffer);
                gtk_text_set_input_hints(GTK_TEXT(widget), GtkInputHints(GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_NO_EMOJI | GTK_INPUT_HINT_PRIVATE));
                if (secret) gtk_text_set_input_purpose(GTK_TEXT(widget), GTK_INPUT_PURPOSE_PASSWORD);
                break;
            }
            auto* delegate = gtk_editable_get_delegate(GTK_EDITABLE(widget));
            if (!delegate || delegate == GTK_EDITABLE(widget)) break;
            widget = GTK_WIDGET(delegate);
        }
    }

    static void clearForm(GtkWidget* widget)
    {
        if (GTK_IS_EDITABLE(widget)) {
            gtk_editable_set_text(GTK_EDITABLE(widget), "");
            return;
        }
        if (GTK_IS_TEXT_VIEW(widget)) {
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget)), "", 0);
            return;
        }
        for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
            clearForm(child);
        }
    }

    void edit(const EntryDetails& details)
    {
        if (!unlocked) return;
        auto* dialog = ADW_DIALOG(adw_dialog_new());
        adw_dialog_set_title(dialog, details.id.empty() ? tr("New entry") : tr("Edit entry"));
        adw_dialog_set_content_width(dialog, 480);
        adw_dialog_set_content_height(dialog, 560);
        auto* toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
        auto* header = ADW_HEADER_BAR(adw_header_bar_new());
        adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));
        auto* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        auto* credentials = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(credentials, tr("Credentials"));
        adw_preferences_group_set_description(credentials, tr("Save the entry to protect it in recovery. Form input is cleared when the application locks."));
        auto field = [credentials](const char* title, const std::string& value, bool secret = false) {
            auto* row = secret ? adw_password_entry_row_new() : adw_entry_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
            secureEditable(row, secret);
            adw_entry_row_set_activates_default(ADW_ENTRY_ROW(row), TRUE);
            gtk_editable_set_text(GTK_EDITABLE(row), value.c_str());
            adw_preferences_group_add(credentials, row);
            return GTK_EDITABLE(row);
        };
        auto* title = field(tr("Title"), details.title);
        auto* username = field(tr("Username"), details.username);
        auto* password = field(tr("Password"), details.password, true);
        auto* generate = button(tr("Generate"), [password] {
            auto generated = CoreSession::generatePassword();
            gtk_editable_set_text(password, generated.c_str()); Sensitive::wipe(generated);
        });
        adw_entry_row_add_suffix(ADW_ENTRY_ROW(password), generate);
        auto* url = field(tr("Website"), details.url);
        adw_preferences_page_add(page, credentials);
        auto* additional = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        auto* notesRow = ADW_EXPANDER_ROW(adw_expander_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(notesRow), tr("Notes"));
        auto* notes = GTK_TEXT_VIEW(gtk_text_view_new());
        gtk_accessible_update_property(GTK_ACCESSIBLE(notes), GTK_ACCESSIBLE_PROPERTY_LABEL, tr("Notes"), -1);
        gtk_text_view_set_wrap_mode(notes, GTK_WRAP_WORD_CHAR);
        gtk_text_buffer_set_enable_undo(gtk_text_view_get_buffer(notes), FALSE);
        gtk_widget_set_size_request(GTK_WIDGET(notes), -1, 140);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(notes), details.notes.c_str(), -1);
        adw_expander_row_add_row(notesRow, GTK_WIDGET(notes));
        adw_preferences_group_add(additional, GTK_WIDGET(notesRow));
        adw_preferences_page_add(page, additional);
        auto iconChoice = std::make_shared<int>(-1);
        auto* iconGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(iconGroup, tr("Icon"));
        adw_preferences_group_add(iconGroup, DatabaseIcons::chooser(details.icon < 0 ? 0 : details.icon, details.customIcon, iconChoice));
        adw_preferences_page_add(page, iconGroup);
        adw_toolbar_view_set_content(toolbar, GTK_WIDGET(page));
        const auto id = details.id;
        const auto group = details.group.empty() ? selectedGroup : details.group;
        const auto epoch = generation;
        auto* save = button(tr("Save entry"), [this, dialog, epoch, id, group, title, username, password, url, notes, iconChoice] {
            if (!unlocked || busy || epoch != generation) return;
            auto* buffer = gtk_text_view_get_buffer(notes);
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buffer, &start, &end);
            auto* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
            auto edited = Sensitive::owned(new EntryDetails{id, group, gtk_editable_get_text(title), gtk_editable_get_text(username),
                                gtk_editable_get_text(password), gtk_editable_get_text(url), text, *iconChoice});
            Sensitive::erase(text, std::strlen(text)); g_free(text);
            if (edited->title.empty()) return;
            clearForm(GTK_WIDGET(dialog));
            adw_dialog_force_close(dialog);
            submit([edited](RemoteSession& session) {
                return edited->id.empty() ? !session.addEntry(edited->group, *edited).empty() : session.updateEntry(*edited);
            }, [this](bool ok) {
                if (ok) { dirty = true; refresh(); }
                else message(tr("Could not update the entry."));
            });
        });
        gtk_widget_add_css_class(save, "suggested-action");
        adw_header_bar_pack_end(header, save);
        adw_dialog_set_child(dialog, GTK_WIDGET(toolbar));
        adw_dialog_set_default_widget(dialog, save);
        gtk_widget_set_sensitive(save, *gtk_editable_get_text(title) != '\0');
        g_signal_connect(title, "changed", G_CALLBACK(+[](GtkEditable* field, gpointer p) {
            gtk_widget_set_sensitive(GTK_WIDGET(p), *gtk_editable_get_text(field) != '\0');
        }), save);
        g_weak_ref_set(&editor, G_OBJECT(dialog));
        g_signal_connect(dialog, "closed", G_CALLBACK(+[](AdwDialog* closed, gpointer) {
            clearForm(GTK_WIDGET(closed));
        }), nullptr);
        adw_dialog_present(dialog, GTK_WIDGET(window));
    }

    void copy(bool password)
    {
        if (!unlocked || busy) return;
        if (!clipboardAccepted) {
            auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(tr("Use the clipboard?"), tr("GNOME or a clipboard history tool can retain copied credentials after this app's ten-second expiry or lock. Use clipboard copying only if you accept this limitation.")));
            adw_alert_dialog_add_responses(dialog, "cancel", tr("Cancel"), "copy", tr("Copy"), nullptr);
            adw_alert_dialog_set_default_response(dialog, "cancel"); adw_alert_dialog_set_close_response(dialog, "cancel");
            const auto epoch = generation;
            auto* callback = new std::function<void(const char*)>([this, password, epoch](const char* response) {
                if (epoch != generation || !unlocked || !g_str_equal(response, "copy")) return;
                clipboardAccepted = true; copy(password);
            });
            g_signal_connect_data(dialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) { (*static_cast<std::function<void(const char*)>*>(data))(response); }), callback,
                +[](gpointer p, GClosure*) { delete static_cast<std::function<void(const char*)>*>(p); }, G_CONNECT_DEFAULT);
            adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window)); return;
        }
        const auto id = selectedEntry();
        if (id.empty()) return;
        submit([id, password](RemoteSession& session) {
            return password ? session.copyPassword(id) : session.copyUsername(id);
        }, [this](std::string& value) {
            clearClipboard();
            if (value.size() > (1U << 20)) { message(tr("This value is too large to copy.")); return; }
            copied = secret_clipboard_new(value);
            if (!gdk_clipboard_set_content(clipboard, copied)) { clearClipboard(); message(tr("The clipboard could not accept this value.")); return; }
            clipboardTimer = g_timeout_add(10000, +[](gpointer data) -> gboolean {
                auto& self = *static_cast<Window*>(data);
                self.clipboardTimer = 0;
                self.clearClipboard();
                return G_SOURCE_REMOVE;
            }, this);
            message(tr("Copied. This app stops providing the value after ten seconds; GNOME or other apps may retain a copy."));
        });
    }

    void clearClipboard()
    {
        if (clipboardTimer) {
            g_source_remove(clipboardTimer);
            clipboardTimer = 0;
        }
        if (copied) {
            auto* old = copied; copied = nullptr;
            secret_clipboard_revoke(old);
            if (gdk_clipboard_get_content(clipboard) == old) gdk_clipboard_set_content(clipboard, nullptr);
            if (secret_clipboard_pending(old)) {
                revokingProviders.push_back(old);
                if (!clipboardCleanup) clipboardCleanup = g_timeout_add(10, +[](gpointer data) -> gboolean {
                    auto& self = *static_cast<Window*>(data); self.reapClipboard();
                    if (self.revokingProviders.empty()) { self.clipboardCleanup = 0; return G_SOURCE_REMOVE; }
                    return G_SOURCE_CONTINUE;
                }, this);
            } else g_object_unref(old);
        }
    }
    void finishLock() {
        if (lockReady && revokingProviders.empty() && guard) guard->releaseSleepDelay();
    }
    void reapClipboard() {
        revokingProviders.erase(std::remove_if(revokingProviders.begin(), revokingProviders.end(), [](GdkContentProvider* provider) {
            if (secret_clipboard_pending(provider)) return false;
            g_object_unref(provider); return true;
        }), revokingProviders.end());
        finishLock();
    }

    AdwApplication* application;
    GtkWindow* window = nullptr;
    GtkStack* stack = nullptr;
    GtkWidget* welcome = nullptr;
    GtkWidget *welcomeNew = nullptr, *welcomeOpen = nullptr, *recentButton = nullptr;
    GtkWidget* forgetButton = nullptr;
    GtkLabel* recentLabel = nullptr;
    RecentDatabase recent;
    bool recentWarning = false;
    std::string currentPath, requestedPath;
    guint openRequestTimer = 0;
    unsigned openAttempts = 0;
    GtkLabel* status = nullptr;
    AdwWindowTitle* windowTitle = nullptr;
    std::unique_ptr<NavigationList> groups;
    std::unique_ptr<SummaryList> entries;
    AdwOverlaySplitView* split = nullptr;
    GtkWidget* sidebarButton = nullptr;
    GtkWidget* emptyLabel = nullptr;
    GtkWidget *preview = nullptr, *previewHint = nullptr;
    GtkLabel *previewTitle = nullptr, *previewUsername = nullptr, *previewUrl = nullptr, *previewNotes = nullptr;
    GtkEditable* previewPassword = nullptr;
    unsigned previewSerial = 0;
    GtkEditable* search = nullptr;
    GtkWidget *newButton = nullptr, *openButton = nullptr, *saveButton = nullptr, *lockButton = nullptr;
    GtkWidget* cancelButton = nullptr;
    GtkWidget* dirtyIndicator = nullptr;
    bool closeSavePending = false;
    GtkWidget* automaticRecoveryButton = nullptr;
    AdwBanner* guardBanner = nullptr;
    std::unique_ptr<SessionGuard> guard;
    IdleLock idle;
    guint idleTimer = 0;
    bool clipboardAccepted = false;
    std::string checkpointFolder;
    GtkWidget *copySaveButton = nullptr, *saveAsButton = nullptr, *recoveryButton = nullptr;
    GtkWidget *groupAdd = nullptr, *groupRename = nullptr, *groupDelete = nullptr;
    GtkWidget *entryAdd = nullptr, *entryEdit = nullptr, *entryDelete = nullptr, *copyUser = nullptr, *copyPass = nullptr;
    GWeakRef editor{};
    NativePicker picker;
    std::unique_ptr<Appearance> appearance;
    GdkClipboard* clipboard = nullptr;
    GdkClipboard* primaryClipboard = nullptr;
    GdkContentProvider* copied = nullptr;
    guint clipboardTimer = 0, clipboardCleanup = 0;
    std::unique_ptr<FontCache> fontCache;
    bool lockReady = false;
    std::vector<GdkContentProvider*> revokingProviders;
    std::vector<GroupSummary> groupRows;
    std::vector<EntrySummary> entryRows;
    std::string selectedGroup;
    bool unlocked = false;
    bool hasOpenedDatabase = false;
    bool dirty = false;
    bool busy = false;
    bool refreshing = false;
    bool closing = false;
    bool loading = false;
    bool saving = false;
    bool sourceKnown = false;
    std::string recoveryFolder;
    std::string activeRecovery;
    unsigned pending = 0;
    unsigned generation = 0;
    unsigned refreshSerial = 0;
    unsigned pickerSerial = 0;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
    Worker worker;
};
}

int main(int argc, char** argv)
{
    initializeUiLocale();
    const auto locale = localeDirectory();
    bindtextdomain("keepassxc-gnome", locale.c_str());
    bind_textdomain_codeset("keepassxc-gnome", "UTF-8");
    textdomain("keepassxc-gnome");
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::puts(KPXC_GNOME_DEVELOPMENT_UI ? "KeePassXC GNOME Development 0.1.0" : "KeePassXC GNOME 0.1.0");
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--language-check") {
        std::puts(tr("Ready"));
        return 0;
    }
    const auto coreConfig = std::filesystem::path(g_get_user_config_dir()) / "keepassxc-gnome";
    std::filesystem::create_directories(coreConfig);
    const auto coreSettings = coreConfig / "core.ini";
    const auto coreLocalSettings = coreConfig / "core-local.ini";
    g_setenv("KPXC_CONFIG", coreSettings.c_str(), TRUE);
    g_setenv("KPXC_CONFIG_LOCAL", coreLocalSettings.c_str(), TRUE);
    QCoreApplication qt(argc, argv);
    if (!CoreSession::initialize()) {
        std::fputs("KeePassXC crypto initialization failed\n", stderr);
        return 1;
    }
    auto* application = adw_application_new("org.keepassxc.Gnome.Development", GApplicationFlags(G_APPLICATION_NON_UNIQUE | G_APPLICATION_HANDLES_OPEN));
    std::unique_ptr<Window> window;
    g_signal_connect(application, "activate", G_CALLBACK(+[](GApplication* app, gpointer data) {
        auto& window = *static_cast<std::unique_ptr<Window>*>(data);
        if (!window) window = std::make_unique<Window>(ADW_APPLICATION(app));
        window->present();
    }), &window);
    g_signal_connect(application, "open", G_CALLBACK(+[](GApplication* app, GFile** files, int count, const char*, gpointer data) {
        auto& window = *static_cast<std::unique_ptr<Window>*>(data);
        if (!window) window = std::make_unique<Window>(ADW_APPLICATION(app));
        window->present(); window->openFiles(files, count);
    }), &window);
    const auto result = g_application_run(G_APPLICATION(application), argc, argv);
    window.reset();
    g_object_unref(application);
    return result;
}
