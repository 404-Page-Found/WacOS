# Logo do WacOS no estilo dos anos 90: a silhueta da maçã (icones/simbolos/apple.logo.svg; marca da Apple, ver a nota
# do README) em pixel art de 32x32, pintada com as quatro cores da bandeira do Windows da época (vermelho, verde, azul,
# amarelo) em quadrantes separados por uma cruz fina, com contorno preto e sombra dura. Ampliada sem suavizar.
# Saída: docs/logo.png (1024), docs/logo-256.png e bin\obj\res\WacOS.ico. Rode: python -I tools/logo-wacos.py
import os, re
from PIL import Image, ImageDraw

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
G = 32  # grade de pixels


def svg_polys(path):
    """Contornos do SVG de um path só (M/L/Q/C/Z), com o y já desinvertido. Devolve (polígonos, largura, altura)."""
    txt = open(path, encoding='utf-8').read()
    w = float(re.search(r"width='([\d.]+)", txt).group(1))
    h = float(re.search(r"height='([\d.]+)", txt).group(1))
    toks = re.findall(r"[MLQCZ]|-?[\d.]+", re.search(r"d='([^']+)'", txt, re.S).group(1))
    subs, cur, i, cmd, pos = [], [], 0, None, (0.0, 0.0)

    def num():
        nonlocal i
        v = float(toks[i]); i += 1
        return v
    while i < len(toks):
        if toks[i] in 'MLQCZ':
            cmd = toks[i]; i += 1
            if cmd == 'Z':
                if cur: subs.append(cur); cur = []
                continue
        if cmd == 'M':
            if cur: subs.append(cur)
            pos = (num(), num()); cur = [pos]; cmd = 'L'
        elif cmd == 'L':
            pos = (num(), num()); cur.append(pos)
        elif cmd == 'Q':
            c1 = (num(), num()); e = (num(), num())
            for k in range(1, 13):
                t = k / 12
                cur.append(tuple((1 - t) ** 2 * pos[j] + 2 * (1 - t) * t * c1[j] + t * t * e[j] for j in range(2)))
            pos = e
        elif cmd == 'C':
            c1 = (num(), num()); c2 = (num(), num()); e = (num(), num())
            for k in range(1, 17):
                t = k / 16
                cur.append(tuple((1 - t) ** 3 * pos[j] + 3 * (1 - t) ** 2 * t * c1[j] + 3 * (1 - t) * t * t * c2[j] + t ** 3 * e[j] for j in range(2)))
            pos = e
    if cur: subs.append(cur)
    return [[(x, h - y) for x, y in s] for s in subs], w, h


def apple_pixels():
    """A maçã rasterizada na grade de 32: desenha grande (32 x 16) e marca o pixel quando metade dele é maçã."""
    polys, w, h = svg_polys(os.path.join(root, 'icones', 'simbolos', 'apple.logo.svg'))
    Z = 16
    big = Image.new('L', (G * Z, G * Z), 0)
    area = 29 * Z                                   # a maçã ocupa 29 px de altura (sobra o contorno e a sombra)
    k = area / h
    ox = (G * Z - w * k) / 2 - Z * 0.5
    oy = (G * Z - h * k) / 2 - Z * 0.5
    dr = ImageDraw.Draw(big)
    for s in polys:
        dr.polygon([(ox + x * k, oy + y * k) for x, y in s], fill=255)
    small = big.resize((G, G), Image.BOX)           # média de cada célula
    return small.point(lambda v: 255 if v >= 128 else 0)


def main():
    m = apple_pixels()
    mp = m.load()
    bb = m.getbbox()
    # onde a folha termina e o corpo começa: a primeira linha larga (a folha tem poucos pixels por linha; grande, ela
    # chega a encostar no corpo e não sobra linha vazia entre os dois)
    widths = [sum(1 for x in range(G) if mp[x, y]) for y in range(G)]
    body_top = next(y for y in range(G) if widths[y] >= max(widths) * 0.4)
    # a cruz que divide as cores: no meio do corpo
    cx = (bb[0] + bb[2]) // 2 - 1
    cy = body_top + round((bb[3] - body_top) * 0.47)  # um pouco acima do meio: a base da maçã afina, então as cores de baixo ganham linhas
    RED, GREEN, BLUE, YELLOW = (240, 48, 32), (48, 184, 56), (32, 104, 232), (252, 204, 24)
    im = Image.new('RGBA', (G, G), (0, 0, 0, 0))
    px = im.load()
    for yy in range(G):
        for xx in range(G):
            if not mp[xx, yy]:
                continue
            if yy < body_top:
                c = GREEN                                   # a folha
            elif xx == cx or yy == cy:
                c = (255, 255, 255)                         # a cruz da bandeira
            elif yy < cy:
                c = RED if xx < cx else GREEN
            else:
                c = BLUE if xx < cx else YELLOW
            # brilho de 1 px no alto de cada cor e sombra de 1 px embaixo (relevo dos ícones dos anos 90)
            if c != (255, 255, 255):
                up = yy > 0 and not mp[xx, yy - 1]
                down = yy < G - 1 and not mp[xx, yy + 1]
                if up or (yy == cy + 1) or (yy == body_top and xx != cx):
                    c = tuple(min(255, v + 60) for v in c)
                elif down or yy == cy - 1:
                    c = tuple(int(v * 0.72) for v in c)
            px[xx, yy] = c + (255,)
    # contorno preto de 1 px e sombra dura (1 px para baixo e para a direita)
    out = Image.new('RGBA', (G, G), (0, 0, 0, 0))
    op = out.load()
    for yy in range(G):
        for xx in range(G):
            if mp[xx, yy]:
                continue
            near = any(0 <= xx + dx < G and 0 <= yy + dy < G and mp[xx + dx, yy + dy]
                       for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            if near:
                op[xx, yy] = (0, 0, 0, 255)
    shadow = Image.new('RGBA', (G, G), (0, 0, 0, 0))
    sp = shadow.load()
    a = Image.alpha_composite(out, im)
    ap = a.load()
    for yy in range(G - 1):
        for xx in range(G - 1):
            if ap[xx, yy][3]:
                sp[xx + 1, yy + 1] = (0, 0, 0, 80)
    final = Image.alpha_composite(shadow, a)

    big = final.resize((1024, 1024), Image.NEAREST)
    os.makedirs(os.path.join(root, 'docs'), exist_ok=True)
    big.save(os.path.join(root, 'docs', 'logo.png'))
    final.resize((256, 256), Image.NEAREST).save(os.path.join(root, 'docs', 'logo-256.png'))
    res = os.path.join(root, 'bin', 'obj', 'res')
    os.makedirs(res, exist_ok=True)
    big.save(os.path.join(res, 'WacOS.ico'), sizes=[(16, 16), (32, 32), (64, 64), (128, 128), (256, 256)])
    print('docs/logo.png, docs/logo-256.png, bin/obj/res/WacOS.ico')


if __name__ == '__main__':
    main()
