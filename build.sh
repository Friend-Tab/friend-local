#!/bin/sh
# build.sh — each of our programs one translation unit, built by one
# compiler invocation; vendored code is compiled per file and cached.
#
#   ./build.sh                 build the library
#   ./build.sh release         ...with optimizations
#   ./build.sh test            build and run the library's tests
#   ./build.sh platform        build and run platform/'s tests
#   ./build.sh ui [release]    the window
#   ./build.sh chat [release]  friendchat
#   ./build.sh talk [release]  friendtalk
#   ./build.sh peek [release]  friendpeek
#   ./build.sh web [version]   friend-local.html: the page, with the
#                              library inside it as WebAssembly
#   ./build.sh site [version]  the same page as a site for GitHub Pages
#   ./build.sh web-test        the library's tests, in WebAssembly (node)
#   ./build.sh dist <version>  a release for this machine's platform,
#                              in dist/ (see below)
#
# No build system. A frontend is one more invocation here, not a
# CMakeLists.
#
# OUT says where a build goes (build/ unless told), so that two builds for
# two architectures can sit side by side. SDL_CFLAGS and SDL_LIBS, if set,
# say how to compile and link SDL3 instead of asking pkg-config: a release
# links a static SDL3 built from source.

set -e

CC=${CC:-cc}
OUT=${OUT:-build}
export OUT
# -Wno-overlength-strings: the system prompt is one 7 KB string literal
# and C99 only promises 4095. Every compiler in use handles it; the
# alternative is chopping prose into chunks to satisfy a 1990 limit.
WARN="-Wall -Wextra -pedantic -Wshadow -Wconversion -Wsign-conversion -Wmissing-prototypes -Wstrict-prototypes -Wno-overlength-strings"
STD="-std=c99"
DEBUG="-g -O0 -DFR_DEBUG=1"
RELEASE="-O2 -DNDEBUG"

mkdir -p "$OUT"

case "$1" in
release) MODE=release ;;
*)       MODE=${2:-debug} ;;
esac
case "$MODE" in
release) FLAGS="$RELEASE" ;;
*)       FLAGS="$DEBUG" ;;
esac

# Everything that is not the library: POSIX for clock_gettime, poll and
# the _r time functions, which -std=c99 alone hides.
HOST="$STD -D_POSIX_C_SOURCE=200809L $WARN"
# Apple hides its own extensions (F_FULLFSYNC, SO_NOSIGPIPE) the moment a
# POSIX level is asked for, unless they are asked for too.
if [ "$(uname -s)" = Darwin ]; then HOST="$HOST -D_DARWIN_C_SOURCE"; fi
HOST_INCLUDE="-Ilibfriend/include -Iplatform -Ivendor/mbedtls/include"

# What every frontend links: the library and platform/ built from today's
# sources, always — linking yesterday's libfriend.a against today's
# friend.h is a way to spend an hour on a bug that is not there — and
# mbedTLS, which is cached because it is not ours and does not change.
needs_platform() {
    "$0" mbedtls
    "$0" "$MODE" >/dev/null
    $CC $HOST $FLAGS $HOST_INCLUDE -c platform/platform_unity.c \
        -o "$OUT/platform.o"
    ar rcs "$OUT/libplatform.a" "$OUT/platform.o"
}
LINK="$OUT/libplatform.a $OUT/libfriend.a $OUT/libmbedtls.a -lm"

case "${1:-debug}" in
test)
    $CC $STD $WARN $DEBUG -o "$OUT/fr_tests" libfriend/tests/fr_tests.c -lm
    exec "$OUT/fr_tests"
    ;;
platform)
    needs_platform
    $CC $HOST -D_DEFAULT_SOURCE $FLAGS $HOST_INCLUDE \
        -o "$OUT/fr_platform_tests" platform/tests/fr_platform_tests.c $LINK
    exec "$OUT/fr_platform_tests"
    ;;
