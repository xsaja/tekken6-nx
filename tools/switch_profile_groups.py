import re, sys, subprocess, collections
log, elf = sys.argv[1], sys.argv[2]
groups = collections.defaultdict(collections.Counter); cpu = collections.defaultdict(float); nsamp = 0
cur = None; sample = 0
for line in open(log, encoding='utf-8', errors='replace'):
    if line.startswith('==== '):
        sample += 1; cur = None; continue
    if sample < 12: continue
    m = re.match(r'-- thread \d+ "([^"]*)": CPU ([\d.]+)%.*preferred core (-?\d+)', line)
    if m:
        name, c, core = m.groups()
        g = 'main' if name.startswith('Main XThread') else 'render' if (name == '?' and core == '1') else \
            'workers' if name.startswith('XThread') else name
        cur = g; cpu[g] += float(c); continue
    m = re.match(r'\s+[\d.]+%\s+pc image\+0x([0-9a-f]+) x(\d+)', line)
    if m and cur: groups[cur][int(m.group(1), 16)] += int(m.group(2))
n = sample - 11
A2L = 'C:/devkitPro/devkitA64/bin/aarch64-none-elf-addr2line.exe'
for g in ['main', 'render', 'workers', 'XMA Decoder (F8']:
    c = groups[g]; tot = sum(c.values())
    funcs = collections.Counter()
    addrs = list(c)
    NL = chr(10)
    out = subprocess.run([A2L, '-f', '-C', '-e', elf], input=NL.join(hex(a) for a in addrs) + NL,
                         capture_output=True, text=True).stdout.split(NL)
    for i, a in enumerate(addrs):
        f = out[2*i] if 2*i < len(out) else '?'
        f = re.sub(r'\(.*', '', f.replace('(anonymous namespace)', 'anon'))[:90]
        funcs[f] += c[a]
    print(f'== {g}: avg CPU {cpu[g]/n:.0f}% over {n} samples, {tot} pc samples')
    for f, k in funcs.most_common(14): print(f'  {100*k/tot:5.1f}%  {f}')
