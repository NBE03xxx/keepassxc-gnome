#pragma once
#include "CoreSession.h"
#include "gnome/SummaryList.h"
#include <set>
#include <unordered_map>

// Tree nodes contain indices only, never credentials. GTK creates widgets for
// visible rows; unbind/teardown/lock scrub even externally retained row widgets.
class NavigationList {
public:
    struct Item { bool group = false; guint index = GTK_INVALID_LIST_POSITION; };
    using Bind = std::function<void(GtkWidget*, Item)>;
    NavigationList(Bind bind, std::function<void()> changed, std::function<void()> activated)
        : state(std::make_shared<State>()) {
        state->bind = std::move(bind); state->changed = std::move(changed); state->activated = std::move(activated);
        auto* factory = gtk_signal_list_item_factory_new();
        connect(factory, "setup", +[](GtkSignalListItemFactory*, GtkListItem* item, gpointer) {
            auto* expander = gtk_tree_expander_new();
            auto* row = SummaryList::newRow(false);
            gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), row);
            gtk_list_item_set_focusable(item, FALSE);
            gtk_list_item_set_child(item, expander);
        });
        connect(factory, "bind", +[](GtkSignalListItemFactory*, GtkListItem* item, gpointer p) {
            auto& s = **static_cast<std::shared_ptr<State>*>(p);
            auto* expander = GTK_TREE_EXPANDER(gtk_list_item_get_child(item));
            auto* treeRow = GTK_TREE_LIST_ROW(gtk_list_item_get_item(item));
            gtk_tree_expander_set_list_row(expander, treeRow);
            const auto node = decodeRow(treeRow);
            if (s.bind) s.bind(gtk_tree_expander_get_child(expander), node);
            gtk_accessible_update_property(GTK_ACCESSIBLE(expander), GTK_ACCESSIBLE_PROPERTY_LABEL,
                SummaryList::title(gtk_tree_expander_get_child(expander)), -1);
        });
        for (auto* signal : {"unbind", "teardown"}) connect(factory, signal, +[](GtkSignalListItemFactory*, GtkListItem* item, gpointer) {
            auto* expander = GTK_TREE_EXPANDER(gtk_list_item_get_child(item));
            if (!expander) return;
            gtk_accessible_update_property(GTK_ACCESSIBLE(expander), GTK_ACCESSIBLE_PROPERTY_LABEL, "", -1);
            scrub(GTK_WIDGET(expander)); gtk_tree_expander_set_list_row(expander, nullptr);
        });
        root = gtk_string_list_new(nullptr);
        tree = gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(root)), FALSE, FALSE,
            +[](gpointer object, gpointer p) -> GListModel* {
                auto& s = **static_cast<std::shared_ptr<State>*>(p);
                const auto node = decode(object);
                if (!node.group || node.index >= s.children.size()) return nullptr;
                // Empty groups still have an expander; additions are visible on refresh.
                return G_LIST_MODEL(g_object_ref(s.children[node.index]));
            }, new std::shared_ptr<State>(state), +[](gpointer p) { delete static_cast<std::shared_ptr<State>*>(p); });
        selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(tree)));
        gtk_single_selection_set_autoselect(selection, FALSE);
        gtk_single_selection_set_can_unselect(selection, TRUE);
        view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(selection)), factory);
        g_object_ref_sink(view); gtk_widget_add_css_class(view, "navigation-sidebar");
        g_signal_connect_data(selection, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer p) {
            auto& s = **static_cast<std::shared_ptr<State>*>(p); if (s.changed) s.changed();
        }), new std::shared_ptr<State>(state), destroy, G_CONNECT_DEFAULT);
        g_signal_connect_data(view, "activate", G_CALLBACK(+[](GtkListView*, guint, gpointer p) {
            auto& s = **static_cast<std::shared_ptr<State>*>(p); if (s.activated) s.activated();
        }), new std::shared_ptr<State>(state), destroy, G_CONNECT_DEFAULT);
        auto* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect_data(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType modifiers, gpointer p) -> gboolean {
            if ((key != GDK_KEY_Return && key != GDK_KEY_KP_Enter) || (modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK))) return FALSE;
            auto& s = **static_cast<std::shared_ptr<State>*>(p); if (s.activated) s.activated(); return TRUE;
        }), new std::shared_ptr<State>(state), destroy, G_CONNECT_DEFAULT);
        gtk_widget_add_controller(view, keys);
    }
    ~NavigationList() {
        state->bind = {}; state->changed = {}; state->activated = {};
        clear(); gtk_list_view_set_model(GTK_LIST_VIEW(view), nullptr); gtk_list_view_set_factory(GTK_LIST_VIEW(view), nullptr);
        g_object_unref(view); g_object_unref(selection); g_object_unref(tree); g_object_unref(root);
    }
    GtkWidget* widget() const { return view; }
    Item selected() const { auto* row = gtk_single_selection_get_selected_item(selection); return row ? decodeRow(GTK_TREE_LIST_ROW(row)) : Item{}; }
    guint visibleCount() const { return g_list_model_get_n_items(G_LIST_MODEL(tree)); }
    std::set<guint> expanded() const {
        std::set<guint> result;
        for (guint i = 0; i < visibleCount(); ++i) {
            auto* row = gtk_tree_list_model_get_row(tree, i); const auto node = decodeRow(row);
            if (node.group && gtk_tree_list_row_get_expanded(row)) result.insert(node.index);
            g_object_unref(row);
        }
        return result;
    }
    void toggleSelected() {
        const auto i = gtk_single_selection_get_selected(selection);
        if (i == GTK_INVALID_LIST_POSITION) return;
        auto* row = gtk_tree_list_model_get_row(tree, i);
        gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row)); g_object_unref(row);
    }
    void expandGroup(guint index) {
        if (index >= parents.size()) return;
        if (parents[index] != GTK_INVALID_LIST_POSITION) expandGroup(parents[index]);
        for (guint i = 0; i < visibleCount(); ++i) {
            auto* row = gtk_tree_list_model_get_row(tree, i); const auto node = decodeRow(row);
            if (node.group && node.index == index) { gtk_tree_list_row_set_expanded(row, TRUE); g_object_unref(row); return; }
            g_object_unref(row);
        }
    }
    void selectEntry(guint index) {
        if (index < entryParents.size() && entryParents[index] != GTK_INVALID_LIST_POSITION) expandGroup(entryParents[index]);
        select({false, index});
    }
    void select(Item target) {
        for (guint i = 0; i < visibleCount(); ++i) {
            auto* row = gtk_tree_list_model_get_row(tree, i); const auto node = decodeRow(row); g_object_unref(row);
            if (node.group == target.group && node.index == target.index) { gtk_single_selection_set_selected(selection, i); return; }
        }
        gtk_single_selection_set_selected(selection, GTK_INVALID_LIST_POSITION);
    }
    void clear() {
        scrub(view); gtk_string_list_splice(root, 0, g_list_model_get_n_items(G_LIST_MODEL(root)), nullptr);
        for (auto* model : state->children) { gtk_string_list_splice(model, 0, g_list_model_get_n_items(G_LIST_MODEL(model)), nullptr); g_object_unref(model); }
        state->children.clear(); parents.clear(); entryParents.clear();
    }
    void reset(const std::vector<GroupSummary>& groups, const std::vector<EntrySummary>& entries, bool searching, const std::set<guint>& expandedGroups = {}) {
        clear(); parents.resize(groups.size(), GTK_INVALID_LIST_POSITION); entryParents.resize(entries.size(), GTK_INVALID_LIST_POSITION);
        std::unordered_map<std::string, guint> lookup;
        std::vector<guint> ancestry;
        for (guint i = 0; i < groups.size(); ++i) {
            state->children.push_back(gtk_string_list_new(nullptr)); lookup.emplace(groups[i].id, i);
            while (ancestry.size() > groups[i].depth) ancestry.pop_back();
            if (!ancestry.empty()) parents[i] = ancestry.back();
            ancestry.push_back(i);
        }
        if (!searching) for (guint i = 0; i < groups.size(); ++i) {
            const auto token = "g" + std::to_string(i);
            gtk_string_list_append(parents[i] == GTK_INVALID_LIST_POSITION ? root : state->children[parents[i]], token.c_str());
        }
        for (guint i = 0; i < entries.size(); ++i) {
            const auto it = lookup.find(entries[i].group);
            if (it != lookup.end()) entryParents[i] = it->second;
            const auto token = "e" + std::to_string(i);
            gtk_string_list_append(searching || it == lookup.end() ? root : state->children[it->second], token.c_str());
        }
        for (auto index : expandedGroups) expandGroup(index);
    }
    static void scrub(GtkWidget* widget) {
        if (GTK_IS_TREE_EXPANDER(widget)) gtk_accessible_update_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, "", -1);
        SummaryList::clearText(widget);
        clearImages(widget);
    }
