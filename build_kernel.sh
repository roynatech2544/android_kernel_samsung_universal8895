#/bin/bash
set -e

export KBUILD_BUILD_USER=Royna
export KBUILD_BUILD_HOST=GrassLand

PATH=$PWD/toolchain/bin:$PATH

rm -rf out
make O=out LLVM=1 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) exynos8895-dreamlte_defconfig
make O=out LLVM=1 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
