# SPDX-License-Identifier: GPL-3.0-or-later
# Needs only Apple's Command Line Tools.
#   make        builds dist/MyTether.app
#   make test   runs the tests (no phone or password needed)
#   make icon   redraws the app icon after a shape change
#   make clean

ARCH   := $(shell uname -m)
TARGET := $(ARCH)-apple-macos14.0
CFLAGS := -target $(TARGET) -std=gnu17 -Os -Wall -Wextra -Wconversion -Wno-sign-conversion -fblocks -Icore -Ihelper
APP    := dist/MyTether.app

CORE   := core/rndis.c core/phone.c
HELPER := helper/usb.c helper/net.c helper/tether.c helper/main.c
SWIFT  := $(wildcard app/*.swift)

.PHONY: all test clean icon
all: $(APP)

build/mytether-helper: $(CORE) $(HELPER) $(wildcard core/*.h helper/*.h)
	@mkdir -p build
	clang $(CFLAGS) $(CORE) $(HELPER) -framework IOKit -framework CoreFoundation -framework SystemConfiguration -o $@
	strip -x $@

build/phone.o: core/phone.c core/phone.h
	@mkdir -p build
	clang $(CFLAGS) -c core/phone.c -o $@

build/MyTether: $(SWIFT) build/phone.o app/bridge.h
	swiftc -target $(TARGET) -swift-version 5 -Osize -wmo -Icore -import-objc-header app/bridge.h $(SWIFT) build/phone.o -framework IOKit -o $@
	strip -x $@

$(APP): build/MyTether build/mytether-helper app/Info.plist app/AppIcon.icns app/install-helper.sh app/remove-helper.sh LICENSE
	@rm -rf $@
	@mkdir -p $@/Contents/MacOS $@/Contents/Resources
	cp build/MyTether build/mytether-helper $@/Contents/MacOS/
	cp app/Info.plist $@/Contents/Info.plist
	cp app/AppIcon.icns app/install-helper.sh app/remove-helper.sh LICENSE $@/Contents/Resources/
	codesign --force --options runtime --timestamp=none --sign - --identifier local.mytether.helperd $@/Contents/MacOS/mytether-helper
	codesign --force --options runtime --timestamp=none --sign - --identifier local.mytether.app $@
	codesign --verify --strict $@
	@du -sh $@

test:
	@mkdir -p build
	clang $(CFLAGS) -g -fsanitize=address,undefined core/rndis.c tests/test_core.c -framework IOKit -framework CoreFoundation -o build/test_core
	build/test_core
	clang $(CFLAGS) tests/test_pin.c -framework Security -framework CoreFoundation -o build/test_pin
	build/test_pin
	build/test_pin | grep -q "fingerprint: $$(codesign -dv --verbose=4 build/test_pin 2>&1 | sed -n 's/^CDHash=//p')$$"
	swiftc -target $(TARGET) -swift-version 5 app/Phase.swift tests/test_phase/main.swift -o build/test_phase
	build/test_phase

# Redraws app/AppIcon.icns from app/Glyph.swift.
icon:
	@mkdir -p build
	swiftc -target $(TARGET) -swift-version 5 app/Glyph.swift tools/make-icon/main.swift -o build/make-icon
	build/make-icon app/AppIcon.icns

clean:
	rm -rf build dist
