#!/bin/bash

set -e

# download toolchain from https://opensource.samsung.com/uploadSearch?searchValue=toolchain 
TOOLCHAIN=$(realpath "../toolchain_samsung_sm8750/kernel_platform/prebuilts")

export PATH=$PATH:$TOOLCHAIN/build-tools/linux-x86/bin
export PATH=$PATH:$TOOLCHAIN/build-tools/path/linux-x86
export PATH=$PATH:$TOOLCHAIN/clang/host/linux-x86/clang-r510928/bin
export PATH=$PATH:$TOOLCHAIN/clang-tools/linux-x86/bin
export PATH=$PATH:$TOOLCHAIN/kernel-build-tools/linux-x86/bin


LLD_COMPILER_RT="-fuse-ld=lld --rtlib=compiler-rt"

sysroot_flags+="--sysroot=$TOOLCHAIN/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/sysroot "

cflags+="-I$TOOLCHAIN/kernel-build-tools/linux-x86/include "
ldflags+="-L $TOOLCHAIN/kernel-build-tools/linux-x86/lib64 "
ldflags+=${LLD_COMPILER_RT}

export LD_LIBRARY_PATH="$TOOLCHAIN/kernel-build-tools/linux-x86/lib64"
export HOSTCFLAGS="$sysroot_flags $cflags"
export HOSTLDFLAGS="$sysroot_flags $ldflags"

TARGET_DEFCONFIG=${1:-stock_gki_defconfig}

cd "$(dirname "$0")"

ARGS="
CC=clang
ARCH=arm64
LLVM=1 LLVM_IAS=1
"

# build kernel
make -j$(nproc) -C $(pwd) O=$(pwd)/out ${ARGS} $TARGET_DEFCONFIG

./scripts/config --file out/.config \
  -d UH \
  -d RKP \
  -d KDP \
  -d SECURITY_DEFEX \
  -d INTEGRITY \
  -d FIVE \
  -d TRIM_UNUSED_KSYMS

if [ "$LTO" = "thin" ]; then
  ./scripts/config --file out/.config -e LTO_CLANG_THIN -d LTO_CLANG_FULL
fi

make -j$(nproc) -C $(pwd) O=$(pwd)/out ${ARGS}

