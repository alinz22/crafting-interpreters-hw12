#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "memory.h"
#include "natives.h"
#include "object.h"
#include "vm.h"

// Reports a runtime error. The VM prints the message with a stack trace.
static bool fail(Value* result, const char* message) {
  *result = OBJ_VAL(copyString(message, (int)strlen(message)));
  return false;
}

static bool isInteger(Value value) {
  return IS_NUMBER(value) && AS_NUMBER(value) == floor(AS_NUMBER(value));
}

static bool clockNative(int argCount, Value* args, Value* result) {
  *result = NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
  return true;
}

static bool sqrtNative(int argCount, Value* args, Value* result) {
  if (!IS_NUMBER(args[0])) return fail(result, "sqrt() expects a number.");
  if (AS_NUMBER(args[0]) < 0) {
    return fail(result, "sqrt() can't take a negative number.");
  }
  *result = NUMBER_VAL(sqrt(AS_NUMBER(args[0])));
  return true;
}

static bool floorNative(int argCount, Value* args, Value* result) {
  if (!IS_NUMBER(args[0])) return fail(result, "floor() expects a number.");
  *result = NUMBER_VAL(floor(AS_NUMBER(args[0])));
  return true;
}

static bool absNative(int argCount, Value* args, Value* result) {
  if (!IS_NUMBER(args[0])) return fail(result, "abs() expects a number.");
  *result = NUMBER_VAL(fabs(AS_NUMBER(args[0])));
  return true;
}

static bool lenNative(int argCount, Value* args, Value* result) {
  if (!IS_STRING(args[0])) return fail(result, "len() expects a string.");
  *result = NUMBER_VAL(AS_STRING(args[0])->length);
  return true;
}

// Turns any value into the same text print would show.
static bool strNative(int argCount, Value* args, Value* result) {
  Value value = args[0];
  if (IS_STRING(value)) {
    *result = value;
    return true;
  }

  char buffer[64];
  int length;
  switch (value.type) {
    case VAL_BOOL:
      length = snprintf(buffer, sizeof(buffer), AS_BOOL(value) ? "true" : "false");
      break;
    case VAL_NIL:
      length = snprintf(buffer, sizeof(buffer), "nil");
      break;
    case VAL_NUMBER:
      length = snprintf(buffer, sizeof(buffer), "%g", AS_NUMBER(value));
      break;
    default:
      if (IS_CLOSURE(value) || IS_FUNCTION(value)) {
        ObjFunction* function = IS_CLOSURE(value)
            ? AS_CLOSURE(value)->function : AS_FUNCTION(value);
        length = snprintf(buffer, sizeof(buffer), "<fn %s>",
                          function->name->chars);
      } else {
        length = snprintf(buffer, sizeof(buffer), "<native fn>");
      }
      break;
  }
  // A long function name gets cut off rather than overflowing the buffer.
  if (length >= (int)sizeof(buffer)) length = sizeof(buffer) - 1;
  *result = OBJ_VAL(copyString(buffer, length));
  return true;
}

// Parses a string as a number, or gives nil if it isn't one.
static bool numNative(int argCount, Value* args, Value* result) {
  if (!IS_STRING(args[0])) return fail(result, "num() expects a string.");
  ObjString* string = AS_STRING(args[0]);
  char* end;
  double value = strtod(string->chars, &end);
  bool valid = string->length > 0 && end == string->chars + string->length;
  *result = valid ? NUMBER_VAL(value) : NIL_VAL;
  return true;
}

// substr(s, start, end) gives the characters from start up to but not
// including end.
static bool substrNative(int argCount, Value* args, Value* result) {
  if (!IS_STRING(args[0])) {
    return fail(result, "substr() expects a string as its first argument.");
  }
  if (!isInteger(args[1]) || !isInteger(args[2])) {
    return fail(result, "substr() expects whole numbers for start and end.");
  }

  ObjString* string = AS_STRING(args[0]);
  double start = AS_NUMBER(args[1]);
  double end = AS_NUMBER(args[2]);
  if (start < 0 || end > string->length || start > end) {
    return fail(result, "substr() range is out of bounds.");
  }

  *result = OBJ_VAL(copyString(string->chars + (int)start,
                               (int)(end - start)));
  return true;
}

// Reads one line from standard input without the newline, or nil at the end.
static bool inputNative(int argCount, Value* args, Value* result) {
  char line[1024];
  if (fgets(line, sizeof(line), stdin) == NULL) {
    *result = NIL_VAL;
    return true;
  }

  int length = (int)strcspn(line, "\r\n");
  *result = OBJ_VAL(copyString(line, length));
  return true;
}

static void defineNative(const char* name, NativeFn function, int arity) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNative(function, arity)));
  tableSet(&vm.globals, vm.stack[0], vm.stack[1]);
  pop();
  pop();
}

void defineNatives(void) {
  defineNative("clock", clockNative, 0);
  defineNative("sqrt", sqrtNative, 1);
  defineNative("floor", floorNative, 1);
  defineNative("abs", absNative, 1);
  defineNative("len", lenNative, 1);
  defineNative("str", strNative, 1);
  defineNative("num", numNative, 1);
  defineNative("substr", substrNative, 3);
  defineNative("input", inputNative, 0);
}
