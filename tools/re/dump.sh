#!/bin/sh
# Shader reverse-engineering helper.
# usage (repo root): sh tools/re/dump.sh SHADER_TAG...
#   writes out/sh/TAG.dxil (game DXIL via nimcli), out/sh/TAG.full (dxc -dumpbin),
#   out/sh/TAG.asm (trimmed) and out/sh/TAG.hl (pseudo-HLSL from tools/re/decomp.py).
# out/ is gitignored: game-derived dumps never go into the repository.
DXC="/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe"
mkdir -p out/sh
for s in "$@"; do
  ./build/Release/nimcli.exe shader $s out/sh/$s.dxil >/dev/null 2>&1
  "$DXC" -dumpbin out/sh/$s.dxil > out/sh/$s.full 2>&1
  grep -E "^; (Name|  |-|EntryFunctionName|NumThreads)|cbuffer|texture |UAV |sampler |define|call|fadd|fmul|fsub|fdiv|phi|br |label|ret|icmp|select|sitofp|uitofp|fptosi|fptoui|extractvalue|store|load|getelementptr|constant" out/sh/$s.full | grep -v "^declare\|^; Function Attrs" | sed -e 's/  ; [A-Za-z]*(.*$//' -e 's/@dx.op.//' -e 's/%dx.types.//g' | cut -c1-200 > out/sh/$s.asm
  python -I tools/re/decomp.py out/sh/$s.full > out/sh/$s.hl 2>/dev/null
  echo "$s: $(wc -l < out/sh/$s.hl) lines of pseudo-HLSL"
done
