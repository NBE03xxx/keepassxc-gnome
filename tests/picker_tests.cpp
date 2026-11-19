#include "PickerPortal.h"
using namespace PickerTest;
int main(int argc, char** argv) {
    auto* test = g_test_dbus_new(G_TEST_DBUS_NONE); g_test_dbus_up(test);
    check(!prctl(PR_SET_DUMPABLE, 0), "parent remains dump protected");
    {
        Portal portal(g_test_dbus_get_bus_address(test)); NativePicker picker(argc == 2 ? argv[1] : "");
        auto run = [&](Portal::Reply reply, NativePicker::Mode mode, NativePicker::Status expected) {
            portal.reply = reply; portal.save = mode == NativePicker::Mode::Save; portal.folder = mode == NativePicker::Mode::Folder;
            portal.keyFile = mode == NativePicker::Mode::KeyFile;
            bool done = false;
            check(picker.choose(nullptr, {mode, "合成データを選択", "/tmp", portal.save ? "合成.kdbx" : ""}, [&](auto status, std::string path) {
                check(status == expected, "picker outcome");
                if (status == NativePicker::Status::Selected) check(path == "/tmp/合成 test.kdbx", "native UTF-8 local path decoded");
                else check(path.empty(), "no path after cancel or error");
                done = true;
            }), "picker started");
            check(!picker.choose(nullptr, {mode, "", {}, {}}, {}), "parallel chooser refused");
            until([&] { return done; }, "picker completion bounded"); check(!picker.active(), "helper reaped before completion");
        };
        run(Portal::Select, NativePicker::Mode::Open, NativePicker::Status::Selected);
        run(Portal::Select, NativePicker::Mode::Save, NativePicker::Status::Selected);
        run(Portal::Select, NativePicker::Mode::Folder, NativePicker::Status::Selected);
        run(Portal::Select, NativePicker::Mode::KeyFile, NativePicker::Status::Selected);
        run(Portal::Cancel, NativePicker::Mode::Open, NativePicker::Status::Cancelled);
        for (auto reply : {Portal::Multiple, Portal::Remote, Portal::Oversized, Portal::BadHandle, Portal::Fail}) run(reply, NativePicker::Mode::Open, NativePicker::Status::Failed);
        portal.reply = Portal::Hold; portal.save = portal.folder = false; bool cancelled = false; const auto calls = portal.calls;
        check(picker.choose(nullptr, {NativePicker::Mode::Open, "合成データを選択", {}, {}}, [&](auto status, auto path) { check(status == NativePicker::Status::Cancelled && path.empty(), "cancelled chooser publishes no path"); cancelled = true; }), "held chooser started");
        until([&] { return portal.calls > calls; }, "held chooser reached portal");
        picker.cancel(); until([&] { return cancelled; }, "cancellation reaps helper"); check(portal.closes == 1, "cancellation closes desktop request");
        run(Portal::Select, NativePicker::Mode::Open, NativePicker::Status::Selected);
        portal.reply = Portal::Hold; bool failed = false; const auto oldCalls = portal.calls;
        check(picker.choose(nullptr, {NativePicker::Mode::Open, "合成データを選択", {}, {}}, [&](auto status, auto path) {
            check(status == NativePicker::Status::Failed && path.empty(), "portal disappearance publishes no path"); failed = true;
        }), "owner-loss chooser started");
        until([&] { return portal.calls > oldCalls; }, "owner-loss chooser reached portal");
        auto* released = g_dbus_connection_call_sync(portal.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", "org.freedesktop.portal.Desktop"), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
        check(released, "release fake portal"); g_variant_unref(released);
        until([&] { return failed; }, "owner-loss completion bounded");
        NativePicker malformed("/usr/bin/printf"); failed = false;
        check(malformed.choose(nullptr, {NativePicker::Mode::Open, "", {}, {}}, [&](auto status, auto path) {
            check(status == NativePicker::Status::Failed && path.empty(), "malformed helper frame refused"); failed = true;
        }), "malformed helper started"); until([&] { return failed; }, "malformed helper reaped");
        NativePicker missing("/nonexistent/kpxc-picker"); failed = false;
        missing.choose(nullptr, {NativePicker::Mode::Open, "", {}, {}}, [&](auto status, auto path) {
            check(status == NativePicker::Status::Failed && path.empty(), "missing helper reports actionable failure"); failed = true;
        }); check(failed && !missing.active(), "failed startup completes synchronously");
        check(prctl(PR_GET_DUMPABLE) == 0, "main process protection never relaxed");
    }
    g_test_dbus_down(test); g_object_unref(test);
    std::printf("PASS native picker: %u checks\n", checks);
}
