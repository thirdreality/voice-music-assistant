#!/bin/bash

# set -e
trap 'ERROR "Build failed at line $LINENO."' ERR

INFO()  { echo -e "\e[1;32m[INFO]    $1\e[0m"; }
ERROR() { echo -e "\e[1;31m[ERROR]   $1\e[0m" >&2; exit 1; }

usage() {
    cat <<EOF
Usage:
  ./go <target> [version]              Build natively
  ./go --docker <target> [version]     Build inside Docker container
  ./go --docker-shell                  Enter Docker container interactively
  ./go <target> rebuild <package>       Rebuild a single package
  ./go --docker <target> rebuild <package>  Rebuild a single package in Docker
  ./go <target> rootfs <version>       Rebuild enabled ThirdReality packages and repack rootfs
  ./go --docker <target> rootfs <version>  Repack rootfs in Docker
  ./go -h, --help                      Show this help message

Arguments:
  target      Build target name (e.g. trspk)
  version     Optional version string (default: date-based, e.g. 0.05.10)
  package     Buildroot package name (e.g. sendspin-client, linux-voice-assistant)

Examples:
  ./go trspk                           Native build with date-based version
  ./go trspk 1.2.3                     Native build with version 1.2.3
  ./go --docker trspk                  Docker build (no host deps required)
  ./go --docker trspk 1.2.3            Docker build with version 1.2.3
  ./go --docker-shell                  Debug inside the build container
  ./go trspk rebuild sendspin-client   Rebuild sendspin-client package
  ./go --docker trspk rebuild sendspin-client
  ./go trspk rootfs 1.2.3             Recreate rootfs using the existing build configuration

rootfs requires a previous full build. It cleans enabled packages defined under
buildroot/package/thirdreality, reinstalls a fresh target/, and regenerates .img/.swu.
Other package builds, host tools and staging are reused; modified dependencies
outside thirdreality may still need rebuilding separately.

Output:
  image/<target>_<version>.img         Firmware image for USB burning
  image/<target>_<version>.swu         OTA update package
EOF
    exit 0
}

#--- Docker build mode ---#
docker_build() {
    local IMAGE_NAME="voice-music-assistant-builder"
    local ROOT_DIR
    ROOT_DIR="$(cd "$(dirname "$0")" && pwd)" || ERROR "Cannot locate repository"

    # Build Docker image if not exists, or rebuild if Dockerfile is newer
    local NEED_BUILD=false
    if ! docker image inspect "${IMAGE_NAME}" &>/dev/null; then
        NEED_BUILD=true
    else
        local IMAGE_CREATED
        IMAGE_CREATED=$(docker image inspect -f '{{.Created}}' "${IMAGE_NAME}" 2>/dev/null)
        local IMAGE_TS
        IMAGE_TS=$(date -d "${IMAGE_CREATED}" +%s 2>/dev/null || echo 0)
        local DOCKERFILE_TS
        DOCKERFILE_TS=$(stat -c %Y "${ROOT_DIR}/Dockerfile" 2>/dev/null || echo 0)
        if [ "${DOCKERFILE_TS}" -gt "${IMAGE_TS}" ]; then
            NEED_BUILD=true
        fi
    fi
    if [ "${NEED_BUILD}" = true ]; then
        INFO "Building Docker image..."
        docker build -t "${IMAGE_NAME}" "${ROOT_DIR}" || ERROR "Docker image build failed"
    fi

    local DOCKER_RUN_ARGS=(
        --rm
        -u "$(id -u):$(id -g)"
        -v "${ROOT_DIR}:/build"
        -e "HOME=/build"
    )

    # If buildroot/dl is a symlink, mount the real target so the container can access it
    local DL_PATH="${ROOT_DIR}/buildroot/dl"
    if [ -L "${DL_PATH}" ]; then
        local DL_REAL
        DL_REAL="$(readlink -f "${DL_PATH}")" || ERROR "Cannot resolve download cache"
        DOCKER_RUN_ARGS+=(-v "${DL_REAL}:${DL_REAL}")
        DOCKER_RUN_ARGS+=(-v "${DL_REAL}:/build/buildroot/dl")
    fi

    if [ "$1" = "--shell" ]; then
        INFO "Starting interactive shell in Docker..."
        exec docker run -it "${DOCKER_RUN_ARGS[@]}" --entrypoint /bin/bash "${IMAGE_NAME}"
    else
        INFO "Starting Docker build: ./go $*"
        exec docker run "${DOCKER_RUN_ARGS[@]}" "${IMAGE_NAME}" "$@"
    fi
}

