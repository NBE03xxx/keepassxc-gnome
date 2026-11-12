#!/usr/bin/env python3
"""Exercise the development GUI through AT-SPI in an isolated GNOME session."""
import time
import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi


def walk(item, depth=0):
    if depth > 40:
        return
    yield item
    for index in range(item.get_child_count()):
        child = item.get_child_at_index(index)
        if child:
            yield from walk(child, depth + 1)


def wait_for(predicate, description, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except Exception:
            pass
        time.sleep(0.1)
    app = application()
    if app:
        for item in walk(app):
            if item.get_role_name() in ("dialog", "alert", "frame"):
                print("OPEN WINDOW", item.get_role_name(), item.get_name(), flush=True)
    raise AssertionError(description)


def application():
    desktop = Atspi.get_desktop(0)
    for index in range(desktop.get_child_count()):
        child = desktop.get_child_at_index(index)
        if child and child.get_name() == "keepassxc-gnome":
            return child


def named(name, role=None):
    app = application()
    if not app:
        return None
    for item in walk(app):
        if item.get_name().strip() == name and (role is None or role in item.get_role_name()):
            return item


def click(name):
    button = wait_for(lambda: named(name, "button"), "button " + name)
    assert button.get_state_set().contains(Atspi.StateType.SENSITIVE), name + " is disabled"
    assert button.get_action_iface().do_action(0), "activate " + name


def editables(root):
    return [item for item in walk(root)
            if item.get_role_name() in ("text", "password text") and item.get_editable_text_iface()]


def set_text(item, value):
    assert item.get_editable_text_iface().set_text_contents(value)


wait_for(application, "application registration")
click("データベースを作成")
dialog = wait_for(lambda: named("データベースを作成", "alert"), "create dialog")
fields = wait_for(lambda: editables(dialog) if len(editables(dialog)) == 3 else None, "three creation fields")
set_text(fields[0], "合成 UI DB")
set_text(fields[1], "synthetic-ui-master-password")
set_text(fields[2], "synthetic-ui-master-password")
click("作成")
wait_for(lambda: named("合成 UI DB", "label"), "created database group")
click("エントリーを作成")
dialog = wait_for(lambda: named("エントリーを作成", "dialog"), "entry editor")
fields = wait_for(lambda: editables(dialog) if len(editables(dialog)) >= 4 else None, "entry fields")
set_text(fields[0], "合成 UI Entry")
set_text(fields[1], "synthetic-ui-user")
set_text(fields[2], "synthetic-ui-entry-password")
set_text(fields[3], "https://example.invalid")
click("エントリーを保存")
wait_for(lambda: not named("エントリーを作成", "dialog"), "new entry dialog closes")
wait_for(lambda: named("合成 UI Entry"), "new entry appears")
search = wait_for(lambda: named("エントリーを検索"), "search field")
set_text(search, "no-such-synthetic-entry")
wait_for(lambda: not named("合成 UI Entry"), "search excludes nonmatching entry")
set_text(search, "合成 UI")
row = wait_for(lambda: named("合成 UI Entry", "list item"), "search finds matching entry")
selection = row.get_parent().get_selection_iface()
assert selection and selection.select_child(row.get_index_in_parent()), "select entry"
wait_for(lambda: row.get_state_set().contains(Atspi.StateType.SELECTED), "entry is selected")
click("編集")
dialog = wait_for(lambda: named("エントリーを編集", "dialog"), "existing entry editor")
fields = editables(dialog)
assert Atspi.Text.get_text(fields[1], 0, -1) == "synthetic-ui-user", "username retained"
set_text(fields[0], "変更済み UI Entry")
click("エントリーを保存")
wait_for(lambda: not named("エントリーを編集", "dialog"), "edit dialog closes")
wait_for(lambda: not named("合成 UI Entry"), "renamed entry leaves old search results")
set_text(search, "")
wait_for(lambda: named("変更済み UI Entry"), "edited entry appears after clearing search")
click("+")
dialog = wait_for(lambda: named("グループを作成", "alert"), "group creation")
set_text(editables(dialog)[0], "合成 UI Group")
click("保存")
wait_for(lambda: named("合成 UI Group", "label"), "new group appears")
click("ロック")
wait_for(lambda: named("未保存の変更を破棄しますか？", "alert"), "discard confirmation")
click("破棄")
wait_for(lambda: named("ロックしました", "label"), "database locked")
assert not named("変更済み UI Entry")
assert not named("合成 UI Group", "label")
print("PASS Japanese UI, database creation, entry creation/editing, search, group creation, discard confirmation and lock")
