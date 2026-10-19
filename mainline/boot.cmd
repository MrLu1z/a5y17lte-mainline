# U-Boot boot script, run from the FIT that S-Boot loads as the "ramdisk"
# (include/configs/exynos78x0-common.h: bootcmd=source $prevbl_initrd_start_addr:bootscript)
echo "a5y17lte: FIT at ${prevbl_initrd_start_addr}, booting mainline Linux (M4d)"
bootm ${prevbl_initrd_start_addr}#a5y17lte
echo "a5y17lte: bootm returned, resetting"
reset
