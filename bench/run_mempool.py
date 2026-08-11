# -*- coding: utf-8 -*-
"""
Roda bench.exe N vezes e agrega as MEDIANAS por metrica/alocador.

bench.c nao tem loop de repeticoes interno (cada cenario roda 1x por alocador),
e a vazao do cenario Larson tem variancia alta entre execucoes -- uma unica
corrida nao distingue os alocadores. Este harness roda N vezes e tira a mediana.

Uso:   python run_mempool.py [N]      (N default = 9)
Saida: mempool_bench_latest.log   (N corridas + tabela de medianas ao final)
       mempool_bench_medians.json  (medianas em JSON)
"""
import subprocess, statistics, re, os, json, sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXE  = os.path.join(HERE, "bench.exe")
N    = int(sys.argv[1]) if len(sys.argv) > 1 else 9

ALLOCS = ["sys-malloc", "memop-pool", "memop-lw-dbg", "memop-lw-prod", "rpmalloc", "mimalloc"]

def parse(out):
    section, res = None, {}
    for line in out.splitlines():
        if "== Cenario A" in line: section = "A"; continue
        if "== Cenario B" in line: section = "B"; continue
        if "== Cenario C" in line: section = "C"; continue
        s = line.strip()
        for a in ALLOCS:
            if s.startswith(a + " ") or s == a:
                res[(section, a)] = [float(x) for x in re.findall(r"[-+]?\d+\.?\d*", s[len(a):])]
                break
    return res

runs, allout = [], []
for i in range(N):
    out = subprocess.run([EXE], cwd=HERE, capture_output=True, text=True).stdout
    allout.append(f"########## RUN {i+1}/{N} ##########\n" + out)
    runs.append(parse(out))
    a = runs[-1].get(("A", "memop-pool"))
    print(f"run {i+1}/{N}: memop larson Mop/s = {a[1] if a else '?'}", flush=True)

def med(section, alloc, idx):
    v = [r[(section, alloc)][idx] for r in runs
         if (section, alloc) in r and len(r[(section, alloc)]) > idx]
    return statistics.median(v) if v else 0.0

summary = {"N": N, "A": {}, "B": {}, "C": {}}
for a in ALLOCS:
    summary["A"][a] = [med("A", a, j) for j in range(2)]  # tempo_s, Mop/s
    summary["B"][a] = [med("B", a, j) for j in range(5)]  # aloc_Mop, aloc_ns, free_Mop, free_ns, par_Mop
    summary["C"][a] = [med("C", a, j) for j in range(4)]  # aloc_q, free_q, aloc_cresc, aloc_pior

with open(os.path.join(HERE, "mempool_bench_latest.log"), "w", encoding="utf-8") as f:
    f.write("\n".join(allout))
    f.write(f"\n\n===== MEDIANAS (N={N} runs) =====\n")
    f.write("Cenario A (Larson churn): tempo_s, vazao Mop/s\n")
    for a in ALLOCS: f.write(f"  {a:14s} {summary['A'][a][0]:8.3f} {summary['A'][a][1]:10.2f}\n")
    f.write("Cenario B (64B fixo): aloc_Mop, aloc_ns, free_Mop, free_ns, par_Mop\n")
    for a in ALLOCS:
        b = summary['B'][a]; f.write(f"  {a:14s} {b[0]:8.2f} {b[1]:6.1f} {b[2]:8.2f} {b[3]:6.1f} {b[4]:8.2f}\n")
    f.write("Cenario C (latencia ns): aloc_q, free_q, aloc_cresc, aloc_pior\n")
    for a in ALLOCS:
        c = summary['C'][a]; f.write(f"  {a:14s} {c[0]:8.1f} {c[1]:8.1f} {c[2]:8.1f} {c[3]:10.1f}\n")

with open(os.path.join(HERE, "mempool_bench_medians.json"), "w", encoding="utf-8") as f:
    json.dump(summary, f, indent=2)

print(f"\nwrote mempool_bench_latest.log + mempool_bench_medians.json (N={N})")
