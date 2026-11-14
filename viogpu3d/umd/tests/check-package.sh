#!/bin/sh
# Checks a built viogpu3d package for what Windows needs of its Direct3D user-mode drivers and Venus Vulkan drivers.
#
#   MESA_SRC=<Mesa tree> MINGW_DIR=<llvm-mingw> sh viogpu3d/umd/tests/check-package.sh <package folder>
#
# The folder is Install/<OS>/ARM64 as Tools/build-macos.sh packs it, with the stamped viogpu3d.inf. Checks:
# - the INF lists every file of the package and the package has every file the INF lists;
# - UserModeDriverName names the ARM64X driver and UserModeDriverNameWow the x86 driver, each in its four slots;
#   VulkanDriverName and VulkanDriverNameWow name the two manifests, whose library_path is the DLL beside them;
# - viogpu3d_umd.dll and vulkan_virtio.dll are ARM64X, with a native and an ARM64EC view; viogpu3d_umd_x86.dll and
#   vulkan_virtio_x86.dll are x86;
# - each view of each Direct3D driver exports OpenAdapter10_2 only, by that name (undecorated on x86, where the
#   function is stdcall), and each view of each Vulkan driver the loader's three vk_icd entry points;
# - every view imports system DLLs only;
# - the file version of all four DLLs is the INF's DriverVer version;
# - the notices file carries the full licence of Triton, of DXVK and of each of DXVK's subprojects, as this tree has
#   them, the licences llvm-mingw ships for its runtime, as MINGW_DIR has them, and a Mesa part naming MIT, with
#   copyright lines, whose every licence text is the one in MESA_SRC/licenses.
#
# Needs llvm-objdump and llvm-readobj (LLVM_OBJDUMP and LLVM_READOBJ, else in LLVM_BIN, else on PATH, else
# Homebrew's llvm), python3, MESA_SRC: the Mesa tree the Venus drivers were built from, and MINGW_DIR: the llvm-mingw
# the DLLs were linked with.
set -eu

