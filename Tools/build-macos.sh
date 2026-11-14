#!/usr/bin/env bash
#
# Builds kernel-mode drivers of this repository for Windows ARM64 on macOS. See usage below.

if [ -z "${BASH_VERSION:-}" ] || shopt -qo posix 2>/dev/null; then
    # sh is bash in POSIX mode here, which has no process substitution.
    [ -z "${BUILD_MACOS_REEXEC:-}" ] || {
        echo "build-macos: needs bash outside POSIX mode" >&2
        exit 1
    }
    BUILD_MACOS_REEXEC=1 exec bash "$0" "$@"
fi

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${SCRIPT_DIR}/.." && pwd)"
WDK_DIR="${WDK_DIR:-$(dirname "${REPO}")/wdk}"
VENUS_ICD_DIR="${VENUS_ICD_DIR:-$(dirname "${REPO}")/mesa-venus-windows-build/build/src/virtio/vulkan}"
VENUS_ICD_ARM64EC_DIR="${VENUS_ICD_ARM64EC_DIR:-$(dirname "${REPO}")/mesa-venus-windows-arm64ec-build/build/src/virtio/vulkan}"
VENUS_ICD_X86_DIR="${VENUS_ICD_X86_DIR:-$(dirname "${REPO}")/mesa-venus-windows-x86-build/build/src/virtio/vulkan}"
MINGW_DIR="${MINGW_DIR:-$(dirname "${REPO}")/llvm-mingw}"
DXVK_PATCHES="${DXVK_PATCHES:-$(dirname "${REPO}")/dxvk-patches}"
UMD_DIR="${REPO}/viogpu3d/umd"
UMD_DLL=viogpu3d_umd.dll
UMD_DLL_X86=viogpu3d_umd_x86.dll
ICD_DLL=vulkan_virtio.dll
ICD_DLL_X86=vulkan_virtio_x86.dll
UMD_NOTICES=viogpu3d_umd_notices.txt

KIT_VERSION="10.0.28000.2526"
KIT_PACKAGES="microsoft.windows.wdk.arm64 microsoft.windows.sdk.cpp microsoft.windows.sdk.cpp.arm64"
SDK_LICENSE_URL="https://aka.ms/WinSDKLicenseURL"
MINGW_VERSION=20260922
# The digest GitHub lists for the release's macOS asset.
MINGW_SHA256=52e5f5a7b131021d0c39a37a38fa380a1da7885cd04bd61afd0cd4ecfb8bc1f3

DEFAULT_PROJECTS=(viogpu/viogpudo/viogpudo.vcxproj viogpu3d/viogpu3d/viogpu3d.vcxproj)

