#!/bin/sh
# The test programs a change reaches, one per line (for `make test-affected`, which the pre-push hook runs).
#   tools/affected_tests.sh BASE        the files changed from commit BASE to HEAD
#   tools/affected_tests.sh -- FILE...  these files
# A test is reached when its .d (the compiler's list of what it includes) names a changed file, or a header whose
# .cpp includes one, repeated until nothing new is found, because that .cpp's code is linked into the test.
# Files that aren't C++ (the Makefile, tools, shipped data) reach every test, to be safe. Text-only files reach none.
# Failsafe: with no .d files (nothing built yet) it says so and exits 1, and the caller runs every test.
ROOT="${AFFECTED_ROOT:-$(git rev-parse --show-toplevel)}"
BUILD="${BUILD:-build}"
cd "$ROOT" || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
if [ "$1" = -- ]; then
    shift
    printf '%s\n' "$@" >"$tmp/changed"
else
    git diff --name-only "$1" HEAD >"$tmp/changed" || exit 1
fi
tests=$(ls "$BUILD"/tests/*.d 2>/dev/null)
[ -n "$tests" ] || { echo "affected_tests: no .d files in $BUILD/tests: build the tests first" >&2; exit 1; }
all=0
while read -r f; do
    case "$f" in
    "" | docs/* | *.md | *.txt | LICENSE* | .gitignore) ;;
    # shortcut: the Wine host's code reaches the tests through its client side only; name the tests if that grows
    src/winehost/*) printf '%s\n' src/plugins/bridged.h src/plugins/wine.h ;;
    # (Exbit's Wine fixes, Windows code built with MinGW: their test runs them in a prefix)
    src/winefix/* | tests/winefix/*) printf '%s\n' tests/test_wine_fixes.cpp ;;
    src/*.cpp | src/*.h | tests/*.cpp | tests/*.h) printf '%s\n' "$f" "${f%.*}.h" "${f%.*}.cpp" ;;
    *) all=1 ;;
    esac
done <"$tmp/changed" >"$tmp/names"
if [ "$all" = 1 ]; then
    printf '%s\n' $tests | sed 's/\.d$//'
    exit 0
fi
[ -s "$tmp/names" ] || exit 0
objs=$(find "$BUILD/src" -name '*.d' 2>/dev/null)
n=0
sort -u "$tmp/names" -o "$tmp/names"
while [ "$(wc -l <"$tmp/names")" -ne "$n" ]; do
    n=$(wc -l <"$tmp/names")
    for d in $(grep -lFf "$tmp/names" $objs /dev/null); do
        s=${d#"$BUILD"/}
        s=${s%.d}
        printf '%s\n' "$s.h" "$s.cpp"
    done >>"$tmp/names"
    sort -u "$tmp/names" -o "$tmp/names"
done
grep -lFf "$tmp/names" $tests /dev/null | sed 's/\.d$//'
exit 0
