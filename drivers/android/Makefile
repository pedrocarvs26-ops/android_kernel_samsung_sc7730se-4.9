ccflags-y += -I$(src)			# needed for trace events
# Same ABI for binder.o and binder_alloc.o: 32-bit binder protocol (v7)
ccflags-$(CONFIG_ANDROID_BINDER_IPC_32BIT) += -DBINDER_IPC_32BIT=1

obj-$(CONFIG_ANDROID_BINDER_IPC)	+= binder.o binder_alloc.o
obj-$(CONFIG_ANDROID_BINDER_IPC_SELFTEST) += binder_alloc_selftest.o
