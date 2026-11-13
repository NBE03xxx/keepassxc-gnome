#!/usr/bin/env bash
# Optional diagnostics only, not product dependencies or an LSan suppression.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
mkdir -p build/gui-memory-audit
case ${1:---pango} in
    --cairo-glyph-cache)
        g++ -g -O1 -fsanitize=address -fno-omit-frame-pointer tests/cairo_glyph_cache_control.cpp \
            -o build/gui-memory-audit/cairo-glyph-cache-control $(pkg-config --cflags --libs cairo)
        ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 build/gui-memory-audit/cairo-glyph-cache-control
        ;;
    --pango-scales)
        g++ -g -fsanitize=address -fno-omit-frame-pointer tests/pango_scale_cache_control.cpp \
            -o build/gui-memory-audit/pango-scale-control $(pkg-config --cflags --libs pangocairo pangoft2)
        ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 build/gui-memory-audit/pango-scale-control
        ;;
    --empty-context)
        g++ -g -O1 -fsanitize=address -fno-omit-frame-pointer -pthread tests/empty_context_control.cpp \
            -o build/gui-memory-audit/empty-context-control
        ASAN_OPTIONS=detect_leaks=1 build/gui-memory-audit/empty-context-control
        ;;
    --vulkan-instance|--vulkan-device)
        g++ -g -fsanitize=address -fno-omit-frame-pointer tests/vulkan_memory_control.cpp \
            -o build/gui-memory-audit/vulkan-control $(pkg-config --cflags --libs vulkan)
        # A private ICD descriptor selects the existing AMD driver explicitly.
        python3 - <<'PY'
import json
from pathlib import Path
data=json.loads(Path('/usr/share/vulkan/icd.d/radeon_icd.json').read_text())
library=Path('.cache/development/sysroot/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so').resolve(strict=True)
data['ICD']['library_path']=str(library)
Path('build/gui-memory-audit/radeon-control.json').write_text(json.dumps(data))
PY
        args=(); if [[ $1 == --vulkan-device ]]; then args+=(--device); fi
        VK_DRIVER_FILES="$PWD/build/gui-memory-audit/radeon-control.json" \
            ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 \
            build/gui-memory-audit/vulkan-control "${args[@]}"
        ;;
    --offset-pattern|--offset-charset)
        name=${1#--offset-}
        g++ -g -O1 -fsanitize=address -fno-omit-frame-pointer "tests/font_${name}_offset_control.cpp" \
            -o "build/gui-memory-audit/font-${name}-offset-control" $(pkg-config --cflags --libs fontconfig)
        ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 \
            "build/gui-memory-audit/font-${name}-offset-control"
        ;;
    --fontconfig|--pango)
        g++ -g -fsanitize=address -fno-omit-frame-pointer tests/font_memory_control.cpp \
            -o build/gui-memory-audit/font-memory-control $(pkg-config --cflags --libs fontconfig pangocairo pangoft2)
        args=(); if [[ ${1:---pango} == --pango ]]; then args+=(pango); fi
        ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 \
            build/gui-memory-audit/font-memory-control "${args[@]}"
        ;;
    --gtk-shutdown)
        g++ -g -fsanitize=address -fno-omit-frame-pointer tests/gtk_font_shutdown_control.cpp \
            -o build/gui-memory-audit/gtk-font-shutdown-control $(pkg-config --cflags --libs libadwaita-1 fontconfig pangoft2)
        GSK_RENDERER=cairo bash tools/test-ui.sh --gtk-font-shutdown
        ;;
    *) echo 'Usage: tools/test-font-memory.sh --fontconfig|--pango|--gtk-shutdown|--offset-pattern|--offset-charset|--empty-context|--pango-scales|--cairo-glyph-cache|--vulkan-instance|--vulkan-device' >&2; exit 2;;
esac
