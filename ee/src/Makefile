EE_LIB = ../lib/libmp3.a
EE_OBJS = libmp3.o bstdfile.o file.o directory.o mp3help.o sjpcm_rpc.o rmalloc.o

EE_CXXFLAGS += -fno-exceptions -fno-rtti
EE_LIBS += -lfileXio -lsample -lmad -lpad -lgs -lm -lcdvdfs -lc -lkernel -lsyscall -lc 
EE_INCS += -I../include
EE_LDFLAGS += -L./LIB -L/usr/local/ps2dev/ps2mp3/LIB

all: $(EE_LIB)

clean:
	rm -f *.elf *.o ../lib/*.a 

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
