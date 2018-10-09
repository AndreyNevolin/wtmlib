TARGET = libwtm.so
export BUILD_FLAGS =

SRCS = src/wtmlib.c
HEADERS = src/wtmlib.h src/wtmlib_config.h

OUTDIR = .
OBJDIR = .objs
FULLTARGET = ${OUTDIR}/${TARGET}

OBJS = $(patsubst src/%.c, ${OBJDIR}/%.o, ${SRCS})

GCC = g++ -std=c++0x -Wall -Werror -pthread $(BUILD_FLAGS)

MACHINE_HARDWARE_NAME = $(shell uname -m)

ifeq (${MACHINE_HARDWARE_NAME},x86_64)
	HOST_ARCH = X86_64
endif

ifneq ($(filter ppc64%, ${MACHINE_HARDWARE_NAME}),)
	HOST_ARCH = PPC_64
endif

ifndef HOST_ARCH
$(error Architecture ${MACHINE_HARDWARE_NAME} is not supported)
endif

BUILD_FLAGS += -DWTMLIB_ARCH_${HOST_ARCH}

.PHONY: clean

default : BUILD_FLAGS += -s
default : ${FULLTARGET}

debug : BUILD_FLAGS += -DWTMLIB_DEBUG -g -O0
debug : ${FULLTARGET}

log : BUILD_FLAGS += -DWTMLIB_LOG
log : ${FULLTARGET}

log_debug : BUILD_FLAGS += -DWTMLIB_LOG
log_debug : debug

clean:
	-rm -f ${FULLTARGET} > /dev/null 2>&1
	-cd ${OBJDIR}
	-rm -f ${OBJS} > /dev/null 2>&1
	-cd ..
	-rm -f ${OBJDIR}/example.o > /dev/null 2>&1
	-rm -f example > /dev/null 2>&1

${FULLTARGET}: ${OBJS}
	-mkdir -p ${OUTDIR} > /dev/null 2>&1
	${GCC} -fPIC -shared -o ${FULLTARGET} ${OBJS} -lm

${OBJDIR}/%.o: src/%.c ${HEADERS}
	-mkdir -p ${OBJDIR} > /dev/null 2>&1
	${GCC} -fPIC -c -o $@ $<

example:
	-mkdir -p ${OBJDIR} > /dev/null 2>&1
	${GCC} -c -o ${OBJDIR}/example.o example.c
	${GCC} -o example ${OBJDIR}/example.o -L./ -lwtm -Wl,-rpath=./
