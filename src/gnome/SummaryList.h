#pragma once
#include <adwaita.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The GTK model stores IDs only. Account text is bound to visible/recycled
// rows, and cleared on unbind, teardown and lock (including retained rows).
class SummaryList {
public:
    using Bind = std::function<void(GtkWidget*, guint)>;
    SummaryList(bool entries, Bind bind, std::function<void()> changed, std::function<void(guint)> activated = {})
        : state(std::make_shared<State>()) {
        state->bind = std::move(bind); state->changed = std::move(changed); state->activated = std::move(activated);
        auto* factory = gtk_signal_list_item_factory_new();
        connect(factory, "setup", +[](GtkSignalListItemFactory*, GtkListItem* item, gpointer p) {
            auto& s = **static_cast<std::shared_ptr<State>*>(p);
            auto* row = newRow(s.entries);
            gtk_list_item_set_focusable(item, TRUE);
            gtk_list_item_set_child(item, row);
        });
        connect(factory, "bind", +[](GtkSignalListItemFactory*, GtkListItem* item, gpointer p) {
            auto& s = **static_cast<std::shared_ptr<State>*>(p);
            if (s.bind) s.bind(gtk_list_item_get_child(item), gtk_list_item_get_position(item));
            gtk_list_item_set_accessible_label(item, title(gtk_list_item_get_child(item)));
        });
        for (auto* signal : {"unbind", "teardown"}) connect(factory, signal, +[](GtkSignalListItemFactory*, GtkListItem* item, gpointer) { gtk_list_item_set_accessible_label(item, ""); clearText(gtk_list_item_get_child(item)); });
        state->entries = entries;
        strings = gtk_string_list_new(nullptr);
        selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(strings)));
        state->selection = selection;
        gtk_single_selection_set_autoselect(selection, FALSE);
        gtk_single_selection_set_can_unselect(selection, TRUE);
        view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(selection)), factory);
        g_object_ref_sink(view);
        gtk_widget_set_focusable(view, TRUE);
        gtk_list_view_set_tab_behavior(GTK_LIST_VIEW(view), GTK_LIST_TAB_ITEM);
        gtk_widget_add_css_class(view, entries ? "boxed-list" : "navigation-sidebar");
        if (entries) gtk_widget_add_css_class(view, "kpxc-entry-list");
        auto* data = new std::shared_ptr<State>(state);
        g_signal_connect_data(selection, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer p) {
            auto& s = **static_cast<std::shared_ptr<State>*>(p); if (s.changed) s.changed();
        }), data, +[](gpointer p, GClosure*) { delete static_cast<std::shared_ptr<State>*>(p); }, G_CONNECT_DEFAULT);
        auto* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect_data(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey* controller, guint key, guint, GdkModifierType modifiers, gpointer p) -> gboolean {
            auto& s = **static_cast<std::shared_ptr<State>*>(p);
            if (!s.selection || (modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK))) return FALSE;
            const auto count = g_list_model_get_n_items(G_LIST_MODEL(s.selection));
            auto selected = gtk_single_selection_get_selected(s.selection);
            if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && selected < count && s.activated) { s.activated(selected); return TRUE; }
            if (!count || (key != GDK_KEY_Down && key != GDK_KEY_Up)) return FALSE;
            if (selected >= count) selected = 0;
            else if (key == GDK_KEY_Down && selected + 1 < count) ++selected;
            else if (key == GDK_KEY_Up && selected > 0) --selected;
            gtk_single_selection_set_selected(s.selection, selected);
            gtk_list_view_scroll_to(GTK_LIST_VIEW(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller))), selected, GTK_LIST_SCROLL_FOCUS, nullptr);
            return TRUE;
        }), new std::shared_ptr<State>(state), +[](gpointer p, GClosure*) { delete static_cast<std::shared_ptr<State>*>(p); }, G_CONNECT_DEFAULT);
        gtk_widget_add_controller(view, keys);
        data = new std::shared_ptr<State>(state);
        g_signal_connect_data(view, "activate", G_CALLBACK(+[](GtkListView*, guint position, gpointer p) {
            auto& s = **static_cast<std::shared_ptr<State>*>(p); if (s.activated) s.activated(position);
        }), data, +[](gpointer p, GClosure*) { delete static_cast<std::shared_ptr<State>*>(p); }, G_CONNECT_DEFAULT);
    }
    ~SummaryList() {
        state->bind = {}; state->changed = {}; state->activated = {}; state->selection = nullptr;
        clear(); gtk_list_view_set_model(GTK_LIST_VIEW(view), nullptr); gtk_list_view_set_factory(GTK_LIST_VIEW(view), nullptr);
        g_object_unref(view); g_object_unref(selection); g_object_unref(strings);
    }
    GtkWidget* widget() const { return view; }
    guint selected() const { return gtk_single_selection_get_selected(selection); }
    void select(guint i) { gtk_single_selection_set_selected(selection, i); }
    void clear() {
        clearText(view); gtk_string_list_splice(strings, 0, g_list_model_get_n_items(G_LIST_MODEL(strings)), nullptr);
    }
    void append(const std::vector<std::string>& ids) {
        std::vector<const char*> values; values.reserve(ids.size() + 1);
        for (const auto& id : ids) values.push_back(id.c_str());
        values.push_back(nullptr);
        gtk_string_list_splice(strings, g_list_model_get_n_items(G_LIST_MODEL(strings)), 0, values.data());
    }
    static GtkWidget* newRow(bool entry) {
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(row, 12); gtk_widget_set_margin_end(row, 12);
        gtk_widget_set_margin_top(row, 8); gtk_widget_set_margin_bottom(row, 8);
        auto* icon = gtk_image_new(); gtk_box_append(GTK_BOX(row), icon);
        g_object_set_data(G_OBJECT(row), "database-icon", icon);
        auto* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2); gtk_widget_set_hexpand(text, TRUE);
        auto* label = gtk_label_new(""); gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(text), label); g_object_set_data(G_OBJECT(row), "summary-title", label);
        if (entry) {
            auto* subtitle = gtk_label_new(""); gtk_label_set_xalign(GTK_LABEL(subtitle), 0);
            gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END); gtk_widget_add_css_class(subtitle, "dim-label");
            gtk_box_append(GTK_BOX(text), subtitle); g_object_set_data(G_OBJECT(row), "summary-subtitle", subtitle);
        }
        gtk_box_append(GTK_BOX(row), text); return row;
    }
    static const char* title(GtkWidget* row) { return gtk_label_get_text(GTK_LABEL(g_object_get_data(G_OBJECT(row), "summary-title"))); }
    static void setTitle(GtkWidget* row, const char* text) { gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(row), "summary-title")), text); }
    static void setSubtitle(GtkWidget* row, const char* text) { gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(row), "summary-subtitle")), text); }
    static void clearText(GtkWidget* widget) {
        if (!widget) return;
        if (GTK_IS_IMAGE(widget)) { gtk_image_clear(GTK_IMAGE(widget)); gtk_widget_set_tooltip_text(widget, nullptr); }
        if (ADW_IS_ACTION_ROW(widget)) adw_action_row_set_subtitle(ADW_ACTION_ROW(widget), "");
        if (ADW_IS_PREFERENCES_ROW(widget)) adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widget), "");
        if (GTK_IS_LABEL(widget)) gtk_label_set_text(GTK_LABEL(widget), "");
        for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) clearText(child);
    }
private:
    struct State { bool entries = false; Bind bind; std::function<void()> changed; std::function<void(guint)> activated; GtkSingleSelection* selection = nullptr; };
    using Callback = void(*)(GtkSignalListItemFactory*, GtkListItem*, gpointer);
    void connect(GtkListItemFactory* factory, const char* signal, Callback callback) {
        g_signal_connect_data(factory, signal, G_CALLBACK(callback), new std::shared_ptr<State>(state),
            +[](gpointer p, GClosure*) { delete static_cast<std::shared_ptr<State>*>(p); }, G_CONNECT_DEFAULT);
    }
    std::shared_ptr<State> state;
    GtkStringList* strings;
    GtkSingleSelection* selection;
    GtkWidget* view;
};
