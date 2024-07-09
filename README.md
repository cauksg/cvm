1、首先，下载本项目并切换到cvm-wsw分支

`git clone https://gitee.com/lubailong/riscv-kvm.git`
`git checkout cvm-wsw`

2、之后，执行build-tool.sh脚本，安装riscv-gnu-toolchain、QEMU、kvmtool等工具，搭建riscv kvm所需要的环境

`./build-tool.sh`

3、若上述步骤成功，执行build-host.sh脚本，编译guest os，host os，opensbi，kvmtool等

`./build-host.sh`

4、若上述步骤成功，执行boot-host-os.sh脚本，启动host os

`./boot-host-os.sh`

4、在host OS的命令行中执行run-guest-os.sh脚本，启动guest os

`./run-guest-os.sh`
