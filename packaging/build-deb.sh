#!/usr/bin/env bash
set -euo pipefail

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
. /etc/os-release
if [ "$ID" != ubuntu ]; then
    echo "Build this package on Ubuntu so it uses Ubuntu's runtime libraries." >&2
    exit 1
fi

build_dir=${PAPER_READER_BUILD_DIR:-"$here/build-ubuntu"}
dist_dir=${PAPER_READER_DIST_DIR:-"$here/dist"}
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-2}
reader_version=$(sed -nE 's/^project\([^ ]+ VERSION ([0-9.]+).*$/\1/p' "$here/CMakeLists.txt")
package_version=${PAPER_READER_PACKAGE_VERSION:-"$reader_version+git$(git -C "$here" rev-parse --short=12 HEAD)"}
architecture=$(dpkg --print-architecture)

cmake -S "$here" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_UI=ON -DBUILD_TESTS=ON
cmake --build "$build_dir" --parallel "$jobs"
ctest --test-dir "$build_dir" --output-on-failure

mkdir -p "$dist_dir"
stage=$(mktemp -d "$build_dir/deb-stage.XXXXXX")
chmod 755 "$stage"
DESTDIR="$stage" cmake --install "$build_dir" --strip
metadata=$(mktemp -d "$build_dir/deb-metadata.XXXXXX")
mkdir -p "$stage/DEBIAN" "$metadata/debian"
cat > "$metadata/debian/control" <<'EOF'
Source: paper-reader
Section: utils
Priority: optional
Maintainer: Paper Reader contributors <noreply@github.com>

Package: paper-reader
Architecture: any
Description: PDF reader with notes and browser chat
EOF
dependencies=$(cd "$metadata" && dpkg-shlibdeps -O -e"$stage/usr/bin/paper-reader")
dependencies=${dependencies#shlibs:Depends=}
cat > "$stage/DEBIAN/control" <<EOF
Package: paper-reader
Version: $package_version
Section: utils
Priority: optional
Architecture: $architecture
Maintainer: Paper Reader contributors <noreply@github.com>
Depends: $dependencies, libqt6sql6-sqlite, qt6-qpa-plugins, qpdf, poppler-utils
Recommends: qt6-wayland
Homepage: https://github.com/ibixina/reader
Description: PDF reader with notes and browser chat
 Read PDF papers, save highlights and notes, and use an embedded browser chat.
EOF
package="$dist_dir/paper-reader_${package_version}_ubuntu${VERSION_ID}_${architecture}.deb"
dpkg-deb --root-owner-group --build "$stage" "$package"
(cd "$dist_dir" && sha256sum "$(basename "$package")" > "$(basename "$package").sha256")
printf 'Package: %s\n' "$package"
