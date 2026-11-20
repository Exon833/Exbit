#!/bin/sh
# Builds build/appimage/Tiny-DAW-<version>-x86_64.AppImage, and with --publish makes it a GitHub release
# (CLAUDE.md, rule 15: one is made and published before every push). The version is the build's time, UTC:
# YEAR.MONTH.DAY.HOUR.MINUTE.SECOND (VERSION=... gives another); the release's tag is the same, on HEAD.
#
#   tools/appimage.sh             build it (make appimage)
#   tools/appimage.sh --publish   build it, push its tag and publish the release with it (gh, logged in)
#
# Needs appimagetool installed (it packs the folder; it is AppImage's own tool, not part of Tiny DAW), and
# podman or docker (the program is built on an older system: see below).
# Inside: the program, linked with libstdc++ and libgcc built in, so it needs only the system's C library;
# X11, ALSA, PulseAudio, JACK, Wine and curl stay as ever: used when installed. The data (skins, icons,
# templates) is in usr/share/tinydaw, where the program looks (plat::dataDir). The Wine host is put beside
# the program when it was built (make, with winegcc installed); without it Windows plugins say they need it.
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
app=$out/TinyDAW.AppDir
image=$out/Tiny-DAW-$version-x86_64.AppImage
echo "== Tiny DAW $version ($(git rev-parse --short HEAD))"

# the program: built in Ubuntu 22.04 (tools/appimage.Containerfile, with podman or docker), so it runs on
# glibc 2.35 and newer, not only on this machine's; its objects in build-appimage/, the C++ runtime inside.
# NATIVE=1 builds it here instead (then it needs this machine's glibc or newer).
mkdir -p "$out"
build="BUILD=build-appimage TARGET=build-appimage/tinydaw LDOPT=-static-libstdc++\ -static-libgcc"
if [ "${NATIVE:-0}" = 1 ]; then
    make -j"$(nproc)" BUILD=build-appimage TARGET=build-appimage/tinydaw LDOPT="-static-libstdc++ -static-libgcc" \
        build-appimage/tinydaw
else
    if command -v podman >/dev/null 2>&1; then engine=podman; user="--userns=keep-id"
    elif command -v docker >/dev/null 2>&1; then engine=docker; user="--user $(id -u):$(id -g)"
    else echo "podman or docker is needed to build on Ubuntu 22.04 (NATIVE=1 builds here)" >&2; exit 1; fi
    $engine build -q -t tinydaw-appimage-build -f tools/appimage.Containerfile tools >/dev/null
    # (git: the repository is someone else's inside, as far as git can tell)
    # shellcheck disable=SC2086
    $engine run --rm $user -v "$PWD:/src" -w /src -e HOME=/tmp \
        -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0=/src \
        tinydaw-appimage-build sh -c "make -j$(nproc) CXX=g++-12 $build build-appimage/tinydaw"
fi
cp build-appimage/tinydaw "$out/tinydaw"
glibc=$(objdump -T "$out/tinydaw" | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)
echo "== the program needs $glibc or newer"

# the folder
rm -rf -- "$app"
mkdir -p "$app/usr/bin" "$app/usr/share/tinydaw" "$app/usr/share/applications" "$app/usr/share/icons/hicolor/scalable/apps"
cp "$out/tinydaw" "$app/usr/bin/tinydaw"
strip --strip-debug "$app/usr/bin/tinydaw"
for host in tinydaw-wine-host.exe tinydaw-wine-host-32.exe; do
    if [ -f "$host" ] && [ -f "$host.so" ]; then cp "$host" "$host.so" "$app/usr/bin/"; fi
done
cp -r data/icons data/skins data/templates "$app/usr/share/tinydaw/"
cp LICENSE LICENSE.md "$app/usr/share/tinydaw/"
cp data/icons/tinydaw.svg "$app/tinydaw.svg"
cp data/icons/tinydaw.svg "$app/usr/share/icons/hicolor/scalable/apps/tinydaw.svg"
cat > "$app/tinydaw.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Tiny DAW
Comment=A digital audio workstation
Exec=tinydaw %F
Icon=tinydaw
Categories=AudioVideo;Audio;
Terminal=false
X-AppImage-Version=$version
EOF
cp "$app/tinydaw.desktop" "$app/usr/share/applications/tinydaw.desktop"
cat > "$app/AppRun" <<'EOF'
#!/bin/sh
# (tinydaw --tui: the terminal UI, from a terminal)
here=$(dirname "$(readlink -f "$0")")
exec "$here/usr/bin/tinydaw" "$@"
EOF
chmod +x "$app/AppRun"

# packed (appimagetool runs from its own AppImage without FUSE too)
rm -f -- "$image"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 appimagetool --no-appstream "$app" "$image"
chmod +x "$image"
echo "== $image ($(du -h "$image" | cut -f1))"

if [ "$publish" = 1 ]; then
    git tag -a "$version" -m "Tiny DAW $version" HEAD
    git push origin "refs/tags/$version"
    gh release create "$version" "$image" --verify-tag --title "Tiny DAW $version" --notes \
"Tiny DAW built from $(git rev-parse --short HEAD), as one file: download it, make it executable (chmod +x), run it.
\`./Tiny-DAW-$version-x86_64.AppImage --tui\` runs it in the terminal. Needs only the system's C library
($glibc or newer: most distributions from 2022 on); X11, ALSA, PulseAudio, JACK, Wine and curl are used when
installed."
fi
