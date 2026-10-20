import json, random, subprocess, sys

DROP = ["Queue", "Big", "x"]

def drop(v):
    if isinstance(v, dict):
        return {k: (None if k in DROP else drop(x)) for k, x in v.items()}
    if isinstance(v, list):
        return [drop(x) for x in v]
    return v

def rstr(r):
    pool = ['a', 'Queue', '"', '\\', '{', '}', '[', ']', ':', ',', ' ', 'x', 'é', '\n']
    return ''.join(r.choice(pool) for _ in range(r.randint(0, 8)))

def rval(r, d=0):
    t = r.randint(0, 6 if d < 4 else 3)
    if t == 0: return r.randint(-1000, 1000) * r.choice([1, 0.5])
    if t == 1: return r.choice([True, False, None])
    if t in (2, 3): return rstr(r)
    if t == 4: return [rval(r, d + 1) for _ in range(r.randint(0, 4))]
    return {r.choice(DROP + ['a', 'b', 'Queue2', 'Que', rstr(r)]): rval(r, d + 1)
            for _ in range(r.randint(0, 5))}

def run(text, seed, names):
    p = subprocess.run(['/tmp/json_skip_test', str(seed)] + names,
                       input=text.encode(), capture_output=True)
    assert p.returncode == 0, p.returncode
    return p.stdout.decode()

r = random.Random(1)
for i in range(3000):
    v = rval(r)
    text = json.dumps(v, indent=r.choice([None, 1]), ensure_ascii=r.random() < 0.5)
    got = json.loads(run(text, i, DROP))
    assert got == drop(v), (text, got)
print('random: 3000 cases OK')

if len(sys.argv) > 1:
    names = ["NowPlayingQueueFullItems", "NowPlayingQueue", "MediaStreams", "MediaSources",
             "Chapters", "People", "ImageBlurHashes", "ExternalUrls", "Overview",
             "TranscodingInfo", "Capabilities", "SupportedCommands"]
    text = open(sys.argv[1], encoding='utf-8').read()
    DROP[:] = names
    out = run(text, 7, names)
    assert json.loads(out) == drop(json.loads(text))
    print(f'{sys.argv[1]}: {len(text):,} -> {len(out):,} bytes, OK')
