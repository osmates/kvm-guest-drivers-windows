#!/bin/sh
# Checks a built viogpu3d package for what Windows needs of its Direct3D user-mode driver.
#
#   sh viogpu3d/umd/tests/check-package.sh <package folder>
#
# The folder is Install/<OS>/ARM64 as Tools/build-macos.sh packs it, with the stamped viogpu3d.inf. Checks:
# the INF lists every file of the package and the package has every file the INF lists; UserModeDriverName names
# the driver in its four slots; the driver is an ARM64 DLL that exports OpenAdapter10_2 only and imports system DLLs
# only; its file version is the INF's DriverVer version; the notices file beside it carries the full licence of
# Triton, of DXVK and of each of DXVK's subprojects, as this tree has them.
#
# Needs llvm-objdump (LLVM_OBJDUMP, else in LLVM_BIN, else on PATH, else Homebrew's llvm) and python3.
set -eu

[ $# -eq 1 ] || { echo "usage: check-package.sh <package folder>" >&2; exit 2; }
PACK=$1
INF="$PACK/viogpu3d.inf"
UMD=viogpu3d_umd.dll
DLL="$PACK/$UMD"
NOTICES=viogpu3d_umd_notices.txt
UMD_SRC=$(cd "$(dirname "$0")/.." && pwd)

if [ -z "${LLVM_OBJDUMP:-}" ]; then
    if [ -n "${LLVM_BIN:-}" ] && [ -x "$LLVM_BIN/llvm-objdump" ]; then
        LLVM_OBJDUMP="$LLVM_BIN/llvm-objdump"
    else
        LLVM_OBJDUMP=$(command -v llvm-objdump || true)
    fi
    # Homebrew's llvm is keg-only: not on PATH.
    if [ -z "$LLVM_OBJDUMP" ] && command -v brew >/dev/null &&
        [ -x "$(brew --prefix)/opt/llvm/bin/llvm-objdump" ]; then
        LLVM_OBJDUMP="$(brew --prefix)/opt/llvm/bin/llvm-objdump"
    fi
fi
[ -n "$LLVM_OBJDUMP" ] || { echo "check-package: no llvm-objdump; set LLVM_OBJDUMP or LLVM_BIN" >&2; exit 2; }
[ -f "$INF" ] || { echo "check-package: no $INF" >&2; exit 2; }

status=0
fail() {
    echo "check-package: FAIL: $*" >&2
    status=1
}
pass() {
    echo "check-package: ok: $*"
}

inf_files=$(tr -d '\r' <"$INF" | awk '
    /^[ \t]*\[/ { section = tolower($0); next }
    section ~ /^[ \t]*\[sourcedisksfiles(\.[^]]*)?\]/ && $0 !~ /^[ \t]*(;|$)/ {
        name = $0
        sub(/[ \t]*=.*/, "", name)
        sub(/^[ \t]*/, "", name)
        print tolower(name)
    }')

missing=""
for file in $inf_files; do
    [ -n "$(find "$PACK" -maxdepth 1 -iname "$file" -print)" ] || missing="$missing $file"
done
[ -z "$missing" ] && pass "the package has every file the INF lists" || fail "the INF lists files the package lacks:$missing"

# The catalog and debug symbols are not installed files.
unlisted=""
for path in "$PACK"/*; do
    file=$(basename "$path" | tr '[:upper:]' '[:lower:]')
    case "$file" in
        *.inf | *.cat | *.pdb) continue ;;
    esac
    echo "$inf_files" | grep -q -x -F "$file" || unlisted="$unlisted $file"
done
[ -z "$unlisted" ] && pass "the INF lists every file of the package" || fail "files the INF does not list:$unlisted"

echo "$inf_files" | grep -q -x -F "$UMD" || fail "the INF does not list $UMD"

slots=$(tr -d '\r' <"$INF" | awk -F, 'tolower($0) ~ /^hkr,[ \t]*,[ \t]*usermodedrivername[ \t]*,/ {
    out = ""
    for (i = 5; i <= NF; i++) { v = $i; gsub(/^[ \t]+|[ \t]+$/, "", v); out = out (out == "" ? "" : " ") tolower(v) }
    print out
}')
type=$(tr -d '\r' <"$INF" | awk -F, 'tolower($0) ~ /^hkr,[ \t]*,[ \t]*usermodedrivername[ \t]*,/ {
    v = $4; gsub(/^[ \t]+|[ \t]+$/, "", v); print v }')
if [ "$slots" = "$UMD $UMD $UMD $UMD" ] && { [ "$type" = "%REG_MULTI_SZ%" ] || [ "$type" = "0x00010000" ]; }; then
    pass "UserModeDriverName is $UMD in the D3D9, D3D10, D3D11 and D3D12 slots"
else
    fail "UserModeDriverName is '$slots' ($type), not $UMD four times as REG_MULTI_SZ"
fi

if ! echo "$inf_files" | grep -q -x -F "$NOTICES" || [ ! -f "$PACK/$NOTICES" ]; then
    fail "the package and its INF do not both have $NOTICES"
else
    # Whitespace aside, each licence file's text must be in the notices.
    lacking=$(python3 -I - "$PACK/$NOTICES" "$UMD_SRC/triton/LICENSE" "$UMD_SRC/dxvk/LICENSE" \
        "$UMD_SRC"/dxvk/subprojects/*/LICENSE <<'EOF'
import sys

def words(path):
    with open(path, encoding="utf-8") as f:
        return " ".join(f.read().split())

notices = words(sys.argv[1])
for licence in sys.argv[2:]:
    try:
        found = words(licence) in notices
    except OSError as error:
        found = False
        print(error, file=sys.stderr)
    if not found:
        print(licence)
EOF
    ) || lacking="(python3 failed)"
    if [ -z "$lacking" ]; then
        pass "$NOTICES has the licences of Triton, DXVK and DXVK's subprojects"
    else
        fail "$NOTICES lacks: $(echo "$lacking" | tr '\n' ' ')"
    fi
fi

if [ ! -f "$DLL" ]; then
    fail "no $UMD in the package"
    exit 1
fi
dump=$("$LLVM_OBJDUMP" -p "$DLL")

echo "$dump" | grep -q 'file format coff-arm64' && pass "$UMD is ARM64" || fail "$UMD is not coff-arm64"

exports=$(echo "$dump" | sed -n '/^Export Table:/,/^$/p' | awk '$1 ~ /^[0-9]+$/ && NF >= 3 { print $NF }')
[ "$exports" = "OpenAdapter10_2" ] && pass "$UMD exports OpenAdapter10_2 only" ||
    fail "$UMD exports '$(echo $exports)', not OpenAdapter10_2 alone"

imports=$(echo "$dump" | awk '/DLL Name:/ { print $3 }')
# System DLLs only. The runtime's own D3D and DXGI DLLs, the Vulkan loader and toolchain runtimes are refused by name.
banned=$(echo "$imports" | grep -i -E '^(vulkan-1|d3d9|d3d10|d3d10_1|d3d10core|d3d11|d3d12|dxgi|libc\+\+|libunwind|libwinpthread-1|libgcc_s_seh-1|libstdc\+\+-6)\.dll$' || true)
other=$(echo "$imports" | grep -v -i -E '^(kernel32|user32|gdi32|advapi32|version|ole32|oleaut32|shell32|shlwapi|ntdll)\.dll$|^api-ms-win-[a-z0-9-]+\.dll$' || true)
if [ -z "$banned" ] && [ -z "$other" ]; then
    pass "$UMD imports system DLLs only: $(echo $imports)"
else
    fail "$UMD imports $(echo $banned $other)"
fi

driverver=$(tr -d '\r' <"$INF" | awk -F'[=,]' 'tolower($1) ~ /^[ \t]*driverver[ \t]*$/ { v = $3; gsub(/[ \t]/, "", v); print v; exit }')
fileversion=$(python3 -I - "$DLL" <<'EOF'
import struct
import sys

data = open(sys.argv[1], "rb").read()
# VS_FIXEDFILEINFO: dwSignature, dwStrucVersion, then the file version's high and low words.
at = data.find(struct.pack("<I", 0xFEEF04BD))
if at < 0:
    sys.exit(1)
ms, ls = struct.unpack_from("<II", data, at + 8)
print(f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}")
EOF
) || fileversion=""
if [ -n "$driverver" ] && [ "$fileversion" = "$driverver" ]; then
    pass "$UMD file version $fileversion is the INF's DriverVer version"
else
    fail "$UMD file version '$fileversion', the INF's DriverVer version '$driverver'"
fi

exit $status