ui)
    # The supplied frontend. SDL3 comes from the system while the rest of
    # it is vendored; building SDL from vendored source is a release-time
    # step and not something a first build should wait for.
    needs_platform
    SDL_CFLAGS=${SDL_CFLAGS-$(pkg-config --cflags sdl3 2>/dev/null || echo "")}
    SDL_LIBS=${SDL_LIBS-$(pkg-config --libs sdl3 2>/dev/null || echo "-lSDL3")}
    # kb_text_shape and stb_truetype are other people's code; they are
    # compiled with our warnings off and ours with them on.
    $CC $STD -w $FLAGS -c frontend/sdl/ui_vendor.c -o "$OUT/ui_vendor.o"
    $CC $HOST $FLAGS $SDL_CFLAGS $HOST_INCLUDE \
        -o "$OUT/friend_sdl" frontend/sdl/friend_sdl.c "$OUT/ui_vendor.o" \
        $LINK $SDL_LIBS
    echo "built $OUT/friend_sdl"
    ;;
mbedtls)
    # Vendored TLS, built once and cached. mbedTLS is not written for a
    # unity build — same-named statics in different files — so it gets
    # the one thing in this repo that compiles per file. Its own
    # warnings are not ours to fix, hence -w.
    if [ ! -f "$OUT/libmbedtls.a" ]; then
        mkdir -p "$OUT/mbedtls"
        MBED_CC="$CC"; MBED_OUT="$OUT/mbedtls"
        export MBED_CC MBED_OUT
        # One file per sh, passed as its argument: BSD xargs will not
        # substitute -I into a command longer than 255 bytes.
        ls vendor/mbedtls/library/*.c |
            xargs -P 8 -n 1 sh -c \
                'exec $MBED_CC -std=c99 -w -O2 -DNDEBUG \
                     -Ivendor/mbedtls/include -Ivendor/mbedtls/library \
                     -c "$1" -o "$MBED_OUT/$(basename "$1" .c).o"' sh
        ar rcs "$OUT/libmbedtls.a" "$OUT"/mbedtls/*.o
        echo "built $OUT/libmbedtls.a"
    fi
    ;;
chat)
    # friendchat: the terminal frontend, and the first thing that talks
    # to a model.
    needs_platform
    $CC $HOST $FLAGS $HOST_INCLUDE -o "$OUT/friendchat" frontend/cli/friendchat.c \
        $LINK
    echo "built $OUT/friendchat"
    ;;
talk)
    # friendtalk: a WAV in, a WAV out, and the voice turn in between.
    needs_platform
    $CC $HOST $FLAGS $HOST_INCLUDE -o "$OUT/friendtalk" frontend/cli/friendtalk.c \
        $LINK
    echo "built $OUT/friendtalk"
    ;;
peek)
    # friendpeek: the smallest frontend, for pointing the library at a
    # real export and seeing what came out.
    needs_platform
    $CC $HOST $FLAGS $HOST_INCLUDE -o "$OUT/friendpeek" frontend/cli/friendpeek.c \
        $LINK
    echo "built $OUT/friendpeek"
    ;;
wasm)
    # The library as WebAssembly, for the page. No C library: libc/ is
    # what of one it uses, and the page supplies the rest (frontend/web/
    # friend_web.c says what crosses). Needs a clang that targets wasm32
    # and wasm-ld (lld); Apple's clang does not, Homebrew's llvm does.
    WASM_CC=${WASM_CC:-clang}
    $WASM_CC --target=wasm32 $STD $WARN -Wno-missing-prototypes $RELEASE \
        -nostdlib -ffreestanding -mbulk-memory \
        -isystem frontend/web/libc -Ilibfriend/include \
        -Wl,--no-entry -Wl,--initial-memory=33554432 \
        -Wl,--max-memory=4294967296 -Wl,-z,stack-size=1048576 \
        -o "$OUT/friend.wasm" frontend/web/friend_web.c
    echo "built $OUT/friend.wasm"
    ;;
web-test)
    # The library's own tests, built for WebAssembly against the same
    # libc/ and run in node, so the page runs what was tested; then the
    # page's .friendlocal against the desktop's, byte for byte.
    WASM_CC=${WASM_CC:-clang}
    $WASM_CC --target=wasm32 $STD $WARN -Wno-missing-prototypes $DEBUG -O1 \
        -nostdlib -ffreestanding -mbulk-memory -isystem frontend/web/libc \
        -Wl,--no-entry -Wl,--export=main -Wl,--initial-memory=67108864 \
        -Wl,--max-memory=4294967296 -Wl,-z,stack-size=8388608 \
        -o "$OUT/web_tests.wasm" frontend/web/tests/web_tests.c
    node frontend/web/tests/run.mjs "$OUT/web_tests.wasm"
    "$0" wasm
    needs_platform
    $CC $HOST $FLAGS $HOST_INCLUDE -o "$OUT/fl_check" \
        frontend/web/tests/fl_check.c $LINK
    exec node frontend/web/tests/web_export.mjs "$OUT/friend.wasm" \
        "$OUT/fl_check" libfriend/tests/export.json
    ;;
web)
    # friend-local.html: the page, its script and style, the library as
    # WebAssembly and the wordmark, all in one file that opens from
    # anywhere. ./build.sh web [version]
    "$0" wasm
    VERSION=${2:-dev}
    VERSION=${VERSION#v}
    W="frontend/web"
    base64 < "$OUT/friend.wasm" | tr -d '\n' > "$OUT/friend.wasm.b64"
    base64 < assets/friend-wordmark.png | tr -d '\n' > "$OUT/wordmark.b64"
    cat "$W/host.js" "$W/friendlocal.js" "$W/icons.js" "$W/app.js" \
        > "$OUT/page.js"
    if grep -qi '</script' "$OUT/page.js"; then
        echo "the page's script would end its own <script> tag" >&2
        exit 1
    fi
    awk -v css="$W/app.css" -v js="$OUT/page.js" \
        -v wasm="$OUT/friend.wasm.b64" -v mark="$OUT/wordmark.b64" '
        function put(path,   line) {
            while ((getline line < path) > 0) print line
            close(path)
        }
        function inline(path,   line, all) {
            all = ""
            while ((getline line < path) > 0) all = all line
            close(path)
            return all
        }
        $0 == "@@CSS@@" { put(css); next }
        $0 == "@@JS@@"  { put(js); next }
        index($0, "@@WASM@@") {
            sub(/@@WASM@@/, inline(wasm)); print; next
        }
        index($0, "@@WORDMARK@@") {
            sub(/@@WORDMARK@@/, inline(mark)); print; next
        }
        { print }
    ' "$W/page.html" | sed "s/@@VERSION@@/$VERSION/" > "$OUT/friend-local.html"
    echo "built $OUT/friend-local.html"
    ;;
site)
    # The web version as a site, for GitHub Pages: the page as index.html,
    # and beside it what lets a phone put it on its home screen and open
    # it offline. ./build.sh site [version]
    "$0" web "${2:-dev}"
    VERSION=${2:-dev}
    VERSION=${VERSION#v}
    SITE="$OUT/site"
    rm -rf "$SITE"
    mkdir -p "$SITE"
    cp "$OUT/friend-local.html" "$SITE/index.html"
    cp frontend/web/site/manifest.webmanifest frontend/web/site/*.png "$SITE/"
    sed "s/@@VERSION@@/$VERSION/" frontend/web/site/sw.js > "$SITE/sw.js"
    touch "$SITE/.nojekyll"
    echo "built $SITE"
    ;;
dist)
    # A release for the platform this runs on: every program built for
    # release, with what the window needs beside it and every licence it
    # owes, in one archive in dist/.
    #
    #   Linux  friend-local-<v>-linux-<arch>.tar.gz: a folder to unpack
    #          anywhere and run
    #   macOS  friend-local-<v>-macos.zip: friend-local.app and the tools,
    #          each one binary for Apple Silicon and Intel
    #
    # SDL3 goes in however SDL_LIBS says; the release workflow builds it
    # from source and links it statically, so nothing has to be installed
    # to run what comes out. SDL_LICENSE names its licence file.
    VERSION=${2:?usage: ./build.sh dist <version>}
    case "$VERSION" in v*) VERSION=${VERSION#v} ;; esac
    TOOLS="friendchat friendtalk friendpeek"
    rm -rf dist
    mkdir -p dist

    notices() {
        {
            echo "friend-local $VERSION is MIT licensed (LICENSE). It is built"
            echo "with the following, under their own licences."
            echo
            echo "== SDL3 (zlib) =="
            if [ -n "$SDL_LICENSE" ]; then cat "$SDL_LICENSE"
            else echo "linked from the system that built this"; fi
            echo
            echo "== Mbed TLS (Apache-2.0 OR GPL-2.0-or-later) =="
            cat vendor/mbedtls/LICENSE
            echo
            echo "== kb_text_shape (zlib) =="
            awk '/^   LICENSE$/ {on = 1} on {print} on && /^\*\// {exit}' \
                vendor/kb_text_shape.h
            for f in stb_truetype stb_image stb_image_write; do
                echo
                echo "== $f (MIT OR public domain) =="
                awk '/This software is available under 2 licenses/ {on = 1}
                     on {print} on && /^\*\// {exit}' "vendor/$f.h"
            done
            echo
            echo "== Phosphor icons (MIT) =="
            cat vendor/Phosphor-LICENSE.txt
        } > "$1"
    }

    # Ours, beside the window: it looks in the folder it is in.
    assets() {
        mkdir -p "$1/vendor" "$1/assets"
        cp vendor/Phosphor.ttf "$1/vendor/"
        cp assets/friend-wordmark.png "$1/assets/"
    }

    if [ "$(uname -s)" = Darwin ]; then
        NAME="friend-local-$VERSION-macos"
        STAGE="dist/$NAME"
        APP="$STAGE/friend-local.app"
        for ARCH in arm64 x86_64; do
            for T in ui chat talk peek; do
                OUT="build/$ARCH" \
                CC="${CC:-cc} -arch $ARCH -mmacosx-version-min=11.0" \
                    "$0" "$T" release
            done
        done
        mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
        lipo -create build/arm64/friend_sdl build/x86_64/friend_sdl \
            -output "$APP/Contents/MacOS/friend-local"
        for T in $TOOLS; do
            lipo -create "build/arm64/$T" "build/x86_64/$T" -output "$STAGE/$T"
        done
        assets "$APP/Contents/Resources"
        cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>          <string>friend-local</string>
    <key>CFBundleIdentifier</key>          <string>org.friend-local.app</string>
    <key>CFBundleName</key>                <string>friend-local</string>
    <key>CFBundleDisplayName</key>         <string>friend-local</string>
    <key>CFBundlePackageType</key>         <string>APPL</string>
    <key>CFBundleShortVersionString</key>  <string>$VERSION</string>
    <key>CFBundleVersion</key>             <string>$VERSION</string>
    <key>LSMinimumSystemVersion</key>      <string>11.0</string>
    <key>NSHighResolutionCapable</key>     <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>So you can talk to your friend out loud.</string>
</dict>
</plist>
PLIST
        # Ad hoc: Apple Silicon will not run a binary with no signature
        # at all. It is not a developer signature, so the first launch is
        # right-click, Open (see the release notes).
        for T in $TOOLS; do codesign --force -s - "$STAGE/$T"; done
        codesign --force --deep -s - "$APP"
        cp LICENSE "$STAGE/"
        notices "$STAGE/THIRD-PARTY-NOTICES.txt"
        (cd dist && ditto -c -k --keepParent "$NAME" "$NAME.zip")
    else
        ARCH=$(uname -m)
        NAME="friend-local-$VERSION-linux-$ARCH"
        STAGE="dist/$NAME"
        for T in ui chat talk peek; do "$0" "$T" release; done
        mkdir -p "$STAGE"
        cp "$OUT/friend_sdl" "$STAGE/friend-local"
        for T in $TOOLS; do cp "$OUT/$T" "$STAGE/"; done
        strip "$STAGE/friend-local" $(for T in $TOOLS; do echo "$STAGE/$T"; done)
        assets "$STAGE"
        cp LICENSE "$STAGE/"
        notices "$STAGE/THIRD-PARTY-NOTICES.txt"
        tar -C dist -czf "dist/$NAME.tar.gz" "$NAME"
    fi
    echo "built dist/$NAME"
    ;;
*)
    $CC $STD $WARN $FLAGS -c libfriend/src/friend_unity.c -o "$OUT/friend.o"
    # Nothing outside friend.h is reachable from outside the library.
    # Anything else exported is an internal a frontend could come to
    # depend on without anyone deciding it should.
    LEAKS=$(nm -g "$OUT/friend.o" | awk '$2 ~ /^[TDBRC]$/ && $3 !~ /^_?friend_/ {print $3}')
    if [ -n "$LEAKS" ]; then
        echo "libfriend exports more than friend.h:" $LEAKS >&2
        exit 1
    fi
    ar rcs "$OUT/libfriend.a" "$OUT/friend.o"
    echo "built $OUT/libfriend.a"
    ;;
esac
