# SPDX-License-Identifier: GPL-2.0
# dtb-$(CONFIG_ARCH_SOPHGO) += cv1800b-milkv-duo.dtb
# dtb-$(CONFIG_ARCH_SOPHGO) += cv1812h-milkv-duo-s.dtb
# dtb-$(CONFIG_ARCH_SOPHGO) += cv1812h-huashan-pi.dtb
# dtb-$(CONFIG_ARCH_SOPHGO) += sg2042-milkv-pioneer.dtb

# dtb-y += cv1800b-milkv-duo.dtb
# dtb-y += cv1812h-milkv-duo-s.dtb
# dtb-y += cv1812h-huashan-pi.dtb
# dtb-y += sg2042-milkv-pioneer.dtb
dtb-y += $(foreach dts,$(patsubst %.dts,%.dtb,$(wildcard ${src}/*.dts)),$(notdir ${dts}))

# srctree is /home/work/linux_6.12
# src is /home/work/linux_6.12/arch/riscv/boot/dts/sophgo
# dts is
# always-y          :=

$(info srctree is $(srctree))
$(info src is $(src))
$(info dts is $(dts))

$(info always-y          := $(dtb-y))

always-y          := $(dtb-y)
subdir-y        := $(dts-dirs)
clean-files     := *.dtb