[ $# -eq 1 ] || { echo "usage: check-package.sh <package folder>" >&2; exit 2; }
PACK=$1
INF="$PACK/viogpu3d.inf"
UMD=viogpu3d_umd.dll
UMD_X86=viogpu3d_umd_x86.dll
ICD=vulkan_virtio.dll
ICD_X86=vulkan_virtio_x86.dll
NOTICES=viogpu3d_umd_notices.txt
UMD_SRC=$(cd "$(dirname "$0")/.." && pwd)

llvm_tool() {
    name=$1
    if [ -n "${LLVM_BIN:-}" ] && [ -x "$LLVM_BIN/$name" ]; then
        echo "$LLVM_BIN/$name"
        return
    fi
    found=$(command -v "$name" || true)
    # Homebrew's llvm is keg-only: not on PATH.
    if [ -z "$found" ] && command -v brew >/dev/null && [ -x "$(brew --prefix)/opt/llvm/bin/$name" ]; then
        found="$(brew --prefix)/opt/llvm/bin/$name"
    fi
    echo "$found"
}
LLVM_OBJDUMP=${LLVM_OBJDUMP:-$(llvm_tool llvm-objdump)}
LLVM_READOBJ=${LLVM_READOBJ:-$(llvm_tool llvm-readobj)}
[ -n "$LLVM_OBJDUMP" ] || { echo "check-package: no llvm-objdump; set LLVM_OBJDUMP or LLVM_BIN" >&2; exit 2; }
[ -n "$LLVM_READOBJ" ] || { echo "check-package: no llvm-readobj; set LLVM_READOBJ or LLVM_BIN" >&2; exit 2; }
[ -f "$INF" ] || { echo "check-package: no $INF" >&2; exit 2; }

status=0
fail() {
    printf 'check-package: FAIL: %s\n' "$*" >&2
    status=1
}
pass() {
    printf 'check-package: ok: %s\n' "$*"
}

inf=$(tr -d '\r' <"$INF")
inf_files=$(printf '%s\n' "$inf" | awk '
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

for file in $UMD $UMD_X86 $ICD $ICD_X86 $NOTICES vulkan_virtio.json vulkan_virtio_x86.json; do
    echo "$inf_files" | grep -q -x -F "$file" || fail "the INF does not list $file"
done

# The values of an HKR,,<name>,<type>,... line: the type, then the data.
reg_value() {
    printf '%s\n' "$inf" | awk -F, -v name="$(echo "$1" | tr '[:upper:]' '[:lower:]')" '
        { key = tolower($3); gsub(/^[ \t]+|[ \t]+$/, "", key) }
        tolower($1) ~ /^hkr$/ && key == name {
            out = ""
            for (i = 4; i <= NF; i++) { v = $i; gsub(/^[ \t]+|[ \t]+$/, "", v); out = out (out == "" ? "" : " ") v }
            print out
        }'
}

for pair in "UserModeDriverName $UMD" "UserModeDriverNameWow $UMD_X86"; do
    set -- $pair
    value=$(reg_value "$1")
    lower=$(printf '%s\n' "$value" | tr '[:upper:]' '[:lower:]')
    if [ "$lower" = "%reg_multi_sz% $2 $2 $2 $2" ] || [ "$lower" = "0x00010000 $2 $2 $2 $2" ]; then
        pass "$1 is $2 in the D3D9, D3D10, D3D11 and D3D12 slots"
    else
        fail "$1 is '$value', not $2 four times as REG_MULTI_SZ"
    fi
done

for pair in "VulkanDriverName vulkan_virtio.json $ICD" "VulkanDriverNameWow vulkan_virtio_x86.json $ICD_X86"; do
    set -- $pair
    value=$(reg_value "$1")
    manifest=$(printf '%s\n' "$value" | awk '{ print $NF }' | sed 's#.*\\##')
    if [ "$(printf '%s\n' "$manifest" | tr '[:upper:]' '[:lower:]')" != "$2" ] ||
        [ "$(printf '%s\n' "$value" | awk '{ print NF }')" != 2 ]; then
        fail "$1 is '$value', not the path of $2"
    elif ! tr -d '\r' <"$PACK/$2" | grep -q "\"library_path\": *\"\\.\\\\\\\\$(echo "$3" | sed 's/\./\\./g')\""; then
        fail "$2's library_path is not .\\$3"
    else
        pass "$1 names $2, whose library_path is .\\$3"
    fi
done

if ! echo "$inf_files" | grep -q -x -F "$NOTICES" || [ ! -f "$PACK/$NOTICES" ]; then
    fail "the package and its INF do not both have $NOTICES"
elif [ -z "${MESA_SRC:-}" ] || [ ! -d "$MESA_SRC/licenses" ]; then
    fail "MESA_SRC is not a Mesa tree with a licenses folder: '${MESA_SRC:-}'"
elif [ -z "${MINGW_DIR:-}" ] || [ ! -f "$MINGW_DIR/LICENSE.TXT" ]; then
    fail "MINGW_DIR is not an llvm-mingw with its LICENSE.TXT: '${MINGW_DIR:-}'"
else
    # Whitespace aside, each licence file's text must be in the notices.
    lacking=$(python3 -I - "$PACK/$NOTICES" "$MESA_SRC" "$UMD_SRC/triton/LICENSE" "$UMD_SRC/dxvk/LICENSE" \
        "$UMD_SRC"/dxvk/subprojects/*/LICENSE "$MINGW_DIR/LICENSE.TXT" \
        "$MINGW_DIR"/aarch64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt \
        "$MINGW_DIR"/i686-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt <<'EOF'
import os
import re
import sys


def words(text):
    return " ".join(text.split())


notices_path, mesa, licences = sys.argv[1], sys.argv[2], sys.argv[3:]
notices = open(notices_path, encoding="utf-8").read()
for licence in licences:
    try:
        found = words(open(licence, encoding="utf-8").read()) in words(notices)
    except OSError as error:
        found = False
        print(error, file=sys.stderr)
    if not found:
        print(licence)

part = notices.find("== Mesa,")
if part < 0:
    print("a Mesa part")
    sys.exit()
mesa_part = notices[part:]
headings = re.findall(r"^-- (\S+) \(licenses/(\S+)\) --$", mesa_part, re.M)
if ("MIT", "MIT") not in headings:
    print("Mesa's MIT licence")
if not re.search(r"^Copyright\b", mesa_part, re.M):
    print("Mesa's copyright lines")
for name, path in headings:
    text = open(os.path.join(mesa, "licenses", path), encoding="utf-8").read()
    text = text.split("License-Text:", 1)[1] if "License-Text:" in text else text
    if words(text) not in words(mesa_part):
        print(f"Mesa's {name} as licenses/{path} has it")
EOF
    ) || lacking="(python3 failed)"
    if [ -z "$lacking" ]; then
        pass "$NOTICES has the licences of Triton, DXVK, DXVK's subprojects, the llvm-mingw runtime and Mesa"
    else
        fail "$NOTICES lacks: $(echo "$lacking" | tr '\n' ' ')"
    fi
    for dll in $UMD $UMD_X86 $ICD $ICD_X86; do
        head -n 2 "$PACK/$NOTICES" | tr '\n' ' ' | grep -q -F "$dll" || fail "$NOTICES does not name $dll"
    done
fi

# Per view: "<view> format <f>", "<view> export <name>", "<view> import <dll>"; view is native or arm64ec.
views() {
    "$LLVM_READOBJ" --coff-exports --coff-imports "$1" | python3 -I -c '
import re
import sys

view = "native"
block = None
for line in sys.stdin:
    text = line.strip()
    if text == "HybridObject {":
        view = "arm64ec"
    elif text.startswith("Format:"):
        print(view, "format", text.split()[1])
    elif text in ("Export {", "Import {"):
        block = text.split()[0].lower()
    elif text.startswith("Name:") and block:
        print(view, block, text.split(None, 1)[1])
        block = None if block == "import" else block
    elif text == "}":
        block = None
'
}

# check_dll <file> <arm64x|i386> <export>...: machine, exports and imports of every view.
check_dll() {
    dll=$1 machine=$2
    shift 2
    if [ ! -f "$PACK/$dll" ]; then
        fail "no $dll in the package"
        return
    fi
    format=$("$LLVM_OBJDUMP" -f "$PACK/$dll" | sed -n 's/.*file format //p')
    out=$(views "$PACK/$dll")
    if [ "$machine" = arm64x ]; then
        want="native arm64ec"
        if [ "$format" = coff-arm64x ] && echo "$out" | grep -q -x 'arm64ec format COFF-ARM64EC'; then
            pass "$dll is ARM64X, with an ARM64EC view"
        else
            fail "$dll is $format, not ARM64X with an ARM64EC view"
        fi
    else
        want="native"
        [ "$format" = coff-i386 ] && pass "$dll is x86" || fail "$dll is $format, not coff-i386"
    fi
    expected=$(printf '%s\n' "$@" | sort | paste -sd ' ' -)
    for view in $want; do
        exports=$(echo "$out" | awk -v v="$view" '$1 == v && $2 == "export" { print $3 }' | sort | paste -sd ' ' -)
        [ "$exports" = "$expected" ] && pass "$dll ($view) exports $expected" ||
            fail "$dll ($view) exports '$exports', not '$expected'"
        imports=$(echo "$out" | awk -v v="$view" '$1 == v && $2 == "import" { print $3 }')
        # System DLLs only. The runtime's own D3D and DXGI DLLs, the Vulkan loader and toolchain runtimes are refused by
        # name.
        banned=$(echo "$imports" | grep -i -E '^(vulkan-1|d3d9|d3d10|d3d10_1|d3d10core|d3d11|d3d12|dxgi|libc\+\+|libunwind|libwinpthread-1|libgcc_s_seh-1|libgcc_s_dw2-1|libstdc\+\+-6)\.dll$' || true)
        other=$(echo "$imports" | grep -v -i -E '^(kernel32|user32|gdi32|advapi32|version|ole32|oleaut32|shell32|shlwapi|ntdll)\.dll$|^api-ms-win-[a-z0-9-]+\.dll$' || true)
        if [ -n "$imports" ] && [ -z "$banned" ] && [ -z "$other" ]; then
            pass "$dll ($view) imports system DLLs only: $(echo $imports)"
        else
            fail "$dll ($view) imports '$(echo $banned $other)'"
        fi
    done
}

check_dll $UMD arm64x OpenAdapter10_2
check_dll $UMD_X86 i386 OpenAdapter10_2
check_dll $ICD arm64x vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion
check_dll $ICD_X86 i386 vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion

driverver=$(printf '%s\n' "$inf" | awk -F'[=,]' 'tolower($1) ~ /^[ \t]*driverver[ \t]*$/ { v = $3; gsub(/[ \t]/, "", v); print v; exit }')
for dll in $UMD $UMD_X86 $ICD $ICD_X86; do
    [ -f "$PACK/$dll" ] || continue
    fileversion=$(python3 -I - "$PACK/$dll" <<'EOF'
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
        pass "$dll file version $fileversion is the INF's DriverVer version"
    else
        fail "$dll file version '$fileversion', the INF's DriverVer version '$driverver'"
    fi
done

exit $status
