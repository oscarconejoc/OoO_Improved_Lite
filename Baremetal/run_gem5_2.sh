export PYTHONPATH="/home/andres/maestria/mp6160-vp/gem5/configs:${PYTHONPATH:-}"
cd /home/andres/maestria/mp6160-vp/gem5

/home/andres/maestria/mp6160-vp/gem5/build/RISCV/gem5.opt \
  --outdir=/home/andres/maestria/repo_bare_metal_sw/gem5/out \
  /home/andres/maestria/mp6160-vp/gem5/configs/deprecated/example/riscv/fs_linux.py \
  --bare-metal \
  --kernel=/home/andres/maestria/repo_bare_metal_sw/build/bare_metal_test \
  --cpu-type=AtomicSimpleCPU \
  --mem-size=512MB \
  --riscv-32bits --maxinsts=5000000 
