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

KIT_VERSION="10.0.28000.2526"
KIT_PACKAGES="microsoft.windows.wdk.arm64 microsoft.windows.sdk.cpp microsoft.windows.sdk.cpp.arm64"
SDK_LICENSE_URL="https://aka.ms/WinSDKLicenseURL"

DEFAULT_PROJECTS=(viogpu/viogpudo/viogpudo.vcxproj viogpu3d/viogpu3d/viogpu3d.vcxproj)

usage() {
    cat <<EOF
Usage:
  Tools/build-macos.sh fetch
  Tools/build-macos.sh [--config "Win11 Release"|"Win10 Release"] [--test-sign] [--venus-icd DIR]
                       [project.vcxproj ...]

fetch downloads the WDK and Windows SDK NuGet packages for ARM64 (${KIT_VERSION}) from
nuget.org into WDK_DIR, checks each against the SHA-512 in nuget.org's catalog and unpacks it.
Their license terms apply: LICENSE.txt in the WDK package, and the SDK's license, which fetch
saves to WDK_DIR/licenses.

A build reads each project's vcxproj for the configuration on ARM64: sources, defines, include
folders, libraries and INX. It first builds the static libraries the project links
(VirtIO/VirtioLib.vcxproj for viogpudo), then compiles with clang in MSVC mode, links with
lld-link, stamps DriverVer into a copy of the INX and, with Feature_PackOne, copies the .sys,
.pdb and .inf into Install/<OS>/ARM64 as the Windows build does. Without a project it builds
${DEFAULT_PROJECTS[*]}.

A package whose INF lists vulkan_virtio.dll (viogpu3d's) also gets Mesa's Venus Vulkan driver:
--venus-icd names the folder of vulkan_virtio.dll and virtio_icd.aarch64.json from a Mesa build for
aarch64-w64-mingw32 configured with -Dvulkan-icd-dir=bin (src/virtio/vulkan in its build folder),
by default the build in mesa-venus-windows-build/build next to this clone. They go into the
package as vulkan_virtio.dll and vulkan_virtio.json.

--test-sign signs those .sys files with build/TestCert.pfx, a self-signed code signing
certificate that openssl makes on first use. build/TestCert.cer is the certificate a test
machine trusts. No keychain is used.

Needs clang, lld-link, llvm-lib and llvm-rc from one LLVM (tested with 22.1 and 23.1),
xmllint, curl, unzip and openssl, and osslsigncode for --test-sign. WDK_DIR and this clone must
be on a case-insensitive volume (the macOS default): the kit's headers include each other in
mixed case.

Environment:
  WDK_DIR            unpacked packages (default: wdk next to this clone, ${WDK_DIR})
  LLVM_BIN           folder of the LLVM tools (default: lld-link on PATH, else Homebrew's lld or
                     newest lld@N, then the rest from Homebrew's llvm of that version, else llvm)
  STAMPINF_DATE      MM/DD/YYYY for DriverVer (default: today)
  _RHEL_RELEASE_VERSION_, _BUILD_MAJOR_VERSION_, _BUILD_MINOR_VERSION_
                     driver version parts, as with MSBuild (defaults from build/Driver.RHEL.props)
  TEST_CERT_SUBJECT  subject of a new test certificate (default: /CN=OSMates GPU Test)
  VENUS_ICD_DIR      the folder --venus-icd names (default: ${VENUS_ICD_DIR})

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

# Mesa writes .\vulkan_virtio.dll as library_path when configured with -Dvulkan-icd-dir=bin: the DLL next to the
# manifest, which is how the package installs them.
pack_venus_icd() {
    local pack=$1 dll manifest
    dll="${VENUS_ICD_DIR}/vulkan_virtio.dll"
    manifest="${VENUS_ICD_DIR}/virtio_icd.aarch64.json"
    [[ -f "${dll}" && -f "${manifest}" ]] ||
        die "${PROJ}: its INF ships the Venus Vulkan driver, and ${VENUS_ICD_DIR} has no vulkan_virtio.dll and" \
            "virtio_icd.aarch64.json; pass --venus-icd with the folder of Mesa's vulkan_virtio.dll"
    "$(llvm_tool llvm-objdump)" -f "${dll}" | grep -q 'file format coff-arm64' || die "${dll} is not an ARM64 DLL"
    tr -d '\r' <"${manifest}" | grep -q '"library_path": *"\.\\\\vulkan_virtio\.dll"' ||
        die "${manifest}: library_path is not .\\vulkan_virtio.dll; configure Mesa with -Dvulkan-icd-dir=bin"
    cp "${dll}" "${pack}/vulkan_virtio.dll"
    cp "${manifest}" "${pack}/vulkan_virtio.json"
    note "packed the Venus Vulkan driver of ${VENUS_ICD_DIR}"
}

# Files of a package that come from other builds, then a check that the package has every file its INF lists.
pack_inf_files() {
    local pack=$1 inf=$2 file
    if inf_source_files "${inf}" | grep -q -x -i 'vulkan_virtio\.dll'; then
        pack_venus_icd "${pack}"
    fi
    while IFS= read -r file; do
        [[ -f "${pack}/${file}" ]] || die "$(basename "${inf}") lists ${file}, which the package lacks"
    done < <(inf_source_files "${inf}")
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
    local sign=false projects=() project
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
