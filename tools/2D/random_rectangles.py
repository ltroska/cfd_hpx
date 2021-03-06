#!/usr/bin/env python
from __future__ import print_function
from grid import *
import sys
import random

if len(sys.argv) < 7:
  print("Usage:", sys.argv[0] , "<output grid> <#cols> <#rows> <#rectangles> <minimal side length> <maximal side length> [distance=0] [include_boundary=0] [maximal number of failures=2*num_rectangles]", file=sys.stderr)
  sys.exit(0)

outfile_path = sys.argv[1]
num_cols = int(sys.argv[2])
num_rows = int(sys.argv[3])
num_rectangles = int(sys.argv[4])
min_length = int(sys.argv[5]) - 1
max_length = int(sys.argv[6]) - 1

distance = 0
if len(sys.argv) >= 8:
  distance = int(sys.argv[7])
  
include_boundary = False
if len(sys.argv) >= 9:
  include_boundary = (int(sys.argv[8]) == 1)
  
max_fails = 2 * num_rectangles
if len(sys.argv) >= 10:
  max_fails = int(sys.argv[9])
  
g = grid(num_cols, num_rows)
count = 0
failed = 0
resets = 0
while count < num_rectangles:
  x1 = random.randint(1, num_cols - 2)
  y1 = random.randint(1, num_rows - 2)
  
  x_length = random.randint(min_length, max_length)
  y_length = random.randint(min_length, max_length)
    
  success = g.add_shape(rectangle([x1, y1], [x1 + x_length, y1 + y_length]), distance, include_boundary) 
  
  count += success
  failed += 1 - success
  
  if failed > max_fails:
    print("Resetting...")
    resets += 1
    count = 0
    failed = 0
    g.reset()
   
g.apply_shapes()
g.write_to(outfile_path)
print("Wrote grid of size", num_cols, "x", num_rows, "after", resets, "resets with", count, "insertions and", failed, "failures!")