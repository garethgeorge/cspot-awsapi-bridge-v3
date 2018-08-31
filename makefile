# default options
CC=gcc
DEP=../
WOOFC=../cspot/
UINC=${DEP}/euca-cutils
MINC=${DEP}/mio
SINC=${WOOFC}
ULIB=${DEP}/euca-cutils/libutils.a
MLIB=${DEP}/mio/mio.o ${DEP}/mio/mymalloc.o
SLIB=${WOOFC}/lsema.o
LIBS=${WOOFC}/uriparser2/liburiparser2.a -lpthread -lm -lczmq 
LOBJ=${WOOFC}/log.o ${WOOFC}/host.o ${WOOFC}/event.o
LINC=${WOOFC}/log.h ${WOOFC}/host.h ${WOOFC}/event.h
WINC=${WOOFC}/woofc.h ${WOOFC}/woofc-access.h ${WOOFC}/woofc-cache.h
WOBJ=${WOOFC}/woofc.o ${WOOFC}/woofc-access.o ${WOOFC}/woofc-cache.o
WHINC=${WOOFC}/woofc-host.h 
WHOBJ=${WOOFC}/woofc-host.o 
SHEP_SRC=${WOOFC}/woofc-shepherd.c

PYVERSION=python3.6
PYCFLAGS=$(shell ${PYVERSION}-config --cflags | sed 's/\-Wall//g' | sed 's/\-Wstrict-prototypes//g') # python flags for include paths
PYLIBS=$(shell ${PYVERSION}-config --ldflags) # python flags for linking

HAND1=awspy_lambda

CFLAGS=-pthread -lrt -g -I${UINC} -I${MINC} -I${SINC} # ${PYCFLAGS}

all: awsapi_client ${HAND1} utiltest

APICLIENT_EXTRA_OBJECTS=3rdparty/base64.o 3rdparty/hashtable.o lib/wp.o lib/utility.o
APICLIENT_EXTRA_LIBS=-lulfius -ljansson
awsapi_client: ${WINC} awsapi_client.c ${APICLIENT_EXTRA_OBJECTS}
	${CC} ${CFLAGS} -Wall -o awsapi_client awsapi_client.c ${APICLIENT_EXTRA_OBJECTS} ${WOBJ} ${WHOBJ} ${SLIB} ${LOBJ} ${MLIB} ${ULIB} ${LIBS} ${APICLIENT_EXTRA_LIBS}
	mkdir -p cspot; cp awsapi_client ./cspot 

${HAND1}: ${HAND1}.c ${SHEP_SRC} ${WINC} ${LINC} ${LOBJ} ${WOBJ} ${SLIB} ${SINC} 3rdparty/json.o
	sed 's/WOOF_HANDLER_NAME/${HAND1}/g' ${SHEP_SRC} > ${HAND1}_shepherd.c
	${CC} ${CFLAGS} ${PYCFLAGS} -c ${HAND1}_shepherd.c -o ${HAND1}_shepherd.o
	${CC} ${CFLAGS} ${PYCFLAGS} -o ${HAND1} ${HAND1}.c ${HAND1}_shepherd.o ${WOBJ} ${SLIB} ${LOBJ} ${MLIB} ${ULIB} ${LIBS} ${PYLIBS} 3rdparty/json.o 
	mkdir -p cspot; cp ${HAND1} ./cspot; cp ${WOOFC}/woofc-container ./cspot; cp ${WOOFC}/woofc-namespace-platform ./cspot

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
	rm -f awsapi_client ${HAND1} *_shepherd.* *.o lib/*.o 3rdparty/*.o
