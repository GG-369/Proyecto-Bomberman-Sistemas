"""Diagramas estáticos del informe (arquitectura, mutex, sincronización interna, temporal de P2)."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Rectangle
import os, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
plt.rcParams.update({"font.family": "Arial", "font.size": 11})

C = {  # paleta sobria, un tono por proceso
    "P0": "#3b5b92", "P1": "#2e7d5b", "P2": "#a0522d", "P3": "#8e3b6f", "P4": "#5f6b7a",
    "SHM": "#f2efe6", "line": "#333333",
}
def light(hexc, f=0.82):
    h = hexc.lstrip("#"); r, g, b = (int(h[i:i+2], 16) for i in (0, 2, 4))
    return "#%02x%02x%02x" % tuple(int(c + (255 - c) * f) for c in (r, g, b))

def box(ax, x, y, w, h, text="", fc="white", ec="#333", lw=1.2, fs=11, bold=False, color="#111", r=0.02, ha="center", va="center", z=2):
    p = FancyBboxPatch((x, y), w, h, boxstyle=f"round,pad=0,rounding_size={r}", fc=fc, ec=ec, lw=lw, zorder=z)
    ax.add_patch(p)
    if text:
        tx = x + w / 2 if ha == "center" else x + 0.012
        ty = y + h / 2 if va == "center" else y + h - 0.012
        ax.text(tx, ty, text, ha=ha, va=va, fontsize=fs, fontweight="bold" if bold else "normal", color=color, zorder=z + 1, linespacing=1.25)
    return p

def arrow(ax, x1, y1, x2, y2, text="", color="#333", style="-|>", ls="-", lw=1.4, fs=9.5, rad=0.0, tx=None, ty=None, z=3):
    a = FancyArrowPatch((x1, y1), (x2, y2), arrowstyle=style, mutation_scale=13, color=color, lw=lw, ls=ls,
                        connectionstyle=f"arc3,rad={rad}", zorder=z)
    ax.add_patch(a)
    if text:
        ax.text(tx if tx is not None else (x1 + x2) / 2, ty if ty is not None else (y1 + y2) / 2, text, fontsize=fs,
                ha="center", va="center", color=color, zorder=5,
                bbox=dict(fc="white", ec="none", pad=1.2))

def canvas(w, h):
    fig = plt.figure(figsize=(w, h), dpi=200)
    ax = fig.add_axes([0, 0, 1, 1]); ax.set_xlim(0, 1); ax.set_ylim(0, 1); ax.axis("off")
    return fig, ax

def proc(ax, x, y, w, h, key, title, threads, cols=1):
    box(ax, x, y, w, h, fc=light(C[key], 0.88), ec=C[key], lw=2)
    ax.text(x + 0.012, y + h - 0.022, title, fontsize=12.5, fontweight="bold", color=C[key], va="top")
    n = len(threads); rows = (n + cols - 1) // cols
    top = y + h - 0.075; bh = min(0.05, (h - 0.095) / rows - 0.012); cw = (w - 0.024 - (cols - 1) * 0.01) / cols
    for i, t in enumerate(threads):
        r, c = divmod(i, cols)
        box(ax, x + 0.012 + c * (cw + 0.01), top - r * (bh + 0.012) - bh, cw, bh, t, fc="white", ec=C[key], lw=1, fs=9.6, r=0.006)

# ------------------------------------------------------------------ F1 arquitectura
def arquitectura():
    fig, ax = canvas(13, 9.2)
    # memoria compartida al centro
    box(ax, 0.35, 0.33, 0.30, 0.30, fc=C["SHM"], ec="#8a7f5a", lw=2.2)
    ax.text(0.5, 0.605, "Memoria compartida POSIX", ha="center", fontsize=13, fontweight="bold")
    ax.text(0.5, 0.578, "shared_state_t  ·  /bomberman_p2_shm", ha="center", fontsize=9.5, color="#555")
    items = [("5 mutex PTHREAD_PROCESS_SHARED", "m_pos · m_map · m_mailbox · m_events · m_state"),
             ("6 semáforos pshared = 1", "bomber/enemy/bomb_turn · turn_done · render(_done)"),
             ("Estado del juego", "tick, prioridades, turno, vidas, puntaje, game_over"),
             ("Mapa y bombas", "map_grid[32][32], bomb_x/y/timer[], power-ups"),
             ("Buzones y eventos", "bomba, SET_PRIORITY, colisión, daño")]
    for i, (a, b) in enumerate(items):
        yy = 0.535 - i * 0.04
        ax.text(0.365, yy, a, fontsize=9.6, fontweight="bold"); ax.text(0.365, yy - 0.017, b, fontsize=8.4, color="#444")

    proc(ax, 0.28, 0.73, 0.44, 0.24, "P0", "P0 · scheduler (4 hilos)",
         ["scheduler_thread\ndecide el turno", "tick_thread\nglobal_tick++ · prioridades", "signal_thread\nturnos a P1/P2 y P3", "event_thread\ncierre: vidas, puntaje, fin"], cols=2)
    proc(ax, 0.02, 0.36, 0.25, 0.30, "P1", "P1 · Bomberman (3 hilos)",
         ["movement_reader_thread", "executor (hilo principal)", "bomber_publisher_thread"])
    proc(ax, 0.73, 0.27, 0.25, 0.43, "P2", "P2 · enemigos (8 hilos)",
         ["controlador (hilo principal)", "enemy_thread A", "enemy_thread B", "enemy_thread C", "enemy_thread D",
          "bomber_tracker_thread", "collision_thread", "enemy_publisher_thread"])
    proc(ax, 0.10, 0.03, 0.34, 0.22, "P3", "P3 · bombas (3 hilos)",
         ["controller_thread", "fuse_thread", "explosion_thread"])
    proc(ax, 0.50, 0.06, 0.20, 0.13, "P4", "P4 · renderer (bonus)", ["render ANSI por tick"])

    # semáforos entre procesos
    arrow(ax, 0.30, 0.73, 0.18, 0.66, "sem_bomber_turn", C["P0"], tx=0.20, ty=0.71)
    arrow(ax, 0.10, 0.66, 0.29, 0.765, "sem_turn_done", C["P1"], ls="--", rad=-0.25, tx=0.085, ty=0.72)
    arrow(ax, 0.70, 0.73, 0.83, 0.70, "sem_enemy_turn", C["P0"], tx=0.80, ty=0.735)
    arrow(ax, 0.93, 0.70, 0.715, 0.80, "sem_turn_done", C["P2"], ls="--", rad=0.3, tx=0.935, ty=0.77)
    arrow(ax, 0.295, 0.73, 0.295, 0.25, "", C["P0"])
    arrow(ax, 0.325, 0.25, 0.325, 0.73, "", C["P3"], ls="--")
    ax.text(0.288, 0.30, "sem_bomb_turn\n(todos los ticks)", fontsize=9.5, color=C["P0"], ha="right", va="center")
    ax.text(0.333, 0.68, "sem_turn_done", rotation=90, fontsize=9.5, color=C["P3"], ha="left", va="top")
    arrow(ax, 0.66, 0.73, 0.66, 0.19, "sem_render", C["P0"], tx=0.685, ty=0.25)
    arrow(ax, 0.56, 0.19, 0.56, 0.33, "sem_render_done", C["P4"], ls="--", tx=0.55, ty=0.26)
    # acceso a memoria (gris)
    for (x1, y1, x2, y2) in [(0.5, 0.73, 0.5, 0.63), (0.27, 0.48, 0.35, 0.48), (0.73, 0.48, 0.65, 0.48), (0.40, 0.25, 0.43, 0.33), (0.62, 0.19, 0.60, 0.33)]:
        arrow(ax, x1, y1, x2, y2, "", "#9a9a9a", style="<|-|>", lw=2.2)
    ax.text(0.02, 0.985, "Flechas de color: semáforos entre procesos (continua = post de P0, punteada = sem_turn_done de vuelta).   Flechas grises: lectura/escritura de la memoria compartida con su mutex.",
            fontsize=9, color="#444", va="top")
    fig.savefig(os.path.join(OUT, "f_arquitectura.png"), bbox_inches="tight", pad_inches=0.08); plt.close(fig)

# ------------------------------------------------------------------ F2 matriz de mutex
def mutex_matriz():
    fig, ax = canvas(9.6, 4.4)
    rows = [("m_pos", "bomber_x/y, enemy_x/y[], *_moves_done"),
            ("m_map", "map_grid, bombas, bloques, power-ups"),
            ("m_mailbox", "buzón de bomba, buzones SET_PRIORITY"),
            ("m_events", "colisión, blast_hit_*"),
            ("m_state", "tick, prioridades, vidas, game_over")]
    cols = ["P0", "P1", "P2", "P3", "P4"]
    M = [["L", "E", "E", "L", "L"],
         ["L", "E*", "L", "E", "L"],
         ["L+limpia", "E", "E", "L+limpia", "—"],
         ["L+limpia", "—", "E", "E", "—"],
         ["E", "L", "L", "L", "L"]]
    x0, y0, cw, rh, lw_ = 0.30, 0.10, 0.12, 0.13, 0.27
    for j, c in enumerate(cols):
        box(ax, x0 + j * cw, y0 + 5 * rh, cw, 0.09, c, fc=C[c], ec="white", fs=12, bold=True, color="white", r=0.005)
    for i, (m, d) in enumerate(rows):
        y = y0 + (4 - i) * rh
        box(ax, x0 - lw_ - 0.005, y, lw_, rh, "", fc="#eeeeee", ec="white", r=0.005)
        ax.text(x0 - lw_ + 0.006, y + rh * 0.62, f"{i+1}. {m}", fontsize=12, fontweight="bold", family="Consolas")
        ax.text(x0 - lw_ + 0.006, y + rh * 0.27, d, fontsize=8.8, color="#444")
        for j, c in enumerate(cols):
            v = M[i][j]
            fc = light(C[c], 0.3) if v == "E*" else C[c] if v.startswith("E") else (light(C[c], 0.6) if v.startswith("L") else "#fafafa")
            col = "white" if v.startswith("E") else "#111"
            box(ax, x0 + j * cw, y, cw, rh, {"E": "escribe", "L": "lee", "E*": "lee; escribe solo\nel power-up", "L+limpia": "lee y limpia", "—": "—"}[v],
                fc=fc, ec="white", lw=2, fs=10, color=col, r=0.005)
    # flecha de orden
    ax.annotate("", xy=(0.93, y0 + 0.02), xytext=(0.93, y0 + 5 * rh - 0.02),
                arrowprops=dict(arrowstyle="-|>", lw=2.2, color="#b03a2e", mutation_scale=18))
    ax.text(0.945, y0 + 2.5 * rh, "orden obligatorio\nde adquisición\n(1 → 5)\n\nsin ciclos →\nsin deadlock", fontsize=10, color="#b03a2e", va="center")
    ax.text(0.03, 0.97, "Un escritor por variable, salvo el estado de power-ups (P1 lo activa y P3 lo consume, ambos con m_map).\nBuzones y eventos: escribe el emisor, el receptor lee y limpia la bandera.",
            fontsize=10, va="top", color="#333")
    fig.savefig(os.path.join(OUT, "f_mutex.png"), bbox_inches="tight", pad_inches=0.08); plt.close(fig)

# ------------------------------------------------------------------ F4 sincronización interna
def sinc_interna():
    fig, axs = plt.subplots(1, 3, figsize=(15, 6.2), dpi=200)
    for a in axs: a.set_xlim(0, 1); a.set_ylim(0, 1); a.axis("off")
    # (a) P1
    ax = axs[0]; k = C["P1"]
    ax.set_title("(a) P1 · productor-consumidor", fontsize=13, fontweight="bold", color=k, loc="left")
    box(ax, 0.05, 0.80, 0.42, 0.11, "movement_reader\n(productor)", fc=light(k), ec=k)
    box(ax, 0.53, 0.80, 0.42, 0.11, "bomber_moves.txt", fc="white", ec="#888", fs=10)
    arrow(ax, 0.53, 0.855, 0.47, 0.855, "", "#888")
    # cola circular
    for i in range(8):
        box(ax, 0.10 + i * 0.1, 0.52, 0.09, 0.09, "" if i > 4 else "■", fc="white" if i > 4 else light(k, 0.5), ec=k, lw=1, fs=9, r=0.005)
    ax.text(0.10, 0.455, "cola circular (64)\nhead · tail · count  ←  queue.mutex", ha="left", va="center", fontsize=9.5)
    arrow(ax, 0.26, 0.80, 0.26, 0.62, "wait(slots)\n= espacio libre", k, tx=0.13, ty=0.71, fs=9)
    arrow(ax, 0.74, 0.52, 0.74, 0.38, "wait(items)\n= hay instrucción", k, tx=0.87, ty=0.45, fs=9)
    box(ax, 0.50, 0.26, 0.47, 0.11, "executor (consumidor)\n1 instrucción por turno", fc=light(k), ec=k)
    box(ax, 0.03, 0.26, 0.40, 0.11, "bomber_publisher", fc=light(k), ec=k)
    arrow(ax, 0.50, 0.315, 0.43, 0.315, "", k)
    ax.text(0.465, 0.20, "publish_request / publish_done", ha="center", fontsize=9)
    box(ax, 0.03, 0.03, 0.94, 0.10, "m_pos → bomber_x, bomber_y (memoria compartida)", fc=C["SHM"], ec="#8a7f5a", fs=10)
    arrow(ax, 0.23, 0.26, 0.23, 0.13, "", k)
    # (b) P2
    ax = axs[1]; k = C["P2"]
    ax.set_title("(b) P2 · barrera de enemigos", fontsize=13, fontweight="bold", color=k, loc="left")
    box(ax, 0.25, 0.86, 0.5, 0.09, "controlador  ← sem_enemy_turn", fc=light(k), ec=k, fs=10)
    for i, L in enumerate("ABCD"):
        x = 0.04 + i * 0.24
        box(ax, x, 0.62, 0.20, 0.10, f"enemy {L}", fc="white", ec=k)
        arrow(ax, 0.5, 0.86, x + 0.10, 0.72, "", k, lw=1)
        arrow(ax, x + 0.10, 0.62, 0.5, 0.50, "", k, lw=1)
    ax.text(0.5, 0.79, "post(start[i]) a cada enemigo vivo", ha="center", fontsize=9, bbox=dict(fc="white", ec="none"))
    box(ax, 0.12, 0.42, 0.76, 0.08, "BARRERA: wait(workers_done) × enemigos vivos", fc=k, ec=k, color="white", bold=True, fs=10)
    steps = [("tracker", "track_go/done"), ("collision", "collision_go/done"), ("publisher", "publish_go/done")]
    for i, (n, s) in enumerate(steps):
        y = 0.28 - i * 0.11
        box(ax, 0.20, y, 0.6, 0.08, f"{n}   ({s})", fc=light(k), ec=k, fs=10)
        if i == 0: arrow(ax, 0.5, 0.42, 0.5, y + 0.08, "", k)
        else: arrow(ax, 0.5, y + 0.11, 0.5, y + 0.08, "", k)
    ax.text(0.5, -0.02, "→ sem_turn_done a P0", ha="center", fontsize=10, color=k)
    # (c) P3
    ax = axs[2]; k = C["P3"]
    ax.set_title("(c) P3 · fases de bombas", fontsize=13, fontweight="bold", color=k, loc="left")
    box(ax, 0.2, 0.86, 0.6, 0.09, "controller  ← sem_bomb_turn", fc=light(k), ec=k, fs=10)
    ph = [("1. limpiar fuego  * → O", "controller", 0.70), ("2. temporizadores −1", "fuse_thread  (fuse_go/done)", 0.54),
          ("3. explosiones + cadena", "explosion_thread  (explosion_go/done)", 0.38), ("4. registrar bomba nueva", "controller (m_mailbox → m_map)", 0.22)]
    for t, s, y in ph:
        box(ax, 0.12, y, 0.76, 0.11, f"{t}\n{s}", fc="white", ec=k, fs=10)
    for y in (0.86, 0.70, 0.54, 0.38):
        arrow(ax, 0.5, y, 0.5, y - 0.05, "", k)
    arrow(ax, 0.88, 0.40, 0.88, 0.47, "", k, rad=-1.6)
    ax.text(0.955, 0.435, "bomba\nalcanzada\n→ cola", fontsize=8.5, color=k, va="center", ha="center")
    ax.text(0.5, 0.12, "game_over se revisa bajo m_state; si terminó,\natomic_store(g_stop, 1) y despierta a fuse/explosion",
            ha="center", fontsize=9, color="#444")
    ax.text(0.5, 0.0, "→ sem_turn_done a P0 (una sola vez)", ha="center", fontsize=10, color=k)
    fig.tight_layout(w_pad=2)
    fig.savefig(os.path.join(OUT, "f_sinc_interna.png"), bbox_inches="tight", pad_inches=0.08); plt.close(fig)

# ------------------------------------------------------------------ F6 diagrama temporal de un turno de P2 (con y sin barrera)
def temporal_p2():
    fig, axs = plt.subplots(1, 2, figsize=(14, 4.6), dpi=200, sharey=True)
    lanes = ["controlador", "enemy A", "enemy B", "enemy C", "enemy D", "tracker", "collision", "publisher"]
    k = C["P2"]
    def draw(ax, with_bar):
        ax.set_xlim(0, 8.5); ax.set_ylim(-0.6, len(lanes) - 0.1); ax.invert_yaxis()
        ax.set_yticks(range(len(lanes))); ax.set_yticklabels(lanes, fontsize=10)
        ax.set_xticks([]); ax.set_xlabel("tiempo dentro del turno de P2 →", fontsize=10)
        for s in ("top", "right"): ax.spines[s].set_visible(False)
        def bar(l, a, b, fc, txt=""):
            ax.add_patch(Rectangle((a, l - 0.32), b - a, 0.64, fc=fc, ec="#333", lw=0.8))
            if txt: ax.text((a + b) / 2, l, txt, ha="center", va="center", fontsize=8.5, color="white" if fc in (k, "#b03a2e") else "#111")
        bar(0, 0, 0.6, light(k, 0.4), "start")
        durs = [2.2, 3.1, 2.6, 3.6]
        for i, d in enumerate(durs): bar(1 + i, 0.6, 0.6 + d, light(k, 0.55), "mueve")
        if with_bar:
            end = 0.6 + max(durs)
            ax.axvline(end, color=k, lw=2, ls="--"); ax.text(end + 0.1, 7.75, "barrera workers_done", ha="left", fontsize=9, color=k)
            bar(5, end + 0.1, end + 1.2, k, "copia P1"); bar(6, end + 1.3, end + 2.6, k, "compara"); bar(7, end + 2.7, end + 3.9, k, "publica")
            ax.set_title("Con barrera: colisión con posiciones finales", fontsize=11.5, fontweight="bold", loc="left")
        else:
            bar(5, 0.7, 1.8, k, "copia P1"); bar(6, 1.9, 3.2, "#b03a2e", "compara"); bar(7, 3.3, 4.5, "#b03a2e", "publica")
            ax.add_patch(Rectangle((1.9, 0.6), 2.6, 4.0, fc="#b03a2e", alpha=0.08, ec="none"))
            ax.text(3.2, 4.85, "lee posiciones a medio turno", ha="center", fontsize=9, color="#b03a2e")
            ax.set_title("Sin barrera: colisión y publicación mientras se mueven", fontsize=11.5, fontweight="bold", loc="left")
    draw(axs[0], True); draw(axs[1], False)
    fig.tight_layout(w_pad=3)
    fig.savefig(os.path.join(OUT, "f_temporal_p2.png"), bbox_inches="tight", pad_inches=0.08); plt.close(fig)

if __name__ == "__main__":
    arquitectura(); mutex_matriz(); sinc_interna(); temporal_p2(); print("ok")
