#include <stdarg.h>
#include <stdio.h>
#ifdef BENCH_TIMING
#include <time.h>
#endif
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "memory.h"
#include "object.h"
#include "natives.h"
#include "vm.h"

VM vm;

static void resetStack(void) {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
}

static void runtimeError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);

  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame* frame = &vm.frames[i];
    ObjFunction* function = frame->function;
    int instruction = (int)(frame->ip - function->chunk.code - 1);
    fprintf(stderr, "[line %d] in ", getLine(&function->chunk, instruction));
    if (function->name == NULL) {
      fprintf(stderr, "script\n");
    } else {
      fprintf(stderr, "%s()\n", function->name->chars);
    }
  }

  resetStack();
}

void initVM(void) {
  vm.stack = GROW_ARRAY(Value, NULL, 0, STACK_INITIAL);
  vm.stackCapacity = STACK_INITIAL;
  resetStack();
  vm.objects = NULL;
  initTable(&vm.globals);
  initTable(&vm.constGlobals);
  initTable(&vm.strings);

  defineNatives();
}

void freeVM(void) {
  FREE_ARRAY(Value, vm.stack, vm.stackCapacity);
  vm.stack = NULL;
  vm.stackCapacity = 0;
  freeTable(&vm.globals);
  freeTable(&vm.constGlobals);
  freeTable(&vm.strings);
  freeObjects();
}

static Value* moveStackPointer(Value* pointer, uintptr_t oldBase) {
  return vm.stack + ((uintptr_t)pointer - oldBase) / sizeof(Value);
}

// Frames and open upvalues point into the stack, so growing it has to move
// those pointers over to the new array.
static void growStack(void) {
  uintptr_t oldBase = (uintptr_t)vm.stack;
  int count = (int)(vm.stackTop - vm.stack);

  int oldCapacity = vm.stackCapacity;
  vm.stackCapacity = GROW_CAPACITY(oldCapacity);
  vm.stack = GROW_ARRAY(Value, vm.stack, oldCapacity, vm.stackCapacity);

  vm.stackTop = vm.stack + count;
  for (int i = 0; i < vm.frameCount; i++) {
    vm.frames[i].slots = moveStackPointer(vm.frames[i].slots, oldBase);
  }
  for (ObjUpvalue* upvalue = vm.openUpvalues; upvalue != NULL;
       upvalue = upvalue->next) {
    upvalue->location = moveStackPointer(upvalue->location, oldBase);
  }
}

void push(Value value) {
  if (vm.stackTop - vm.stack == vm.stackCapacity) growStack();
  *vm.stackTop = value;
  vm.stackTop++;
}

Value pop(void) {
  vm.stackTop--;
  return *vm.stackTop;
}

static Value peek(int distance) {
  return vm.stackTop[-1 - distance];
}

// closure is NULL for a function that was never wrapped.
static bool call(ObjFunction* function, ObjClosure* closure, int argCount) {
  if (argCount != function->arity) {
    runtimeError("Expected %d arguments but got %d.",
        function->arity, argCount);
    return false;
  }

  if (vm.frameCount == FRAMES_MAX) {
    runtimeError("Stack overflow.");
    return false;
  }

  CallFrame* frame = &vm.frames[vm.frameCount++];
  frame->function = function;
  frame->closure = closure;
  frame->ip = function->chunk.code;
  frame->slots = vm.stackTop - argCount - 1;
  return true;
}

static bool callNative(ObjNative* native, int argCount) {
  if (argCount != native->arity) {
    runtimeError("Expected %d arguments but got %d.",
        native->arity, argCount);
    return false;
  }

  Value result;
  if (!native->function(argCount, vm.stackTop - argCount, &result)) {
    runtimeError("%s", AS_CSTRING(result));
    return false;
  }

  vm.stackTop -= argCount + 1;
  push(result);
  return true;
}

static bool callValue(Value callee, int argCount) {
  if (IS_OBJ(callee)) {
    switch (OBJ_TYPE(callee)) {
      case OBJ_CLOSURE: {
        ObjClosure* closure = AS_CLOSURE(callee);
        return call(closure->function, closure, argCount);
      }
      case OBJ_FUNCTION:
        return call(AS_FUNCTION(callee), NULL, argCount);
      case OBJ_NATIVE:
        return callNative(AS_NATIVE(callee), argCount);
      default:
        break; // Non-callable object type.
    }
  }
  runtimeError("Can only call functions and classes.");
  return false;
}

