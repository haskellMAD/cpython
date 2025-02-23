
#include "Python.h"
#include "frameobject.h"
#include "pycore_code.h"           // stats
#include "pycore_frame.h"
#include "pycore_object.h"        // _PyObject_GC_UNTRACK()
#include "opcode.h"

int
_PyFrame_Traverse(InterpreterFrame *frame, visitproc visit, void *arg)
{
    Py_VISIT(frame->frame_obj);
    Py_VISIT(frame->f_locals);
    Py_VISIT(frame->f_func);
    Py_VISIT(frame->f_code);
   /* locals */
    PyObject **locals = _PyFrame_GetLocalsArray(frame);
    int i = 0;
    /* locals and stack */
    for (; i <frame->stacktop; i++) {
        Py_VISIT(locals[i]);
    }
    return 0;
}

PyFrameObject *
_PyFrame_MakeAndSetFrameObject(InterpreterFrame *frame)
{
    assert(frame->frame_obj == NULL);
    PyObject *error_type, *error_value, *error_traceback;
    PyErr_Fetch(&error_type, &error_value, &error_traceback);

    PyFrameObject *f = _PyFrame_New_NoTrack(frame->f_code);
    if (f == NULL) {
        Py_XDECREF(error_type);
        Py_XDECREF(error_value);
        Py_XDECREF(error_traceback);
    }
    else {
        f->f_owns_frame = 0;
        f->f_frame = frame;
        frame->frame_obj = f;
        PyErr_Restore(error_type, error_value, error_traceback);
    }
    return f;
}

void
_PyFrame_Copy(InterpreterFrame *src, InterpreterFrame *dest)
{
    assert(src->stacktop >= src->f_code->co_nlocalsplus);
    Py_ssize_t size = ((char*)&src->localsplus[src->stacktop]) - (char *)src;
    memcpy(dest, src, size);
}


static void
take_ownership(PyFrameObject *f, InterpreterFrame *frame)
{
    assert(f->f_owns_frame == 0);
    Py_ssize_t size = ((char*)&frame->localsplus[frame->stacktop]) - (char *)frame;
    memcpy((InterpreterFrame *)f->_f_frame_data, frame, size);
    frame = (InterpreterFrame *)f->_f_frame_data;
    f->f_owns_frame = 1;
    f->f_frame = frame;
    assert(f->f_back == NULL);
    if (frame->previous != NULL) {
        /* Link PyFrameObjects.f_back and remove link through InterpreterFrame.previous */
        PyFrameObject *back = _PyFrame_GetFrameObject(frame->previous);
        if (back == NULL) {
            /* Memory error here. */
            assert(PyErr_ExceptionMatches(PyExc_MemoryError));
            /* Nothing we can do about it */
            PyErr_Clear();
        }
        else {
            f->f_back = (PyFrameObject *)Py_NewRef(back);
        }
        frame->previous = NULL;
    }
    if (!_PyObject_GC_IS_TRACKED((PyObject *)f)) {
        _PyObject_GC_TRACK((PyObject *)f);
    }
}

void
_PyFrame_Clear(InterpreterFrame *frame)
{
    /* It is the responsibility of the owning generator/coroutine
     * to have cleared the enclosing generator, if any. */
    assert(!frame->is_generator);
    if (frame->frame_obj) {
        PyFrameObject *f = frame->frame_obj;
        frame->frame_obj = NULL;
        if (Py_REFCNT(f) > 1) {
            take_ownership(f, frame);
            Py_DECREF(f);
            return;
        }
        Py_DECREF(f);
    }
    assert(frame->stacktop >= 0);
    for (int i = 0; i < frame->stacktop; i++) {
        Py_XDECREF(frame->localsplus[i]);
    }
    Py_XDECREF(frame->frame_obj);
    Py_XDECREF(frame->f_locals);
    Py_DECREF(frame->f_func);
    Py_DECREF(frame->f_code);
}

/* Consumes reference to func */
InterpreterFrame *
_PyFrame_Push(PyThreadState *tstate, PyFunctionObject *func)
{
    PyCodeObject *code = (PyCodeObject *)func->func_code;
    size_t size = code->co_nlocalsplus + code->co_stacksize + FRAME_SPECIALS_SIZE;
    CALL_STAT_INC(frames_pushed);
    InterpreterFrame *new_frame = _PyThreadState_BumpFramePointer(tstate, size);
    if (new_frame == NULL) {
        Py_DECREF(func);
        return NULL;
    }
    _PyFrame_InitializeSpecials(new_frame, func, NULL, code->co_nlocalsplus);
    return new_frame;
}
