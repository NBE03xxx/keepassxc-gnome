"""Orca customization copied only into the disposable test's XDG data dir."""
from orca import debug, gsettings_registry
if debug.debugFile is not None:
    # The driver must see the current utterance without generating another event.
    debug.debugFile.reconfigure(line_buffering=True, write_through=True)
# No physical braille device exists on the private session. Avoid retry waits.
gsettings_registry.get_registry().set_runtime_value('braille', 'enabled', False)
