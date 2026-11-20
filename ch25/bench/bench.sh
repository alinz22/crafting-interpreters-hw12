#!/bin/bash
# Compares only wrapping functions that capture something (clox) against
# wrapping every function in a closure like the book (ALWAYS_CLOSURE). Each
# benchmark runs 7 times per build, alternating, and the fastest time is kept.
cd "$(dirname "$0")" || exit 1
FLAGS="-std=c99 -O2"
(cd .. && cc $FLAGS *.c -o bench/clox_some -lm &&
  cc $FLAGS -DALWAYS_CLOSURE *.c -o bench/clox_always -lm) || exit 1

for b in fib declare counter make; do
  best_some=999; best_always=999
  for i in 1 2 3 4 5 6 7; do
    t=$(./clox_always $b.lox); best_always=$(echo "if ($t < $best_always) $t else $best_always" | bc -l)
    t=$(./clox_some $b.lox); best_some=$(echo "if ($t < $best_some) $t else $best_some" | bc -l)
  done
  pct=$(echo "($best_always - $best_some) * 100 / $best_always" | bc -l)
  printf "%-8s always %.3fs   only when needed %.3fs   %.1f%% faster\n" $b $best_always $best_some $pct
done
rm -f clox_some clox_always
