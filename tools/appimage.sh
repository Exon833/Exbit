#!/bin/sh
# Builds build/appimage/Exbit-<version>-x86_64.AppImage, and with --publish makes it a GitHub release
# (one is made and published before every push). The version is the build's time, UTC:
# YEAR.MONTH.DAY.HOUR.MINUTE.SECOND (VERSION=... gives another); the release's tag is the same, on HEAD.
#
#   tools/appimage.sh             build it (make appimage)
#   tools/appimage.sh --publish   build it, push its tag and publish the release with it (gh, logged in)
#
# Needs appimagetool installed (it packs the folder; it is AppImage's own tool, not part of Exbit), and
# podman or docker (the program is built on an older system: see below).
# Inside: the program, linked with libstdc++ and libgcc built in, so it needs only the system's C library;
# X11, ALSA, PulseAudio, JACK, Wine and curl stay as ever: used when installed. The data (skins, icons,
# templates, presets, the symbol fonts) is in usr/share/exbit, where the program looks (plat::dataDir). The Wine
# host is put beside the program when it was built (make, with winegcc installed); without it Windows plugins say
# they need it.
set -eu
cd "$(git rev-parse --show-toplevel)"

publish=0
[ "${1:-}" = "--publish" ] && publish=1
command -v appimagetool >/dev/null 2>&1 || {
    echo "appimagetool isn't installed: it packs the AppImage (https://github.com/AppImage/appimagetool)" >&2
    exit 1
}
if [ "$publish" = 1 ]; then
    command -v gh >/dev/null 2>&1 || { echo "gh (GitHub's CLI) isn't installed: it publishes the release" >&2; exit 1; }
    [ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "uncommitted changes: commit them first" >&2; exit 1; }
fi

version=${VERSION:-$(date -u +%Y.%m.%d.%H.%M.%S)}
out=build/appimage
app=$out/Exbit.AppDir
image=$out/Exbit-$version-x86_64.AppImage
echo "== Exbit $version ($(git rev-parse --short HEAD))"

# the program: built in Ubuntu 22.04 (tools/appimage.Containerfile, with podman or docker), so it runs on
# glibc 2.35 and newer, not only on this machine's; its objects in build-appimage/, the C++ runtime inside.
# NATIVE=1 builds it here instead (then it needs this machine's glibc or newer).
mkdir -p "$out"
build="BUILD=build-appimage TARGET=build-appimage/exbit LDOPT=-static-libstdc++\ -static-libgcc"
objects=build-appimage  # (the container's objects: never mixed with this machine's, built against another glibc)
if [ "${NATIVE:-0}" = 1 ]; then
    objects=build-appimage-native
    make -j"$(nproc)" BUILD=$objects TARGET=$objects/exbit LDOPT="-static-libstdc++ -static-libgcc" $objects/exbit
else
    if command -v podman >/dev/null 2>&1; then engine=podman; user="--userns=keep-id"
    elif command -v docker >/dev/null 2>&1; then engine=docker; user="--user $(id -u):$(id -g)"
    else echo "podman or docker is needed to build on Ubuntu 22.04 (NATIVE=1 builds here)" >&2; exit 1; fi
    $engine build -q -t exbit-appimage-build -f tools/appimage.Containerfile tools >/dev/null
    # (git: the repository is someone else's inside, as far as git can tell)
    # shellcheck disable=SC2086
    $engine run --rm $user -v "$PWD:/src" -w /src -e HOME=/tmp \
        -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0=/src \
        exbit-appimage-build sh -c "make -j$(nproc) CXX=g++-12 $build build-appimage/exbit"
fi
cp $objects/exbit "$out/exbit"
glibc=$(objdump -T "$out/exbit" | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)
echo "== the program needs $glibc or newer"
if [ "${NATIVE:-0}" != 1 ] && [ "$(printf '%s\n' GLIBC_2.35 "$glibc" | sort -V | tail -1)" != GLIBC_2.35 ]; then
    echo "it should need GLIBC_2.35 at most (built in Ubuntu 22.04): not packed" >&2
    exit 1
