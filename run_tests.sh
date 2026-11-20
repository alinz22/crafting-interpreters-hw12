#!/bin/bash
# Builds three versions of the VM with the sanitizers on and checks what
# programs print. clox only wraps functions that capture something in a closure
# (Challenge 1), clox_always wraps every function like the book does, and
# clox_frameip reads ip through the CallFrame like before chapter 24's first
# challenge. They all have to behave the same.
cd "$(dirname "$0")" || exit 1

failed=0
FLAGS="-std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -g -fsanitize=address,undefined"
export ASAN_OPTIONS=abort_on_error=0

(cd ch25 && cc $FLAGS *.c -o clox -lm &&
  cc $FLAGS -DALWAYS_CLOSURE *.c -o clox_always -lm &&
  cc $FLAGS -DFRAME_IP *.c -o clox_frameip -lm) ||
  { echo "BUILD FAILED"; exit 1; }

# expect <binary> <expected exit code> <expected output> <program> [stdin]
expect() {
  local file actual code
  file=$(mktemp)
  printf '%s' "$4" > "$file"
  actual=$(printf '%s' "$5" | "$1" "$file" 2>&1)
  code=$?
  rm -f "$file"
  if [ "$code" != "$2" ] || [ "$actual" != "$3" ]; then
    echo "FAIL [$1] $4"
    echo "  expected ($2): $3"
    echo "  actual   ($code): $actual"
    failed=1
  fi
}

for vm in ch25/clox ch25/clox_always ch25/clox_frameip; do
  # Chapters 22 and 23 still work.
  expect $vm 0 'inner
outer' '{ var a = "outer"; { var a = "inner"; print a; } print a; }'
  expect $vm 65 "[line 1] Error at 'a': Can't read local variable in its own initializer." \
    '{ var a = "outer"; { var a = a; } }'
  expect $vm 70 "Undefined variable 'nope'.
[line 1] in script" '{ var a = 1; } print a = nope;'
  expect $vm 65 "[line 1] Error at '=': Can't assign to a constant." '{ const c = 3; c = 4; }'
  expect $vm 0 'zero
one
other' 'for (var i = 0; i < 3; i = i + 1) {
  switch (i) { case 0: print "zero"; case 1: print "one"; default: print "other"; }
}'
  expect $vm 0 '0
2
bok' '{
  var before = "b";
  for (var i = 0; i < 3; i = i + 1) {
    var a = 1;
    switch (i) { case 1: { var z = 2; continue; } default: print i; }
  }
  var after = "ok";
  print before + after;
}'

  # Slot zero now belongs to the function, so 65,535 locals is the limit.
  many=$(mktemp)
  python3 -c "
for b in range(256):
    print('{')
    for i in range(256 if b < 255 else 255): print('var v%d = %d;' % (b * 256 + i, b * 256 + i))
print('print v65534 + v0 + v300;')
print('}' * 256)" > "$many"
  out=$("$vm" "$many" 2>&1)
  [ "$out" = "65834" ] || { echo "FAIL [$vm] 65535 locals: $out"; failed=1; }
  python3 -c "
print('{')
for i in range(65536): print('var v%d;' % i)
print('}')" > "$many"
  out=$("$vm" "$many" 2>&1 | head -1)
  [ "$out" = "[line 65537] Error at 'v65535': Too many local variables in function." ] ||
    { echo "FAIL [$vm] local limit: $out"; failed=1; }

  # Chapter 24: functions, calls, returns.
  expect $vm 0 '6765
Do stuff
nil
<fn fib>
<native fn>
22' 'fun fib(n) { if (n < 2) return n; return fib(n - 2) + fib(n - 1); }
print fib(20);
fun noReturn() { print "Do stuff"; }
print noReturn();
print fib;
print clock;
fun sum(a, b, c) { return a + b + c; }
print 4 + sum(5, 6, 7);'
  # A local function, and returning from inside a loop and a switch.
  expect $vm 0 '120
found 3
two' '{
  fun fact(n) { var r = 1; while (n > 1) { r = r * n; n = n - 1; } return r; }
  print fact(5);
}
fun find(x) {
  for (var i = 0; i < 10; i = i + 1) { var sq = i * i; if (sq == x) return "found " + str(i); }
  return "missing";
}
print find(9);
fun name(n) { switch (n) { case 1: return "one"; case 2: return "two"; } return "many"; }
print name(2);'
  expect $vm 0 'a
