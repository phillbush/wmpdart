PROG = wmpdart
OBJS = ${PROG:=.o}
SRCS = ${OBJS:.o=.c}

PREFIX ?= /usr/local
MANPREFIX ?= ${PREFIX}/share/man
LOCALINC ?= /usr/local/include
LOCALLIB ?= /usr/local/lib
X11INC ?= /usr/X11R6/include
X11LIB ?= /usr/X11R6/lib

DEFS = -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -D_BSD_SOURCE
INCS = -I${LOCALINC} -I${X11INC}
LIBS = -L${LOCALLIB} -L${X11LIB} -lm -ljpeg -lmpdclient -lX11 -lXpm
PROG_CFLAGS = -std=c99 -pedantic ${DEFS} ${INCS} ${CFLAGS} ${CPPFLAGS}
PROG_LDFLAGS = ${LIBS} ${LDLIBS} ${LDFLAGS}

bindir = ${DESTDIR}${PREFIX}/bin

all: ${PROG}

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${PROG_LDFLAGS}

.c.o:
	${CC} ${PROG_CFLAGS} -o $@ -c $<

${OBJS}: buttons.xpm album.data

tags: ${SRCS}
	ctags ${SRCS}

lint: ${SRCS}
	-clang-tidy ${SRCS} -- ${PROG_CFLAGS}

clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core} tags

install: all
	mkdir -p ${bindir}
	install -m 755 ${PROG} ${bindir}/${PROG}

uninstall:
	-rm ${bindir}/${PROG}

.PHONY: all clean install uninstall lint
