SDK ?= $(firstword $(wildcard sdk/*HCNetSDK*linux64))

hikview: hikview.cpp lib
	g++ -O2 -Wall -o $@ $< -I$(SDK)/incEn -Llib -lhcnetsdk -lX11 \
		-Wl,-rpath,'$$ORIGIN/lib:$$ORIGIN/lib/HCNetSDKCom'

lib:
	@test -n "$(SDK)" || { echo "Hikvision SDK not found - run ./setup-sdk.sh <sdk.zip> (see README)"; exit 1; }
	ln -sfn $(abspath $(SDK))/lib lib

clean:
	rm -f hikview lib

.PHONY: clean
