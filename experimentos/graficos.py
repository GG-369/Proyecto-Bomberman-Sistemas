"""Gráficas a partir de los datos medidos (data/*.csv, data/resumen_*.txt)."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
import csv, re, os, sys, statistics as st, collections, json

D, OUT = sys.argv[1], sys.argv[2]
plt.rcParams.update({"font.family": "Arial", "font.size": 10.5, "axes.spines.top": False, "axes.spines.right": False})
C = {"P0": "#3b5b92", "P1": "#2e7d5b", "P2": "#a0522d", "P3": "#8e3b6f", "bad": "#b03a2e", "good": "#2e7d5b", "grey": "#8a8a8a"}
summary = {}

def rows(name): return list(csv.DictReader(open(os.path.join(D, name), encoding="utf-8")))

# ---------------------------------------------------------------- diagrama temporal de ticks (casos 01 y 02)
def parse_resumen(fn):
    out = []
    for line in open(os.path.join(D, fn), encoding="utf-8"):
        if not line.startswith("RESUMEN"): continue
        kv = dict(re.findall(r"(\w+)=(\S+)", line))
        out.append({"tick": int(kv["tick"]), "turno": kv["turno"], "bomber": kv["bomber"], "vidas": int(kv["vidas"]),
                    "bombas": int(kv["bombas"]), "bloques": int(kv["bloques"]), "prio": kv["prio"]})
    return out

def gantt():
    cases = [("resumen_01.txt", "Caso 01 · prioridades 40/30 (sección 8.5)"), ("resumen_02.txt", "Caso 02 · prioridades 30/30 (Round Robin)")]
    fig, axs = plt.subplots(2, 1, figsize=(13, 6.4), dpi=200)
    for ax, (fn, title) in zip(axs, cases):
        R = parse_resumen(fn); n = len(R)
        lanes = ["P1 · acción", "P2 · acción", "P3 · bombas", "P0 · cierre"]
        ax.set_xlim(0.5, n + 0.5); ax.set_ylim(-0.7, 3.9); ax.invert_yaxis()
        ax.set_yticks(range(4)); ax.set_yticklabels(lanes)
        ax.set_xticks(range(1, n + 1)); ax.set_xlabel("tick")
        ax.set_title(title, loc="left", fontsize=11.5, fontweight="bold")
        prev = {"vidas": R[0]["vidas"] if R else 3, "bombas": 0, "bloques": 0, "prio": R[0]["prio"] if R else ""}
        prev_v = 3
        for r in R:
            t = r["tick"]
            lane = 0 if r["turno"] == "P1" else 1
            col = C["P1"] if lane == 0 else C["P2"]
            ax.add_patch(Rectangle((t - 0.42, lane - 0.33), 0.84, 0.66, fc=col, ec="white"))
            ax.text(t, lane, r["bomber"] if lane == 0 else "mueve", ha="center", va="center", fontsize=8, color="white")
            ax.add_patch(Rectangle((t - 0.42, 2 - 0.33), 0.84, 0.66, fc="#e6d3df", ec="white"))
            note = f"{r['bombas']} activa" + ("s" if r["bombas"] != 1 else "") if r["bombas"] else "—"
            if r["bloques"] > prev["bloques"]: note = "¡explota!"
            ax.text(t, 2, note, ha="center", va="center", fontsize=7.6, color=C["P3"] if note != "¡explota!" else C["bad"], fontweight="bold" if note == "¡explota!" else "normal")
            ax.add_patch(Rectangle((t - 0.42, 3 - 0.33), 0.84, 0.66, fc="#dfe5ef", ec="white"))
            ev = []
            if r["vidas"] < prev_v: ev.append("−1 vida")
            if r["prio"] != prev["prio"]: ev.append("prio " + r["prio"])
            ax.text(t, 3, " · ".join(ev) if ev else f"vidas {r['vidas']}", ha="center", va="center", fontsize=7.4,
                    color=C["bad"] if ev else "#333", fontweight="bold" if ev else "normal")
            prev_v = r["vidas"]; prev.update(bloques=r["bloques"], prio=r["prio"])
        ax.grid(axis="x", color="#eee"); ax.set_axisbelow(True)
    fig.tight_layout(h_pad=2)
    fig.savefig(os.path.join(OUT, "f_gantt_ticks.png"), bbox_inches="tight"); plt.close(fig)

# ---------------------------------------------------------------- turnos por caso
TURNOS = [("01", 7, 4), ("02", 7, 6), ("03", 0, 10), ("04", 4, 0), ("05", 2, 2), ("06", 4, 2), ("07", 7, 0), ("08a", 8, 0),
          ("08b", 0, 3), ("08c", 3, 3), ("08d", 2, 1), ("09", 8, 0), ("10", 6, 4), ("11", 0, 10), ("13", 20, 0)]
def turnos():
    fig, ax = plt.subplots(figsize=(12, 3.8), dpi=200)
    xs = range(len(TURNOS)); p1 = [t[1] for t in TURNOS]; p2 = [t[2] for t in TURNOS]
    ax.bar(xs, p1, color=C["P1"], label="turnos de P1 (Bomberman)")
    ax.bar(xs, p2, bottom=p1, color=C["P2"], label="turnos de P2 (enemigos)")
    for i, (a, b) in enumerate(zip(p1, p2)):
        ax.text(i, a + b + 0.3, str(a + b), ha="center", fontsize=8.5, color="#333")
    ax.set_xticks(list(xs)); ax.set_xticklabels([t[0] for t in TURNOS]); ax.set_xlabel("caso de prueba")
    ax.set_ylabel("ticks"); ax.legend(frameon=False, loc="upper left")
    ax.set_title("Reparto de turnos de acción por caso (P3 juega en los mismos ticks: una fase de bombas por tick)", loc="left", fontsize=11)
    fig.tight_layout(); fig.savefig(os.path.join(OUT, "f_turnos.png"), bbox_inches="tight"); plt.close(fig)

# ---------------------------------------------------------------- E1 barrera P2
def e1():
    R = rows("e1_barrera.csv")
    g = collections.defaultdict(list)
    for r in R:
        if r["dirty"] == "NA": continue
        g[(r["variante"], r["caso"])].append(r)
    cases = ["02_round_robin", "05_colision_y_explosion", "08b_derrota"]
    short = {"05_colision_y_explosion": "05", "08b_derrota": "08b", "02_round_robin": "02", "04_reaccion_cadena": "04"}
    fig, axs = plt.subplots(1, 2, figsize=(13, 4.2), dpi=200, gridspec_kw={"width_ratios": [1.6, 1]})
    ax = axs[0]; off = 0
    for v, col, lab in (("nobar", C["bad"], "sin barrera"), ("base", C["good"], "con barrera")):
        ys = []
        for c in cases: ys += [int(r["dirty"]) for r in g[(v, c)]]
        ax.scatter(range(len(ys)), ys, s=6, color=col, alpha=0.6, label=lab)
    ax.set_xlabel("ejecución (casos 02, 05 y 08b; 500 cada uno)"); ax.set_ylabel("lecturas a medio turno por ejecución")
    ax.legend(frameon=False); ax.set_title("(a) Lecturas sucias de collision_thread", loc="left", fontweight="bold", fontsize=11)
    ax = axs[1]; w = 0.38; info = {}
    for k, (v, col, lab) in enumerate((("base", C["good"], "con barrera"), ("nobar", C["bad"], "sin barrera"))):
        vals = []
        for c in cases:
            L = g[(v, c)]; pct = 100 * sum(1 for r in L if int(r["dirty"]) > 0) / max(1, len(L)); vals.append(pct)
            info[(v, c)] = dict(n=len(L), pct_dirty=round(pct, 1), mean_dirty=round(st.mean(int(r["dirty"]) for r in L), 3),
                                max_dirty=max(int(r["dirty"]) for r in L), turns=round(st.mean(int(r["turns"]) for r in L), 1),
                                correct=sum(int(r["correcto"]) for r in L))
        ax.bar([i + (k - 0.5) * w for i in range(len(cases))], vals, w, color=col, label=lab)
        for i, val in enumerate(vals): ax.text(i + (k - 0.5) * w, val + 1, f"{val:.0f}%", ha="center", fontsize=8.5)
    ax.set_xticks(range(len(cases))); ax.set_xticklabels(["caso " + short[c] for c in cases]); ax.set_ylabel("% de ejecuciones con lectura sucia")
    ax.set_ylim(0, 110); ax.set_title("(b) Ejecuciones afectadas", loc="left", fontweight="bold", fontsize=11)
    fig.tight_layout(w_pad=3); fig.savefig(os.path.join(OUT, "f_e1_barrera.png"), bbox_inches="tight"); plt.close(fig)
    summary["e1"] = {f"{v}|{c}": d for (v, c), d in info.items()}

# ---------------------------------------------------------------- E2 cola P1
def e2():
    R = rows("e2_cola.csv")
    fig, axs = plt.subplots(1, 2, figsize=(13, 4.0), dpi=200, gridspec_kw={"width_ratios": [1.6, 1]})
    info = {}
    for v, col, lab in (("mutex", C["good"], "con queue.mutex"), ("nomutex", C["bad"], "sin queue.mutex")):
        L = [r for r in R if r["variante"] == v]
        ok = [r for r in L if r["perdidas"] != "NA"]
        per = [int(r["perdidas"]) for r in ok]
        axs[0].scatter([int(r["iter"]) for r in ok], per, s=7, color=col, alpha=0.65, label=lab)
        info[v] = dict(n=len(L), sin_salida=len(L) - len(ok), perdidas_mean=round(st.mean(per), 1) if per else None,
                       perdidas_min=min(per) if per else None, perdidas_max=max(per) if per else None,
                       pct_integras=round(100 * sum(1 for p in per if p == 0) / max(1, len(L)), 1),
                       count_final_distinto=sum(1 for r in ok if r["count_final"] != "0"),
                       fifo=sum(1 for r in ok if r["fifo_err"] != "0"), corte=sum(1 for r in ok if r["corte"] == "1"))
    axs[0].set_xlabel("ejecución"); axs[0].set_ylabel("instrucciones perdidas (de 50 000)")
    axs[0].legend(frameon=False, loc="center right"); axs[0].set_title("(a) Instrucciones que el executor nunca recibió", loc="left", fontweight="bold", fontsize=11)
    labs = ["con mutex", "sin mutex"]; vals = [info["mutex"]["pct_integras"], info["nomutex"]["pct_integras"]]
    axs[1].bar(labs, vals, color=[C["good"], C["bad"]])
    for i, v in enumerate(vals): axs[1].text(i, v + 2, f"{v:.0f}%", ha="center")
    axs[1].set_ylim(0, 110); axs[1].set_ylabel("% de ejecuciones sin pérdidas")
    axs[1].set_title("(b) Ejecuciones íntegras", loc="left", fontweight="bold", fontsize=11)
    fig.tight_layout(w_pad=3); fig.savefig(os.path.join(OUT, "f_e2_cola.png"), bbox_inches="tight"); plt.close(fig)
    summary["e2"] = info

# ---------------------------------------------------------------- E3 / E4 cierre
def e3():
    R = rows("e3_cierre.csv")
    fig, axs = plt.subplots(1, 2, figsize=(13, 4.0), dpi=200)
    info = {}
    for v, col, lab in (("old", C["bad"], "antes del arreglo (d2c520a)"), ("clean", C["good"], "versión final (3a5347d)")):
        L = [r for r in R if r["version"] == v]; ms = sorted(int(r["ms"]) for r in L)
        axs[0].step(ms, [(i + 1) / len(ms) for i in range(len(ms))], where="post", color=col, lw=2, label=lab)
        hung = [r for r in L if r["colgado"] == "1"]
        info[v] = dict(n=len(L), colgados=len(hung), casos=sorted(set(r["caso"] for r in hung)),
                       ms_mediana=st.median(ms), ms_p99=ms[int(0.99 * (len(ms) - 1))])
    axs[0].set_xscale("log"); axs[0].set_xlabel("tiempo hasta terminar (ms, escala log)"); axs[0].set_ylabel("fracción de ejecuciones terminadas")
    axs[0].axvline(10000, color="#999", ls=":"); axs[0].text(10000, 0.05, " timeout 10 s", fontsize=8.5, color="#666")
    axs[0].legend(frameon=False, loc=(0.42, 0.3)); axs[0].set_title("(a) Curva de terminación, 15 casos × 100", loc="left", fontweight="bold", fontsize=11)
    E = rows("e4_release.csv"); i4 = {}
    for v in ("base", "norel"):
        L = [r for r in E if r["variante"] == v]
        i4[v] = dict(n=len(L), colgados=sum(int(r["colgado"]) for r in L), ms_media=round(st.mean(int(r["ms"]) for r in L), 1))
    labs = ["final\n(cuelgues P3)", "antes\n(cuelgues P3)", "con release\n_children", "sin release\n_children"]
    vals = [100 * info["clean"]["colgados"] / info["clean"]["n"], 100 * info["old"]["colgados"] / info["old"]["n"],
            100 * i4["base"]["colgados"] / i4["base"]["n"], 100 * i4["norel"]["colgados"] / i4["norel"]["n"]]
    axs[1].bar(labs, vals, color=[C["good"], C["bad"], C["good"], C["bad"]])
    for i, v in enumerate(vals): axs[1].text(i, v + 2, f"{v:.1f}%", ha="center", fontsize=9)
    axs[1].set_ylim(0, 112); axs[1].set_ylabel("% de ejecuciones colgadas")
    axs[1].set_title("(b) Cuelgues al terminar el juego", loc="left", fontweight="bold", fontsize=11)
    fig.tight_layout(w_pad=3); fig.savefig(os.path.join(OUT, "f_e3_cierre.png"), bbox_inches="tight"); plt.close(fig)
    summary["e3"] = info; summary["e4"] = i4

def recursos():
    R = rows("recursos.csv"); g = collections.defaultdict(list)
    for r in R: g[r["caso"]].append(r)
    summary["recursos"] = {c: dict(seg=round(st.mean(float(r["seg"]) for r in L), 3), cpu=round(st.mean(float(r["cpu"]) for r in L)),
                                   rss=round(st.mean(int(r["maxrss_kb"]) for r in L))) for c, L in sorted(g.items())}

for f in (gantt, turnos, e1, e2, e3, recursos):
    try: f()
    except Exception as ex: print("ERROR", f.__name__, ex)
json.dump(summary, open(os.path.join(OUT, "resumen_exp.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
print(json.dumps(summary, indent=1, ensure_ascii=False))