#--- Parse arguments ---#
case "$1" in
    --docker)
        shift
        [ -z "$1" ] && ERROR "Missing target name! Usage: ./go --docker <target> [version]"
        docker_build "$@"
        ;;
    --docker-shell)
        docker_build --shell
        ;;
    -h|--help|"")
        usage
        ;;
esac

ROOT_DIR=$(cd "$(dirname "$0")" && pwd -P)
BUILDROOT_DIR="${ROOT_DIR}/buildroot"
BUILD_OUTPUT_DIR="${ROOT_DIR}/output"
[[ "$1" =~ ^[a-zA-Z0-9_-]+$ ]] || ERROR "Invalid target name: $1"

lock_output() {
    command -v flock >/dev/null 2>&1 || ERROR "flock is required to protect the build output"
    exec 9>"${TARGET_OUTPUT_DIR}/.go.lock" || ERROR "Cannot open build lock"
    flock -n 9 || ERROR "Another go command is using ${TARGET_OUTPUT_DIR}"
}

repack_rootfs() {
    local metadata line pkg dir relative
    local -a packages=()
    if [ ! -f "${TARGET_OUTPUT_DIR}/.config" ] || \
        [ ! -d "${TARGET_OUTPUT_DIR}/build" ] || \
        [ ! -d "${TARGET_OUTPUT_DIR}/host" ] || \
        [ ! -f "${TARGET_OUTPUT_DIR}/images/boot.img" ]; then
        ERROR "Run ./go $1 once before repacking rootfs"
    fi
    if [ -L "${TARGET_OUTPUT_DIR}/target" ] || [ -L "${TARGET_OUTPUT_DIR}/build" ]; then
        ERROR "Refusing to clean symlinked target/build directories"
    fi
    ! grep -qx 'BR2_PER_PACKAGE_DIRECTORIES=y' "${TARGET_OUTPUT_DIR}/.config" \
        || ERROR "rootfs does not support per-package directories"

    # Read the enabled dependency graph and exact directories from Buildroot,
    # rather than guessing package names from Config.in symbols or glob prefixes.
    metadata=$(umask 0022; make -s -C "${BUILDROOT_DIR}" O="${TARGET_OUTPUT_DIR}" \
        -f "${ROOT_DIR}/script/rootfs-packages.mk" rootfs-packages) \
        || ERROR "Cannot determine enabled ThirdReality packages"
    while IFS= read -r line; do
        [[ "$line" == ROOTFS_PACKAGE$'\t'* ]] || continue
        IFS=$'\t' read -r _ pkg dir <<< "$line"
        [[ "$pkg" =~ ^[a-zA-Z0-9_-]+$ ]] || ERROR "Invalid package name: $pkg"
        [[ "$dir" == "${TARGET_OUTPUT_DIR}/build/"* ]] || ERROR "Package directory outside build/: $dir"
        relative=${dir#"${TARGET_OUTPUT_DIR}/build/"}
        [[ -n "$relative" && "$relative" != */* && "$relative" != . && "$relative" != .. ]] \
            || ERROR "Unsafe package directory: $dir"
        [ ! -L "$dir" ] || ERROR "Refusing to clean symlinked package directory: $dir"
        packages+=("$pkg")
        INFO "Will clean and rebuild: $pkg ($dir)"
    done <<< "$metadata"
    [ "${#packages[@]}" -gt 0 ] || ERROR "No enabled ThirdReality packages found"

    # Let Buildroot remove the actual package directories. Keep all unrelated
    # build directories and the shared host/staging installation intact.
    for pkg in "${packages[@]}"; do
        make -C "${BUILDROOT_DIR}" O="${TARGET_OUTPUT_DIR}" "${pkg}-dirclean" \
            || ERROR "Cannot clean $pkg"
    done
    # Invalidate installs before deleting target so an interrupted run can retry.
    find "${TARGET_OUTPUT_DIR}/build" -mindepth 2 -maxdepth 2 -type f \
        -name .stamp_target_installed -delete || ERROR "Cannot reset target installation stamps"
    rm -rf -- "${TARGET_OUTPUT_DIR}/target" || ERROR "Cannot clean target directory"
    mkdir -p "${TARGET_OUTPUT_DIR}/target" || ERROR "Cannot recreate target directory"
    INFO "Rebuilding ThirdReality packages and repacking rootfs (${IMAGE_VERSION})"
    SPEAKER_FIRMWARE_VERSION="${IMAGE_VERSION}" make -C "${BUILDROOT_DIR}" O="${TARGET_OUTPUT_DIR}" \
        || ERROR "rootfs build failed; fix the error and rerun ./go $1 rootfs ${IMAGE_VERSION}"
}

#--- Rebuild single package ---#
if [ "$2" = "rebuild" ]; then
    [ -z "$3" ] && ERROR "Missing package name! Usage: ./go <target> rebuild <package>"
    TARGET_OUTPUT_DIR="${BUILD_OUTPUT_DIR}/3reality_${1}"
    PKG="$3"
    [ -d "${TARGET_OUTPUT_DIR}" ] || ERROR "Output dir not found: ${TARGET_OUTPUT_DIR}. Run a full build first."
    lock_output
    cd "${BUILDROOT_DIR}" || ERROR "Cannot enter Buildroot directory"
    INFO "Rebuilding package: ${PKG} (target: ${1})"
    make O="${TARGET_OUTPUT_DIR}" "${PKG}-rebuild"
    INFO "Package ${PKG} rebuilt successfully!"
    exit 0
fi
TARGET_BUILD_CONFIG="3reality_${1}"
TARGET_OUTPUT_DIR="${BUILD_OUTPUT_DIR}/${TARGET_BUILD_CONFIG}"
TOOLCHAIN_DIR="${ROOT_DIR}/sources/toolchain"
IMAGE_DIR="${ROOT_DIR}/image"
IMAGE_VERSION="${2}"
ROOTFS_ONLY=0
if [ "$2" = "rootfs" ]; then
    [ "$#" -eq 3 ] || ERROR "Usage: ./go <target> rootfs <version>"
    IMAGE_VERSION="$3"
    [[ "$IMAGE_VERSION" =~ ^[0-9]+(\.[0-9]+){1,3}$ ]] || ERROR "Invalid firmware version: $IMAGE_VERSION"
    ROOTFS_ONLY=1
    [ -d "${TARGET_OUTPUT_DIR}" ] || ERROR "Run ./go $1 once before repacking rootfs"
fi
if [ -z "${IMAGE_VERSION}" ]; then
    IMAGE_VERSION=$(date "+0.%m.%d")
fi

# Check submodules are initialized
if [ ! -d "${TOOLCHAIN_DIR}/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu" ]; then
    ERROR "Toolchain submodule not initialized. Run: git submodule update --init"
fi

TOOLCHAINS=(
    "${TOOLCHAIN_DIR}/CodeSourcery/Sourcery_G++_Lite/bin"
    "${TOOLCHAIN_DIR}/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-elf/bin"
)

for tc in "${TOOLCHAINS[@]}"; do
    if [ -d "${tc}" ]; then
        if [[ ":${PATH}:" != *":${tc}:"* ]]; then
            export PATH="${tc}:${PATH}"
            # INFO "Added to PATH: $tc"
        fi
    else
        ERROR "Toolchain directory not found: ${tc}"
    fi
done

mkdir -p "${TARGET_OUTPUT_DIR}"
lock_output
cd "${BUILDROOT_DIR}" || ERROR "Cannot enter Buildroot directory"

if [ "$ROOTFS_ONLY" -eq 1 ]; then
    repack_rootfs "$1"
else
    INFO "Starting build for ${TARGET_BUILD_CONFIG}..."
    make O="${TARGET_OUTPUT_DIR}" "${TARGET_BUILD_CONFIG}_defconfig"
    SPEAKER_FIRMWARE_VERSION="${IMAGE_VERSION}" make O="${TARGET_OUTPUT_DIR}"
fi

INFO "Build completed successfully!"
mkdir -p "${IMAGE_DIR}"
cp -f "${TARGET_OUTPUT_DIR}/images/aml_upgrade_package.img" "${IMAGE_DIR}/${1}_${IMAGE_VERSION}.img"
cp -f "${TARGET_OUTPUT_DIR}/images/software.swu" "${IMAGE_DIR}/${1}_${IMAGE_VERSION}.swu"
