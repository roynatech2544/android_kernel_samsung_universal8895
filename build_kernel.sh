#/bin/bash
set -e

export KBUILD_BUILD_USER=Royna
export KBUILD_BUILD_HOST=GrassLand

PATH=$PWD/toolchain/bin:$PATH

rm -rf out
make O=out ARCH=arm64 CC=clang CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) exynos8895-dreamlte_defconfig
make O=out ARCH=arm64 CC=clang AS=llvm-as LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
