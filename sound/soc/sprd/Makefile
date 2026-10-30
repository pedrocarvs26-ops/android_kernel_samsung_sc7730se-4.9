# Spreadtrum SC8830-family audio
subdir-ccflags-y += -I$(srctree)/sound/soc/sprd \
		    -I$(srctree)/sound/soc/sprd/dai \
		    -I$(srctree)/sound/soc/sprd/dai/vbc \
		    -I$(srctree)/sound/soc/sprd/dai/vaudio \
		    -I$(srctree)/sound/soc/sprd/codec

obj-$(CONFIG_SND_SOC_SPRD)		+= sprd-asoc-common.o sprd-asoc-card.o
obj-$(CONFIG_SND_SOC_SPRD_AUDIO_DMA)	+= dai/
obj-$(CONFIG_SND_SOC_SPRD_VBC_R2P0)	+= dai/vbc/r2p0/
obj-$(CONFIG_SND_SOC_SPRD_VAUDIO)	+= dai/vaudio/
obj-$(CONFIG_SND_SOC_SPRD_CODEC_V4)	+= codec/sprd/v4/