private:
    static void clearImages(GtkWidget* widget) {
        if (!widget) return;
        if (GTK_IS_TREE_EXPANDER(widget)) gtk_accessible_update_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, "", -1);
        if (GTK_IS_IMAGE(widget)) { gtk_image_clear(GTK_IMAGE(widget)); gtk_widget_set_tooltip_text(widget, nullptr); }
        for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) clearImages(child);
    }
    static Item decodeRow(GtkTreeListRow* row) {
        auto* object = gtk_tree_list_row_get_item(row); // transfer full
        if (!object) return {};
        const auto result = decode(object); g_object_unref(object); return result;
    }
    static Item decode(gpointer object) {
        const char* token = gtk_string_object_get_string(GTK_STRING_OBJECT(object));
        return {token[0] == 'g', guint(g_ascii_strtoull(token + 1, nullptr, 10))};
    }
    struct State {
        Bind bind; std::function<void()> changed, activated;
        std::vector<GtkStringList*> children;
    };
    static void destroy(gpointer p, GClosure*) { delete static_cast<std::shared_ptr<State>*>(p); }
    using Callback = void(*)(GtkSignalListItemFactory*, GtkListItem*, gpointer);
    void connect(GtkListItemFactory* factory, const char* signal, Callback callback) {
        g_signal_connect_data(factory, signal, G_CALLBACK(callback), new std::shared_ptr<State>(state), destroy, G_CONNECT_DEFAULT);
    }
    std::shared_ptr<State> state;
    std::vector<guint> parents, entryParents;
    GtkStringList* root;
    GtkTreeListModel* tree;
    GtkSingleSelection* selection;
    GtkWidget* view;
};
