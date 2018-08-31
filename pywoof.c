// see: https://gist.github.com/physacco/2e1b52415f3a964ad2a542a99bebed8f

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include <Python.h>

#include "woofc.h"
#include "woofc-host.h"

static PyObject *
woof_init(PyObject *self, PyObject *args) {
    PyThreadState *state = PyEval_SaveThread();
    int ret = WooFInit();
    PyEval_RestoreThread(state);

    return Py_BuildValue("i", ret);
}

static PyObject *
woof_create(PyObject *self, PyObject *args) {
    char *WooFname;
    int size;
    int elements;

    // see https://docs.python.org/2.0/ext/parseTuple.html
    if (!PyArg_ParseTuple(args, "sii", &WooFname, &size, &elements))
        return NULL;

    PyThreadState *state = PyEval_SaveThread();
    int ret = WooFCreate(WooFname, size, elements);
    PyEval_RestoreThread(state);

    return Py_BuildValue("i", ret);
}

static PyObject *
woof_put(PyObject *self, PyObject *args) {
    char *WooFname;
    char *handlerName;
    int buffer_size;
    int buffer_expected_size;
    char *buffer;

    if (!PyArg_ParseTuple(args, "sss#", &WooFname, &handlerName, &buffer, &buffer_size, &buffer_expected_size))
        return NULL;

    assert(buffer_size == buffer_expected_size);

    PyThreadState *state = PyEval_SaveThread();
    int ret = WooFPut(WooFname, handlerName, (void *)buffer);
    PyEval_RestoreThread(state);
    
    return Py_BuildValue("i", ret);
}

static PyObject *
woof_getlatestseqno(PyObject *self, PyObject *args) {
    char *WooFname;
    
    if (!PyArg_ParseTuple(args, "s", &WooFname))
        return NULL;
    
    PyThreadState *state = PyEval_SaveThread();
    int seqno = WooFGetLatestSeqno(WooFname);
    PyEval_RestoreThread(state);

    return Py_BuildValue("i", seqno);
}

static PyObject *
woof_spinfornewseqno(PyObject *self, PyObject *args) {
    char *WooFname;
    int startseqno, seqno;
    int timeout; // timeout specified in milliseconds, this is approximate we 
                 // only actually respect the number of times we have done the check
                 // if the CPU gets behind we too fall behind

    if (!PyArg_ParseTuple(args, "sii", &WooFname, &startseqno, &timeout))
        return NULL;
    
    PyThreadState *state = PyEval_SaveThread();

    seqno = startseqno;

    long sleep_time = 1L;
    int bump_time = 1;

    while (timeout > 0 && (seqno = WooFGetLatestSeqno(WooFname)) == startseqno) {
        if (nanosleep((const struct timespec[]){{0, (long)sleep_time * 1000000L}}, NULL) != 0) {
            fprintf(stderr, "Fatal error: failure to sleep\n");
            return Py_BuildValue("i", -1);
        }
        timeout -= sleep_time;
        if (bump_time == 0) {
            bump_time = 1;
            sleep_time <<= 1;
        } else {
            bump_time--;
        }
    }

    PyEval_RestoreThread(state);

    return Py_BuildValue("i", seqno);
}


/*
    arguments: woof name, element size, seq no
*/
static PyObject *
woof_get(PyObject *self, PyObject *args) {
    char *WooFname;
    int element_size;
    int seq_no;

    if (!PyArg_ParseTuple(args, "sii", &WooFname, &element_size, &seq_no))
        return NULL;

    char *buffer = malloc(element_size + 1); // allow for null termination or w/e
    if (!buffer) 
        return NULL;
    memset(buffer, 0, sizeof(element_size + 1));

    int wgetret = WooFGet(WooFname, buffer, seq_no);
    PyObject *returnValue = Py_BuildValue("(i,s#)", wgetret, buffer, element_size);
    free(buffer);

    return returnValue;
}

static PyMethodDef woof_methods[] = {
    {"init", woof_init, METH_VARARGS, "initialize WooF"},
    {"create", woof_create, METH_VARARGS, "create WooF, takes (woof name, size, number of entries)"},
    {"put", woof_put, METH_VARARGS, "puts a buffer into the WooF, it must be exactly the size configured or behavior will be undefined"},
    {"getlatestseqno", woof_getlatestseqno, METH_VARARGS, "gets the latest sequence number from the woof"},
    {"spinfornewseqno", woof_spinfornewseqno, METH_VARARGS, "spins until a new seqno is available, takes woof name, cur seq no, and a timeout value that is treated as a suggestion"},
    {"get", woof_get, METH_VARARGS, "takes the woofname, element size, latest sequence number (as returned from getlatestseqno)"},
    {NULL, NULL, 0, NULL}
};

// Module definition
// The arguments of this structure tell Python what to call your extension,
// what it's methods are and where to look for it's method definitions
static struct PyModuleDef woof_definition = { 
    PyModuleDef_HEAD_INIT,
    "woof",
    "A python module that exposes an API for interacting with WooFs",
    -1, 
    woof_methods
};

/*
	Initialize the module for embedded python
*/
extern PyObject*
PyInit_woof(void)
{
    return PyModule_Create(&woof_definition);
}