#!/usr/bin/env bash
# shellcheck disable=SC1091

# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# ALPINE_X86_64_BUILD_TAG

set -e
set -o xtrace

export LLVM_VERSION="${LLVM_VERSION:=16}"

EPHEMERAL=(
)


DEPS=(
    bash
    bison
    ccache
    clang16-dev
    cmake
    clang-dev
    coreutils
    curl
    flex
    gcc
    g++
    git
    gettext
    glslang
    graphviz
    linux-headers
    llvm16-static
    llvm16-dev
    meson
    mold
    musl-dev
    expat-dev
    elfutils-dev
    libdrm-dev
    libselinux-dev
    libva-dev
    libpciaccess-dev
    zlib-dev
    python3-dev
    py3-clang
    py3-cparser
    py3-mako
    py3-packaging
    py3-pip
    py3-ply
    py3-yaml
    vulkan-headers
    spirv-tools-dev
    util-macros
    wayland-dev
    wayland-protocols
)

apk --no-cache add "${DEPS[@]}" "${EPHEMERAL[@]}"

pip3 install --break-system-packages sphinx===5.1.1 hawkmoth===0.16.0

. .gitlab-ci/container/build-llvm-spirv.sh

. .gitlab-ci/container/build-libclc.sh

. .gitlab-ci/container/container_pre_build.sh


############### Uninstall the build software

apk del "${EPHEMERAL[@]}"

. .gitlab-ci/container/container_post_build.sh
