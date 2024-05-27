./apps/lkvm-static run -m 128 -c2 --console serial -p "console=ttyS0 earlycon=uart8250,mmio,0x3f8" -k ./apps/Image --debug