b' 'fun outer() { var x = "a"; fun inner(y) { return y; } print inner(x); return "b"; } print outer();'
  # A continue in a function body is checked against that function's loops.
  expect $vm 65 "[line 1] Error at 'continue': Can't use 'continue' outside of a loop." \
    'while (false) { fun f() { continue; } }'
  expect $vm 65 "[line 1] Error at 'return': Can't return from top-level code." 'return 1;'
  # A local function can call itself by name now that it can capture itself.
  expect $vm 0 '120' '{ fun fact(n) { if (n <= 1) return 1; return n * fact(n - 1); } print fact(5); }'
  expect $vm 70 'Expected 0 arguments but got 2.
[line 4] in c()
[line 2] in b()
[line 1] in a()
[line 7] in script' 'fun a() { b(); }
fun b() { c(); }
fun c() {
  c("too", "many");
}

a();'
  expect $vm 70 'Can only call functions and classes.
[line 1] in script' 'var notAFunction = 123; notAFunction();'
  expect $vm 70 'Operands must be numbers.
[line 2] in f()
[line 4] in script' 'fun f(a) {
  return a - "x";
}
f(1);'
  # The runaway call shows the right line in every frame. If ip weren't saved
  # before a call, the lines would be wrong.
  many=$(mktemp)
  printf 'fun inf(n) {\n  return inf(n + 1);\n}\ninf(0);\n' > "$many"
  out=$("$vm" "$many" 2>&1)
  [ "$(echo "$out" | head -2)" = "Stack overflow.
[line 2] in inf()" ] && [ "$(echo "$out" | tail -1)" = "[line 4] in script" ] &&
    [ "$(echo "$out" | wc -l | tr -d ' ')" = 65 ] ||
    { echo "FAIL [$vm] stack overflow trace"; failed=1; }
  # Each frame has 300 locals, so the value stack is reallocated in the middle
  # of the recursion. Every frame's slots pointer has to move with it.
  python3 -c "
print('fun deep(n) {')
for i in range(300): print('  var v%d = n;' % i)
print('  if (n > 0) { var r = deep(n - 1); return r + v299 + v0; }')
print('  return v299;')
print('}')
print('print deep(60);')" > "$many"
  out=$("$vm" "$many" 2>&1)
  [ "$out" = "3660" ] || { echo "FAIL [$vm] stack growth: $out"; failed=1; }
  rm -f "$many"

  # Challenge 2: natives check arity.
  expect $vm 70 'Expected 0 arguments but got 1.
[line 1] in script' 'print clock(1);'
  expect $vm 70 'Expected 3 arguments but got 1.
[line 1] in f()
[line 1] in script' 'fun f() { return substr("abc"); } f();'

  # Challenge 3: natives report runtime errors.
  expect $vm 70 'sqrt() expects a number.
[line 2] in root()
[line 4] in script' 'fun root(x) {
  return sqrt(x);
}
print root("nine");'
  expect $vm 70 "sqrt() can't take a negative number.
[line 1] in script" 'print sqrt(-4);'
  expect $vm 70 'substr() range is out of bounds.
[line 1] in script' 'print substr("abc", 1, 9);'
  expect $vm 70 'substr() expects whole numbers for start and end.
[line 1] in script' 'print substr("abc", 0.5, 2);'
  expect $vm 70 'len() expects a string.
[line 1] in script' 'print len(12);'

  # Challenge 4: more natives.
  expect $vm 0 '4
2
3.5
5
3.5|nil|true|<fn f>|<native fn>|hi
12.5
nil
nil
wor
3' 'fun f() {}
print sqrt(16); print floor(2.9); print abs(-3.5); print len("hello");
print str(3.5) + "|" + str(nil) + "|" + str(true) + "|" + str(f) + "|" + str(clock) + "|" + str("hi");
print num("12.5"); print num("12x"); print num("");
print substr("hello world", 6, 9);
print len(substr("abc", 0, 3));'
  expect $vm 0 'hi Ann
nil' 'print "hi " + input(); print input();' 'Ann
'
  for prog in ch25/examples/*.lox; do
    want="ch25/examples/$(basename "$prog" .lox).out"
    in="ch25/examples/$(basename "$prog" .lox).in"
    if [ -f "$in" ]; then out=$("$vm" "$prog" < "$in" 2>&1); else out=$("$vm" "$prog" 2>&1); fi
    [ "$out" = "$(cat "$want")" ] || { echo "FAIL [$vm] $prog"; echo "$out"; failed=1; }
  done
done

# Chapter 25 behavior and challenges, on every build.
for vm in ch25/clox ch25/clox_always ch25/clox_frameip; do
  expect $vm 0 'outer
doughnut
bagel
return from outer
create inner closure
value
assigned
updated' 'var x = "global";
fun outer() { var x = "outer"; fun inner() { print x; } inner(); }
outer();
fun makeClosure(value) { fun closure() { print value; } return closure; }
var doughnut = makeClosure("doughnut");
var bagel = makeClosure("bagel");
doughnut();
bagel();
fun outer2() {
  var x = "value";
  fun middle() { fun inner() { print x; } print "create inner closure"; return inner; }
  print "return from outer";
  return middle;
}
var mid = outer2();
var in = mid();
in();
fun outer3() { var x = "before"; fun inner() { x = "assigned"; } inner(); print x; }
outer3();
var globalSet; var globalGet;
fun main() { var a = "initial"; fun set() { a = "updated"; } fun get() { print a; } globalSet = set; globalGet = get; }
main();
globalSet();
globalGet();'
  # Two closures made in different blocks see different variables, and a
  # variable captured in a block is closed when the block ends.
  expect $vm 0 'one
two
1
2' 'var globalOne; var globalTwo;
fun main() {
  { var a = "one"; fun one() { print a; } globalOne = one; }
  { var a = "two"; fun two() { print a; } globalTwo = two; }
}
main(); globalOne(); globalTwo();
var c1; var c2;
{ var n = 0; fun inc() { n = n + 1; return n; } c1 = inc; c2 = inc; }
print c1(); print c2();'
  expect $vm 65 "[line 1] Error at '=': Can't assign to a constant." \
    'fun f() { const k = 1; fun g() { fun h() { k = 2; } } }'
  expect $vm 0 '<fn get>' 'fun make() { var a = 1; fun get() { return a; } return get; } print str(make());'
  # A closure can capture a local past slot 255.
  many=$(mktemp)
  python3 -c "
print('fun big() {')
for i in range(300): print('  var v%d = %d;' % (i, 7 if i == 299 else 1))
print('  fun get() { return v299 + v0; }')
print('  return get;')
print('}')
print('print big()();')" > "$many"
  out=$("$vm" "$many" 2>&1)
  [ "$out" = "8" ] || { echo "FAIL [$vm] capture slot 299: $out"; failed=1; }
  # Open upvalues point into the stack, so they have to move when it grows.
  python3 -c "
print('fun deep(n, inc) {')
for i in range(300): print('  var v%d = n;' % i)
print('  if (n > 0) return deep(n - 1, inc);')
print('  return inc();')
print('}')
print('fun counter() { var c = 10; fun inc() { c = c + 1; return c; } print deep(40, inc); print inc(); }')
print('counter();')" > "$many"
  out=$("$vm" "$many" 2>&1)
  [ "$out" = "11
12" ] || { echo "FAIL [$vm] upvalue across stack growth: $out"; failed=1; }
  rm -f "$many"

  # Challenge 2: each iteration of a for loop gets its own variable.
  expect $vm 0 '1
2' 'var globalOne; var globalTwo;
fun main() {
  for (var a = 1; a <= 2; a = a + 1) {
    fun closure() { print a; }
    if (globalOne == nil) globalOne = closure; else globalTwo = closure;
  }
}
main(); globalOne(); globalTwo();'
  # Changing the loop variable in the body still affects the loop.
  expect $vm 0 '3
6
9
12' 'for (var i = 0; i < 10; ) { i = i + 3; print i; }'
  # continue keeps the body change and closes a captured copy.
  expect $vm 0 'body 0
body 5
0
3
4' 'var f1; var f2; var f3;
for (var k = 0; k < 6; k = k + 1) {
  fun get() { return k; }
  if (k == 0) f1 = get;
  if (k == 1) { k = 3; f2 = get; continue; }
  if (k == 4) { f3 = get; continue; }
  print "body " + str(k);
}
print f1(); print f2(); print f3();'
  # A captured local inside the body is closed by continue too.
  expect $vm 0 'a0
a1
a2' 'var g0; var g1; var g2;
var i = 0;
while (i < 3) {
  var name = "a" + str(i);
  fun get() { return name; }
  if (i == 0) g0 = get; if (i == 1) g1 = get; if (i == 2) g2 = get;
  i = i + 1;
  continue;
}
print g0(); print g1(); print g2();'
  expect $vm 0 '02
12' 'for (var m = 0; m < 2; m = m + 1) { var j = 0; while (j < 2) { j = j + 1; if (j == 1) continue; print str(m) + str(j); } }'
done

if [ "$failed" = 0 ]; then
  echo "all tests passed"
else
  exit 1
fi