fi

# the folder
rm -rf -- "$app"
mkdir -p "$app/usr/bin" "$app/usr/share/exbit" "$app/usr/share/applications" "$app/usr/share/icons/hicolor/scalable/apps"
cp "$out/exbit" "$app/usr/bin/exbit"
strip --strip-debug "$app/usr/bin/exbit"
# the Wine host built (or brought up to date) here, so a release never ships a stale one or none; a release made
# where winegcc is installed refuses to go out without it
if command -v winegcc >/dev/null 2>&1; then
    make -j"$(nproc)" winehost >/dev/null
    if [ "$publish" = 1 ] && [ ! -f exbit-wine-host.exe ]; then
        echo "the Wine host (exbit-wine-host.exe) didn't build: not published (Windows plugins would not load)" >&2
        exit 1
    fi
fi
for host in exbit-wine-host.exe exbit-wine-host-32.exe; do
    if [ -f "$host" ] && [ -f "$host.so" ]; then cp "$host" "$host.so" "$app/usr/bin/"; fi
done
# (the 32-bit host built with MinGW, where Wine is built only for WoW64: its Linux side is in wine32/)
if [ -f exbit-wine-host-32.exe ] && [ -f wine32/i386-windows/exbit-unix32.dll ]; then
    cp exbit-wine-host-32.exe "$app/usr/bin/"
    cp -r wine32 "$app/usr/bin/"
fi
# Exbit's Wine fixes (src/winefix, built with MinGW), put in the prefixes it makes or is asked to fix
# (plugins/wine_prefix_fixes.h); without them prefixes are left as Wine makes them
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    make -j"$(nproc)" winefixes >/dev/null
fi
if [ -f wine-fixes/x86_64-windows/msi.dll ]; then
    cp -r wine-fixes "$app/usr/bin/"
fi
cp -r data/icons data/skins data/templates data/presets data/fonts "$app/usr/share/exbit/"
cp LICENSE LICENSE.md "$app/usr/share/exbit/"
cp data/icons/exbit.svg "$app/exbit.svg"
# (.DirIcon must be a PNG: the logo, data/icons/exbit.svg, drawn at 256 px: remade with rsvg-convert when the SVG changes)
cp tools/appimage-icon.png "$app/.DirIcon"
cp data/icons/exbit.svg "$app/usr/share/icons/hicolor/scalable/apps/exbit.svg"
cat > "$app/exbit.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Exbit
Comment=A digital audio workstation
Exec=exbit %F
Icon=exbit
Categories=AudioVideo;Audio;
Terminal=false
X-AppImage-Version=$version
EOF
cp "$app/exbit.desktop" "$app/usr/share/applications/exbit.desktop"
cat > "$app/AppRun" <<'EOF'
#!/bin/sh
# (exbit --tui: the terminal UI, from a terminal)
here=$(dirname "$(readlink -f "$0")")
exec "$here/usr/bin/exbit" "$@"
EOF
chmod +x "$app/AppRun"

# packed (appimagetool runs from its own AppImage without FUSE too)
rm -f -- "$image"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 appimagetool --no-appstream "$app" "$image"
chmod +x "$image"
echo "== $image ($(du -h "$image" | cut -f1))"

if [ "$publish" = 1 ]; then
    git tag "$version" HEAD  # (a plain tag: no git identity needed)
    git push origin "refs/tags/$version"
    gh release create "$version" "$image" --verify-tag --title "Exbit $version" --notes \
"Exbit built from $(git rev-parse --short HEAD), as one file: download it, make it executable (chmod +x), run it.
\`./Exbit-$version-x86_64.AppImage --tui\` runs it in the terminal. Needs only the system's C library
($glibc or newer: most distributions from 2022 on); X11, ALSA, PulseAudio, JACK, Wine and curl are used when
installed."
fi
