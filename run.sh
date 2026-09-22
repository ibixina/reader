#!/usr/bin/env bash
set -eu

# Launch using system Qt or an optional unpacked Qt sysroot. Paths can be
# overridden for local builds with PAPER_READER_BUILD_DIR and
# PAPER_READER_QT_ROOT.
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
build_dir=${PAPER_READER_BUILD_DIR:-"$here/build-qt"}
qt_root=${PAPER_READER_QT_ROOT:-"${HOME:-}/qtsysroot"}
miniforge_root=${PAPER_READER_MINIFORGE_ROOT:-"${CONDA_PREFIX:-${HOME:-}/miniforge3}"}

if [ -d "$qt_root/usr/lib64" ]; then
    # Prefer the host's ABI-critical runtimes (expat, OpenSSL, ICU, zstd)
    # before the Qt sysroot, and keep Conda last. QtWebEngine otherwise may
    # load an incompatible Conda runtime and fail before the window appears.
    lib_path=""
    for system_lib in /usr/lib64 /usr/lib/x86_64-linux-gnu; do
        if [ -d "$system_lib" ]; then
            lib_path="${lib_path:+$lib_path:}$system_lib"
        fi
    done
    lib_path="${lib_path:+$lib_path:}$qt_root/usr/lib64"
    if [ -d "$miniforge_root/lib" ]; then
        lib_path="$lib_path:$miniforge_root/lib"
    fi
    if [ -n "${LD_LIBRARY_PATH:-}" ]; then
        lib_path="$lib_path:$LD_LIBRARY_PATH"
    fi
    export LD_LIBRARY_PATH="$lib_path"

    if [ -x "$qt_root/usr/lib64/qt6/libexec/QtWebEngineProcess" ]; then
        export QTWEBENGINEPROCESS_PATH="$qt_root/usr/lib64/qt6/libexec/QtWebEngineProcess"
    fi
    if [ -d "$qt_root/usr/share/qt6/resources" ]; then
        export QTWEBENGINE_RESOURCES_PATH="$qt_root/usr/share/qt6/resources"
    fi
    if [ -d "$qt_root/usr/share/qt6/translations/qtwebengine_locales" ]; then
        export QTWEBENGINE_LOCALES_PATH="$qt_root/usr/share/qt6/translations/qtwebengine_locales"
    fi
    if [ -d "$qt_root/usr/share/qt6/qtwebengine_dictionaries" ]; then
        export QTWEBENGINE_DICTIONARIES_PATH="$qt_root/usr/share/qt6/qtwebengine_dictionaries"
    fi
fi

# Chromium's sandbox remains enabled by default. Restricted containers can
# opt into the workaround explicitly with PAPER_READER_NO_SANDBOX=1.
if [ "${PAPER_READER_NO_SANDBOX:-0}" = "1" ]; then
    export QTWEBENGINE_DISABLE_SANDBOX=1
    export QTWEBENGINE_CHROMIUM_FLAGS="${QTWEBENGINE_CHROMIUM_FLAGS:---no-sandbox --disable-gpu}"
fi

binary="$build_dir/paper-reader"
if [ ! -x "$binary" ]; then
    echo "paper-reader executable not found: $binary" >&2
    echo "Set PAPER_READER_BUILD_DIR to a configured build directory." >&2
    exit 1
fi
exec "$binary" "$@"
