#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Check the Linux installers by installing each in a clean container of the
# system it is for, the way a user would, and running what it installed.
#
#   tools/ci/check_linux_packages.sh <directory holding the .deb and .rpm> [<tag, such as v0.1.0-rc.1>]
#
# Needs docker. The release workflow runs it after cpack; it runs the same way
# on a machine with docker and the two packages.
#
# The images are the oldest systems the packages claim to support: Ubuntu 26.04
# (glibc 2.42, GCC 16) and Fedora 44.

set -euo pipefail

dist=$(cd "${1:?usage: check_linux_packages.sh <directory> [tag]}" && pwd)
tag=${2:-}
# What docker is told to mount, when it is not the path this shell sees (a Windows docker beside a POSIX shell).
mount=${VSS_DOCKER_MOUNT:-$dist}
ubuntu_image=${VSS_UBUNTU_IMAGE:-ubuntu:26.04}
fedora_image=${VSS_FEDORA_IMAGE:-fedora:44}

deb=$(ls "$dist"/*.deb)
rpm=$(ls "$dist"/*.rpm)
echo "deb: $deb"
echo "rpm: $rpm"

# ---------------------------------------------------------------------------
# What the packages say about themselves
# ---------------------------------------------------------------------------

if command -v dpkg-deb >/dev/null 2>&1; then dpkg-deb --info "$deb"; dpkg-deb --contents "$deb"; fi
if command -v rpm >/dev/null 2>&1; then
  rpm -qpi "$rpm"
  rpm -qpR "$rpm"
  rpm -qpl "$rpm"
fi

if [ -n "$tag" ]; then
  version=${tag#v}
  base=${version%%-*}
  if [ "$base" = "$version" ]; then
    want_deb="$version-1"
    want_rpm_release=1
  else
    pre=${version#*-}
    want_deb="$base~$pre-1"
    want_rpm_release="0.$pre"
  fi
  got_deb=$(dpkg-deb --field "$deb" Version)
  if [ "$got_deb" != "$want_deb" ]; then
    echo "The deb is version $got_deb, but the tag $tag needs $want_deb" >&2
    exit 1
  fi
  if command -v rpm >/dev/null 2>&1; then
    got_rpm=$(rpm -qp --qf '%{VERSION}-%{RELEASE}' "$rpm")
    if [ "$got_rpm" != "$base-$want_rpm_release" ]; then
      echo "The rpm is version $got_rpm, but the tag $tag needs $base-$want_rpm_release" >&2
      exit 1
    fi
  fi
fi

# ---------------------------------------------------------------------------
# What a clean system does with them. The script runs inside the container.
# ---------------------------------------------------------------------------

inside=$(cat <<'EOF'
set -euxo pipefail
root=/opt/virtual-serial-studio

# Both programs are on PATH, are the files in /opt, and the GUI finds its libraries.
test "$(readlink -f "$(command -v vss_cli)")" = "$root/bin/vss_cli"
test "$(readlink -f "$(command -v vss_gui)")" = "$root/bin/vss_gui"
vss_cli selftest

# No library that the programs, the Qt libraries they load and the plug-ins Studio uses (the xcb
# platform with its GL integrations, the image formats and the SVG icon engine) need is missing
# from the system, which is what a dependency list that lacks a package shows. ldd only reads
# the files. The other plug-ins (the GTK theme, Wayland, EGLFS and KMS) are optional integrations
# that Qt skips when their libraries are absent, so they are not checked.
missing=$(
  cd "$root"
  ldd bin/vss_gui bin/vss_cli lib/libQt6Core.so.6 lib/libQt6DBus.so.6 lib/libQt6Gui.so.6 \
    lib/libQt6Network.so.6 lib/libQt6OpenGL.so.6 lib/libQt6Svg.so.6 lib/libQt6Widgets.so.6 \
    lib/libQt6XcbQpa.so.6 plugins/platforms/*.so plugins/xcbglintegrations/*.so \
    plugins/imageformats/*.so plugins/iconengines/*.so 2>&1 | grep 'not found' || true
)
if [ -n "$missing" ]; then
  echo "The package depends on libraries the system does not have:" >&2
  echo "$missing" >&2
  exit 1
fi

# The launcher entry and icon are valid and the licence texts are where they should be.
desktop-file-validate /usr/share/applications/virtual-serial-studio.desktop
test -f /usr/share/icons/hicolor/scalable/apps/vss_gui.svg
for f in copyright LICENSE THIRD_PARTY_NOTICES.md LICENSES/GPL-3.0-or-later.txt LICENSES/BSL-1.0.txt LICENSES/IBMPlexMono-OFL.txt LICENSES/NotoSansArabic-OFL.txt LICENSES/NotoSansHebrew-OFL.txt; do
  test -s "/usr/share/doc/virtual-serial-studio/$f"
done

# The GUI starts: loading the xcb platform plug-in is what needs the xcb libraries.
xvfb-run -a vss_gui --version
EOF
)

echo "== Debian: $ubuntu_image"
docker run --rm -v "$mount:/dist:ro" "$ubuntu_image" bash -euc "
  export DEBIAN_FRONTEND=noninteractive
  rm -f /etc/dpkg/dpkg.cfg.d/excludes   # the image leaves out /usr/share/doc, where the licence texts are
  apt-get update -qq
  apt-get install -y --no-install-recommends desktop-file-utils xauth xvfb
  apt-get install -y --no-install-recommends /dist/*.deb
  $inside
  apt-get remove -y virtual-serial-studio
  test ! -e /opt/virtual-serial-studio/bin/vss_gui
  test ! -e /usr/bin/vss_gui
"

echo "== Red Hat: $fedora_image"
docker run --rm -v "$mount:/dist:ro" "$fedora_image" bash -euc "
  dnf install -y --setopt=install_weak_deps=False desktop-file-utils xorg-x11-server-Xvfb xauth
  dnf install -y --setopt=install_weak_deps=False --setopt=tsflags= /dist/*.rpm
  $inside
  dnf remove -y virtual-serial-studio
  test ! -e /opt/virtual-serial-studio/bin/vss_gui
  test ! -e /usr/bin/vss_gui
"

echo "The Linux packages install, run and remove cleanly."
