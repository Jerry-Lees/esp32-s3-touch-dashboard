import random, subprocess, sys, os, glob
random.seed(int(sys.argv[1]))
seeds = [f for f in glob.glob("img/*") + glob.glob("img16/*") if not f.endswith(".ref.png") and "big" not in f]
crashes = hangs = errs = oks = 0
for i in range(int(sys.argv[2])):
    src = random.choice(seeds)
    data = bytearray(open(src, "rb").read())
    kind = random.random()
    if kind < 0.2:
        data = data[:random.randrange(len(data))]
    else:
        for _ in range(random.choice([1, 2, 4, 16, 64])):
            p = random.randrange(len(data))
            r = random.random()
            if r < 0.6: data[p] = random.randrange(256)
            elif r < 0.8: data[p] = random.choice([0, 0xFF, 0x7F, 0x80])
            else: data[p:p] = bytes(random.randrange(256) for _ in range(random.randrange(1, 8)))
    ext = os.path.splitext(src)[1]
    fn = f"fuzz/case{ext}"
    open(fn, "wb").write(data)
    try:
        r = subprocess.run(["./picdec", fn, str(random.choice([0, 1, 3]))], capture_output=True, timeout=10)
    except subprocess.TimeoutExpired:
        hangs += 1; open(f"fuzz/hang{hangs}{ext}", "wb").write(data); continue
    if r.returncode < 0 or r.returncode > 1:
        crashes += 1; open(f"fuzz/crash{crashes}{ext}", "wb").write(data)
        print("CRASH", src, r.returncode, r.stdout, r.stderr[:200])
    elif r.returncode == 1: errs += 1
    else: oks += 1
print(f"crashes {crashes} hangs {hangs} rejected {errs} decoded {oks}")
