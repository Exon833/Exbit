#!/bin/sh
# Build and run the tests that run on Windows (the Makefile's WIN_TESTS: `make tests-win`) on a Windows machine
# reached by ssh, with MSYS2's UCRT64 in C:\msys64 there. The source tree is copied over first (not build/ or .git).
#   tools/wintests.sh [host] [folder]     host: an ssh host (default winvm); folder: where the copy goes, as MSYS2
#                                         names it (default /c/Users/$USER-of-the-host/tinydaw-tests)
#   WHAT=tinydaw.exe tools/wintests.sh    builds that target instead (the program)
cd "$(dirname "$0")/.." || exit 1
host=${1:-winvm}
dir=${2:-'~/tinydaw-tests'}
what=${WHAT:-tests-win}
quiet() { grep -v -e 'post-quantum' -e 'store now, decrypt later' -e 'may need to be upgraded' -e 'openssh.com/pq'; }
msys='C:\msys64\usr\bin\env.exe MSYSTEM=UCRT64 CHERE_INVOKING=1 C:\msys64\usr\bin\bash.exe -l'
tar cf - --exclude=./build --exclude=./build-debug --exclude=./.git --exclude=./.claude --exclude=./tinydaw \
    --exclude='./*.exe' . | ssh -o BatchMode=yes "$host" "$msys -c \"mkdir -p $dir && tar xf - -C $dir\"" 2>&1 | quiet
printf 'cd %s && make -j"$(nproc)" %s\n' "$dir" "$what" | ssh -o BatchMode=yes "$host" "$msys -s" 2>&1 | quiet
