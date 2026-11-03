obj-m += ntbox.o

ccflags-y := -I$(src)/include

ntbox-y := src/core/ntbox_main.o \
           src/core/ntbox_init.o \
           src/core/ntbox_diag.o \
           src/core/ntbox_part.o \
           src/core/ntbox_file.o \
           src/pe/pe_loader.o \
           src/pe/pe_arch.o \
           src/pe/ntbox_imports.o \
           src/pe/pe_run.o \
           src/pe/pe_user.o \
           src/obj/nt_object.o \
           src/irp/irp_core.o \
           src/bridge/linux_bridge.o \
           src/ntapi/ntapi_stubs.o \
           src/ntapi/ntb_table.o \
           src/hal/hal_mini.o
