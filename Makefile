PROG = wmpdart
OBJS = wmpdart.o
SRCS = wmpdart.c

PREFIX ?= /usr/local
MANPREFIX ?= ${PREFIX}/share/man
LOCALINC ?= /usr/local/include
LOCALLIB ?= /usr/local/lib
X11INC ?= /usr/X11R6/include
X11LIB ?= /usr/X11R6/lib

INCS = -I${LOCALINC} -I${X11INC}
LIBS = -L${LOCALLIB} -L${X11LIB} -lm -ljpeg -lmpdclient -lX11 -lXpm

all: ${PROG}

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LIBS} ${LDFLAGS}

.c.o:
	${CC} ${INCS} ${CFLAGS} ${CPPFLAGS} -c $<

${OBJS}: buttons.xpm album.data

tags: ${SRCS}
	ctags ${SRCS}

clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core} tags

install: all
	install -d ${DESTDIR}${PREFIX}/bin
	#install -d ${DESTDIR}${MANPREFIX}/man1
	install -m 755 ${PROG} ${DESTDIR}${PREFIX}/bin/${PROG}
	#install -m 644 ${PROG}.1 ${DESTDIR}${MANPREFIX}/man1/${PROG}.1

uninstall:
	rm ${DESTDIR}${PREFIX}/bin/${PROG}
	#rm ${DESTDIR}${MANPREFIX}/man1/${PROG}.1

.PHONY: all tags clean install uninstall
