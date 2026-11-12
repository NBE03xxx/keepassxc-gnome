#include "CoreSession.h"

#include <adwaita.h>
#include <QCoreApplication>
#include <QThread>

#include <clocale>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <libintl.h>
#include <memory>
#include <string>
#include <utility>

namespace
{
const char* tr(const char* message) { return gettext(message); }

class Worker
{
public:
    Worker()
    {
        receiver->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, receiver, &QObject::deleteLater);
        thread.start();
    }

    ~Worker()
    {
        QMetaObject::invokeMethod(receiver, [this] { session.reset(); }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
    }

    template<typename Task, typename Done>
    void run(Task task, Done done)
    {
        QMetaObject::invokeMethod(receiver, [this, task = std::move(task), done = std::move(done)]() mutable {
            if (!session) {
                session = std::make_unique<CoreSession>();
            }
            auto result = task(*session);
            auto* reply = new std::function<void()>([done = std::move(done), result = std::move(result)]() mutable {
                done(std::move(result));
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
    QThread thread;
    QObject* receiver = new QObject;
    std::unique_ptr<CoreSession> session;
};

struct Snapshot {
    std::vector<GroupSummary> groups;
    std::vector<EntrySummary> entries;
};

class Window
{
public:
    explicit Window(AdwApplication* application)
        : application(application)
    {
        g_weak_ref_init(&editor, nullptr);
        window = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(application)));
        gtk_window_set_title(window, tr("KeePassXC GNOME — Development"));
        gtk_window_set_default_size(window, 980, 660);
        g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
            auto& self = *static_cast<Window*>(data);
            self.confirmDiscard([&self] {
                self.closing = true;
                self.conceal();
                gtk_window_destroy(self.window);
            });
            return TRUE;
        }), this);

        auto* toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
        auto* header = ADW_HEADER_BAR(adw_header_bar_new());
        newButton = button(tr("New database"), [this] { newDatabase(); });
        openButton = button(tr("Open database"), [this] { chooseFile(false); });
        saveButton = button(tr("Save a copy"), [this] { chooseFile(true); });
        lockButton = button(tr("Lock"), [this] { confirmDiscard([this] { lock(); }); });
        adw_header_bar_pack_start(header, newButton);
        adw_header_bar_pack_start(header, openButton);
        adw_header_bar_pack_end(header, lockButton);
        adw_header_bar_pack_end(header, saveButton);
        adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));
        auto* banner = adw_banner_new(tr("Development build. Use synthetic test databases only."));
        adw_banner_set_revealed(ADW_BANNER(banner), TRUE);
        adw_toolbar_view_add_top_bar(toolbar, banner);

        auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        stack = GTK_STACK(gtk_stack_new());
        gtk_widget_set_vexpand(GTK_WIDGET(stack), TRUE);
        auto* welcome = adw_status_page_new();
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(welcome), "dialog-password-symbolic");
        adw_status_page_set_title(ADW_STATUS_PAGE(welcome), tr("Your database is locked"));
        adw_status_page_set_description(ADW_STATUS_PAGE(welcome), tr("Create or open a test database to get started."));
        gtk_stack_add_named(stack, welcome, "locked");

        auto* paned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
        gtk_paned_set_position(paned, 240);
        gtk_paned_set_shrink_start_child(paned, FALSE);
        auto* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_size_request(sidebar, 190, -1);
        auto* groupActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        groupAdd = button("+", [this] { groupName(false); });
        gtk_widget_set_tooltip_text(groupAdd, tr("New group"));
        groupRename = button(tr("Rename"), [this] { groupName(true); });
        groupDelete = button(tr("Delete"), [this] { deleteGroup(); });
        for (auto* item : {groupAdd, groupRename, groupDelete}) {
            gtk_box_append(GTK_BOX(groupActions), item);
        }
        gtk_box_append(GTK_BOX(sidebar), groupActions);
        groups = GTK_LIST_BOX(gtk_list_box_new());
        gtk_list_box_set_selection_mode(groups, GTK_SELECTION_SINGLE);
        gtk_widget_add_css_class(GTK_WIDGET(groups), "navigation-sidebar");
        gtk_box_append(GTK_BOX(sidebar), scrolled(GTK_WIDGET(groups)));
        gtk_paned_set_start_child(paned, sidebar);
        g_signal_connect(groups, "row-selected", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
            auto& self = *static_cast<Window*>(data);
            if (self.refreshing || !row) {
                return;
            }
            const auto index = gtk_list_box_row_get_index(row);
            if (index >= 0 && std::size_t(index) < self.groupRows.size()) {
                self.selectedGroup = self.groupRows[index].id;
                self.refresh();
            }
        }), this);

        auto* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        search = GTK_EDITABLE(gtk_search_entry_new());
        gtk_widget_set_tooltip_text(GTK_WIDGET(search), tr("Search entries"));
        gtk_box_append(GTK_BOX(right), GTK_WIDGET(search));
        g_signal_connect(search, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
            static_cast<Window*>(data)->refresh();
        }), this);
        auto* entryActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        entryAdd = button(tr("New entry"), [this] { edit({}); });
        entryEdit = button(tr("Edit"), [this] { editSelected(); });
        entryDelete = button(tr("Delete"), [this] { deleteEntry(); });
        copyUser = button(tr("Copy username"), [this] { copy(false); });
        copyPass = button(tr("Copy password"), [this] { copy(true); });
        for (auto* item : {entryAdd, entryEdit, entryDelete, copyUser, copyPass}) {
            gtk_box_append(GTK_BOX(entryActions), item);
        }
        gtk_box_append(GTK_BOX(right), entryActions);
        entries = GTK_LIST_BOX(gtk_list_box_new());
        gtk_list_box_set_selection_mode(entries, GTK_SELECTION_SINGLE);
        gtk_widget_add_css_class(GTK_WIDGET(entries), "boxed-list");
        gtk_box_append(GTK_BOX(right), scrolled(GTK_WIDGET(entries)));
        g_signal_connect(entries, "row-selected", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow*, gpointer data) {
            static_cast<Window*>(data)->sensitivity();
        }), this);
        g_signal_connect(entries, "row-activated", G_CALLBACK(+[](GtkListBox* list, GtkListBoxRow* row, gpointer data) {
            gtk_list_box_select_row(list, row);
            static_cast<Window*>(data)->editSelected();
        }), this);
        gtk_paned_set_end_child(paned, right);
        gtk_stack_add_named(stack, GTK_WIDGET(paned), "unlocked");
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
        adw_style_manager_set_color_scheme(adw_application_get_style_manager(application), ADW_COLOR_SCHEME_DEFAULT);
        clipboard = GDK_CLIPBOARD(g_object_ref(gtk_widget_get_clipboard(GTK_WIDGET(window))));
        sensitivity();
    }

    ~Window()
    {
        alive.reset();
        if (picker) g_cancellable_cancel(picker);
        g_clear_object(&picker);
        g_weak_ref_clear(&editor);
        clearClipboard();
        g_clear_object(&clipboard);
    }

    void present() { gtk_window_present(window); }

