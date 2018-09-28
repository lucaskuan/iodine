#include "iodine_caller.h"

#include <ruby/thread.h>
#include <string.h>

static __thread volatile uint8_t iodine_GVL_state;

/* *****************************************************************************
Calling protected Ruby methods
***************************************************************************** */

/* task container */
typedef struct {
  VALUE obj;
  int argc;
  VALUE *argv;
  ID method;
  int exception;
} iodine_rb_task_s;

/* printout backtrace in case of exceptions */
static void *iodine_handle_exception(void *ignr) {
  (void)ignr;
  VALUE exc = rb_errinfo();
  if (exc != Qnil) {
    VALUE msg = rb_funcall2(exc, rb_intern("message"), 0, NULL);
    VALUE exc_class = rb_class_name(CLASS_OF(exc));
    VALUE bt = rb_funcall2(exc, rb_intern("backtrace"), 0, NULL);
    if (TYPE(bt) == T_ARRAY) {
      bt = rb_ary_join(bt, rb_str_new_literal("\n"));
      fprintf(stderr, "Iodine caught an unprotected exception - %.*s: %.*s\n%s",
              (int)RSTRING_LEN(exc_class), RSTRING_PTR(exc_class),
              (int)RSTRING_LEN(msg), RSTRING_PTR(msg), StringValueCStr(bt));
    } else {
      fprintf(stderr,
              "Iodine caught an unprotected exception - %.*s: %.*s\n"
              "No backtrace available.\n",
              (int)RSTRING_LEN(exc_class), RSTRING_PTR(exc_class),
              (int)RSTRING_LEN(msg), RSTRING_PTR(msg));
    }
    rb_backtrace();
    fprintf(stderr, "\n\n");
    rb_set_errinfo(Qnil);
  }
  return (void *)Qnil;
}

/* calls the Ruby method within the protection block */
static VALUE iodine_ruby_caller_perform(VALUE tsk_) {
  iodine_rb_task_s *task = (void *)tsk_;
  return rb_funcall2(task->obj, task->method, task->argc, task->argv);
}

/* wrap the function call in exception handling block (uses longjmp) */
static void *iodine_protect_ruby_call(void *task_) {
  int state = 0;
  VALUE ret = rb_protect(iodine_ruby_caller_perform, (VALUE)(task_), &state);
  if (state) {
    iodine_handle_exception(NULL);
  }
  return (void *)ret;
}

/* *****************************************************************************
API
***************************************************************************** */

/** Calls a C function within the GVL. */
static void *iodine_enterGVL(void *(*func)(void *), void *arg) {
  if (iodine_GVL_state) {
    return func(arg);
  }
  void *rv = NULL;
  iodine_GVL_state = 1;
  rv = rb_thread_call_with_gvl(func, arg);
  iodine_GVL_state = 0;
  return rv;
}

/** Calls a C function outside the GVL. */
static void *iodine_leaveGVL(void *(*func)(void *), void *arg) {
  if (!iodine_GVL_state) {
    return func(arg);
  }
  void *rv = NULL;
  iodine_GVL_state = 0;
  rv = rb_thread_call_without_gvl(func, arg, NULL, NULL);
  iodine_GVL_state = 1;
  return rv;
}

/** Calls a Ruby method on a given object, protecting against exceptions. */
static VALUE iodine_call(VALUE obj, ID method) {
  iodine_rb_task_s task = {
      .obj = obj,
      .argc = 0,
      .argv = NULL,
      .method = method,
  };
  void *rv = iodine_enterGVL(iodine_protect_ruby_call, &task);
  return (VALUE)rv;
}

/** Calls a Ruby method on a given object, protecting against exceptions. */
static VALUE iodine_call2(VALUE obj, ID method, int argc, VALUE *argv) {
  iodine_rb_task_s task = {
      .obj = obj,
      .argc = argc,
      .argv = argv,
      .method = method,
  };
  void *rv = iodine_enterGVL(iodine_protect_ruby_call, &task);
  return (VALUE)rv;
}

/** Returns the GVL state flag. */
static uint8_t iodine_in_GVL(void) { return iodine_GVL_state; }

/** Forces the GVL state flag. */
static void iodine_set_GVL(uint8_t state) { iodine_GVL_state = state; }

/* *****************************************************************************
Caller Initialization
***************************************************************************** */

struct IodineCaller_s IodineCaller = {
    /** Calls a C function within the GVL. */
    .enterGVL = iodine_enterGVL,
    /** Calls a C function outside the GVL. */
    .leaveGVL = iodine_leaveGVL,
    /** Calls a Ruby method on a given object, protecting against exceptions. */
    .call = iodine_call,
    /** Calls a Ruby method on a given object, protecting against exceptions. */
    .call2 = iodine_call2,
    /** Returns the GVL state flag. */
    .in_GVL = iodine_in_GVL,
    /** Forces the GVL state flag. */
    .set_GVL = iodine_set_GVL,
};