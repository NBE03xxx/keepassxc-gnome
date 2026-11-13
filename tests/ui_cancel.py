"""Use the copied product UI with the deliberately blocked/crashing test helper."""
import time
import gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi


def walk(item):
    yield item
    for index in range(item.get_child_count()):
        child = item.get_child_at_index(index)
        if child:
            yield from walk(child)


def named(name, role=None):
    for item in walk(Atspi.get_desktop(0)):
        if item.get_name().strip() == name and (role is None or role in item.get_role_name()):
            return item


def wait_for(predicate, timeout=10):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try:
            value = predicate()
            if value:
                return value
        except Exception:
            pass
        time.sleep(.02)
    raise AssertionError("UI wait timed out")


def sensitive(name):
    item = named(name, "button")
    return item if item and item.get_state_set().contains(Atspi.StateType.SENSITIVE) else None


def click(name):
    item = wait_for(lambda: sensitive(name))
    assert item.get_action_iface().do_action(0)


def create(mode):
    click("データベースを作成")
    dialog = wait_for(lambda: named("データベースを作成", "alert"))
    def fields_ready():
        fields = [item for item in walk(dialog) if item.get_role_name() in ("text", "password text")
                  and item.get_editable_text_iface()]
        return fields if len(fields) == 3 else None
    fields = wait_for(fields_ready)
    for item, value in zip(fields, (mode, "synthetic-ui-cancel", "synthetic-ui-cancel")):
        assert item.get_editable_text_iface().set_text_contents(value)
    click("作成")
    wait_for(lambda: not named("データベースを作成", "alert"))


for _ in range(3):
    create("wall")
    wait_for(lambda: sensitive("キャンセル"))
    started = time.monotonic()
    click("キャンセル")
    wait_for(lambda: named("ロックしました", "label"), timeout=2)
    assert time.monotonic() - started < 2
    wait_for(lambda: sensitive("データベースを作成"))
    assert not sensitive("エントリーを作成")

create("crash")
wait_for(lambda: named("データベース処理が予期せず停止しました。データベースはロックされています。", "label"))
wait_for(lambda: sensitive("データベースを作成"))
assert not sensitive("エントリーを作成")
create("normal")
wait_for(lambda: named("未保存の変更があります", "label"))
click("ロック")
wait_for(lambda: named("未保存の変更を破棄しますか？", "alert"))
click("破棄")
wait_for(lambda: named("ロックしました", "label"))
create("wall")
wait_for(lambda: sensitive("キャンセル"))
close = wait_for(lambda: sensitive("ウィンドウを閉じる") or sensitive("閉じる") or sensitive("Close"))
started = time.monotonic()
assert close.get_action_iface().do_action(0)
wait_for(lambda: not named("データベースを作成", "button"), timeout=2)
assert time.monotonic() - started < 2
print("PASS UI cancellation, crash-to-lock, concealed entry controls, restart and close during a blocked request")
