export ARCH=riscv
export CROSS_COMPILE=riscv64-linux-gnu-
cd kvmtool
make lkvm-static -j $(nproc)