#pragma once
#include <gtk/gtk.h>
#include <functional>
#include <memory>
#include <string>

class NativePicker {
public:
    enum class Mode { Open, Save, Folder };
    enum class Status { Selected, Cancelled, Failed };
    struct Options { Mode mode; std::string title, folder, name; };
    using Done = std::function<void(Status, std::string)>;
    explicit NativePicker(std::string executable = {});
    ~NativePicker();
    NativePicker(const NativePicker&) = delete;
    NativePicker& operator=(const NativePicker&) = delete;
    bool choose(GtkWindow* parent, Options options, Done done);
    void cancel();
    bool active() const;
private:
    struct Request;
    std::string executable;
    std::shared_ptr<Request> request;
};
