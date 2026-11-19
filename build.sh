#!/bin/sh
# engine/ (the FM-1 firmware's engine) -> WebAssembly -> one self-contained index.html
set -e
cd "$(dirname "$0")"
mkdir -p build
clang --target=wasm32 -O2 -ffreestanding -nostdlib -fno-builtin-memset -DPLAT_HOST -Isrc/shim -Iengine \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,--initial-memory=4194304 -Wl,-z,stack-size=65536 \
  -o build/skrewell.wasm src/wasm_glue.c engine/skrewell.c engine/skr_bank1.c
python3 -c "
import base64
s=open('src/app.html').read(); w=base64.b64encode(open('build/skrewell.wasm','rb').read()).decode()
open('index.html','w').write(s.replace('__WASM__', w))
import re
open('worklet.js','w').write(re.search(r'<script id=\"worklet\" type=\"text/plain\">\n(.*?)</script>', s, re.S).group(1))"
echo "index.html rebuilt"
