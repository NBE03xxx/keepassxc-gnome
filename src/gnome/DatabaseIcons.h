#pragma once
#include <adwaita.h>
#include <functional>
#include <libintl.h>
#include <memory>
#include <string>

GResource* database_icons_get_resource();

namespace DatabaseIcons {
inline void set(GtkImage* image, int number, bool custom = false) {
    (void)database_icons_get_resource(); // Also retains the resource object when statically linked.
    if (custom || number < 0 || number > 68) {
        gtk_image_set_from_icon_name(image, "image-x-generic-symbolic");
        gtk_widget_set_tooltip_text(GTK_WIDGET(image), gettext("Custom icon (preserved)"));
    } else {
        const auto path = std::string("/org/keepassxc/Gnome/database-icons/") + std::to_string(number) + ".svg";
        gtk_image_set_from_resource(image, path.c_str());
        gtk_widget_set_tooltip_text(GTK_WIDGET(image), nullptr);
    }
    gtk_image_set_pixel_size(image, 20);
}

// An unchanged selection is represented by -1, so editing text never replaces
// a custom or future icon. Only an explicit choice writes a standard IconID.
inline GtkWidget* chooser(int current, bool custom, std::shared_ptr<int> choice) {
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    auto* menu = gtk_menu_button_new();
    auto* selected = GTK_IMAGE(gtk_image_new()); set(selected, current < 0 ? 0 : current, custom);
    auto* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(selected));
    gtk_box_append(GTK_BOX(content), gtk_label_new(gettext("Choose icon")));
    gtk_menu_button_set_child(GTK_MENU_BUTTON(menu), content);
    gtk_accessible_update_property(GTK_ACCESSIBLE(menu), GTK_ACCESSIBLE_PROPERTY_LABEL, gettext("Choose icon"), -1);
    auto* popover = GTK_POPOVER(gtk_popover_new());
    auto* grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 4); gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    for (int i = 0; i < 69; ++i) {
        auto* button = gtk_button_new(); auto* icon = GTK_IMAGE(gtk_image_new()); set(icon, i);
        gtk_button_set_child(GTK_BUTTON(button), GTK_WIDGET(icon));
        auto* label = g_strdup_printf(gettext("Icon %d"), i);
        gtk_widget_set_tooltip_text(button, label);
        gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1); g_free(label);
        auto* callback = new std::function<void()>([choice, selected, popover, i] { *choice = i; set(selected, i); gtk_popover_popdown(popover); });
        g_signal_connect_data(button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { (*static_cast<std::function<void()>*>(p))(); }),
            callback, +[](gpointer p, GClosure*) { delete static_cast<std::function<void()>*>(p); }, G_CONNECT_DEFAULT);
        gtk_grid_attach(GTK_GRID(grid), button, i % 7, i / 7, 1, 1);
    }
    auto* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 300);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
    gtk_popover_set_child(popover, scroll); gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), GTK_WIDGET(popover));
    gtk_box_append(GTK_BOX(box), menu);
    if (custom) {
        auto* hint = gtk_label_new(gettext("The existing custom icon is kept unless you choose another icon."));
        gtk_label_set_wrap(GTK_LABEL(hint), TRUE); gtk_box_append(GTK_BOX(box), hint);
    }
    return box;
}
}
