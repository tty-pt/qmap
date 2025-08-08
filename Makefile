PREFIX ?= /usr/local
pwd != pwd
prefix := ${pwd} /usr/local
CFLAGS := ${prefix:%=-I%/include} -g -O3 -Wall -Wextra -Wpedantic
LDLIBS := -lxxhash
LDFLAGS := ${prefix:%=-L%/lib} ${LDLIBS}
dirs := bin lib

all: test

test: test.c lib/libqmap.so
	${CC} -o $@ test.c ${CFLAGS} lib/libqmap.so

lib/libqmap.so: libqmap.c include/qmap.h include/qidm.h lib
	${CC} -o $@ libqmap.c ${CFLAGS} -fPIC -shared ${LDFLAGS}

$(dirs):
	mkdir $@ 2>/dev/null || true

install: lib/libqmap.so
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 lib/libqmap.so ${DESTDIR}${PREFIX}/lib
	install -m 644 qmap.pc $(DESTDIR)${PREFIX}/lib/pkgconfig
	install -d ${DESTDIR}${PREFIX}/include
	install -m 644 include/qmap.h $(DESTDIR)${PREFIX}/include
	install -m 644 include/qidm.h $(DESTDIR)${PREFIX}/include

clean:
	rm lib/libqmap.so|| true

.PHONY: all install clean
