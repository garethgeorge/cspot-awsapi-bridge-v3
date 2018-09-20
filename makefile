# default options
CC=gcc
CPPCC=g++ -std=c++11
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

CFLAGS=-pthread -lrt -g -O2 -I${UINC} -I${MINC} -I${SINC} -I.
CPPFLAGS=${CFLAGS}

HAND1=awspy_lambda

all: lambda_client s3_client ${HAND1} utiltest

lambda_client: ${WINC} src/lambda/lambda_client.cpp src/lambda/wpcmds.o src/lambda/function_helpers.o ${MY_LIBS}
	${CPPCC} ${CPPFLAGS} -Wall -o lambda_client src/lambda/lambda_client.cpp \
		${CSPOT_COMMON_LIBS} \
		${MY_LIBS} \
		src/lambda/wpcmds.o \
		src/lambda/function_helpers.o \
		-lulfius -ljansson \
		-lssl -lcrypto
	mkdir -p cspot; cp lambda_client ./cspot 

s3_client: ${WINC} src/s3/s3_client.cpp ${MY_LIBS}
	${CPPCC} ${CPPFLAGS} -Wall -o s3_client src/s3/s3_client.cpp \
		${CSPOT_COMMON_LIBS} \
		${MY_LIBS} \
		-lulfius -ljansson
	mkdir -p cspot; cp s3_client ./cspot 

${HAND1}: ${HAND1}.cpp ${SHEP_SRC} ${WINC} ${LINC} ${LOBJ} ${WOBJ} ${SLIB} ${SINC} ${MY_LIBS}
	sed 's/WOOF_HANDLER_NAME/${HAND1}/g' ${SHEP_SRC} > ${HAND1}_shepherd.c
	${CC} ${CFLAGS} ${PYCFLAGS} -c ${HAND1}_shepherd.c -o ${HAND1}_shepherd.o
	${CPPCC} ${CPPFLAGS} ${PYCFLAGS} -o ${HAND1} ${HAND1}.cpp ${HAND1}_shepherd.o ${CSPOT_COMMON_LIBS} ${MY_LIBS} ${PYLIBS} 
	mkdir -p cspot; cp ${HAND1} ./cspot; cp ${WOOFC}/woofc-container ./cspot; cp ${WOOFC}/woofc-namespace-platform ./cspot


# compile general object files
%.o: %.cpp
	${CPPCC} -c ${CPPFLAGS} $< -o $@

%.o: %.c
	${CC} -c ${CFLAGS} $< -o $@


clean:
	rm -f awsapi_client ${HAND1} *_shepherd.* 
	find . -type f -name '*.o' -delete