usage() {
    cat <<EOF
Usage:
  Tools/build-macos.sh fetch
  Tools/build-macos.sh [--config "Win11 Release"|"Win10 Release"] [--test-sign] [--venus-icd DIR]
                       [--venus-icd-arm64ec DIR] [--venus-icd-x86 DIR] [--d3d-umd] [--dxvk-patches DIR]
                       [project.vcxproj ...]

fetch downloads the WDK and Windows SDK NuGet packages for ARM64 (${KIT_VERSION}) from
nuget.org into WDK_DIR, checks each against the SHA-512 in nuget.org's catalog and unpacks it.
Their license terms apply: LICENSE.txt in the WDK package, and the SDK's license, which fetch
saves to WDK_DIR/licenses. fetch also downloads llvm-mingw ${MINGW_VERSION} for macOS from
github.com/mstorsjo/llvm-mingw's releases, checks it against the SHA-256 pinned here and unpacks it
into MINGW_DIR, for --d3d-umd.

A build reads each project's vcxproj for the configuration on ARM64: sources, defines, include
folders, libraries and INX. It first builds the static libraries the project links
(VirtIO/VirtioLib.vcxproj for viogpudo), then compiles with clang in MSVC mode, links with
lld-link, stamps DriverVer into a copy of the INX and, with Feature_PackOne, copies the .sys,
.pdb and .inf into Install/<OS>/ARM64 as the Windows build does. Without a project it builds
${DEFAULT_PROJECTS[*]}.

A package whose INF lists ${ICD_DLL} (viogpu3d's) also gets Mesa's Venus Vulkan driver, from
three Mesa builds configured with -Dvulkan-icd-dir=bin, each named by the src/virtio/vulkan folder
in its build folder: --venus-icd for aarch64-w64-mingw32, --venus-icd-arm64ec for
arm64ec-w64-mingw32 and --venus-icd-x86 for i686-w64-mingw32 (by default mesa-venus-windows-build,
mesa-venus-windows-arm64ec-build and mesa-venus-windows-x86-build next to this clone, each with
its build in build). The first two link the same objects with the same flags; ld.lld links both
into one ARM64X ${ICD_DLL} with -marm64x, whose ARM64EC view x64 processes load. The x86 driver
goes in as ${ICD_DLL_X86}, linked again from its build's link command. Both links add the version
resource of viogpu3d/umd/vulkan_virtio.rc, so their file version is the DriverVer version of this
build. Their manifests go in as vulkan_virtio.json and vulkan_virtio_x86.json.

A package whose INF lists ${UMD_DLL} (viogpu3d's) also gets the Direct3D 10 and 11 user-mode
driver of viogpu3d/umd. --d3d-umd builds it first, into viogpu3d/umd/objfre_<os>_arm64, with
llvm-mingw: DXVK from the viogpu3d/umd/dxvk submodule with meson and its umd option, Triton and the
driver's own sources, each for aarch64-w64-mingw32, arm64ec-w64-mingw32 and i686-w64-mingw32. The
first two make the ARM64X ${UMD_DLL}, the last ${UMD_DLL_X86}. Their file version is the
DriverVer version of this build. Without --d3d-umd the package takes the files an earlier
--d3d-umd left there, and the package check refuses a DLL whose version differs. A DXVK checkout
that has no umd meson option yet gets the patches of a folder first (*.patch, in name order,
applied with patch -p1 to a copy): the one --dxvk-patches names, by default dxvk-patches next to
this clone.

${UMD_NOTICES}, written into the package, carries the licences of the third-party code in
these drivers: Triton and DXVK with the DXVK subprojects linked in, from this tree and the DXVK copy of
--d3d-umd, Mesa's, from the licence statement and copyright lines of every Mesa source and header
the three Venus builds compiled (ninja's dependency records), with the licence texts of the Mesa
tree's licenses folder, and the toolchain's: every DLL links llvm-mingw's runtime statically, so
the licences llvm-mingw ships for mingw-w64's runtime and for LLVM's libc++, libunwind and
compiler-rt, from MINGW_DIR and the llvm-mingw each Venus build linked with. After packing,
viogpu3d/umd/tests/check-package.sh checks the package.

--test-sign signs those .sys files with build/TestCert.pfx, a self-signed code signing
certificate that openssl makes on first use. build/TestCert.cer is the certificate a test
machine trusts. No keychain is used.

Needs clang, lld-link, llvm-lib and llvm-rc from one LLVM (tested with 22.1 and 23.1),
xmllint, curl, unzip and openssl, and osslsigncode for --test-sign. --d3d-umd also needs the
llvm-mingw of fetch, meson, rsync, patch and glslang (or glslangValidator) on PATH, and the DXVK
submodule checked out with its own submodules. Packing viogpu3d needs ninja, python3 and the
llvm-mingw of fetch. WDK_DIR
and this clone must be on a case-insensitive volume (the macOS default): the kit's headers include
each other in mixed case.

Environment:
  WDK_DIR            unpacked packages (default: wdk next to this clone, ${WDK_DIR})
  LLVM_BIN           folder of the LLVM tools (default: lld-link on PATH, else Homebrew's lld or
                     newest lld@N, then the rest from Homebrew's llvm of that version, else llvm)
  STAMPINF_DATE      MM/DD/YYYY for DriverVer (default: today)
  _RHEL_RELEASE_VERSION_, _BUILD_MAJOR_VERSION_, _BUILD_MINOR_VERSION_
                     driver version parts, as with MSBuild (defaults from build/Driver.RHEL.props)
  TEST_CERT_SUBJECT  subject of a new test certificate (default: /CN=OSMates GPU Test)
  VENUS_ICD_DIR      the folder --venus-icd names (default: ${VENUS_ICD_DIR})
  VENUS_ICD_ARM64EC_DIR, VENUS_ICD_X86_DIR
                     the folders --venus-icd-arm64ec and --venus-icd-x86 name
  MINGW_DIR          unpacked llvm-mingw (default: llvm-mingw next to this clone, ${MINGW_DIR})
  DXVK_PATCHES       the folder --dxvk-patches names (default: ${DXVK_PATCHES})

Left for Windows:
  tracewpp           no WPP: a project whose WPP configuration is DbgPrint(LEVEL, (MSG, ...))
                     gets .tmh files that send each message to DbgPrintEx (DPFLTR_IHVVIDEO_ID)
  inf2cat            no catalog, so pnputil cannot install the package yet: make the .cat with
                     Inf2Cat and sign it with signtool on Windows (both are in these packages)
  InfVerif, ApiValidator, CodeQL, SDV and the DVL

Not as MSBuild does it: no /GL and /LTCG, and clang's warnings in place of MSVC's /W4 /WX.
EOF
}

die() {
    echo "build-macos: $*" >&2
    exit 1
}

note() {
    echo "build-macos: $*"
}

catalog_hash() {
    local id=$1 entry
    entry=$(curl -fsS --compressed "https://api.nuget.org/v3/registration5-gz-semver2/${id}/${KIT_VERSION}.json" |
        sed -n 's/.*"catalogEntry": *"\([^"]*\)".*/\1/p')
    [[ -n "${entry}" ]] || die "no catalog entry for ${id} ${KIT_VERSION}"
    curl -fsS --compressed "${entry}" | sed -n 's/.*"packageHash": *"\([^"]*\)".*/\1/p'
}

fetch() {
    mkdir -p "${WDK_DIR}/nupkg" "${WDK_DIR}/licenses"
    local id file want got stamp
    for id in ${KIT_PACKAGES}; do
        file="${WDK_DIR}/nupkg/${id}.${KIT_VERSION}.nupkg"
        if [[ ! -f "${file}" ]]; then
            note "downloading ${id} ${KIT_VERSION}"
            curl -fL --retry 3 -o "${file}.part" \
                "https://api.nuget.org/v3-flatcontainer/${id}/${KIT_VERSION}/${id}.${KIT_VERSION}.nupkg"
            mv "${file}.part" "${file}"
        fi
        want=$(catalog_hash "${id}")
        [[ -n "${want}" ]] || die "nuget.org's catalog has no hash for ${id} ${KIT_VERSION}"
        got=$(openssl dgst -sha512 -binary "${file}" | base64)
        [[ "${want}" == "${got}" ]] || die "${file}: SHA-512 ${got}, nuget.org says ${want}"
        stamp="${WDK_DIR}/${id}/.nupkg-sha512"
        if [[ -f "${stamp}" && "$(cat "${stamp}")" == "${got}" ]]; then
            note "${id} ${KIT_VERSION} already unpacked, SHA-512 matches nuget.org"
            continue
        fi
        rm -rf "${WDK_DIR:?}/${id}"
        mkdir -p "${WDK_DIR}/${id}"
        unzip -q "${file}" -d "${WDK_DIR}/${id}"
        printf '%s' "${got}" >"${stamp}"
        note "${id} ${KIT_VERSION} unpacked, SHA-512 matches nuget.org"
    done
    curl -fsSL --retry 3 -o "${WDK_DIR}/licenses/sdk_license.rtf" "${SDK_LICENSE_URL}"
    note "license terms: ${WDK_DIR}/microsoft.windows.wdk.arm64/LICENSE.txt, ${WDK_DIR}/licenses/sdk_license.rtf"
    fetch_mingw
}

fetch_mingw() {
    local name="llvm-mingw-${MINGW_VERSION}-ucrt-macos-universal.tar.xz" stamp="${MINGW_DIR}/.archive-sha256"
    local archive="${MINGW_DIR}.tar.xz.part" got
    if [[ -f "${stamp}" && "$(cat "${stamp}")" == "${MINGW_SHA256}" ]]; then
        note "llvm-mingw ${MINGW_VERSION} already unpacked, SHA-256 matches the pin"
        return
    fi
    note "downloading llvm-mingw ${MINGW_VERSION}"
    curl -fL --retry 3 -o "${archive}" \
        "https://github.com/mstorsjo/llvm-mingw/releases/download/${MINGW_VERSION}/${name}"
    got=$(openssl dgst -sha256 -r "${archive}" | cut -d' ' -f1)
    if [[ "${got}" != "${MINGW_SHA256}" ]]; then
        rm -f "${archive}"
        die "${name}: SHA-256 ${got}, pinned ${MINGW_SHA256}"
    fi
    rm -rf "${MINGW_DIR:?}"
    mkdir -p "${MINGW_DIR}"
    tar -xJf "${archive}" -C "${MINGW_DIR}" --strip-components 1
    rm "${archive}"
    printf '%s' "${got}" >"${stamp}"
    note "llvm-mingw ${MINGW_VERSION} unpacked into ${MINGW_DIR}, SHA-256 matches the pin"
}

major_version() {
    [[ -x "$1" ]] || return 0
    "$1" --version | sed -n -E '1s/.* ([0-9]+)\.[0-9]+\.[0-9]+.*/\1/p'
}

# Homebrew's llvm has no lld-link: it comes in lld or lld@N.
brew_lld_link() {
    local opt=$1 dir
    while IFS= read -r dir; do
        if [[ -x "${dir}/bin/lld-link" ]]; then
            echo "${dir}/bin/lld-link"
            return
        fi
    done < <(
        echo "${opt}/lld"
        printf '%s\n' "${opt}"/lld@* | sort -t@ -k2 -rn
    )
}

llvm_dirs() {
    if [[ -n "${LLVM_BIN:-}" ]]; then
        echo "${LLVM_BIN}"
        return
    fi
    local opt="" lld major
    if command -v brew >/dev/null; then
        opt="$(brew --prefix)/opt"
    fi
    lld=$(command -v lld-link || true)
    [[ -n "${lld}" || -z "${opt}" ]] || lld=$(brew_lld_link "${opt}")
    [[ -z "${lld}" ]] || dirname "${lld}"
    if [[ -n "${opt}" ]]; then
        major=$(major_version "${lld}")
        [[ -z "${major}" ]] || echo "${opt}/llvm@${major}/bin"
        echo "${opt}/llvm/bin"
    fi
}

llvm_tool() {
    local name=$1 dir
    while IFS= read -r dir; do
        if [[ -n "${dir}" && -x "${dir}/${name}" ]]; then
            echo "${dir}/${name}"
            return
        fi
    done < <(llvm_dirs)
    die "${name} not found; set LLVM_BIN to the bin folder of an LLVM toolchain, or brew install llvm lld"
}

setup_kits() {
    WDK_ROOT="${WDK_DIR}/microsoft.windows.wdk.arm64/c"
    SDK_ROOT="${WDK_DIR}/microsoft.windows.sdk.cpp/c"
    SDK_ARM64_ROOT="${WDK_DIR}/microsoft.windows.sdk.cpp.arm64/c"
    [[ -d "${WDK_ROOT}/Include" && -d "${SDK_ROOT}/Include" && -d "${SDK_ARM64_ROOT}/um" ]] ||
        die "no unpacked kits in ${WDK_DIR}; run Tools/build-macos.sh fetch"
    KIT=$(cd "${WDK_ROOT}/Include" && ls -d 10.* | sort | tail -1)
    KM_INC="${WDK_ROOT}/Include/${KIT}/km"
    CRT_INC="${KM_INC}/crt"
    WDK_SHARED_INC="${WDK_ROOT}/Include/${KIT}/shared"
    WDK_UM_INC="${WDK_ROOT}/Include/${KIT}/um"
    SDK_SHARED_INC="${SDK_ROOT}/Include/${KIT}/shared"
    UM_INC="${SDK_ROOT}/Include/${KIT}/um"
    DDK_LIB="${WDK_ROOT}/Lib/${KIT}/km/arm64"
    SDK_LIB="${SDK_ARM64_ROOT}/um/arm64"
    [[ -f "${DDK_LIB}/ntoskrnl.lib" && -f "${SDK_LIB}/arm64rt.lib" ]] || die "kit libraries missing in ${WDK_DIR}"
    [[ -f "${KM_INC}/NTDDK.H" && -f "${REPO}/BUILD/DRIVER.COMMON.PROPS" ]] ||
        die "${WDK_DIR} and ${REPO} must be on a case-insensitive volume"

    LLD_LINK=$(llvm_tool lld-link)
    CLANG=$(llvm_tool clang)
    LLVM_LIB=$(llvm_tool llvm-lib)
    LLVM_RC=$(llvm_tool llvm-rc)
    [[ "$(major_version "${CLANG}")" == "$(major_version "${LLD_LINK}")" ]] ||
        note "clang and lld-link are from different LLVM releases: ${CLANG}, ${LLD_LINK}"
}

setup_config() {
    case "${CONFIG}" in
        "Win11 Release")
            TARGET_OS=Win11
            INF_ARCH="ARM64.10.0...16299"
            ;;
        "Win10 Release")
            TARGET_OS=Win10
            INF_ARCH="ARM64.10.0"
            ;;
        *) die "unknown configuration ${CONFIG}" ;;
    esac
    NT_TARGET_MAJ=100
    CFG_MATCH="${CONFIG}|ARM64"

    local rhel="${REPO}/build/Driver.RHEL.props" vendor="${REPO}/build/Driver.Vendor.props"
    RHEL_RELEASE_VERSION="${_RHEL_RELEASE_VERSION_:-$(xp "string(//*[local-name()='_RHEL_RELEASE_VERSION_'])" "${rhel}")}"
    BUILD_MAJOR_VERSION="${_BUILD_MAJOR_VERSION_:-$(xp "string(//*[local-name()='_BUILD_MAJOR_VERSION_'])" "${rhel}")}"
    BUILD_MINOR_VERSION="${_BUILD_MINOR_VERSION_:-$(xp "string(//*[local-name()='_BUILD_MINOR_VERSION_'])" "${rhel}")}"
    COPYRIGHT_CURRENT_YEAR=$(xp "string(//*[local-name()='COPYRIGHT_CURRENT_YEAR'])" "${vendor}")
    STAMPINF_VERSION="${NT_TARGET_MAJ}.${RHEL_RELEASE_VERSION}.${BUILD_MAJOR_VERSION}.${BUILD_MINOR_VERSION}"
    [[ "${STAMPINF_VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "bad driver version ${STAMPINF_VERSION}"
}

xp() {
    xmllint --xpath "$1" "$2" 2>/dev/null || true
}

# MSBuild property: the configuration's PropertyGroup first, then an unconditioned one.
prop() {
    local file=$1 name=$2 value
    value=$(xp "string((//*[local-name()='PropertyGroup'][contains(@Condition,'${CFG_MATCH}')]/*[local-name()='${name}'])[1])" "${file}")
    [[ -n "${value}" ]] ||
        value=$(xp "string((//*[local-name()='PropertyGroup'][not(@Condition)]/*[local-name()='${name}'])[1])" "${file}")
    printf '%s' "${value}"
}

# Item definition metadata of the unconditioned groups and then the configuration's, joined with ';'.
meta() {
    local file=$1 tool=$2 name=$3 sel
    for sel in "not(@Condition)" "contains(@Condition,'${CFG_MATCH}')"; do
        xp "string((//*[local-name()='ItemDefinitionGroup'][${sel}]/*[local-name()='${tool}']/*[local-name()='${name}'])[1])" "${file}"
        printf ';'
    done
}

items() {
    xp "//*[local-name()='$2'][@Include]/@Include" "$1" | sed -n 's/^ *Include="\(.*\)"$/\1/p'
}

expand() {
    local s=$1
    s=${s//'$(IntDir)'/${INTDIR_REL}}
    s=${s//'$(OutDir)'/${OUTDIR_REL}}
    s=${s//'$(DDK_INC_PATH)'/${KM_INC}/}
    s=${s//'$(SDK_INC_PATH)'/${UM_INC}/}
    s=${s//'$(DDK_LIB_PATH)'/${DDK_LIB}/}
    s=${s//'$(KernelBufferOverflowLib)'/${DDK_LIB}/BufferOverflowFastFailK.lib}
    s=${s//'$(ProjectDir)'/${PROJ_DIR}/}
    s=$(printf '%s' "${s}" | sed -E 's/%\([A-Za-z]+\)//g')
    s=${s//\\//}
    [[ "${s}" != *'$('* ]] || die "${PROJ}: unknown MSBuild macro in: ${s}"
    printf '%s' "${s}"
}

split() {
    printf '%s' "$1" | tr ';' '\n' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' | sed '/^$/d'
}

abs_path() {
    local path
    case "$1" in
        /*) path=$1 ;;
        *) path="${PROJ_DIR}/$1" ;;
    esac
    path=$(printf '%s' "${path}" | sed -E 's#/+#/#g; s#/$##')
    printf '%s' "${path}"
}

msbuild_unescape() {
    printf '%s' "$1" | sed -e 's/%25/%/g' -e 's/%3B/;/g' -e 's/%24/$/g'
}

copyright_years() {
    local strings=$1 start
    start=$(xp "string(//*[local-name()='RHEL_COPYRIGHT_STARTING_YEAR'][contains(@Condition,\"'${strings}'\")])" \
        "${REPO}/build/Driver.RHEL.props")
    [[ -n "${start}" ]] || start="20??"
    RHEL_COPYRIGHT_STARTING_YEAR=${start}
    if [[ "${start}" == "${COPYRIGHT_CURRENT_YEAR}" ]]; then
        RHEL_COPYRIGHT_YEARS=${start}
    else
        RHEL_COPYRIGHT_YEARS="${start}-${COPYRIGHT_CURRENT_YEAR}"
    fi
}

# tracewpp.exe runs on Windows only: these .tmh files map the DbgPrint(LEVEL, (MSG, ...)) WPP
# function onto DbgPrintEx.
write_wpp_stubs() {
    local config=$1 src base
    grep -q 'DbgPrint{FLAG *= *TRACE_ALL}(LEVEL, *(MSG, *\.\.\.))' "${config}" ||
        die "${PROJ}: ${config} declares WPP functions this script cannot map; build it on Windows"
    for src in "${SOURCES[@]}"; do
        base=$(basename "${src}")
        base=${base%.*}
        cat >"${INTDIR}/${base}.tmh" <<'EOF'
// Written by Tools/build-macos.sh in place of tracewpp output.
#pragma once
#define WPP_INIT_TRACING(DriverObject, RegistryPath)
#define WPP_CLEANUP(DriverObject)
#undef DbgPrint
#define WPP_DBGPRINT_ARGS(...) __VA_ARGS__
#define WPP_DBGPRINT_LEVEL(Level)                                                                  \
    ((Level) <= TRACE_LEVEL_ERROR ? DPFLTR_ERROR_LEVEL : (Level) - TRACE_LEVEL_WARNING + DPFLTR_WARNING_LEVEL)
#define WPP_DBGPRINT(Level, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, WPP_DBGPRINT_LEVEL(Level), __VA_ARGS__)
#define DbgPrint(Level, MSG) WPP_DBGPRINT(Level, WPP_DBGPRINT_ARGS MSG)
EOF
    done
}

# The WindowsKernelModeDriver10.0 toolset's switches for ARM64 (WindowsDriver.Common.props,
# WindowsDriver.Shared.props, WindowsDriver.arm64.props, WindowsDriver.OS.props in the WDK).
compile_flags() {
    local allow_date_time=$1
    CFLAGS=(--driver-mode=cl --target=aarch64-pc-windows-msvc -fms-compatibility-version=19.50 /nologo /c
        /kernel /GS /guard:cf /Gy /GF /Zp8 /Zc:wchar_t- /GR- /Z7 /W3 /Ox /Os /Oy-
        /FI"${SDK_SHARED_INC}/warning.h"
        -Wno-nonportable-include-path -Wno-microsoft-include -Wno-pragma-pack -Wno-unknown-pragmas
        -Wno-error=incompatible-function-pointer-types
        -D_ARM64_ -DARM64 -D_USE_DECLSPECS_FOR_SAL=1 -DSTD_CALL
        -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWINNT=1 -DNTDDI_VERSION=0x0A000000
        "-DVENDOR_VER=RHEL.ver" "-DVER_OS=${TARGET_OS}" -DVER_ARCH=ARM64
        "-DCOPYRIGHT_CURRENT_YEAR=${COPYRIGHT_CURRENT_YEAR}"
        "-D_BUILD_MAJOR_VERSION_=${BUILD_MAJOR_VERSION}" "-D_BUILD_MINOR_VERSION_=${BUILD_MINOR_VERSION}"
        "-D_NT_TARGET_MAJ=${NT_TARGET_MAJ}" "-D_RHEL_RELEASE_VERSION_=${RHEL_RELEASE_VERSION}"
        "-DRHEL_COPYRIGHT_STARTING_YEAR=${RHEL_COPYRIGHT_STARTING_YEAR}")
    [[ "${allow_date_time}" == 1 ]] || CFLAGS+=(/d1nodatetime)
}

rc_flags() {
    RCFLAGS=(/D_ARM64_=1 /DARM64=1 /D_USE_DECLSPECS_FOR_SAL=1 /DSTD_CALL
        /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DWINNT=1 /DNTDDI_VERSION=0x0A000000
        /DVENDOR_VER=RHEL.ver "/DVER_OS=${TARGET_OS}" /DVER_ARCH=ARM64
        "/DCOPYRIGHT_CURRENT_YEAR=${COPYRIGHT_CURRENT_YEAR}"
        "/D_BUILD_MAJOR_VERSION_=${BUILD_MAJOR_VERSION}" "/D_BUILD_MINOR_VERSION_=${BUILD_MINOR_VERSION}"
        "/D_NT_TARGET_MAJ=${NT_TARGET_MAJ}" "/D_RHEL_RELEASE_VERSION_=${RHEL_RELEASE_VERSION}"
        "/DRHEL_COPYRIGHT_YEARS=${RHEL_COPYRIGHT_YEARS}")
}

# stampinf's changes (DriverVer, $ARCH$) and the substitutions of build/Driver.*.props.
stamp_inf() {
    local inx=$1 inf=$2 date subst=() file count i key value
    date="${STAMPINF_DATE:-$(date +%m/%d/%Y)}"
    [[ "${date}" =~ ^[0-9]{2}/[0-9]{2}/[0-9]{4}$ ]] || die "STAMPINF_DATE must be MM/DD/YYYY"
    for file in "${REPO}/build/Driver.Common.props" "${REPO}/build/Driver.Vendor.props" "${REPO}/build/Driver.RHEL.props"; do
        local sel="//*[local-name()='Substitution'][not(@Condition) or contains(@Condition,\"'${CONFIG}'\")]"
        count=$(xp "count(${sel})" "${file}")
        for ((i = 1; i <= ${count:-0}; i++)); do
            key=$(xp "string((${sel})[${i}]/@Include)" "${file}")
            value=$(xp "string((${sel})[${i}]/*[local-name()='ReplaceWith'])" "${file}")
            value=${value//'$(RHEL_COPYRIGHT_YEARS)'/${RHEL_COPYRIGHT_YEARS}}
            value=${value//'$(SUBSYSTEM_VENDOR_ID)'/1AF4}
            value=$(msbuild_unescape "${value}")
            [[ "${value}" != *'$('* ]] || die "unknown MSBuild macro in substitution ${key}: ${value}"
            subst+=("${key}" "${value}")
        done
    done
    local joined="" sep=$'\037'
    for value in "${subst[@]+"${subst[@]}"}"; do
        joined+="${value}${sep}"
    done
    tr -d '\r' <"${inx}" |
        INF_SUBST="${joined}" awk -v arch="${INF_ARCH}" -v driverver="DriverVer = ${date},${STAMPINF_VERSION}" '
            BEGIN { n = split(ENVIRON["INF_SUBST"], s, "\037") }
            function replace_all(line, key, value,    out, at) {
                out = ""
                while ((at = index(line, key)) > 0) {
                    out = out substr(line, 1, at - 1) value
                    line = substr(line, at + length(key))
                }
                return out line
            }
            {
                for (i = 1; i + 1 <= n; i += 2) if (s[i] != "") $0 = replace_all($0, s[i], s[i + 1])
                $0 = replace_all($0, "$ARCH$", arch)
                if (tolower($0) ~ /^\[version\]/) section = "version"
                else if ($0 ~ /^\[/) section = ""
                if (section == "version" && tolower($0) ~ /^driverver[ \t]*=/) { $0 = driverver; stamped = 1 }
                print $0 "\r"
            }
            END { if (!stamped) exit 3 }' >"${inf}" || die "${inx}: no DriverVer in [Version]"
    ! grep -q -E '\$(ARCH|KMDFVERSION|UMDFVERSION|KMDFCOINSTALLERVERSION)\$' "${inf}" ||
        die "${inx}: stampinf tokens this script does not fill"
}

# The file names an INF's [SourceDisksFiles] sections list.
inf_source_files() {
    tr -d '\r' <"$1" | awk '
        /^[ \t]*\[/ { section = tolower($0); next }
        section ~ /^[ \t]*\[sourcedisksfiles(\.[^]]*)?\]/ && $0 !~ /^[ \t]*(;|$)/ {
            name = $0
            sub(/[ \t]*=.*/, "", name)
            sub(/^[ \t]*/, "", name)
            print name
        }'
}

# The Mesa build a folder of Venus's (src/virtio/vulkan in it) belongs to.
mesa_build_of() {
    local dir
    dir=$(cd "$1" && pwd)
    while [[ "${dir}" != / && ! -f "${dir}/meson-info/meson-info.json" ]]; do
        dir=$(dirname "${dir}")
    done
    [[ "${dir}" != / ]] || die "$1 is not in a Mesa build folder (no meson-info above it)"
    printf '%s' "${dir}"
}

# check_icd_build <folder> <llvm-objdump format> <manifest>: a Venus build Windows can use from the package.
# Mesa writes .\vulkan_virtio.dll as library_path when configured with -Dvulkan-icd-dir=bin: the DLL next to the
# manifest, which is how the package installs them.
check_icd_build() {
    local dir=$1 format=$2 manifest="$1/$3"
    [[ -f "${dir}/${ICD_DLL}" && -f "${manifest}" ]] ||
        die "${PROJ}: its INF ships the Venus Vulkan driver, and ${dir} has no ${ICD_DLL} and $3; pass" \
            "--venus-icd, --venus-icd-arm64ec and --venus-icd-x86 with the src/virtio/vulkan folders of Mesa's builds"
    "$(llvm_tool llvm-objdump)" -f "${dir}/${ICD_DLL}" | grep -q "file format ${format}\$" ||
        die "${dir}/${ICD_DLL} is not ${format}"
    tr -d '\r' <"${manifest}" | grep -q '"library_path": *"\.\\\\vulkan_virtio\.dll"' ||
        die "${manifest}: library_path is not .\\vulkan_virtio.dll; configure Mesa with -Dvulkan-icd-dir=bin"
}

# link_icd <out> <resource object> <ICD folder> [<aarch64 ICD folder>]: the ICD build's link again, into <out>, with
# the version resource. With an aarch64 build, the ARM64X driver: -marm64x and the aarch64 build's object or library
# after each of the ARM64EC build's own, which lld puts in its view.
link_icd() {
    local out=$1 res=$2 build native="" target
    build=$(mesa_build_of "$3")
    target="$(cd "$3" && pwd)"
    target="${target#"${build}"/}/${ICD_DLL}"
    if [[ -n "${4:-}" ]]; then
        native=$(mesa_build_of "$4")
        [[ "$(cd "$4" && pwd)" == "${native}/${target%/*}" ]] || die "$4 and $3 are not the same folder of their builds"
    fi
    python3 -I - "$(command -v ninja)" "${target}" "${out}" "${res}" "${build}" ${native:+"${native}"} <<'EOF'
import os
import shlex
import subprocess
import sys

ninja, target, out, res, build = sys.argv[1:6]
native_build = sys.argv[6] if len(sys.argv) > 6 else None


def link_command(build):
    commands = subprocess.run([ninja, "-C", build, "-t", "commands", target], check=True, capture_output=True,
                              text=True).stdout.splitlines()
    return shlex.split(commands[-1])


own = link_command(build)
cmd = [own[0]]
builds = [build]
if native_build:
    native = link_command(native_build)
    if not own[0].endswith("arm64ec-w64-mingw32-clang++") or not native[0].endswith("aarch64-w64-mingw32-clang++"):
        sys.exit(f"build-macos: {build} is not an arm64ec-w64-mingw32 build or {native_build} not an "
                 "aarch64-w64-mingw32 one")
    if native[1:] != own[1:]:
        sys.exit(f"build-macos: {native_build} and {build} link {target} differently; configure both with the "
                 "same options")
    cmd.append("-marm64x")
    builds.append(native_build)
args = iter(own[1:])
for arg in args:
    if arg == "-o":
        next(args)
        cmd += ["-o", os.path.join(out, "vulkan_virtio.dll")]
    elif arg.startswith("-Wl,--out-implib="):
        cmd.append("-Wl,--out-implib=" + os.path.join(out, "vulkan_virtio.dll.a"))
    elif native_build and not arg.startswith("-") and not os.path.isabs(arg) and arg.endswith((".obj", ".o", ".a")):
        for each in builds:
            if not os.path.isfile(os.path.join(each, arg)):
                sys.exit(f"build-macos: no {arg} in {each}")
            cmd.append(os.path.join(each, arg))
    elif not arg.startswith("-") and not os.path.isabs(arg) and os.path.isfile(os.path.join(build, arg)):
        cmd.append(os.path.join(build, arg))
    else:
        cmd.append(arg)
subprocess.run(cmd + [res], cwd=build, check=True)
EOF
}

# One ARM64X Venus driver for ARM64 and x64 processes, and one for x86 processes, with their manifests. Each is linked
# again with a version resource: Mesa's builds have none.
pack_venus_icd() {
    local pack=$1 tmp
    command -v ninja >/dev/null || die "packing the Venus Vulkan driver needs ninja"
    check_icd_build "${VENUS_ICD_DIR}" coff-arm64 virtio_icd.aarch64.json
    check_icd_build "${VENUS_ICD_ARM64EC_DIR}" coff-arm64ec virtio_icd.aarch64.json
    check_icd_build "${VENUS_ICD_X86_DIR}" coff-i386 virtio_icd.i686.json
    tmp=$(mktemp -d)
    mkdir -p "${tmp}/arm64x" "${tmp}/x86"
    version_resource vulkan_virtio.rc arm64 ARM64 "${tmp}/arm64x/vulkan_virtio.res.o"
    version_resource vulkan_virtio.rc x86 x86 "${tmp}/x86/vulkan_virtio.res.o" VIOGPU3D_ICD_X86
    link_icd "${tmp}/arm64x" "${tmp}/arm64x/vulkan_virtio.res.o" "${VENUS_ICD_ARM64EC_DIR}" "${VENUS_ICD_DIR}" ||
        { rm -rf "${tmp}"; die "the ARM64X link of ${ICD_DLL} failed"; }
    link_icd "${tmp}/x86" "${tmp}/x86/vulkan_virtio.res.o" "${VENUS_ICD_X86_DIR}" ||
        { rm -rf "${tmp}"; die "the link of ${ICD_DLL_X86} failed"; }
    cp "${tmp}/arm64x/vulkan_virtio.dll" "${pack}/${ICD_DLL}"
    cp "${tmp}/x86/vulkan_virtio.dll" "${pack}/${ICD_DLL_X86}"
    rm -rf "${tmp}"
    cp "${VENUS_ICD_DIR}/virtio_icd.aarch64.json" "${pack}/vulkan_virtio.json"
    sed 's/"\.\\\\vulkan_virtio\.dll"/".\\\\vulkan_virtio_x86.dll"/' "${VENUS_ICD_X86_DIR}/virtio_icd.i686.json" \
        >"${pack}/vulkan_virtio_x86.json"
    note "packed the Venus Vulkan drivers of ${VENUS_ICD_DIR}, ${VENUS_ICD_ARM64EC_DIR} (one ARM64X DLL) and" \
        "${VENUS_ICD_X86_DIR}, version ${STAMPINF_VERSION}"
}

UMD_TRITON_SOURCES=(tritonDDI.c tritonDxbc.c tritonDxgi.c tritonHostCaps.c tritonPresent.c tritonQuery.c
    tritonResource.c tritonShader.c tritonState.c tritonView.c)
UMD_SOURCES=(umd_adapter_info.c umd_backend.c umd_format.c umd_main.c umd_path.c umd_pipeline.c
    umd_resource_record.c umd_vk.c umd_vk_chain.c)
# What DXVK's meson makes with -Dumd=true, all linked into the driver.
UMD_DXVK_LIBS=(src/d3d11/libd3d11_umd.a src/dxgi/libdxgi_umd.a src/dxvk/libdxvk.a src/spirv/libspirv.a
    src/util/libutil.a src/vulkan/libvkcommon.a src/wsi/libwsi.a subprojects/dxbc-spirv/libdxbc_spv.a
    subprojects/libdisplay-info/libdisplay-info.a)
# The native and ARM64EC views of the ARM64X driver, and the x86 driver.
UMD_ARCHES=(aarch64 arm64ec i686)

umd_out() {
    printf '%s/objfre_%s_arm64' "${UMD_DIR}" "$(printf '%s' "${TARGET_OS}" | tr '[:upper:]' '[:lower:]')"
}

# The display DDI headers mingw-w64 lacks, linked from the kits: the kits stay outside the tree.
umd_ddi_headers() {
    local dir=$1 h
    rm -rf "${dir}"
    mkdir -p "${dir}"
    for h in "${SDK_SHARED_INC}/d3dkmthk.h" "${SDK_SHARED_INC}/d3dukmdt.h" "${SDK_SHARED_INC}/d3dkmdt.h" \
        "${WDK_SHARED_INC}/d3dkmddi.h" "${WDK_UM_INC}/d3d10umddi.h" "${WDK_UM_INC}/d3dumddi.h" \
        "${UM_INC}/dxgiddi.h" "${UM_INC}/dxmini.h" "${WDK_UM_INC}/d3d10TokenizedProgramFormat.hpp" \
        "${WDK_UM_INC}/d3d11TokenizedProgramFormat.hpp" "${WDK_UM_INC}/d3d12TokenizedProgramFormat.hpp"; do
        [[ -f "${h}" ]] || die "no ${h} in ${WDK_DIR}; run Tools/build-macos.sh fetch"
        ln -s "${h}" "${dir}/"
    done
}

# The submodule, with the patches until DXVK has the umd option. A file the patches leave as it was keeps its time, so
# ninja rebuilds only what changed.
umd_dxvk_source() {
    local out=$1 sub="${UMD_DIR}/dxvk" patch patches
    [[ -f "${sub}/meson.build" && -f "${sub}/include/vulkan/include/vulkan/vulkan_core.h" ]] ||
        die "no DXVK in ${sub}; run git submodule update --init --recursive viogpu3d/umd/dxvk"
    mkdir -p "${out}/dxvk-stage" "${out}/dxvk-src"
    rsync -a --delete --exclude .git "${sub}/" "${out}/dxvk-stage/"
    if ! grep -q "option('umd'" "${out}/dxvk-stage/meson_options.txt"; then
        patches=("${DXVK_PATCHES}"/*.patch)
        [[ -f "${patches[0]}" ]] ||
            die "the DXVK in ${sub} has no umd meson option and ${DXVK_PATCHES} has no *.patch to add it;" \
                "pass --dxvk-patches with the patches"
        for patch in "${patches[@]}"; do
            patch -s -p1 -d "${out}/dxvk-stage" <"${patch}" || die "${patch} does not apply to ${sub}"
            note "applied $(basename "${patch}") to DXVK"
        done
    fi
    rsync -rlp --checksum --delete "${out}/dxvk-stage/" "${out}/dxvk-src/"
}

# umd_dxvk_build <out> <llvm-mingw bin> <arch>: DXVK's static libraries for one target, in <out>/<arch>/dxvk-build. A
# changed cross file (llvm-mingw or glslang moved) sets DXVK up from scratch: a reconfigure keeps the old compilers.
umd_dxvk_build() {
    local dir="$1/$3" mingw=$2 arch=$3 triple="$3-w64-mingw32" family=aarch64 cross glslang
    cross="${dir}/cross.txt"
    { command -v meson && command -v ninja; } >/dev/null || die "--d3d-umd needs meson and ninja"
    glslang=$(command -v glslang || command -v glslangValidator || true)
    [[ -n "${glslang}" ]] || die "--d3d-umd needs glslang for DXVK's shaders (brew install glslang)"
    # ARM64EC code is ARM64 code; DXVK's meson tells only x86 apart.
    [[ "${arch}" != i686 ]] || family=x86
    mkdir -p "${dir}"
    cat >"${cross}.new" <<EOF
[binaries]
c = '${mingw}/${triple}-clang'
cpp = '${mingw}/${triple}-clang++'
ar = '${mingw}/${triple}-llvm-ar'
strip = '${mingw}/${triple}-strip'
windres = '${mingw}/${triple}-windres'
pkg-config = '/usr/bin/false'
glslang = '${glslang}'

[host_machine]
system = 'windows'
cpu_family = '${family}'
cpu = '${arch}'
endian = 'little'
EOF
    if cmp -s "${cross}.new" "${cross}"; then
        rm "${cross}.new"
    else
        rm -rf "${dir}/dxvk-build"
        mv "${cross}.new" "${cross}"
    fi
    if [[ ! -f "${dir}/dxvk-build/build.ninja" ]]; then
        meson setup "${dir}/dxvk-build" "$1/dxvk-src" --cross-file "${cross}" --buildtype=release \
            -Db_ndebug=true -Dumd=true >"${dir}/dxvk-setup.log" ||
            die "DXVK's meson setup for ${triple} failed; see ${dir}/dxvk-setup.log"
    fi
    ninja -C "${dir}/dxvk-build" || die "DXVK did not build for ${triple}"
    local lib
    for lib in "${UMD_DXVK_LIBS[@]}"; do
        [[ -f "${dir}/dxvk-build/${lib}" ]] || die "DXVK built no ${lib} for ${triple}"
    done
}

# umd_compile <out> <llvm-mingw bin> <arch>: Triton into <out>/<arch>/libtriton.a and the driver's objects into
# <out>/<arch>/obj.
umd_compile() {
    local out=$1 dir="$1/$3" cc="$2/$3-w64-mingw32-clang" src cflags triton=() undefined
    mkdir -p "${dir}/obj"
    # d3d10umddi.h defines its D3D10 constants in every C file that includes it; d3d10.h has them already. The kits'
    # headers are system headers: on x86, d3dkmthk.h declares a stdcall function without a prototype.
    cflags=(-std=gnu11 -O2 -g -gcodeview -Wall -Wno-unknown-pragmas -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
        -D_D3D10_CONSTANTS -D_D3D10_1_CONSTANTS -isystem "${out}/winddk" "-I${UMD_DIR}/triton")
    for src in "${UMD_TRITON_SOURCES[@]}"; do
        "${cc}" "${cflags[@]}" -c "${UMD_DIR}/triton/${src}" -o "${dir}/obj/${src%.c}.o"
        triton+=("${dir}/obj/${src%.c}.o")
    done
    rm -f "${dir}/libtriton.a"
    "$2/llvm-ar" rcs "${dir}/libtriton.a" "${triton[@]}"
    undefined=$("$2/llvm-nm" -u "${dir}/libtriton.a" | grep -E '(^|[^A-Za-z0-9_])npt_' || true)
    [[ -z "${undefined}" ]] || die "Triton still calls Neptune: ${undefined}"
    "${cc}" "${cflags[@]}" -c "${UMD_DIR}/triton/triton_backend_null.c" -o "${dir}/obj/triton_backend_null.o"
    for src in "${UMD_SOURCES[@]}"; do
        "${cc}" "${cflags[@]}" -Wextra -Werror "-I${UMD_DIR}/src" "-I${out}/dxvk-src/src/d3d11" \
            "-I${out}/dxvk-src/include/vulkan/include" -c "${UMD_DIR}/src/${src}" -o "${dir}/obj/${src%.c}.o"
    done
}

# umd_inputs <out> <arch>...: the driver's objects, Triton and DXVK for those targets, in link order.
umd_inputs() {
    local out=$1 arch src lib
    shift
    for arch in "$@"; do
        for src in "${UMD_SOURCES[@]}"; do
            printf '%s\n' "${out}/${arch}/obj/${src%.c}.o"
        done
    done
    for arch in "$@"; do
        printf '%s\n' "${out}/${arch}/libtriton.a"
    done
    for arch in "$@"; do
        for lib in "${UMD_DXVK_LIBS[@]}"; do
            printf '%s\n' "${out}/${arch}/dxvk-build/${lib}"
        done
    done
}

# version_resource <rc file in viogpu3d/umd> <machine> <VER_ARCH> <object> [rc define]: a DLL's version resource, with
# the defines of viogpu3d.sys's.
version_resource() {
    local rc=$1 machine=$2 ver_arch=$3 object=$4 flags=() flag
    [[ -x "${MINGW_DIR}/bin/llvm-cvtres" ]] || die "no llvm-mingw in ${MINGW_DIR}; run Tools/build-macos.sh fetch"
    rc_flags
    for flag in "${RCFLAGS[@]}"; do
        [[ "${flag}" == /DVER_ARCH=* ]] && flag="/DVER_ARCH=${ver_arch}"
        flags+=("${flag}")
    done
    [[ -z "${5:-}" ]] || flags+=("/D$5")
    (cd "${UMD_DIR}" && "${LLVM_RC}" "${flags[@]}" "/I${UM_INC}" "/I${SDK_SHARED_INC}" "/I${WDK_SHARED_INC}" \
        /FO "${object%.o}" "${rc}")
    "${MINGW_DIR}/bin/llvm-cvtres" "/machine:${machine}" "/out:${object}" "${object%.o}" >/dev/null
}

# viogpu3d's Direct3D user-mode drivers: Triton's DDI translation, the driver's backend and DXVK in one DLL, built for
# three targets. The aarch64 and arm64ec builds link into one ARM64X DLL: ARM64 processes load its native view, x64
# processes under emulation its ARM64EC view. x86 processes load the i686 DLL.
build_d3d_umd() {
    local mingw="${MINGW_DIR}/bin" out arch inputs=() inputs_x86=() line
    for arch in "${UMD_ARCHES[@]}"; do
        [[ -x "${mingw}/${arch}-w64-mingw32-clang" ]] ||
            die "--d3d-umd needs llvm-mingw in ${MINGW_DIR}; run Tools/build-macos.sh fetch"
    done
    out=$(umd_out)
    mkdir -p "${out}"

    note "${UMD_DLL}, ${UMD_DLL_X86}: DXVK sources"
    umd_dxvk_source "${out}"
    umd_ddi_headers "${out}/winddk"
    for arch in "${UMD_ARCHES[@]}"; do
        note "DXVK, Triton and the driver for ${arch}-w64-mingw32"
        umd_dxvk_build "${out}" "${mingw}" "${arch}"
        umd_compile "${out}" "${mingw}" "${arch}"
    done

    rm -f "${out}/triton_null.dll" "${out}/triton_null_x86.dll"
    "${mingw}/arm64ec-w64-mingw32-clang" -marm64x -shared -o "${out}/triton_null.dll" \
        "${UMD_DIR}/triton/triton_null.def" "${out}/aarch64/obj/triton_backend_null.o" \
        "${out}/arm64ec/obj/triton_backend_null.o" "${out}/aarch64/libtriton.a" "${out}/arm64ec/libtriton.a" \
        -static -ldxguid
    "${mingw}/i686-w64-mingw32-clang" -shared -o "${out}/triton_null_x86.dll" "${UMD_DIR}/triton/triton_null.def" \
        "${out}/i686/obj/triton_backend_null.o" "${out}/i686/libtriton.a" -static -ldxguid
    note "Triton links with the null backend: ${out#"${REPO}"/}/triton_null.dll, triton_null_x86.dll"

    copyright_years "$(prop "${REPO}/viogpu3d/viogpu3d/viogpu3d.props" CopyrightStrings)"
    version_resource viogpu3d_umd.rc arm64 ARM64 "${out}/aarch64/obj/viogpu3d_umd.res.o"
    version_resource viogpu3d_umd.rc x86 x86 "${out}/i686/obj/viogpu3d_umd.res.o" VIOGPU3D_UMD_X86

    while IFS= read -r line; do
        inputs+=("${line}")
    done < <(umd_inputs "${out}" aarch64 arm64ec)
    while IFS= read -r line; do
        inputs_x86+=("${line}")
    done < <(umd_inputs "${out}" i686)
    rm -f "${out}/${UMD_DLL}" "${out}/${UMD_DLL_X86}"
    "${mingw}/arm64ec-w64-mingw32-clang++" -marm64x -shared -g -o "${out}/${UMD_DLL}" "${UMD_DIR}/viogpu3d_umd.def" \
        "${out}/aarch64/obj/viogpu3d_umd.res.o" "${inputs[@]}" -static -ldxguid -lgdi32 -lversion \
        "-Wl,--pdb=${out}/viogpu3d_umd.pdb"
    "${mingw}/i686-w64-mingw32-clang++" -shared -g -o "${out}/${UMD_DLL_X86}" "${UMD_DIR}/viogpu3d_umd.def" \
        "${out}/i686/obj/viogpu3d_umd.res.o" "${inputs_x86[@]}" -static -ldxguid -lgdi32 -lversion \
        "-Wl,--pdb=${out}/viogpu3d_umd_x86.pdb"
    note "built ${out#"${REPO}"/}/${UMD_DLL} (ARM64X) and ${UMD_DLL_X86}, version ${STAMPINF_VERSION}"
}

pack_d3d_umd() {
    local pack=$1 out dll
    out=$(umd_out)
    for dll in "${UMD_DLL}" "${UMD_DLL_X86}"; do
        [[ -f "${out}/${dll}" ]] ||
            die "${PROJ}: its INF ships the Direct3D drivers, and ${out#"${REPO}"/} has no ${dll}; pass --d3d-umd"
        cp "${out}/${dll}" "${pack}/"
        [[ ! -f "${out}/${dll%.dll}.pdb" ]] || cp "${out}/${dll%.dll}.pdb" "${pack}/"
    done
    note "packed the Direct3D drivers of ${out#"${REPO}"/}"
}

# mesa_notices <Mesa build>...: the licences of the Mesa code in the Venus drivers. Every Mesa file (source, header or
# generated) that ninja's dependency records list for an object the driver links: its SPDX identifier, else the licence
# its text states, else its subproject's licence file, else Mesa's (MIT); and its copyright lines.
mesa_notices() {
    python3 -I - "$(command -v ninja)" "${ICD_DLL}" "$@" <<'EOF'
import json
import os
import re
import subprocess
import sys

ninja, icd, builds = sys.argv[1], sys.argv[2], sys.argv[3:]
SPDX = re.compile(r"SPDX-License-Identifier:\s*([A-Za-z0-9.+\- ()]+)")
COPYRIGHT = re.compile(r"\bCopyright\b\s*(?::\s*)?(?:\(c\)|©)?\s*(?::\s*)?(?=\d{4}|©|\(c\))", re.I)
COMMENT = re.compile(r"^\s*(?:/\*+|\*+/|\*+|//+|#+|;+)?\s?")
STATED = [
    ("Permission is hereby granted, free of charge", "MIT"),
    ("License: MIT", "MIT"),
    ("Boost Software License", "BSL-1.0"),
    ("Licensed under the Apache License", "Apache-2.0"),
    ("SGI Free Software B License", "SGI-B-2.0"),
]
# BLAKE3's own licence (github.com/BLAKE3-team/BLAKE3), which Mesa's copy does not repeat.
BLAKE3 = "CC0-1.0 OR Apache-2.0"

src = None
files = {}
for build in builds:
    info = json.load(open(os.path.join(build, "meson-info", "meson-info.json")))
    tree = os.path.realpath(info["directories"]["source"])
    if src not in (None, tree):
        sys.exit(f"build-macos: the Venus builds come from different Mesa trees: {src}, {tree}")
    src = tree
    root = os.path.realpath(build)
    vulkan = os.path.join(root, "src", "virtio", "vulkan", icd)
    target = os.path.relpath(vulkan, root)
    inputs = subprocess.run([ninja, "-C", build, "-t", "inputs", target], check=True, capture_output=True,
                            text=True).stdout.split()
    objects = [i for i in inputs if i.endswith((".obj", ".o"))]
    deps = subprocess.run([ninja, "-C", build, "-t", "deps", *objects], check=True, capture_output=True,
                          text=True).stdout
    for line in deps.splitlines():
        if line.startswith(" ") and line.strip():
            path = os.path.realpath(os.path.join(root, line.strip()))
            if path.startswith(src + os.sep):
                files[path] = os.path.relpath(path, src)
            elif path.startswith(root + os.sep):
                files[path] = os.path.relpath(path, root)

licences = os.path.join(src, "licenses")


def choose(expression):
    """The licences to follow for an SPDX expression: MIT of an OR, else its first one Mesa has the text of."""
    expression = expression.strip().strip("()")
    if " OR " in expression:
        options = [o.strip().strip("()") for o in expression.split(" OR ")]
        for option in ["MIT"] + options:
            if option in options and os.path.isfile(os.path.join(licences, option.split(" WITH ")[0])):
                return choose(option)
        return choose(options[0])
    ids = []
    for part in expression.split(" AND "):
        ids += [p.strip() for p in part.split(" WITH ")]
    return ids


used = {}
texts = {}
notices = set()
for path, rel in sorted(files.items(), key=lambda f: f[1]):
    try:
        head = open(path, encoding="utf-8", errors="replace").read(16384)
    except OSError as error:
        sys.exit(f"build-macos: {error}")
    match = SPDX.search(head)
    ids = None
    if match:
        ids = choose(match.group(1))
    else:
        for phrase, licence in STATED:
            if phrase in head:
                ids = [licence]
                break
    if ids is None and "Redistribution and use in source and binary forms" in head:
        block = re.search(r"/\*(.*?Redistribution and use in source and binary forms.*?)\*/", head, re.S)
        if block is None:
            sys.exit(f"build-macos: {rel}: no comment with its licence")
        texts[rel] = "\n".join(COMMENT.sub("", l).rstrip() for l in block.group(1).splitlines()).strip()
        ids = []
    if ids is None and "No copyright claimed" in head:
        ids = []
    if ids is None and rel.startswith("subprojects" + os.sep):
        project = os.path.join(src, *rel.split(os.sep)[:2])
        found = [f for f in sorted(os.listdir(project)) if f.upper().startswith(("LICENSE", "COPYING"))]
        if not found:
            sys.exit(f"build-macos: {rel} states no licence and {project} has no licence file")
        texts[os.path.relpath(os.path.join(project, found[0]), src)] = open(os.path.join(project, found[0]),
                                                                            encoding="utf-8").read().strip()
        ids = []
    if ids is None and rel.startswith(os.path.join("src", "util", "blake3") + os.sep):
        ids = choose(BLAKE3)
    if ids is None:
        ids = ["MIT"]
    for licence in ids:
        used.setdefault(licence, 0)
        used[licence] += 1
    lines = [COMMENT.sub("", l).strip() for l in head.splitlines()]
    for i, line in enumerate(lines):
        if COPYRIGHT.search(line):
            notice = line[COPYRIGHT.search(line).start():]
            for more in lines[i + 1:i + 3]:
                if not more or COPYRIGHT.search(more) or re.match(r"(Permission|Redistribution|All rights|SPDX|"
                                                                  r"Licensed|This |Use of|The |See |Author)", more,
                                                                  re.I):
                    break
                notice += " " + more
            notice = re.sub(r"\s+", " ", notice.rstrip(" */")).strip()
            notices.add(notice)

print(f"== Mesa, https://gitlab.freedesktop.org/mesa/mesa, changed: the Venus Vulkan drivers ==")
print()
print(f"The Mesa files compiled into the Venus Vulkan drivers ({len(files)} sources, headers and generated files)")
print("carry these copyright notices, under the licences that follow: each file's SPDX identifier, or the licence")
print("its text states, says which; a file that states none is under Mesa's licence, MIT.")
print()
for notice in sorted(notices):
    print(notice)
for licence in sorted(used):
    path = os.path.join(licences, licence)
    if not os.path.isfile(path):
        path = os.path.join(licences, "exceptions", licence)
    if not os.path.isfile(path):
        sys.exit(f"build-macos: Mesa has no text of {licence} in {licences}")
    text = open(path, encoding="utf-8").read()
    text = text.split("License-Text:", 1)[1] if "License-Text:" in text else text
    print(f"\n-- {licence} (licenses/{os.path.relpath(path, licences)}) --\n")
    print(text.strip())
for rel, text in sorted(texts.items()):
    print(f"\n-- The licence of {rel}, as it states it --\n")
    print(text)
EOF
}

# The llvm-mingw a Venus build linked its ICD with.
icd_toolchain() {
    local build target compiler
    build=$(mesa_build_of "$1")
    target="$(cd "$1" && pwd)"
    compiler=$(ninja -C "${build}" -t commands "${target#"${build}"/}/${ICD_DLL}" | tail -1 | cut -d' ' -f1)
    [[ "${compiler}" == */bin/*-w64-mingw32-clang++ ]] ||
        die "${build} does not link ${ICD_DLL} with an llvm-mingw clang++: ${compiler}"
    dirname "$(dirname "${compiler}")"
}

# toolchain_notices <llvm-mingw>...: the licences each llvm-mingw ships for the runtime code every DLL links statically,
# once per distinct text.
toolchain_notices() {
    local root file known seen=()
    printf '\n== The llvm-mingw runtime, https://github.com/mstorsjo/llvm-mingw: %s ==\n\n' \
        "${UMD_DLL}, ${UMD_DLL_X86}, ${ICD_DLL}, ${ICD_DLL_X86}"
    echo "Each DLL links parts of its toolchain's runtime statically: mingw-w64's startup code, libmingw32 and"
    echo "libmingwex (gdtoa among it), and LLVM's libc++, libc++abi, libunwind and compiler-rt builtins. Their"
    echo "licences, as llvm-mingw ships them:"
    for root in "$@"; do
        for file in "${root}/aarch64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt" \
            "${root}/i686-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt" "${root}/LICENSE.TXT"; do
            [[ -f "${file}" ]] || die "no ${file#"${root}"/} in llvm-mingw ${root}"
            for known in "${seen[@]+"${seen[@]}"}"; do
                if cmp -s "${known}" "${file}"; then
                    continue 2
                fi
            done
            seen+=("${file}")
            printf '\n-- %s --\n\n' "${file#"${root}"/}"
            cat "${file}"
        done
    done
}

# The licences of the third-party code in the package's user-mode drivers, in full: MIT asks every copy to carry its
# notice.
write_notices() {
    local pack=$1 out src licence name toolchains=("${MINGW_DIR}") dir root
    out=$(umd_out)
    src="${out}/dxvk-src"
    [[ -f "${src}/LICENSE" ]] || die "${PROJ}: no DXVK copy in ${out#"${REPO}"/} for the notices; pass --d3d-umd"
    for dir in "${VENUS_ICD_DIR}" "${VENUS_ICD_ARM64EC_DIR}" "${VENUS_ICD_X86_DIR}"; do
        root=$(icd_toolchain "${dir}")
        toolchains+=("${root}")
    done
    {
        echo "${UMD_DLL}, ${UMD_DLL_X86}, ${ICD_DLL} and ${ICD_DLL_X86} contain the code below, each part under"
        echo "its own licence."
        printf '\n== Triton, src/virtio/neptune/triton of https://github.com/osy/virtio-win-mesa, changed: %s ==\n\n' \
            "${UMD_DLL}, ${UMD_DLL_X86}"
        cat "${UMD_DIR}/triton/LICENSE"
        printf '\n== DXVK, https://github.com/anonymix007/dxvk branch umd, changed: %s ==\n\n' \
            "${UMD_DLL}, ${UMD_DLL_X86}"
        cat "${src}/LICENSE"
        for licence in "${src}"/subprojects/*/LICENSE; do
            [[ -f "${licence}" ]] || die "DXVK has no subprojects/*/LICENSE"
            name=$(basename "$(dirname "${licence}")")
            printf '\n== %s, linked in by DXVK (subprojects/%s): %s ==\n\n' "${name}" "${name}" \
                "${UMD_DLL}, ${UMD_DLL_X86}"
            cat "${licence}"
        done
        toolchain_notices "${toolchains[@]}"
        echo
        mesa_notices "$(mesa_build_of "${VENUS_ICD_DIR}")" "$(mesa_build_of "${VENUS_ICD_ARM64EC_DIR}")" \
            "$(mesa_build_of "${VENUS_ICD_X86_DIR}")" || die "the Mesa notices failed"
    } >"${pack}/${UMD_NOTICES}" || die "${PROJ}: could not write ${UMD_NOTICES}"
    note "wrote ${UMD_NOTICES}"
}

# Files of a package that come from other builds, then a check that the package has every file its INF lists.
pack_inf_files() {
    local pack=$1 inf=$2 file mesa_src
    if inf_source_files "${inf}" | grep -q -x -i "${ICD_DLL//./\\.}"; then
        pack_venus_icd "${pack}"
    fi
    if inf_source_files "${inf}" | grep -q -x -i "${UMD_DLL//./\\.}"; then
        pack_d3d_umd "${pack}"
    fi
    if inf_source_files "${inf}" | grep -q -x -i "${UMD_NOTICES//./\\.}"; then
        write_notices "${pack}"
    fi
    while IFS= read -r file; do
        [[ -f "${pack}/${file}" ]] || die "$(basename "${inf}") lists ${file}, which the package lacks"
    done < <(inf_source_files "${inf}")
    if inf_source_files "${inf}" | grep -q -x -i "${UMD_DLL//./\\.}"; then
        mesa_src=$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["directories"]["source"])' \
            "$(mesa_build_of "${VENUS_ICD_DIR}")/meson-info/meson-info.json")
        LLVM_OBJDUMP="$(llvm_tool llvm-objdump)" LLVM_READOBJ="$(llvm_tool llvm-readobj)" MESA_SRC="${mesa_src}" \
            MINGW_DIR="${MINGW_DIR}" sh "${UMD_DIR}/tests/check-package.sh" "${pack}" || die "the package check of ${pack#"${REPO}"/} failed"
    fi
}

build_project() {
    local PROJ PROJ_DIR
    [[ -f "$1" ]] || die "no project $1"
    PROJ_DIR=$(cd "$(dirname "$1")" && pwd)
    PROJ="${PROJ_DIR}/$(basename "$1")"

    local type target_name target_version driver_type debug allow_date_time
    type=$(prop "${PROJ}" ConfigurationType)
    target_name=$(prop "${PROJ}" TargetName)
    [[ -n "${target_name}" ]] || target_name=$(basename "${PROJ}" .vcxproj)
    target_version=$(prop "${PROJ}" TargetVersion)
    driver_type=$(prop "${PROJ}" DriverType)
    debug=$(prop "${PROJ}" UseDebugLibraries)
    allow_date_time=$(prop "${PROJ}" ALLOW_DATE_TIME)
    [[ "$(prop "${PROJ}" PlatformToolset)" == WindowsKernelModeDriver10.0 ]] ||
        die "${PROJ}: has no ${CFG_MATCH} kernel-mode configuration"
    [[ "${target_version}" == Windows10 ]] || die "${PROJ}: TargetVersion ${target_version} is not supported"
    [[ "${debug}" != true ]] || die "${PROJ}: ${CFG_MATCH} uses debug libraries, which this script does not build"

    local INTDIR_REL OUTDIR_REL INTDIR OUTDIR
    INTDIR_REL=$(prop "${PROJ}" IntDir)
    OUTDIR_REL=$(prop "${PROJ}" OutDir)
    [[ -n "${INTDIR_REL}" && -n "${OUTDIR_REL}" ]] || die "${PROJ}: no IntDir or OutDir for ${CFG_MATCH}"
    INTDIR=$(abs_path "${INTDIR_REL//\\//}")
    OUTDIR=$(abs_path "${OUTDIR_REL//\\//}")
    mkdir -p "${INTDIR}" "${OUTDIR}"

    local libdirs_raw dep_lib dir sub
    libdirs_raw=$(meta "${PROJ}" Link AdditionalLibraryDirectories)
    for dep_lib in $(split "$(meta "${PROJ}" Link AdditionalDependencies)" | grep -v -E '^[$%]|/|\\' || true); do
        while IFS= read -r dir; do
            dir=${dir//'$(OutDir)'/}
            [[ "${dir}" != *'$('* ]] || continue
            dir=$(abs_path "${dir//\\//}")
            for sub in "${dir}"/*.vcxproj; do
                [[ -f "${sub}" ]] || continue
                [[ "$(prop "${sub}" TargetName).lib" == "${dep_lib}" &&
                    "$(prop "${sub}" ConfigurationType)" == StaticLibrary ]] || continue
                (build_project "${sub}")
            done
        done < <(split "${libdirs_raw}")
    done

    note "${CFG_MATCH} ${PROJ#"${REPO}"/}"

    local copyright_strings=""
    [[ ! -f "${PROJ_DIR}/${target_name}.props" ]] ||
        copyright_strings=$(prop "${PROJ_DIR}/${target_name}.props" CopyrightStrings)
    copyright_years "${copyright_strings}"

    SOURCES=()
    local src
    while IFS= read -r src; do
        SOURCES+=("$(abs_path "${src//\\//}")")
    done < <(items "${PROJ}" ClCompile)
    [[ ${#SOURCES[@]} -gt 0 ]] || die "${PROJ}: no sources"

    local includes=() defines=() line
    while IFS= read -r line; do
        includes+=("/I$(abs_path "${line}")")
    done < <(split "$(expand "$(meta "${PROJ}" ClCompile AdditionalIncludeDirectories)")")
    while IFS= read -r line; do
        defines+=("-D${line}")
    done < <(split "$(expand "$(meta "${PROJ}" ClCompile PreprocessorDefinitions)")")

    if [[ "$(meta "${PROJ}" ClCompile WppEnabled)" == *true* ]]; then
        local wpp_config
        wpp_config=$(split "$(meta "${PROJ}" ClCompile WppScanConfigurationData)" | tail -1)
        write_wpp_stubs "$(abs_path "${wpp_config//\\//}")"
    fi

    compile_flags "${allow_date_time}"
    local objs=() obj
    for src in "${SOURCES[@]}"; do
        obj="${INTDIR}/$(basename "${src%.*}").obj"
        (cd "${PROJ_DIR}" && "${CLANG}" "${CFLAGS[@]}" "${defines[@]+"${defines[@]}"}" \
            "/I${INTDIR}" "${includes[@]+"${includes[@]}"}" \
            /imsvc "${CRT_INC}" /imsvc "${KM_INC}" /imsvc "${SDK_SHARED_INC}" /imsvc "${WDK_SHARED_INC}" \
            "/Fo${obj}" -- "${src}")
        objs+=("${obj}")
    done

    if [[ "${type}" == StaticLibrary ]]; then
        rm -f "${OUTDIR}/${target_name}.lib"
        "${LLVM_LIB}" /nologo /machine:arm64 "/out:${OUTDIR}/${target_name}.lib" "${objs[@]}"
        return
    fi
    [[ "${type}" == Driver ]] || die "${PROJ}: ConfigurationType ${type} is not supported"
    [[ "${driver_type}" == WDM ]] || die "${PROJ}: DriverType ${driver_type} is not supported"

    local rc res
    rc_flags
    while IFS= read -r rc; do
        rc=$(abs_path "${rc//\\//}")
        res="${INTDIR}/$(basename "${rc%.*}").res"
        if [[ "$(head -c 2 "${rc}" | od -An -tx1 | tr -d ' ')" == fffe ]]; then
            iconv -f UTF-16 -t UTF-8 "${rc}"
        else
            cat "${rc}"
        fi | sed -E '/^[[:space:]]*#[[:space:]]*include/ s#\\#/#g' >"${INTDIR}/$(basename "${rc}")"
        (cd "${INTDIR}" && "${LLVM_RC}" "${RCFLAGS[@]}" "/I${PROJ_DIR}" "/I${INTDIR}" "/I${UM_INC}" \
            "/I${CRT_INC}" "/I${KM_INC}" "/I${SDK_SHARED_INC}" "/I${WDK_SHARED_INC}" \
            /FO "${res}" "$(basename "${rc}")")
        objs+=("${res}")
    done < <(items "${PROJ}" ResourceCompile)

    # The toolset's own libraries in its order: arm64rt.lib (WindowsDriver.arm64.props), then
    # WindowsDriver.KernelMode.Wdm.props', then the project's.
    local libs=("${SDK_LIB}/arm64rt.lib" "${DDK_LIB}/BufferOverflowFastFailK.lib" "${DDK_LIB}/ntoskrnl.lib"
        "${DDK_LIB}/hal.lib" "${DDK_LIB}/wmilib.lib")
    while IFS= read -r line; do
        libs+=("${line}")
    done < <(split "$(expand "$(meta "${PROJ}" Link AdditionalDependencies)")")
    local libpaths=()
    while IFS= read -r line; do
        libpaths+=("/libpath:$(abs_path "${line}")")
    done < <(split "$(expand "${libdirs_raw}")")

    local sys="${OUTDIR}/${target_name}.sys"
    rm -f "${sys}"
    "${LLD_LINK}" /nologo /machine:arm64 /driver /subsystem:native,10.00 /entry:GsDriverEntry /nodefaultlib \
        /merge:_TEXT=.text /merge:_PAGE=PAGE \
        /section:.text,ERP /section:.rdata,RP /section:.data,RWP /section:.pdata,RP /section:INIT,DER \
        /section:.rsrc,DR /opt:ref /opt:icf /release /version:10.0 /osversion:10.0 /stack:0x40000,0x2000 \
        /tsaware:no /guard:cf /debug "/pdb:${OUTDIR}/${target_name}.pdb" '/pdbaltpath:%_PDB%' \
        "/out:${sys}" "${libpaths[@]+"${libpaths[@]}"}" "${objs[@]}" "${libs[@]}"

    local inx inf
    inx=$(items "${PROJ}" Inf | head -1)
    [[ -n "${inx}" ]] || die "${PROJ}: no Inf item"
    inx=$(abs_path "${inx//\\//}")
    inf="${OUTDIR}/$(basename "${inx%.*}").inf"
    stamp_inf "${inx}" "${inf}"

    local pack=""
    if [[ -f "${PROJ_DIR}/${target_name}.props" &&
        "$(prop "${PROJ_DIR}/${target_name}.props" Feature_PackOne)" == true ]]; then
        local prefix
        prefix=$(prop "${PROJ_DIR}/${target_name}.props" PackOne_DestinationPrefix)
        pack=$(abs_path "${prefix//\\//}")
        pack="$(cd "${pack}" && pwd)/Install/${TARGET_OS}/ARM64"
        rm -rf "${pack}"
        mkdir -p "${pack}"
        cp "${sys}" "${OUTDIR}/${target_name}.pdb" "${inf}" "${pack}/"
        pack_inf_files "${pack}" "${pack}/$(basename "${inf}")"
        PACKAGES+=("${pack}")
    fi
    note "built ${sys#"${REPO}"/}${pack:+, packed in ${pack#"${REPO}"/}}"
}

openssl_tool() {
    local candidate
    for candidate in "$(brew --prefix openssl@3 2>/dev/null)/bin/openssl" "$(command -v openssl || true)"; do
        if [[ -x "${candidate}" ]] && "${candidate}" version | grep -q '^OpenSSL 3'; then
            echo "${candidate}"
            return
        fi
    done
    die "OpenSSL 3 not found (brew install openssl@3)"
}

# A self-signed code signing certificate in build/, never in a keychain.
make_test_cert() {
    local openssl=$1 pfx=$2 cer=$3 tmp
    [[ -f "${pfx}" && -f "${cer}" ]] && return
    tmp=$(mktemp -d)
    "${openssl}" req -x509 -newkey rsa:3072 -sha256 -days 3650 -nodes \
        -subj "${TEST_CERT_SUBJECT:-/CN=OSMates GPU Test}" \
        -addext "basicConstraints=critical,CA:FALSE" -addext "keyUsage=critical,digitalSignature" \
        -addext "extendedKeyUsage=codeSigning" \
        -keyout "${tmp}/key.pem" -out "${tmp}/cert.pem" 2>/dev/null
    "${openssl}" pkcs12 -export -inkey "${tmp}/key.pem" -in "${tmp}/cert.pem" -out "${pfx}" -passout pass:
    "${openssl}" x509 -in "${tmp}/cert.pem" -outform der -out "${cer}"
    rm -rf "${tmp}"
    note "made test certificate ${cer#"${REPO}"/}"
}

test_sign() {
    local openssl pfx="${REPO}/build/TestCert.pfx" cer="${REPO}/build/TestCert.cer" pem pack sys
    command -v osslsigncode >/dev/null || die "osslsigncode not found (brew install osslsigncode)"
    openssl=$(openssl_tool)
    make_test_cert "${openssl}" "${pfx}" "${cer}"
    pem=$(mktemp)
    "${openssl}" x509 -inform der -in "${cer}" -out "${pem}"
    for pack in "$@"; do
        for sys in "${pack}"/*.sys; do
            osslsigncode sign -pkcs12 "${pfx}" -pass "" -h sha256 -in "${sys}" -out "${sys}.signed" >/dev/null
            mv "${sys}.signed" "${sys}"
            osslsigncode verify -CAfile "${pem}" -in "${sys}" >/dev/null || die "${sys}: signature does not verify"
            note "test-signed ${sys#"${REPO}"/}"
        done
    done
    rm -f "${pem}"
}

main() {
    CONFIG="Win11 Release"
    local sign=false d3d_umd=false projects=() project
    while [[ $# -gt 0 ]]; do
        case "$1" in
            fetch)
                fetch
                return
                ;;
            --config)
                CONFIG=${2:?}
                shift
                ;;
            --test-sign) sign=true ;;
            --venus-icd)
                VENUS_ICD_DIR=${2:?}
                shift
                ;;
            --venus-icd-arm64ec)
                VENUS_ICD_ARM64EC_DIR=${2:?}
                shift
                ;;
            --venus-icd-x86)
                VENUS_ICD_X86_DIR=${2:?}
                shift
                ;;
            --d3d-umd) d3d_umd=true ;;
            --dxvk-patches)
                DXVK_PATCHES=${2:?}
                shift
                ;;
            -h | --help)
                usage
                return
                ;;
            -*) die "unknown option $1" ;;
            *) projects+=("$1") ;;
        esac
        shift
    done
    if [[ ${#projects[@]} -eq 0 ]]; then
        for project in "${DEFAULT_PROJECTS[@]}"; do
            projects+=("${REPO}/${project}")
        done
    fi
    command -v xmllint >/dev/null || die "xmllint not found"

    setup_kits
    setup_config
    if [[ "${d3d_umd}" == true ]]; then
        build_d3d_umd
    fi
    PACKAGES=()
    for project in "${projects[@]}"; do
        build_project "${project}"
    done
    if [[ "${sign}" == true && ${#PACKAGES[@]} -gt 0 ]]; then
        test_sign "${PACKAGES[@]}"
    fi
    note "not done here: inf2cat (the package has no .cat), tracewpp (.tmh files call DbgPrintEx)"
}

main "$@"
