cd "$TEST"
mkdir -p a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z
mkdir -p a/b/c/d/e/f/g/h/i/j/k/l/m/0/1/2/3/4/5/6/7/8/9/A/B/C

ulimit -n $((NOPENFD + 10))
bfs_diff . -execdir echo {} \;
