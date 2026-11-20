#ifndef clox_vm_h
#define clox_vm_h

#include "object.h"
#include "table.h"
#include "value.h"

#define FRAMES_MAX 64
#define STACK_INITIAL 256

typedef struct {
  ObjFunction* function;
  // Challenge 1: NULL when the function captures nothing and was never
  // wrapped in a closure.
  ObjClosure* closure;
  uint8_t* ip;
  Value* slots;
} CallFrame;

typedef struct {
  CallFrame frames[FRAMES_MAX];
  int frameCount;

  // The stack grows on demand, so frames' slots pointers and open upvalues get
  // moved whenever it is reallocated.
  Value* stack;
  int stackCapacity;
  Value* stackTop;
  Table globals;
  // Names of globals declared with const. The compiler checks these, and they
  // live here so they carry over from one REPL line to the next.
  Table constGlobals;
  Table strings;
  // Sorted with the upvalue nearest the top of the stack first.
  ObjUpvalue* openUpvalues;
  Obj* objects;
} VM;

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR
} InterpretResult;

extern VM vm;

void initVM(void);
void freeVM(void);
InterpretResult interpret(const char* source);
void push(Value value);
Value pop(void);

#endif
