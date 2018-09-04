# default options
CC=gcc
CPPCC=g++
DEP=../
WOOFC=../cspot/
UINC=${DEP}/euca-cutils
MINC=${DEP}/mio
SINC=${WOOFC}
ULIB=${DEP}/euca-cutils/libutils.a
MLIB=${DEP}/mio/mio.o ${DEP}/mio/mymalloc.o
SLIB=${WOOFC}/lsema.o
LIBS=${WOOFC}/uriparser2/liburiparser2.a -lm -lczmq 
LOBJ=${WOOFC}/log.o ${WOOFC}/host.o ${WOOFC}/event.o
LINC=${WOOFC}/log.h ${WOOFC}/host.h ${WOOFC}/event.h
WINC=${WOOFC}/woofc.h ${WOOFC}/woofc-access.h ${WOOFC}/woofc-cache.h
WOBJ=${WOOFC}/woofc.o ${WOOFC}/woofc-access.o ${WOOFC}/woofc-cache.o
WHINC=${WOOFC}/woofc-host.h 
WHOBJ=${WOOFC}/woofc-host.o 
SHEP_SRC=${WOOFC}/woofc-shepherd.c

# libs that all of the targets share (primarily for CSPOT linkage)
MY_LIBS=3rdparty/json.o 3rdparty/base64.o 3rdparty/hashtable.o lib/utility.o lib/wp.o 
CSPOT_COMMON_LIBS=${WOBJ} ${WHOBJ} ${SLIB} ${LOBJ} ${MLIB} ${ULIB} ${LIBS}

PYVERSION=python3.6
PYCFLAGS=$(shell ${PYVERSION}-config --cflags | sed 's/\-Wall//g' | sed 's/\-Wstrict-prototypes//g') # python flags for include paths
PYLIBS=$(shell ${PYVERSION}-config --ldflags) # python flags for linking

CFLAGS=-pthread -lrt -g -I${UINC} -I${MINC} -I${SINC} -I.

HAND1=awspy_lambda

all: awsapi_client ${HAND1} utiltest

awsapi_client: ${WINC} src/client/awsapi_client.cpp src/client/wpcmds.c ${MY_LIBS}
	${CC} ${CFLAGS} -Wall -c src/client/wpcmds.c -o src/client/wpcmds.o
	${CPPCC} ${CFLAGS} -Wall -o awsapi_client src/client/awsapi_client.cpp ${CSPOT_COMMON_LIBS} ${MY_LIBS} src/client/wpcmds.o -lulfius -ljansson
	mkdir -p cspot; cp awsapi_client ./cspot 

${HAND1}: ${HAND1}.cpp ${SHEP_SRC} ${WINC} ${LINC} ${LOBJ} ${WOBJ} ${SLIB} ${SINC} ${MY_LIBS}
	sed 's/WOOF_HANDLER_NAME/${HAND1}/g' ${SHEP_SRC} > ${HAND1}_shepherd.c
	${CC} ${CFLAGS} ${PYCFLAGS} -c ${HAND1}_shepherd.c -o ${HAND1}_shepherd.o
	${CPPCC} ${CFLAGS} ${PYCFLAGS} -o ${HAND1} ${HAND1}.cpp ${HAND1}_shepherd.o ${CSPOT_COMMON_LIBS} ${MY_LIBS} ${PYLIBS} 
	mkdir -p cspot; cp ${HAND1} ./cspot; cp ${WOOFC}/woofc-container ./cspot; cp ${WOOFC}/woofc-namespace-platform ./cspot

# helper libraries
utiltest: utiltest.c lib/utility.o lib/wp.o
	${CC} ${CFLAGS} -o utiltest utiltest.c lib/utility.o lib/wp.o

lib/utility.o: lib/utility.c lib/utility.h
	${CC} ${CFLAGS} -c lib/utility.c -o lib/utility.o

lib/wp.o: lib/wp.c lib/wp.h
	${CC} ${CFLAGS} -c lib/wp.c -o lib/wp.o

3rdparty/json.o: 3rdparty/json.c 3rdparty/json.h 
	${CC} ${CFLAGS} -c 3rdparty/json.c -o 3rdparty/json.o 

3rdparty/base64.o: 3rdparty/base64.c 3rdparty/base64.h 
	${CC} ${CFLAGS} -c 3rdparty/base64.c -o 3rdparty/base64.o

3rdparty/hashtable.o: 3rdparty/hashtable.c 3rdparty/hashtable.h 
	${CC} ${CFLAGS} -c 3rdparty/hashtable.c -o 3rdparty/hashtable.o

clean:
	rm -f awsapi_client ${HAND1} *_shepherd.* *.o **/*.o