static ObjUpvalue* captureUpvalue(Value* local) {
  ObjUpvalue* prevUpvalue = NULL;
  ObjUpvalue* upvalue = vm.openUpvalues;
  while (upvalue != NULL && upvalue->location > local) {
    prevUpvalue = upvalue;
    upvalue = upvalue->next;
  }

  if (upvalue != NULL && upvalue->location == local) {
    return upvalue;
  }

  ObjUpvalue* createdUpvalue = newUpvalue(local);
  createdUpvalue->next = upvalue;

  if (prevUpvalue == NULL) {
    vm.openUpvalues = createdUpvalue;
  } else {
    prevUpvalue->next = createdUpvalue;
  }

  return createdUpvalue;
}

// Moves every variable at or above last off the stack and into its upvalue.
static void closeUpvalues(Value* last) {
  while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
    ObjUpvalue* upvalue = vm.openUpvalues;
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    vm.openUpvalues = upvalue->next;
  }
}

static bool isFalsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate(void) {
  ObjString* b = AS_STRING(pop());
  ObjString* a = AS_STRING(pop());

  int length = a->length + b->length;
  char* chars = ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';

  ObjString* result = takeString(chars, length);
  push(OBJ_VAL(result));
}

static InterpretResult run(void) {
  CallFrame* frame = &vm.frames[vm.frameCount - 1];

// Challenge 1: the ip lives in a local so the C compiler can keep it in a
// register. It has to be written back to the frame before anything that reads
// frame->ip, which is a call or a runtime error, and reloaded whenever the
// current frame changes. Building with -DFRAME_IP goes back to reading it
// through the frame, so the two can be benchmarked from the same source.
#ifdef FRAME_IP
#define IP (frame->ip)
#define SAVE_IP() ((void)0)
#define LOAD_IP() ((void)0)
#else
  register uint8_t* ip = frame->ip;
#define IP ip
#define SAVE_IP() (frame->ip = ip)
#define LOAD_IP() (ip = frame->ip)
#endif

#define READ_BYTE() (*IP++)
#define READ_SHORT() \
    (IP += 2, (uint16_t)((IP[-2] << 8) | IP[-1]))
#define READ_CONSTANT() \
    (frame->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define RUNTIME_ERROR(...) \
    do { \
      SAVE_IP(); \
      runtimeError(__VA_ARGS__); \
      return INTERPRET_RUNTIME_ERROR; \
    } while (false)
#define BINARY_OP(valueType, op) \
    do { \
      if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
        RUNTIME_ERROR("Operands must be numbers."); \
      } \
      double b = AS_NUMBER(pop()); \
      double a = AS_NUMBER(peek(0)); \
      vm.stackTop[-1] = valueType(a op b); \
    } while (false)

  for (;;) {
    uint8_t instruction;
    switch (instruction = READ_BYTE()) {
      case OP_CONSTANT:
        push(READ_CONSTANT());
        break;
      case OP_CONSTANT_LONG: {
        int index = READ_BYTE();
        index |= READ_BYTE() << 8;
        index |= READ_BYTE() << 16;
        push(frame->function->chunk.constants.values[index]);
        break;
      }
      case OP_NIL:   push(NIL_VAL); break;
      case OP_TRUE:  push(BOOL_VAL(true)); break;
      case OP_FALSE: push(BOOL_VAL(false)); break;
      case OP_POP:   pop(); break;
      case OP_GET_LOCAL: {
        uint8_t slot = READ_BYTE();
        push(frame->slots[slot]);
        break;
      }
      case OP_SET_LOCAL: {
        uint8_t slot = READ_BYTE();
        frame->slots[slot] = peek(0);
        break;
      }
      case OP_GET_LOCAL_LONG: {
        uint16_t slot = READ_SHORT();
        push(frame->slots[slot]);
        break;
      }
      case OP_SET_LOCAL_LONG: {
        uint16_t slot = READ_SHORT();
        frame->slots[slot] = peek(0);
        break;
      }
      case OP_GET_GLOBAL: {
        ObjString* name = READ_STRING();
        Value value;
        if (!tableGet(&vm.globals, OBJ_VAL(name), &value)) {
          RUNTIME_ERROR("Undefined variable '%s'.", name->chars);
        }
        push(value);
        break;
      }
      case OP_DEFINE_GLOBAL: {
        ObjString* name = READ_STRING();
        tableSet(&vm.globals, OBJ_VAL(name), peek(0));
        pop();
        break;
      }
      case OP_SET_GLOBAL: {
        ObjString* name = READ_STRING();
        if (tableSet(&vm.globals, OBJ_VAL(name), peek(0))) {
          tableDelete(&vm.globals, OBJ_VAL(name));
          RUNTIME_ERROR("Undefined variable '%s'.", name->chars);
        }
        break;
      }
      case OP_GET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        push(*frame->closure->upvalues[slot]->location);
        break;
      }
      case OP_SET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        *frame->closure->upvalues[slot]->location = peek(0);
        break;
      }
      case OP_ZERO:  push(NUMBER_VAL(0)); break;
      case OP_ONE:   push(NUMBER_VAL(1)); break;
      case OP_EQUAL: {
        Value b = pop();
        vm.stackTop[-1] = BOOL_VAL(valuesEqual(peek(0), b));
        break;
      }
      case OP_NOT_EQUAL: {
        Value b = pop();
        vm.stackTop[-1] = BOOL_VAL(!valuesEqual(peek(0), b));
        break;
      }
      case OP_GREATER:       BINARY_OP(BOOL_VAL, >); break;
      case OP_GREATER_EQUAL: BINARY_OP(BOOL_VAL, >=); break;
      case OP_LESS:          BINARY_OP(BOOL_VAL, <); break;
      case OP_LESS_EQUAL:    BINARY_OP(BOOL_VAL, <=); break;
      case OP_ADD: {
        if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
          concatenate();
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_NUMBER(pop());
          double a = AS_NUMBER(pop());
          push(NUMBER_VAL(a + b));
        } else {
          RUNTIME_ERROR("Operands must be two numbers or two strings.");
        }
        break;
      }
      case OP_SUBTRACT:      BINARY_OP(NUMBER_VAL, -); break;
      case OP_MULTIPLY:      BINARY_OP(NUMBER_VAL, *); break;
      case OP_DIVIDE:        BINARY_OP(NUMBER_VAL, /); break;
      case OP_NOT:
        vm.stackTop[-1] = BOOL_VAL(isFalsey(peek(0)));
        break;
      case OP_NEGATE:
        if (!IS_NUMBER(peek(0))) {
          RUNTIME_ERROR("Operand must be a number.");
        }
        vm.stackTop[-1] = NUMBER_VAL(-AS_NUMBER(peek(0)));
        break;
      case OP_CONDITIONAL: {
        Value elseValue = pop();
        Value thenValue = pop();
        vm.stackTop[-1] = isFalsey(peek(0)) ? elseValue : thenValue;
        break;
      }
      case OP_PRINT:
        printValue(pop());
        printf("\n");
        break;
      case OP_JUMP: {
        uint16_t offset = READ_SHORT();
        IP += offset;
        break;
      }
      case OP_JUMP_IF_FALSE: {
        uint16_t offset = READ_SHORT();
        if (isFalsey(peek(0))) IP += offset;
        break;
      }
      case OP_LOOP: {
        uint16_t offset = READ_SHORT();
        IP -= offset;
        break;
      }
      case OP_CALL: {
        int argCount = READ_BYTE();
        // The caller's ip is its return address, and a runtime error inside
        // the call reads it for the stack trace.
        SAVE_IP();
        if (!callValue(peek(argCount), argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        LOAD_IP();
        break;
      }
      case OP_CLOSURE: {
        ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure* closure = newClosure(function);
        push(OBJ_VAL(closure));
        for (int i = 0; i < closure->upvalueCount; i++) {
          uint8_t isLocal = READ_BYTE();
          uint16_t index = READ_SHORT();
          if (isLocal) {
            closure->upvalues[i] = captureUpvalue(frame->slots + index);
          } else {
            closure->upvalues[i] = frame->closure->upvalues[index];
          }
        }
        break;
      }
      case OP_CLOSE_UPVALUE:
        closeUpvalues(vm.stackTop - 1);
        pop();
        break;
      case OP_RETURN: {
        Value result = pop();
        closeUpvalues(frame->slots);
        vm.frameCount--;
        if (vm.frameCount == 0) {
          pop();
          return INTERPRET_OK;
        }

        vm.stackTop = frame->slots;
        push(result);
        frame = &vm.frames[vm.frameCount - 1];
        LOAD_IP();
        break;
      }
    }
  }

#undef IP
#undef SAVE_IP
#undef LOAD_IP
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef RUNTIME_ERROR
#undef BINARY_OP
}

InterpretResult interpret(const char* source) {
#ifdef BENCH_TIMING
  clock_t compileStart = clock();
#endif

  ObjFunction* function = compile(source);
  if (function == NULL) return INTERPRET_COMPILE_ERROR;

  push(OBJ_VAL(function));
#ifdef ALWAYS_CLOSURE
  ObjClosure* closure = newClosure(function);
  pop();
  push(OBJ_VAL(closure));
  call(function, closure, 0);
#else
  call(function, NULL, 0);
#endif

#ifdef BENCH_TIMING
  clock_t runStart = clock();
#endif

  InterpretResult result = run();

#ifdef BENCH_TIMING
  fprintf(stderr, "compile %.3fs  run %.3fs\n",
          (double)(runStart - compileStart) / CLOCKS_PER_SEC,
          (double)(clock() - runStart) / CLOCKS_PER_SEC);
#endif

  return result;
}
