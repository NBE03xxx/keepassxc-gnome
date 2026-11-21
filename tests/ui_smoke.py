#!/usr/bin/env python3
"""Exercise the development GUI through AT-SPI in an isolated GNOME session."""
import os
import subprocess
import time
import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi


def walk(item, depth=0):
    if depth > 40:
        return
    item.clear_cache()
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
            if item.get_role_name() in ("dialog", "alert", "frame") or "button" in item.get_role_name():
                print("OPEN WIDGET", item.get_role_name(), item.get_name(), "sensitive=", item.get_state_set().contains(Atspi.StateType.SENSITIVE), flush=True)
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
    def ready():
        item = named(name, "button")
        if not item or not item.get_state_set().contains(Atspi.StateType.SENSITIVE):
            return None
        for candidate in walk(item):
            action = candidate.get_action_iface()
            if action and action.get_n_actions() > 0:
                return candidate
        return None
    button = wait_for(ready, "enabled button " + name)
    assert button.get_action_iface().do_action(0), "activate " + name


def editables(root):
    return [item for item in walk(root)
            if item.get_role_name() in ("text", "password text") and item.get_editable_text_iface()]


def set_text(item, value):
    assert item.get_editable_text_iface().set_text_contents(value)


wait_for(application, "application registration")
wait_for(lambda: named("KeePassXC GNOMEへようこそ", "label"), "startup has no database open")
assert not named("データベースはロックされています", "label"), "startup must not claim a database is locked"
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
click("アイコンを選択")
click("アイコン 19")
click("エントリーを保存")
wait_for(lambda: not named("エントリーを作成", "dialog"), "new entry dialog closes")
wait_for(lambda: named("合成 UI Entry"), "new entry appears")
search = wait_for(lambda: named("エントリーを検索"), "search field")
set_text(search, "no-such-synthetic-entry")
wait_for(lambda: not named("合成 UI Entry"), "search excludes nonmatching entry")
set_text(search, "合成 UI")
row = wait_for(lambda: named("合成 UI Entry", "list item"), "search finds matching entry")
selected_row = row
while selected_row.get_parent() and not selected_row.get_parent().get_selection_iface():
    selected_row = selected_row.get_parent()
selection = selected_row.get_parent().get_selection_iface()
assert selection and selection.select_child(selected_row.get_index_in_parent()), "select entry"
def current_selection(label, role):
    # Model refresh can unbind and recycle the row selected above. Reacquire
    # it by its displayed identity and query live AT-SPI state each time.
    current = named(label, role)
    if not current:
        return None
    while current.get_parent() and not current.get_parent().get_selection_iface():
        current = current.get_parent()
    current.clear_cache()
    return current if current.get_state_set().contains(Atspi.StateType.SELECTED) else None

wait_for(lambda: current_selection("合成 UI Entry", "list item"), "entry is selected")
preview = wait_for(lambda: named("パスワードのプレビュー", "password text"), "password preview")
assert preview.get_state_set().contains(Atspi.StateType.VISIBLE), "preview is visible after selection"
assert Atspi.Text.get_text(preview, 0, -1) != "synthetic-ui-entry-password", "preview initially masks password"
assert not named("エントリーを編集", "dialog"), "selection only previews"
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
click("グループを作成")
dialog = wait_for(lambda: named("グループを作成", "alert"), "group creation")
set_text(editables(dialog)[0], "合成 UI Group")
group_save = wait_for(lambda: next((item for item in walk(dialog) if item.get_name() == "保存" and "button" in item.get_role_name()), None), "group save button")
assert group_save.get_action_iface().do_action(0)
wait_for(lambda: named("合成 UI Group", "label"), "new group appears")
click("合成 UI DB")
wait_for(lambda: not named("合成 UI Group", "label"), "one click collapses root group")
click("合成 UI DB")
wait_for(lambda: named("合成 UI Group", "label"), "one click expands root group")
group_row = wait_for(lambda: named("合成 UI Group", "button"), "subgroup expander")
while group_row.get_parent() and not group_row.get_parent().get_selection_iface():
    group_row = group_row.get_parent()
assert group_row.get_parent().get_selection_iface().select_child(group_row.get_index_in_parent()), "select subgroup"
wait_for(lambda: current_selection("合成 UI Group", "button"), "subgroup selected")
click("グループを編集")
icon_dialog = wait_for(lambda: named("グループを編集", "alert"), "combined group editor")
group_name = editables(icon_dialog)[0]
assert group_name.get_name() == "グループ名"
assert Atspi.Text.get_text(group_name, 0, -1) == "合成 UI Group", "editor targets selected subgroup"
set_text(group_name, "変更済み UI Group")
click("アイコンを選択"); click("アイコン 66")
icon_save = wait_for(lambda: next((item for item in walk(icon_dialog) if item.get_name() == "保存" and "button" in item.get_role_name()), None), "group icon save")
assert icon_save.get_action_iface().do_action(0)
wait_for(lambda: not named("グループを編集", "alert"), "combined group editor closes")
wait_for(lambda: named("変更済み UI Group", "label"), "combined edit updates group name")
assert named("合成 UI DB", "label"), "root name is unchanged"
click("グループを編集")
icon_dialog = wait_for(lambda: named("グループを編集", "alert"), "reopen group editor")
set_text(editables(icon_dialog)[0], "")
assert not next(item for item in walk(icon_dialog) if item.get_name() == "保存" and "button" in item.get_role_name()).get_state_set().contains(Atspi.StateType.SENSITIVE), "empty group name disables Save"
set_text(editables(icon_dialog)[0], "キャンセルするグループ")
click("アイコンを選択"); click("アイコン 19"); click("キャンセル")
wait_for(lambda: not named("グループを編集", "alert"), "cancel combined editor")
assert named("変更済み UI Group", "label") and not named("キャンセルするグループ", "label"), "cancel preserves group name"

click("ロック")
wait_for(lambda: named("未保存の変更を破棄しますか？", "alert"), "discard confirmation")
click("破棄")
wait_for(lambda: named("ロックしました", "label"), "database locked")
wait_for(lambda: named("データベースはロックされています", "label"), "previously opened database is actually locked")
assert not named("変更済み UI Entry")
assert not named("変更済み UI Group", "label")
version = subprocess.check_output([os.environ["KPXC_UI_APP"], "--version"], text=True).split()[-1]
click("データベースメニュー")
click("このアプリについて")
wait_for(lambda: named(version), "About dialog shows the same version as --version")
print("PASS Japanese About dialog from the locked screen")
print("PASS Japanese UI, database creation, entry creation/editing, search, group creation, discard confirmation and lock")
