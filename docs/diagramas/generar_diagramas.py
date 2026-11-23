#!/usr/bin/env python3
"""Genera los diagramas de la documentacion (PNG claro y oscuro) con la skill diagram-design.

Perfil de estilo: nes-esp32 (colores de ESP32: plata, negro y rojo Espressif;
azul = camino de datos, rojo = costo o cuello de botella), en tema claro y oscuro.
Uso:  python docs/diagramas/generar_diagramas.py [--html CARPETA]
Salida: docs/diagramas/<nombre>.png y <nombre>-dark.png (2x). Necesita Microsoft Edge (o Chrome)
y conexion a Internet para las fuentes de Google. Con --html tambien escribe las paginas HTML
en CARPETA, para pasarles los verificadores de la skill (self_check.py, verify_geometry.py).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

def _rgb(h):
    h = h.lstrip("#")
    return ",".join(str(int(h[i:i + 2],16)) for i in (0, 2, 4))


def _theme(paper, ink, muted, soft, accent, link, fill_a, tint_a, rule_a, bar_a):
    return dict(paper=paper, ink=ink, muted=muted, soft=soft, accent=accent, link=link,
                fill=f"rgba({_rgb(ink)},{fill_a})", tint=f"rgba({_rgb(accent)},{tint_a})",
                rule=f"rgba({_rgb(ink)},{rule_a})", bar=f"rgba({_rgb(ink)},{bar_a})")


THEMES = {
    "light": _theme("#F3F4F6", "#15171B", "#464B55", "#575D69", "#CF2B22", "#2B5C9E", 0.04, 0.09, 0.14, 0.10),
    "dark": _theme("#16181D", "#ECEEF1", "#B1B7C1", "#8F97A4", "#FF5D50", "#7FB0F5", 0.06, 0.16, 0.16, 0.14),
}
P = dict(THEMES["light"])


def use_theme(name):
    P.clear()
    P.update(THEMES[name])


def ink_a(alpha):
    return f"rgba({_rgb(P['ink'])},{alpha})"


SANS = "'Geist', system-ui, sans-serif"
MONO = "'Geist Mono', ui-monospace, monospace"
FONTS = ("https://fonts.googleapis.com/css2?family=Geist:wght@400;500;600"
         "&family=Geist+Mono:wght@400;500;600&display=swap")


class Svg:
    def __init__(self, slug, title, desc, w, h):
        self.slug, self.title, self.desc, self.w, self.h = slug, title, desc, w, h
        self.o = []
        self.a(f'<svg viewBox="0 0 {w} {h}" xmlns="http://www.w3.org/2000/svg" role="img" '
               f'aria-labelledby="{slug}-title {slug}-desc">')
        self.a(f'<title id="{slug}-title">{title}</title>')
        self.a(f'<desc id="{slug}-desc">{desc}</desc>')
        self.a('<defs>' + "".join(
            f'<marker id="arrow-{k}" markerWidth="8" markerHeight="6" refX="7" refY="3" orient="auto">'
            f'<polygon points="0 0, 8 3, 0 6" fill="{P[k]}"/></marker>' for k in ("muted", "accent", "link")) + '</defs>')
        self.a(f'<rect width="100%" height="100%" fill="{P["paper"]}"/>')

    def a(self, s):
        self.o.append(s)

    # ---- conectores -------------------------------------------------------------------
    def path(self, pts, kind="muted", width=1.4, dashed=False, arrow=True, r=8):
        d = f"M{pts[0][0]},{pts[0][1]}"
        for i in range(1, len(pts)):
            x0, y0 = pts[i - 1]
            x1, y1 = pts[i]
            if i < len(pts) - 1:
                x2, y2 = pts[i + 1]
                def toward(ax, ay, bx, by):
                    dx, dy = bx - ax, by - ay
                    n = max(abs(dx), abs(dy)) or 1
                    return ax + dx / n * r, ay + dy / n * r
                px, py = x1 - (x1 - x0) / (abs(x1 - x0) + abs(y1 - y0)) * r, y1 - (y1 - y0) / (abs(x1 - x0) + abs(y1 - y0)) * r
                qx, qy = toward(x1, y1, x2, y2)
                d += f" L{px:.1f},{py:.1f} Q{x1},{y1} {qx:.1f},{qy:.1f}"
            else:
                d += f" L{x1},{y1}"
        dash = ' stroke-dasharray="4,3"' if dashed else ""
        mk = f' marker-end="url(#arrow-{kind})"' if arrow else ""
        self.a(f'<path d="{d}" fill="none" stroke="{P[kind]}" stroke-width="{width}"{dash}{mk}/>')

    def label(self, cx, cy, text, kind="soft", anchor="middle"):
        mw = len(text) * 6.1 + 14
        self.a(f'<rect x="{cx - mw / 2:.0f}" y="{cy - 6:.0f}" width="{mw:.0f}" height="12" rx="2" fill="{P["paper"]}"/>')
        self.a(f'<text x="{cx:.0f}" y="{cy + 3:.0f}" fill="{P[kind]}" font-size="9" font-family="{MONO}" '
               f'text-anchor="middle" letter-spacing="0.06em">{text}</text>')

    # ---- nodos --------------------------------------------------------------------------
    def node(self, x, y, w, h, name, sub="", tag="", rows=(), lines=(), kind="plain", name_size=15):
        foc, fast, dash = kind == "focal", kind == "fast", kind == "dashed"
        stroke = P["accent"] if foc else (P["soft"] if dash else P["ink"])
        sw = 1.5 if (foc or fast) else 1
        fill = P["tint"] if foc else P["fill"]
        self.a(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" fill="{P["paper"]}"/>')
        da = ' stroke-dasharray="5,4"' if dash else ""
        self.a(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" fill="{fill}" stroke="{stroke}" '
               f'stroke-width="{sw}"{da}/>')
        ty = y + 14
        if tag:
            tw = len(tag) * 5.6 + 12
            self.a(f'<rect x="{x + 16}" y="{ty}" width="{tw:.0f}" height="12" rx="2" fill="transparent" '
                   f'stroke="{stroke}" stroke-opacity="0.45" stroke-width="0.8"/>')
            self.a(f'<text x="{x + 16 + tw / 2:.0f}" y="{ty + 9}" fill="{P["accent"] if foc else P["soft"]}" '
                   f'font-size="8" font-family="{MONO}" text-anchor="middle" letter-spacing="0.1em">{tag}</text>')
        ny = y + (50 if tag else 34)
        self.a(f'<text x="{x + 16}" y="{ny}" fill="{P["ink"] if not dash else P["muted"]}" font-size="{name_size}" '
               f'font-weight="600" font-family="{SANS}">{name}</text>')
        cy = ny
        if sub:
            cy = ny + 18
            self.a(f'<text x="{x + 16}" y="{cy}" fill="{P["muted"]}" font-size="10" font-family="{MONO}">{sub}</text>')
        if rows or lines:
            self.a(f'<line x1="{x + 16}" y1="{cy + 16}" x2="{x + w - 16}" y2="{cy + 16}" stroke="{P["rule"]}" stroke-width="1"/>')
        ry = cy + 36
        for (l, r_, hot) in rows:
            if hot:
                self.a(f'<circle cx="{x + 20}" cy="{ry - 3}" r="2.5" fill="{P["link"]}"/>')
            self.a(f'<text x="{x + 30}" y="{ry}" fill="{P["ink"] if hot else P["muted"]}" font-size="10" '
                   f'font-weight="{500 if hot else 400}" font-family="{MONO}">{l}</text>')
            self.a(f'<text x="{x + w - 16}" y="{ry}" fill="{P["soft"]}" font-size="10" font-family="{MONO}" '
                   f'text-anchor="end">{r_}</text>')
            ry += 26
        for t in lines:
            self.a(f'<text x="{x + 16}" y="{ry}" fill="{P["muted"]}" font-size="10" font-family="{MONO}">{t}</text>')
            ry += 20

    def callout(self, x, y, text, kind="ink", anchor="start"):
        self.a(f'<text x="{x}" y="{y}" fill="{P[kind]}" font-size="13" font-weight="500" '
               f'font-family="{SANS}" text-anchor="{anchor}">{text}</text>')

    def legend(self, items, y=None):
        y = (self.h - 44) if y is None else y
        self.a(f'<line x1="30" y1="{y - 10}" x2="{self.w - 30}" y2="{y - 10}" stroke="{P["rule"]}" stroke-width="1"/>')
        self.a(f'<text x="30" y="{y + 8}" fill="{P["muted"]}" font-size="8" font-family="{MONO}" letter-spacing="0.14em">LEYENDA</text>')
        x = 130
        for kind, dashed, text in items:
            if kind == "dot":
                self.a(f'<circle cx="{x + 3}" cy="{y + 5}" r="2.5" fill="{P["link"]}"/>')
                tx = x + 14
            else:
                da = ' stroke-dasharray="4,3"' if dashed else ""
                self.a(f'<line x1="{x}" y1="{y + 5}" x2="{x + 30}" y2="{y + 5}" stroke="{P[kind]}" stroke-width="1.6"{da} marker-end="url(#arrow-{kind})"/>')
                tx = x + 42
            self.a(f'<text x="{tx}" y="{y + 8}" fill="{P["muted"]}" font-size="10" font-family="{MONO}">{text}</text>')
            x = tx + len(text) * 6.2 + 40

    def svg(self):
        return chr(10).join(self.o + ['</svg>'])

    def bare_html(self):
        css = (f'html,body{{margin:0;padding:0;background:{P["paper"]}}}'
               f'svg{{display:block;width:{self.w}px;height:{self.h}px}}')
        return (f'<!DOCTYPE html><html><head><meta charset="UTF-8"><link href="{FONTS}" rel="stylesheet">'
                f'<style>{css}</style></head><body>{self.svg()}</body></html>')

    def html(self, eyebrow, h1, lang="es"):
        svg = self.svg()
        return f'''<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{self.title}</title>
<link href="{FONTS}" rel="stylesheet">
<style>
*,*::before,*::after{{box-sizing:border-box;margin:0;padding:0}}
:root{{--paper:{P["paper"]};--ink:{P["ink"]};--muted:{P["muted"]}}}
body{{font-family:{SANS};background:var(--paper);color:var(--ink);padding:3rem 2rem}}
.frame{{max-width:1200px;margin:0 auto}}
.diagram-container{{width:100%;overflow-x:auto}}
.eyebrow{{font-family:{MONO};font-size:.66rem;font-weight:500;letter-spacing:.18em;text-transform:uppercase;color:var(--muted);margin-bottom:.5rem}}
h1{{font-family:{SANS};font-size:1.75rem;font-weight:600;letter-spacing:-.02em;line-height:1.15;margin-bottom:1.5rem}}
svg{{width:100%;min-width:960px;display:block}}
@media print{{.diagram-container{{overflow-x:visible}}svg{{min-width:0}}}}
</style></head><body><div class="frame">
<p class="eyebrow">{eyebrow} · NES ESP32-4848S040</p>
<h1>{h1}</h1>
<div class="diagram-container">
{svg}
</div></div></body></html>'''


# ======================================================================================
# 1. Memorias (Architecture)
# ======================================================================================
def memorias():
    s = Svg("memorias", "Las cuatro memorias del ESP32-S3 N16R8",
            "La ROM viaja de la microSD a la flash, de la flash a la PSRAM y de la PSRAM a la RAM interna a través de la caché; la PSRAM es unas 7 veces más lenta que la RAM interna.",
            1200, 470)
    NW, NH, NY = 210, 252, 64
    X = [30, 340, 650, 960]
    ay = NY + 100
    for i, lab, kind in ((0, "AL ELEGIR", "link"), (1, "AL ARRANCAR", "link"), (2, "VÍA CACHÉ", "accent")):
        s.path([(X[i] + NW, ay), (X[i + 1], ay)], kind, 1.8 if kind == "accent" else 1.4)
        s.label((X[i] + NW + X[i + 1]) / 2, ay - 16, lab, kind)
    s.node(X[0], NY, NW, NH, "microSD", "~7.5 GB · SDMMC 1 bit", "TARJETA",
           rows=[("archivos .nes", "biblioteca", True), ("menú de ROMs", "", False), ("se lee al elegir juego", "", False)])
    s.node(X[1], NY, NW, NH, "Flash", "16 MB · DIO 80 MHz", "MÓDULO",
           rows=[("factory", "1 MB", False), ("nesgame", "1 MB", True), ("nvs", "24 KB", False), ("libre", "~13.9 MB", False)])
    s.node(X[2], NY, NW, NH, "PSRAM", "8 MB · octal · ~48 MB/s", "MÓDULO",
           rows=[("ROM en uso", "≤ 1 MB", True), ("lienzo interfaz", "450 KB", False),
                 ("3 copias cuadro", "180 KB", False), ("libre", "~6.4 MB", False)], kind="focal")
    s.node(X[3], NY, NW, NH, "RAM interna", "512 KB · ~360 MB/s", "CHIP",
           rows=[("cachés 16 + 64", "80 KB", False), ("IRAM 6502+PPU+APU", "46 KB", False),
                 ("bitmap del cuadro", "60 KB", False), ("bounce buffers", "~38 KB", False),
                 ("audio + RAM NES", "~10 KB", False)], kind="fast")
    cy = NY + NH + 36
    s.a(f'<line x1="{X[2] + NW // 2}" y1="{NY + NH}" x2="{X[2] + NW // 2}" y2="{cy - 16}" stroke="{P["accent"]}" stroke-width="1" stroke-dasharray="3,3"/>')
    s.callout(X[2] + NW // 2, cy, "~7× más lenta que la RAM interna", "accent", anchor="middle")
    s.callout(X[3] + NW, cy, "lo que se toca por cuadro vive aquí", "ink", anchor="end")
    s.legend([("dot", False, "por aquí pasa la ROM"), ("link", False, "copia de la ROM"),
              ("accent", False, "acceso por la caché (el costo)")])
    s.a(f'<text x="{s.w - 30}" y="{s.h - 36}" fill="{P["soft"]}" font-size="10" font-family="{MONO}" text-anchor="end">de la más lenta a la más rápida →</text>')
    return s, "Architecture", "Cuatro memorias, un solo camino para la ROM"



# ======================================================================================
# 2. Hardware (Architecture)
# ======================================================================================
def hardware():
    s = Svg("hardware", "Placa 4848S040: qué conecta con qué",
            "El ESP32-S3 conecta con el panel ST7701 por RGB y SPI, con el táctil por I2C, con la microSD por SDMMC, con el audio por I2S y con el USB-C por UART.",
            1200, 650)
    ex, ey, ew, eh = 470, 130, 260, 260
    # conectores primero
    s.path([(ex, 160), (270, 160)], "link", 1.8)
    s.label(370, 144, "RGB 16 BITS", "link")
    s.path([(ex, 220), (270, 220)], "accent", 1.4, dashed=True)
    s.label(370, 204, "SPI 3 HILOS", "accent")
    s.path([(ex + ew, 150), (930, 150)], "link", 1.4)
    s.label(830, 134, "I2C", "link")
    s.path([(ex + ew, 260), (930, 260)], "muted", 1.4)
    s.label(830, 244, "SDMMC 1 BIT", "soft")
    s.path([(ex + ew, 370), (930, 370)], "muted", 1.4)
    s.label(830, 354, "I2S", "soft")
    s.path([(600, ey + eh), (600, 460)], "muted", 1.4)
    s.label(640, 425, "UART0", "soft")
    # nodos
    s.node(ex, ey, ew, eh, "ESP32-S3-WROOM-1", "N16R8 · 2 × LX7 @ 240 MHz", "MÓDULO",
           rows=[("núcleo 0", "panel · audio · táctil", False), ("núcleo 1", "emulador NES", False),
                 ("RAM interna", "512 KB", False), ("flash + PSRAM", "16 MB + 8 MB", False), ("GPIO 35–37", "reservados", False)], kind="fast", name_size=14)
    s.node(30, 80, 240, 200, "Panel ST7701", "480×480 · sin framebuffer", "RGB + SPI",
           rows=[("HSYNC · VSYNC", "16 · 17", False), ("DE · PCLK", "18 · 21", False),
                 ("SPI CS·SCK·SDA", "39 · 48 · 47", False), ("luz de fondo", "GPIO 38", False)])
    s.node(930, 50, 240, 120, "Táctil GT911", "I2C · dirección 0x5D", "I2C",
           lines=["SDA 19 · SCL 45"])
    s.node(930, 190, 240, 140, "microSD", "SDMMC 1 bit · D0 = GPIO 41", "SDMMC",
           lines=["CLK 48 · CMD 47", "D1–D3 con pull-up"])
    s.node(930, 350, 240, 140, "Audio", "I2S → amplificador → P7", "NO CONFIRMADO",
           lines=["BCK 1 · LRCK 2 · DIN 40", "mismos GPIO que los relés"], kind="focal")
    s.node(470, 460, 260, 120, "CH340 → USB-C", "UART0 · TX 43 · RX 44", "SERIE",
           lines=["consola, log y grabación"])
    s.callout(30, 330, "GPIO 47 y 48 se reutilizan:", "ink")
    s.callout(30, 352, "primero el ST7701, luego la microSD.", "ink")
    s.callout(930, 522, "el audio no está confirmado", "ink")
    s.callout(930, 544, "en esta placa.", "ink")
    s.legend([("link", False, "datos"), ("muted", False, "periférico"),
              ("accent", True, "pines que se reutilizan")], y=s.h - 44)
    return s, "Architecture", "Qué conecta con qué en la placa"


# ======================================================================================
# 3. Camino de un cuadro (Swimlane)
# ======================================================================================
def pipeline():
    s = Svg("pipeline", "Camino de un cuadro de la ROM al panel",
            "El núcleo 1 emula y deja el cuadro en tres copias; el núcleo 0 arma cada tira de 20 filas en una interrupción y la envía al panel por DMA, sin framebuffer.",
            1280, 640)
    # carriles
    L1y, L0y, LH = 56, 296, 200
    for y, n, sub in ((L1y, "NÚCLEO 1", "emulador"), (L0y, "NÚCLEO 0", "panel y táctil")):
        s.a(f'<rect x="30" y="{y}" width="1220" height="{LH}" rx="8" fill="{P["ink"]}" fill-opacity="0.02" stroke="{P["rule"]}" stroke-width="1"/>')
        s.a(f'<text x="46" y="{y + 30}" fill="{P["muted"]}" font-size="8" font-family="{MONO}" letter-spacing="0.18em">{n}</text>')
        s.a(f'<text x="46" y="{y + 46}" fill="{P["soft"]}" font-size="9" font-family="{MONO}">{sub}</text>')
    NW, NH = 160, 120
    X = [150, 380, 610, 840, 1070]
    y1, y0 = L1y + 40, L0y + 40
    # conectores lane 1
    for i, lab in ((0, "CACHÉ"), (1, "DIBUJA")):
        s.path([(X[i] + NW, y1 + 60), (X[i + 1], y1 + 60)], "link", 1.4)
        s.label(X[i] + NW + 40, y1 + 44, lab, "link")
    s.path([(X[2] + NW, y1 + 60), (X[3], y1 + 60)], "link", 1.4)
    s.label(X[2] + NW + 35, y1 + 44, "COPIA", "link")
    # handoff entre nucleos (acento)
    mid = L1y + LH + 20
    s.path([(X[3] + 110, y1 + NH), (X[3] + 110, mid), (X[2] + 80, mid), (X[2] + 80, y0)], "accent", 1.8)
    s.label(X[2] + 150, mid + 18, "LEE EL ÚLTIMO", "accent")
    # tactil -> cpu
    s.path([(X[0] + 80, y0), (X[0] + 80, mid), (X[1] + 80, mid), (X[1] + 80, y1 + NH)], "muted", 1.2, dashed=True)
    s.label(X[0] + 150, mid - 18, "BOTONES", "soft")
    # lane 0
    s.path([(X[1] + NW, y0 + 60), (X[2], y0 + 60)], "link", 1.4)
    s.path([(X[2] + NW, y0 + 60), (X[3], y0 + 60)], "link", 1.4)
    s.label(X[2] + NW + 40, y0 + 44, "TIRAS", "link")
    s.path([(X[3] + NW, y0 + 60), (X[4], y0 + 60)], "link", 1.4)
    s.label(X[3] + NW + 40, y0 + 44, "DMA", "link")
    # nodos lane 1
    s.node(X[0], y1, NW, NH, "ROM", "PRG + CHR", "PSRAM", lines=["≤ 1 MB"])
    s.node(X[1], y1, NW, NH, "6502+PPU+APU", "46 KB en IRAM", "IRAM", lines=["CPU 2 · PPU 3.3 ms"], name_size=13)
    s.node(X[2], y1, NW, NH, "Bitmap 256×240", "60 KB · copia 1.3 ms", "RAM INTERNA", name_size=13)
    s.node(X[3], y1, NW, NH, "3 copias", "180 KB", "PSRAM", lines=["gana la más reciente"])
    # nodos lane 0
    s.node(X[0], y0, NW, NH, "Táctil GT911", "botones y Zapper", "I2C", lines=["prioridad 7"], name_size=13)
    s.node(X[1], y0, NW, NH, "Lienzo interfaz", "botones y menú", "PSRAM", lines=["450 KB"], name_size=13)
    s.node(X[2], y0, NW, NH, "Interrupción", "tira de 20 filas", "ISR", lines=["paleta, escala 1.5×"], kind="focal")
    s.node(X[3], y0, NW, NH, "Bounce buffer", "~38 KB", "RAM INTERNA", lines=["precarga la caché"], name_size=13)
    s.node(X[4], y0, NW, NH, "Panel ST7701", "480×480 · ~60.8 Hz", "RGB", name_size=13)
    s.callout(150, 540, "sin framebuffer: se liberan ~1.4 MB de PSRAM y ~7.5 ms por cuadro", "ink")
    s.callout(150, 564, "el emulador va a 60.0 Hz y el panel a ~60.8 Hz: las fases derivan", "ink")
    s.legend([("link", False, "datos del cuadro"), ("accent", False, "traspaso entre núcleos"),
              ("muted", True, "entrada del jugador")])
    return s, "Swimlane", "De la ROM al panel, núcleo por núcleo"


# ======================================================================================
# 4. Mapa de la flash (Layer stack)
# ======================================================================================
def flash():
    s = Svg("flash", "Mapa de la flash de 16 MB",
            "Cinco secciones de la flash, de la dirección 0x0000 a la 0xFFFFFF: bootloader y tabla, nvs, el firmware, la ROM instalada y 13.9 MB libres.",
            1000, 580)
    sx, sw, top, lh = 150, 800, 70, 70
    rows = [
        ("0x0000", "Bootloader + tabla", "36 KB", None, "arranca el chip y lee la tabla", "plain"),
        ("0x9000", "nvs + phy_init", "28 KB", None, "ajustes y calibración", "plain"),
        ("0x10000", "factory", "1 MB", (0.44, "44 %"), "el firmware (~450 KB)", "plain"),
        ("0x110000", "nesgame", "1 MB", (0.08, "SMB 80 KB"), "ROM instalada (máx. 1 MB)", "focal"),
        ("0x210000", "libre", "~13.9 MB", None, "sin particionar", "dashed"),
    ]
    # marco y capas
    s.a(f'<rect x="{sx}" y="{top}" width="{sw}" height="{lh * len(rows)}" rx="6" fill="{P["paper"]}"/>')
    for i, (addr, name, size, bar, sub, kind) in enumerate(rows):
        y = top + i * lh
        fill = P["tint"] if kind == "focal" else (P["fill"] if i % 2 == 0 else "none")
        s.a(f'<rect x="{sx}" y="{y}" width="{sw}" height="{lh}" fill="{fill}"/>')
        if i:
            s.a(f'<line x1="{sx}" y1="{y}" x2="{sx + sw}" y2="{y}" stroke="{P["rule"]}" stroke-width="1"/>')
        cy = y + lh / 2
        s.a(f'<text x="{sx + 20}" y="{cy + 3:.0f}" fill="{P["soft"]}" font-size="10" font-family="{MONO}" letter-spacing="0.06em">{addr}</text>')
        col = P["muted"] if kind == "dashed" else P["ink"]
        s.a(f'<text x="{sx + 120}" y="{cy + 5:.0f}" fill="{col}" font-size="15" font-weight="600" font-family="{SANS}">{name}</text>')
        s.a(f'<text x="{sx + 300}" y="{cy + 3:.0f}" fill="{P["muted"]}" font-size="10" font-family="{MONO}">{size}</text>')
        if bar:
            frac, txt = bar
            bx, bw = sx + 390, 140
            s.a(f'<rect x="{bx}" y="{cy - 4:.0f}" width="{bw}" height="8" rx="2" fill="{P["bar"]}"/>')
            fcol = P["accent"] if kind == "focal" else P["link"]
            s.a(f'<rect x="{bx}" y="{cy - 4:.0f}" width="{bw * frac:.0f}" height="8" rx="2" fill="{fcol}"/>')
            s.a(f'<text x="{bx + bw + 10}" y="{cy + 3:.0f}" fill="{P["soft"]}" font-size="10" font-family="{MONO}">{txt}</text>')
        s.a(f'<text x="{sx + sw - 20}" y="{cy + 3:.0f}" fill="{P["soft"]}" font-size="10" font-family="{MONO}" text-anchor="end">{sub}</text>')
    h = lh * len(rows)
    dash = ' stroke-dasharray="5,4"'
    s.a(f'<rect x="{sx}" y="{top + 4 * lh}" width="{sw}" height="{lh}" fill="none" stroke="{P["soft"]}" stroke-width="1"{dash} rx="0"/>')
    s.a(f'<rect x="{sx}" y="{top}" width="{sw}" height="{h}" rx="6" fill="none" stroke="{P["ink"]}" stroke-width="1"/>')
    s.a(f'<rect x="{sx}" y="{top + 3 * lh}" width="{sw}" height="{lh}" fill="none" stroke="{P["accent"]}" stroke-width="1.5"/>')
    # indicador de direccion (margen izquierdo)
    s.a(f'<text x="30" y="{top + 10}" fill="{P["muted"]}" font-size="8" font-family="{MONO}" letter-spacing="0.14em">0x000000</text>')
    s.path([(60, top + 24), (60, top + h - 24)], "muted", 1.2)
    s.a(f'<text x="30" y="{top + h - 6}" fill="{P["muted"]}" font-size="8" font-family="{MONO}" letter-spacing="0.14em">0xFFFFFF</text>')
    s.callout(150, top + h + 44, "escribir en nesgame suspende la caché: la imagen puede parpadear", "accent")
    s.callout(150, top + h + 70, "el procesador no lee la flash directo: usa la caché o la API de particiones", "ink")
    s.legend([("dot", False, "firmware en uso"), ("accent", False, "se reescribe desde el menú"),
              ("muted", True, "sin usar")], y=s.h - 44)
    return s, "Layer stack", "Qué hay en cada trozo de la flash"


# ======================================================================================
# 5. Secuencia de arranque (Steps)
# ======================================================================================
def arranque():
    s = Svg("arranque", "Secuencia de arranque",
            "Cuatro etapas desde el encendido: la ROM del chip, el bootloader de ESP-IDF, el arranque del sistema y app_main.",
            1200, 450)
    steps = [
        ("01", "ROM del chip", ["lee los pines strapping", "y carga el 2.º cargador"], "ROM interna · fija"),
        ("02", "Bootloader", ["busca factory (0x10000),", "copia a RAM y activa MMU"], "flash 0x0000 · 32 KB"),
        ("03", "Sistema", ["inicia PSRAM, heap y", "los dos núcleos"], "flash · partición factory"),
        ("04", "app_main", ["calibra, menú de ROMs y", "copia la ROM a PSRAM"], "main/main.c"),
    ]
    sw_, n = 224, 4
    x0 = (1200 - sw_ * n) // 2
    railY, railH = 130, 48
    foc = 1
    for i, (num, title, desc, where) in enumerate(steps):
        x = x0 + i * sw_
        cx = x + sw_ / 2
        isf = i == foc
        fill = P["tint"] if isf else (ink_a(0.05 if i % 2 == 0 else 0.09))
        s.a(f'<rect x="{x}" y="{railY}" width="{sw_}" height="{railH}" fill="{fill}"/>')
        if isf:
            s.a(f'<line x1="{x}" y1="{railY}" x2="{x + sw_}" y2="{railY}" stroke="{P["accent"]}" stroke-width="1"/>')
            s.a(f'<line x1="{x}" y1="{railY + railH}" x2="{x + sw_}" y2="{railY + railH}" stroke="{P["accent"]}" stroke-width="1"/>')
        col = P["accent"] if isf else P["muted"]
        s.a(f'<text x="{cx}" y="{railY + 28}" fill="{col}" font-size="10" font-family="{MONO}" text-anchor="middle">{where}</text>')
        s.a(f'<text x="{cx}" y="76" fill="{col}" font-size="8" font-weight="600" font-family="{MONO}" text-anchor="middle" letter-spacing="0.14em">{num}</text>')
        s.a(f'<line x1="{cx}" y1="88" x2="{cx}" y2="{railY - 2}" stroke="{P["accent"] if isf else P["rule"]}" stroke-width="1"/>')
        s.a(f'<text x="{cx}" y="222" fill="{P["ink"]}" font-size="16" font-weight="600" font-family="{SANS}" text-anchor="middle">{title}</text>')
        for k, line in enumerate(desc):
            s.a(f'<text x="{cx}" y="{250 + k * 20}" fill="{P["muted"]}" font-size="10" font-family="{MONO}" text-anchor="middle">{line}</text>')
    s.a(f'<line x1="{x0}" y1="{railY}" x2="{x0 + sw_ * n}" y2="{railY}" stroke="{P["ink"]}" stroke-opacity="0.2" stroke-width="1"/>')
    s.callout(x0, 340, "solo ~28 % de la imagen se copia a RAM al arrancar;", "ink")
    s.callout(x0, 364, "el otro ~72 % se queda en la flash y se lee por la caché.", "ink")
    s.a(f'<line x1="30" y1="{s.h - 54}" x2="{s.w - 30}" y2="{s.h - 54}" stroke="{P["rule"]}" stroke-width="1"/>')
    s.a(f'<text x="30" y="{s.h - 36}" fill="{P["muted"]}" font-size="8" font-family="{MONO}" letter-spacing="0.14em">LEYENDA</text>')
    s.a(f'<rect x="130" y="{s.h - 44}" width="22" height="12" fill="{P["tint"]}" stroke="{P["accent"]}" stroke-width="1"/>')
    s.a(f'<text x="162" y="{s.h - 35}" fill="{P["muted"]}" font-size="10" font-family="{MONO}">la etapa que enlaza la flash con el procesador</text>')
    return s, "Steps", "Del encendido a app_main"


# ======================================================================================
# 6. Flash/PSRAM -> procesador (Architecture)
# ======================================================================================
def caches():
    s = Svg("caches", "Cómo llega la flash al procesador",
            "Los núcleos leen la flash y la PSRAM por dos cachés mapeadas con el MMU, y acceden a las particiones por la API del driver sin pasar por el MMU.",
            1200, 620)
    # conectores
    s.path([(240, 170), (400, 170)], "link", 1.4)
    s.label(320, 154, "EJECUTAR", "link")
    s.path([(240, 250), (400, 250)], "link", 1.4)
    s.label(320, 234, "CONSTANTES", "link")
    s.path([(620, 165), (900, 165)], "link", 1.4)
    s.label(760, 149, "VÍA MMU", "link")
    s.path([(620, 250), (900, 250)], "link", 1.4)
    s.label(760, 234, "VÍA MMU", "link")
    s.path([(620, 330), (900, 330)], "link", 1.4)
    s.label(760, 314, "VÍA MMU", "link")
    s.path([(135, 330), (135, 495), (400, 495)], "accent", 1.8)
    s.path([(620, 495), (1175, 495), (1175, 190), (1150, 190)], "accent", 1.8)
    s.label(800, 479, "SIN MMU", "accent")
    # nodos
    s.node(30, 130, 210, 200, "Núcleos", "2 × LX7 @ 240 MHz", "CHIP",
           lines=["ejecutar código", "leer constantes", "ROM y ajustes por API"])
    s.node(400, 100, 220, 120, "Caché de instrucciones", "16 KB · IROM", "CACHÉ",
           lines=["0x4200_0000 en adelante"], name_size=13)
    s.node(400, 230, 220, 140, "Caché de datos", "64 KB · DROM", "CACHÉ",
           lines=["0x3C00_0000 en adelante", "páginas de 64 KB"], name_size=13)
    s.node(900, 100, 250, 170, "Flash", "16 MB · persistente", "EXTERNA",
           rows=[("código", "252 KB", False), ("datos const.", "80 KB", False), ("ROM nesgame", "1 MB", False)])
    s.node(900, 300, 250, 120, "PSRAM", "8 MB · volátil", "EXTERNA", lines=["ROM en uso, lienzo, cuadros"])
    s.node(400, 450, 220, 90, "API de particiones", "esp_partition_read/write", "DRIVER", kind="focal", name_size=13)
    s.callout(510, 405, "la ventana 0x3C… es compartida", "ink", anchor="middle")
    s.callout(510, 425, "con la PSRAM", "ink", anchor="middle")
    s.callout(700, 535, "escribir o borrar suspende la caché: el código en flash se detiene", "accent", anchor="start")
    s.legend([("link", False, "acceso por la caché"), ("accent", False, "acceso directo al driver")], y=s.h - 44)
    return s, "Architecture", "Tres caminos de la flash al procesador"


def find_browser():
    for env in ("EDGE", "CHROME"):
        if os.environ.get(env):
            return os.environ[env]
    for c in ("C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
              "C:/Program Files/Microsoft/Edge/Application/msedge.exe",
              "C:/Program Files/Google/Chrome/Application/chrome.exe"):
        if os.path.exists(c):
            return c
    for n in ("msedge", "google-chrome", "chromium", "chrome"):
        if shutil.which(n):
            return shutil.which(n)
    sys.exit("No se encontro Edge ni Chrome (defina EDGE o CHROME con la ruta del ejecutable).")


def to_png(svg, dest, browser):
    with tempfile.TemporaryDirectory() as tmp:
        page = os.path.join(tmp, "d.html")
        with open(page, "w", encoding="utf-8") as fh:
            fh.write(svg.bare_html())
        cmd = [browser, "--headless", "--disable-gpu", "--hide-scrollbars", "--virtual-time-budget=15000",
               "--force-device-scale-factor=2", f"--window-size={svg.w},{svg.h}",
               f"--screenshot={dest}", "file:///" + page.replace(os.sep, "/")]
        for _ in range(3):
            subprocess.run(cmd, capture_output=True)
            if os.path.exists(dest) and os.path.getmtime(dest) > time.time() - 60:
                return
        sys.exit(f"No se pudo generar {dest}")


if __name__ == "__main__":
    out = os.path.dirname(os.path.abspath(__file__))
    html_dir = sys.argv[sys.argv.index("--html") + 1] if "--html" in sys.argv else None
    browser = find_browser()
    for theme in ("light", "dark"):
        use_theme(theme)
        for fn, name in ((memorias, "memorias"), (hardware, "hardware"), (pipeline, "pipeline-video"),
                         (flash, "flash"), (arranque, "arranque"), (caches, "caches")):
            s, eyebrow, h1 = fn()
            stem = f"{name}{'-dark' if theme == 'dark' else ''}"
            if html_dir:
                os.makedirs(html_dir, exist_ok=True)
                with open(os.path.join(html_dir, stem + ".html"), "w", encoding="utf-8") as fh:
                    fh.write(s.html(eyebrow, h1))
            dest = os.path.join(out, stem + ".png")
            if os.path.exists(dest):
                os.remove(dest)
            to_png(s, dest, browser)
            print("escrito", dest)
