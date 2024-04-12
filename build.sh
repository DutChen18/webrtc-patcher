#!/bin/sh

DIR=$(dirname "$(readlink -f "$0")")
DEPOT_TOOLS=$DIR/depot_tools
WEBRTC=$DIR/webrtc
WEBRTC_SRC=$WEBRTC/src
FUNCHOOK=$DIR/funchook

LIB_FAKE=$(readlink -f "$1")
LIB=$(dirname "$LIB_FAKE")
LIB_REAL=$LIB/real_$(basename "$LIB_FAKE")
LIB_FUNCHOOK=$LIB/libfunchook.so.2

if [ ! -f "$LIB_REAL" ]; then
	if [ ! -f "$LIB_FAKE" ]; then
		echo "error: real lib is missing, and original file is missing"
		exit 1
	elif grep -q "webrtc_patcher_cookie_66706761" "$LIB_FAKE"; then
		echo "error: real lib is missing, and original file is already patched"
		exit 1
	fi

	echo "info: original file is unmodified, moving file and installing"
	mv "$LIB_FAKE" "$LIB_REAL"
elif [ ! -f "$LIB_FAKE" ]; then
	echo "info: real lib found, and loader is missing, installing"
elif grep -q "webrtc_patcher_cookie_66706761" "$LIB_FAKE"; then
	echo "info: real lib found, and loader found, updating"
else
	echo "info: real lib found, and loader was overwritten, moving file and reinstalling"
	mv "$LIB_FAKE" "$LIB_REAL"
fi

export PATH="$DEPOT_TOOLS:$PATH"

if [ ! -d "$DEPOT_TOOLS" ]; then
	echo "info: downloading depot_tools"
	git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "$DEPOT_TOOLS"
fi

if [ ! -d "$WEBRTC" ]; then
	echo "info: downloading webrtc"
	mkdir -p "$WEBRTC"
	(cd "$WEBRTC"; fetch --nohooks webrtc)
fi

WEBRTC_DATE=$(strings "$LIB_REAL" | grep "WebRTC source stamp" | awk '{ print substr($4, 0, 10) }')
WEBRTC_HASH=$(git -C "$WEBRTC_SRC" log main --before="$WEBRTC_DATE" -1 --pretty=format:%H)

echo "info: syncing webrtc to $WEBRTC_HASH ($WEBRTC_DATE)"
(cd "$WEBRTC"; gclient sync -Dr "$WEBRTC_HASH")
echo "info: generating webrtc project files"
(cd "$WEBRTC_SRC"; gn gen out/Default)
echo "info: building webrtc"
ninja -C "$WEBRTC_SRC/out/Default"

if [ ! -d "$FUNCHOOK" ]; then
	echo "info: downloading funchook"
	git clone https://github.com/kubo/funchook.git "$FUNCHOOK"
	echo "info: configuring funchook"
	cmake -B "$FUNCHOOK/build" "$FUNCHOOK"
fi

echo "info: building funchook"
cmake --build "$FUNCHOOK/build" -t funchook-shared
echo "info: copying funchook"
cp "$FUNCHOOK/build/libfunchook.so" "$LIB_FUNCHOOK"

echo "info: building loader"
clang++ \
	-std=c++14 \
	-Wall -Wextra -pedantic \
	-nostdinc++ -fno-rtti \
	"$DIR/loader.cc" \
	"$WEBRTC_SRC/out/Default/obj/libwebrtc.a" \
	"$WEBRTC_SRC/out/Default/obj/buildtools/third_party/libc++/libc++/string.o" \
	"$FUNCHOOK/build/libfunchook.so" \
	-isystem"$WEBRTC_SRC/buildtools/third_party/libc++" \
	-isystem"$WEBRTC_SRC/buildtools/third_party/libc++/trunk/include" \
	-isystem"$WEBRTC_SRC/third_party/perfetto/buildtools/libcxx_config" \
	-isystem"$WEBRTC_SRC/third_party/abseil-cpp" \
	-isystem"$WEBRTC_SRC/third_party/jsoncpp/source/include" \
	-isystem"$WEBRTC_SRC" \
	-isystem"$FUNCHOOK/include" \
	-Wl,-rpath="\$ORIGIN" \
	-lX11 -lXcomposite -lEGL -lGL -ldl -lelf \
	-fPIC -shared -o "$DIR/libloader.so" \
	-g -Og \
	-DWEBRTC_POSIX \
	-DWEBRTC_USE_X11 \
	-DWEBRTC_USE_GIO \
	$(pkg-config --cflags glib-2.0)

echo "info: copying loader"
cp "$DIR/libloader.so" "$LIB_FAKE"
