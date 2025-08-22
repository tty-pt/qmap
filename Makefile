LIB-LDLIBS := -lxxhash -lqsys
LIB := qmap
BIN := test
HEADERS := qidm.h
CFLAGS := -g

npm-lib := @tty-pt/qsys

-include ../mk/include.mk
