/*
 * Built-in device tree blob for the Amazon Fire HD 8 (douglas).
 * The LK bootloader always passes its own 3.18-era DTB which is unusable
 * by mainline; setup_arch() unconditionally points __fdt_pointer at this
 * blob instead (see arch/arm64/kernel/setup.c).
 */
	.section .rodata
	.balign 4096
	.global dtb_builtin_begin
dtb_builtin_begin:
	.incbin DTB_PATH
	.global dtb_builtin_end
dtb_builtin_end:
	.balign 8
