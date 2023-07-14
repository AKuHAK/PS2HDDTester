
EE_BIN_DIR = bin/
EE_OBJS_DIR = obj/

IOP_MODS = iomanX fileXio ps2dev9
IOP_IRXS = $(addsuffix .irx,$(IOP_MODS))
IOP_IRX_OBJS = $(addsuffix .o,$(IOP_MODS))

EE_BIN = $(EE_BIN_DIR)hddtester.elf
EE_OBJS = main.o hddtests.o exceptions.o exphandler.o
EE_OBJS_FULL = $(addprefix $(EE_OBJS_DIR),$(EE_OBJS)) $(addprefix $(EE_OBJS_DIR),$(IOP_IRX_OBJS)) $(EE_OBJS_DIR)ps2atad.o $(EE_OBJS_DIR)xhdd.o
EE_LIBS = -lpatches -lkernel -ldebug -lfileXio -lpad

EE_CFLAGS += -fdata-sections -ffunction-sections -DDUMMY_TIMEZONE -DDUMMY_LIBC_INIT -DKERNEL_NOPATCH -D_EE -Wall -Wno-sizeof-pointer-memaccess -Wno-pointer-sign
EE_LDFLAGS += -Wl,--gc-sections -T $(PS2SDK)/ee/startup/linkfile

EE_INCS += -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include -I. -I../common -I../iop/atad/include

all: | $(IOP_IRXS) ps2atad.irx xhdd.irx $(EE_OBJS) $(EE_BIN)
	echo "Compressing..."
	ps2-packer -v $(EE_BIN) $(EE_BIN_DIR)hddtester_compressed.elf

clean:
	$(MAKEREC) ../iop/atad clean
	$(MAKEREC) ../iop/xhdd clean
	rm -r -f $(EE_OBJS_DIR) $(EE_BIN_DIR)

run: $(EE_BIN)
	ps2client -h 192.168.1.55 execee host:$(EE_BIN)

reset:
	ps2client -h 192.168.1.55 reset

$(EE_BIN) : $(EE_OBJS_FULL)
	@$(MKDIR) -p $(EE_BIN_DIR)
	$(EE_CC) $(EE_CFLAGS) $(EE_LDFLAGS) -o $@ $^ -L$(PS2SDK)/ee/lib $(EE_LIBS)

%.o : %.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $(EE_OBJS_DIR)$@

%.o : %.s
	$(EE_AS) $(EE_ASFLAGS) $< -o $(EE_OBJS_DIR)$@

$(IOP_IRXS):
	@$(MKDIR) -p $(EE_OBJS_DIR)
	bin2c $(PS2SDK)/iop/irx/$@ $(EE_OBJS_DIR)$(basename $@)_irx.c $(basename $@)_irx
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $(EE_OBJS_DIR)$(basename $@)_irx.c -o $(EE_OBJS_DIR)$(basename $@).o

ps2atad.irx:
	$(MAKEREC) ../iop/atad
	bin2c ../iop/atad/irx/ps2atad.irx $(EE_OBJS_DIR)ps2atad_irx.c ps2atad_irx
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $(EE_OBJS_DIR)ps2atad_irx.c -o $(EE_OBJS_DIR)ps2atad.o

xhdd.irx:
	$(MAKEREC) ../iop/xhdd
	bin2c ../iop/xhdd/xhdd.irx $(EE_OBJS_DIR)xhdd_irx.c xhdd_irx
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $(EE_OBJS_DIR)xhdd_irx.c -o $(EE_OBJS_DIR)xhdd.o

include $(PS2SDKSRC)/Defs.make
#include $(PS2SDKSRC)/ee/Rules.make