private:
    GtkWidget* button(const char* title, std::function<void()> clicked)
    {
        auto* result = gtk_button_new_with_label(title);
        auto* callback = new std::function<void()>(std::move(clicked));
        g_signal_connect_data(result, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            (*static_cast<std::function<void()>*>(data))();
        }), callback, +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, G_CONNECT_DEFAULT);
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

    static void clear(GtkListBox* list)
    {
        while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(list))) {
            gtk_list_box_remove(list, child);
        }
    }

    void sensitivity()
    {
        const bool ready = unlocked && !busy;
        const bool selected = ready && gtk_list_box_get_selected_row(entries);
        for (auto* item : {newButton, openButton}) {
            gtk_widget_set_sensitive(item, !busy);
        }
        for (auto* item : {saveButton, entryAdd, groupAdd, groupRename}) {
            gtk_widget_set_sensitive(item, ready);
        }
        gtk_widget_set_sensitive(groupDelete, ready && !groupRows.empty() && selectedGroup != groupRows.front().id);
        for (auto* item : {entryEdit, entryDelete, copyUser, copyPass}) {
            gtk_widget_set_sensitive(item, selected);
        }
        gtk_widget_set_sensitive(lockButton, unlocked);
    }

    void message(const char* text) { gtk_label_set_text(status, text); }

    template<typename Task, typename Done>
    void submit(Task task, Done done)
    {
        ++pending;
        busy = true;
        sensitivity();
        message(tr("Working…"));
        const auto epoch = generation;
        const std::weak_ptr<int> lifetime = alive;
        worker.run(std::move(task), [this, lifetime, epoch, done = std::move(done)](auto result) mutable {
            if (lifetime.expired()) return;
            --pending;
            busy = pending != 0;
            if (closing || epoch != generation) return;
            sensitivity();
            done(std::move(result));
        });
    }

    void confirm(const char* title, const char* body, const char* accept, std::function<void()> action)
    {
        auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, body));
        adw_alert_dialog_add_responses(dialog, "cancel", tr("Cancel"), "accept", accept, nullptr);
        adw_alert_dialog_set_default_response(dialog, "cancel");
        adw_alert_dialog_set_close_response(dialog, "cancel");
        adw_alert_dialog_set_response_appearance(dialog, "accept", ADW_RESPONSE_DESTRUCTIVE);
        auto* callback = new std::function<void()>(std::move(action));
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
        confirm(tr("Discard unsaved changes?"), tr("Save a copy first if you want to keep your changes."),
                tr("Discard"), std::move(action));
    }

    void conceal()
    {
        ++generation;
        unlocked = false;
        if (picker) g_cancellable_cancel(picker);
        auto* active = g_weak_ref_get(&editor);
        if (active) {
            clearForm(GTK_WIDGET(active));
            adw_dialog_force_close(ADW_DIALOG(active));
            g_object_unref(active);
        }
        clearClipboard();
        clear(groups);
        clear(entries);
        groupRows.clear();
        entryRows.clear();
        selectedGroup.clear();
        gtk_editable_set_text(search, "");
        unlocked = false;
        dirty = false;
        busy = pending != 0;
        gtk_stack_set_visible_child_name(stack, "locked");
        sensitivity();
    }

    void lock()
    {
        conceal();
        submit([](CoreSession& session) { session.lock(); return true; }, [this](bool) { message(tr("Locked")); });
    }

    void credentials(bool create, std::string path)
    {
        auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(create ? tr("New database") : tr("Unlock database"), nullptr));
        auto* fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkEditable* name = nullptr;
        if (create) {
            name = GTK_EDITABLE(gtk_entry_new());
            gtk_entry_set_placeholder_text(GTK_ENTRY(name), tr("Database name"));
            gtk_editable_set_text(name, tr("Passwords"));
            gtk_box_append(GTK_BOX(fields), GTK_WIDGET(name));
        }
        auto* password = GTK_EDITABLE(gtk_password_entry_new());
        g_object_set(password, "placeholder-text", tr("Master password"), "show-peek-icon", TRUE, nullptr);
        gtk_box_append(GTK_BOX(fields), GTK_WIDGET(password));
        GtkEditable* repeated = nullptr;
        if (create) {
            repeated = GTK_EDITABLE(gtk_password_entry_new());
            g_object_set(repeated, "placeholder-text", tr("Repeat master password"), nullptr);
            gtk_box_append(GTK_BOX(fields), GTK_WIDGET(repeated));
        }
        adw_alert_dialog_set_extra_child(dialog, fields);
        adw_alert_dialog_add_responses(dialog, "cancel", tr("Cancel"), "accept", create ? tr("Create") : tr("Unlock"), nullptr);
        adw_alert_dialog_set_response_appearance(dialog, "accept", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_close_response(dialog, "cancel");
        auto* callback = new std::function<void(const char*)>([this, create, path = std::move(path), name, password, repeated](const char* response) {
            if (!g_str_equal(response, "accept")) {
                gtk_editable_set_text(password, "");
                if (repeated) gtk_editable_set_text(repeated, "");
                return;
            }
            std::string secret = gtk_editable_get_text(password);
            const bool valid = !secret.empty() && (!repeated || secret == gtk_editable_get_text(repeated));
            const std::string title = create ? gtk_editable_get_text(name) : "";
            gtk_editable_set_text(password, "");
            if (repeated) gtk_editable_set_text(repeated, "");
            if (!valid) {
                message(tr("Enter matching, nonempty passwords."));
                return;
            }
            if (create && title.empty()) {
                message(tr("Enter a nonempty name."));
                return;
            }
            conceal();
            submit([create, path, title, secret = std::move(secret)](CoreSession& session) {
                session.lock();
                const bool ok = create ? session.create(title, secret) : session.unlock(path, secret);
                return std::pair{ok, create ? ReadFailure::InvalidInput : session.lastReadFailure()};
            }, [this, create](auto result) {
                const auto [ok, failure] = result;
                if (!ok) {
                    if (failure == ReadFailure::ResourceLimit) {
                        message(tr("This database exceeds the development build's read limits. The file was not changed."));
                    } else if (failure == ReadFailure::UnsupportedKdf) {
                        message(tr("This database uses KDF settings this build cannot preserve or compute correctly. The file was not changed."));
                    } else if (failure == ReadFailure::IoError) {
                        message(tr("Could not read a local regular database file."));
                    } else if (failure == ReadFailure::UnsupportedData) {
                        message(tr("This database contains data this build cannot safely preserve. The file was not changed."));
                    } else {
                        message(tr("Could not unlock the database. Check the password and file."));
                    }
                    return;
                }
                unlocked = true;
                dirty = create;
                gtk_stack_set_visible_child_name(stack, "unlocked");
                refresh();
            });
        });
        g_signal_connect_data(dialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
            (*static_cast<std::function<void(const char*)>*>(data))(response);
        }), callback, +[](gpointer data, GClosure*) { delete static_cast<std::function<void(const char*)>*>(data); }, G_CONNECT_DEFAULT);
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
    }

    void newDatabase() { confirmDiscard([this] { credentials(true, {}); }); }

    void chooseFile(bool save)
    {
        if (!save && dirty) {
            confirmDiscard([this] { chooseFileImpl(false); });
            return;
        }
        chooseFileImpl(save);
    }

    void chooseFileImpl(bool save)
    {
        if (picker) g_cancellable_cancel(picker);
        g_clear_object(&picker);
        picker = g_cancellable_new();
        const auto epoch = generation;
        const auto serial = ++pickerSerial;
        const std::weak_ptr<int> lifetime = alive;
        auto* chooser = gtk_file_dialog_new();
        gtk_file_dialog_set_title(chooser, save ? tr("Save a database copy") : tr("Open database"));
        if (save) gtk_file_dialog_set_initial_name(chooser, "Passwords.kdbx");
        auto* callback = new std::function<void(GObject*, GAsyncResult*)>([this, save, epoch, serial, lifetime](GObject* source, GAsyncResult* result) {
            GError* error = nullptr;
            auto* file = save ? gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error)
                              : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
            if (lifetime.expired() || closing || epoch != generation || serial != pickerSerial) {
                g_clear_object(&file);
                g_clear_error(&error);
                return;
            }
            g_clear_object(&picker);
            if (!file) {
                if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)
                    && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED)
                    && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                    message(tr("Could not open the file chooser. Try again."));
                }
                g_clear_error(&error);
                return;
            }
            auto* local = g_file_get_path(file);
            g_object_unref(file);
            if (!local) {
                message(tr("Choose a local file."));
                return;
            }
            std::string path(local);
            g_free(local);
            if (save) {
                submit([path](CoreSession& session) { return session.saveCopy(path); }, [this](bool ok) {
                    if (ok) dirty = false;
                    message(ok ? tr("Copy saved.") : tr("Could not save. Choose a new filename in a writable folder."));
                });
            } else {
                credentials(false, std::move(path));
            }
        });
        const auto done = +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<std::function<void(GObject*, GAsyncResult*)>> callback(
                static_cast<std::function<void(GObject*, GAsyncResult*)>*>(data));
            (*callback)(source, result);
        };
        if (save) gtk_file_dialog_save(chooser, window, picker, done, callback);
        else gtk_file_dialog_open(chooser, window, picker, done, callback);
        g_object_unref(chooser);
    }

    void refresh()
    {
        if (!unlocked) return;
        const auto serial = ++refreshSerial;
        const auto group = selectedGroup;
        const std::string query = gtk_editable_get_text(search);
        submit([group, query](CoreSession& session) { return Snapshot{session.groups(), session.search(query, group)}; },
               [this, serial](Snapshot snapshot) {
            if (serial != refreshSerial) return;
            const auto previousEntry = selectedEntry();
            refreshing = true;
            clear(groups);
            clear(entries);
            groupRows = std::move(snapshot.groups);
            entryRows = std::move(snapshot.entries);
            int selected = 0;
            for (std::size_t i = 0; i < groupRows.size(); ++i) {
                const auto& item = groupRows[i];
                std::string label(item.depth * 2, ' ');
                label += item.name;
                auto* row = gtk_label_new(label.c_str());
                gtk_label_set_xalign(GTK_LABEL(row), 0);
                gtk_widget_set_margin_top(row, 10);
                gtk_widget_set_margin_bottom(row, 10);
                gtk_list_box_append(groups, row);
                if (item.id == selectedGroup) selected = i;
            }
            if (!groupRows.empty()) {
                selectedGroup = groupRows[selected].id;
                gtk_list_box_select_row(groups, gtk_list_box_get_row_at_index(groups, selected));
            }
            for (const auto& item : entryRows) {
                auto* row = adw_action_row_new();
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), item.title.c_str());
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), item.username.c_str());
                gtk_list_box_append(entries, row);
                if (item.id == previousEntry) gtk_list_box_select_row(entries, GTK_LIST_BOX_ROW(row));
            }
            refreshing = false;
            sensitivity();
            message(dirty ? tr("Unsaved changes") : tr("Ready"));
        });
    }

    std::string selectedEntry() const
    {
        auto* row = gtk_list_box_get_selected_row(entries);
        if (!row) return {};
        const auto index = gtk_list_box_row_get_index(row);
        return index >= 0 && std::size_t(index) < entryRows.size() ? entryRows[index].id : std::string{};
    }

    void groupName(bool rename)
    {
        if (!unlocked || busy) return;
        auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(rename ? tr("Rename group") : tr("New group"), nullptr));
        auto* input = GTK_EDITABLE(gtk_entry_new());
        if (rename) {
            for (const auto& group : groupRows) {
                if (group.id == selectedGroup) gtk_editable_set_text(input, group.name.c_str());
            }
        }
        adw_alert_dialog_set_extra_child(dialog, GTK_WIDGET(input));
        adw_alert_dialog_add_responses(dialog, "cancel", tr("Cancel"), "save", tr("Save"), nullptr);
        const auto group = selectedGroup;
        auto* callback = new std::function<void(const char*)>([this, rename, group, input](const char* response) {
            if (!g_str_equal(response, "save")) return;
            const std::string name = gtk_editable_get_text(input);
            submit([rename, group, name](CoreSession& session) {
                return rename ? session.renameGroup(group, name) : !session.addGroup(group, name).empty();
            }, [this](bool ok) {
                if (ok) { dirty = true; refresh(); }
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
            submit([id](CoreSession& session) { return session.removeGroup(id); }, [this](bool ok) {
                if (ok) { dirty = true; selectedGroup.clear(); refresh(); }
            });
        });
    }

    void deleteEntry()
    {
        const auto id = selectedEntry();
        if (id.empty()) return;
        confirm(tr("Delete entry?"), tr("The entry will move to the recycle bin."), tr("Delete"), [this, id] {
            submit([id](CoreSession& session) { return session.removeEntry(id); }, [this](bool ok) {
                if (ok) { dirty = true; refresh(); }
            });
        });
    }

    void editSelected()
    {
        const auto id = selectedEntry();
        if (id.empty()) return;
        submit([id](CoreSession& session) { EntryDetails details; session.entry(id, details); return details; },
               [this](EntryDetails details) { if (!details.id.empty()) edit(std::move(details)); });
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

    void edit(EntryDetails details)
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
        auto field = [credentials](const char* title, const std::string& value, bool secret = false) {
            auto* row = secret ? adw_password_entry_row_new() : adw_entry_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
            gtk_editable_set_text(GTK_EDITABLE(row), value.c_str());
            adw_preferences_group_add(credentials, row);
            return GTK_EDITABLE(row);
        };
        auto* title = field(tr("Title"), details.title);
        auto* username = field(tr("Username"), details.username);
        auto* password = field(tr("Password"), details.password, true);
        auto* generate = button(tr("Generate"), [password] {
            const auto generated = CoreSession::generatePassword();
            gtk_editable_set_text(password, generated.c_str());
        });
        adw_entry_row_add_suffix(ADW_ENTRY_ROW(password), generate);
        auto* url = field(tr("Website"), details.url);
        adw_preferences_page_add(page, credentials);
        auto* additional = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        auto* notesRow = ADW_EXPANDER_ROW(adw_expander_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(notesRow), tr("Notes"));
        auto* notes = GTK_TEXT_VIEW(gtk_text_view_new());
        gtk_text_view_set_wrap_mode(notes, GTK_WRAP_WORD_CHAR);
        gtk_widget_set_size_request(GTK_WIDGET(notes), -1, 140);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(notes), details.notes.c_str(), -1);
        adw_expander_row_add_row(notesRow, GTK_WIDGET(notes));
        adw_preferences_group_add(additional, GTK_WIDGET(notesRow));
        adw_preferences_page_add(page, additional);
        adw_toolbar_view_set_content(toolbar, GTK_WIDGET(page));
        const auto id = details.id;
        const auto group = details.group.empty() ? selectedGroup : details.group;
        auto* save = button(tr("Save entry"), [this, dialog, id, group, title, username, password, url, notes] {
            auto* buffer = gtk_text_view_get_buffer(notes);
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buffer, &start, &end);
            auto* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
            EntryDetails edited{id, group, gtk_editable_get_text(title), gtk_editable_get_text(username),
                                gtk_editable_get_text(password), gtk_editable_get_text(url), text};
            g_free(text);
            if (edited.title.empty()) return;
            gtk_editable_set_text(password, "");
            adw_dialog_force_close(dialog);
            submit([edited = std::move(edited)](CoreSession& session) {
                return edited.id.empty() ? !session.addEntry(edited.group, edited).empty() : session.updateEntry(edited);
            }, [this](bool ok) {
                if (ok) { dirty = true; refresh(); }
                else message(tr("Could not update the entry."));
            });
        });
        gtk_widget_add_css_class(save, "suggested-action");
        adw_header_bar_pack_end(header, save);
        adw_dialog_set_child(dialog, GTK_WIDGET(toolbar));
        g_weak_ref_set(&editor, G_OBJECT(dialog));
        g_signal_connect(dialog, "closed", G_CALLBACK(+[](AdwDialog* closed, gpointer) {
            clearForm(GTK_WIDGET(closed));
        }), nullptr);
        adw_dialog_present(dialog, GTK_WIDGET(window));
    }

    void copy(bool password)
    {
        const auto id = selectedEntry();
        if (id.empty()) return;
        submit([id, password](CoreSession& session) {
            EntryDetails details;
            return session.entry(id, details) ? (password ? details.password : details.username) : std::string{};
        }, [this](std::string value) {
            clearClipboard();
            copied = gdk_content_provider_new_typed(G_TYPE_STRING, value.c_str());
            gdk_clipboard_set_content(clipboard, copied);
            clipboardTimer = g_timeout_add_seconds(10, +[](gpointer data) -> gboolean {
                auto& self = *static_cast<Window*>(data);
                self.clipboardTimer = 0;
                self.clearClipboard();
                return G_SOURCE_REMOVE;
            }, this);
            message(tr("Copied for 10 seconds."));
        });
    }

    void clearClipboard()
    {
        if (clipboardTimer) {
            g_source_remove(clipboardTimer);
            clipboardTimer = 0;
        }
        if (copied) {
            if (gdk_clipboard_get_content(clipboard) == copied) gdk_clipboard_set_content(clipboard, nullptr);
            g_clear_object(&copied);
        }
    }

    AdwApplication* application;
    GtkWindow* window = nullptr;
    GtkStack* stack = nullptr;
    GtkLabel* status = nullptr;
    GtkListBox* groups = nullptr;
    GtkListBox* entries = nullptr;
    GtkEditable* search = nullptr;
    GtkWidget *newButton = nullptr, *openButton = nullptr, *saveButton = nullptr, *lockButton = nullptr;
    GtkWidget *groupAdd = nullptr, *groupRename = nullptr, *groupDelete = nullptr;
    GtkWidget *entryAdd = nullptr, *entryEdit = nullptr, *entryDelete = nullptr, *copyUser = nullptr, *copyPass = nullptr;
    GWeakRef editor{};
    GCancellable* picker = nullptr;
    GdkClipboard* clipboard = nullptr;
    GdkContentProvider* copied = nullptr;
    guint clipboardTimer = 0;
    std::vector<GroupSummary> groupRows;
    std::vector<EntrySummary> entryRows;
    std::string selectedGroup;
    bool unlocked = false;
    bool dirty = false;
    bool busy = false;
    bool refreshing = false;
    bool closing = false;
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
    setlocale(LC_ALL, "");
    const auto executable = std::filesystem::canonical("/proc/self/exe");
    const auto locale = executable.parent_path().parent_path() / "share/locale";
    bindtextdomain("keepassxc-gnome", locale.c_str());
    bind_textdomain_codeset("keepassxc-gnome", "UTF-8");
    textdomain("keepassxc-gnome");
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::puts("KeePassXC GNOME Development 0.1.0");
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
    auto* application = adw_application_new("org.keepassxc.Gnome.Development", G_APPLICATION_NON_UNIQUE);
    std::unique_ptr<Window> window;
    g_signal_connect(application, "activate", G_CALLBACK(+[](GApplication* app, gpointer data) {
        auto& window = *static_cast<std::unique_ptr<Window>*>(data);
        if (!window) window = std::make_unique<Window>(ADW_APPLICATION(app));
        window->present();
    }), &window);
    const auto result = g_application_run(G_APPLICATION(application), argc, argv);
    window.reset();
    g_object_unref(application);
    return result;
}
