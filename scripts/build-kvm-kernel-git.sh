export ARCH=riscv
export CROSS_COMPILE=riscv64-unknown-linux-gnu-
make -C linux O=`pwd`/build-riscv64 -j $(nproc)
