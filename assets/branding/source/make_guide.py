#!/usr/bin/env python3
"""Create a portable visual guide with embedded assets and no external files."""
import base64
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PALETTE = json.loads((ROOT / 'palette.json').read_text())


def data_uri(relative, background=None):
    path = ROOT / relative
    content = path.read_bytes()
    mime = 'image/png' if path.suffix == '.png' else 'image/svg+xml'
    if background and path.suffix == '.svg':
        text = content.decode()
        pos = text.index('>') + 1
        text = text[:pos] + f'<rect width="100%" height="100%" fill="{background}"/>' + text[pos:]
        content = text.encode()
    return f'data:{mime};base64,' + base64.b64encode(content).decode()


def card(title, subtitle, relative, background=None, wide=False):
    return f'''<article class="card{' wide' if wide else ''}"><img class="sample" src="{data_uri(relative, background)}" alt="{title}"/>
<h2>{title}</h2><p>{subtitle}</p></article>'''


def main():
    cards = ''.join([
        card('Full logo · light', 'Primary layout in forest green and terracotta.', 'svg/backgrounds/branch-truce-full-for-light.svg'),
        card('Full logo · dark', 'Cream and warm clay for dark backgrounds.', 'svg/backgrounds/branch-truce-full-for-dark.svg'),
        card('Horizontal', 'One-line lettering for headers and navigation.', 'svg/backgrounds/branch-truce-horizontal-for-light.svg', wide=True),
        card('Stacked', 'Flag above the name for taller spaces.', 'svg/for-light/branch-truce-stacked.svg', PALETTE['cream']),
        card('Wordmark', 'Custom outlined lettering, with no font dependency.', 'svg/for-dark/branch-truce-wordmark.svg', PALETTE['dark_ground']),
    ])
    icons = ''.join([
        card('Square', 'App tile', 'svg/for-dark/branch-truce-square.svg'),
        card('Circle', 'Avatar', 'svg/for-light/branch-truce-circle.svg'),
        card('Symbol', 'Transparent icon', 'svg/for-light/branch-truce-symbol.svg', PALETTE['cream']),
    ])
    mono = ''.join([
        card('Monochrome dark', 'One dark ink on a light field.', 'svg/mono-black/branch-truce-horizontal.svg', '#FFFFFF'),
        card('Monochrome white', 'White ink on a dark field.', 'svg/mono-white/branch-truce-horizontal.svg', PALETTE['dark_ground']),
    ])
    small = ''.join(f'<div class="size"><img src="{data_uri(f"png/for-light/branch-truce-small-{s}.png")}" width="{s}" height="{s}" alt="Small mark at {s} pixels"><span>{s} px</span></div>' for s in (16,24,32))
    small += f'<div class="size"><img src="{data_uri("platform/linux/hicolor/48x48/apps/com.canpicasoft.branch-truce.png")}" width="48" height="48" alt="Detailed mark at 48 pixels"><span>48 px</span></div>'
    # The small-size proof has a real light background inside an SVG asset.
    swatches = ''.join(f'<div class="swatch"><svg viewBox="0 0 100 30" aria-label="{name}"><rect width="100" height="30" rx="5" fill="{color}"/></svg><span>{name.replace("_"," ")}</span><code>{color}</code></div>' for name,color in list(PALETTE.items())[:5])
    text = f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Branch Truce logo asset guide</title>
<link rel="icon" href="{data_uri('web/favicon.svg')}" type="image/svg+xml"><link rel="icon" href="{data_uri('web/favicon-32.png')}" sizes="32x32" type="image/png">
<style>
*{{box-sizing:border-box}}html,body{{margin:0;padding:0}}body{{font-family:var(--font-sans,system-ui,sans-serif);color:var(--foreground,CanvasText);line-height:1.5}}
.grid{{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}}.icons{{grid-template-columns:repeat(3,minmax(0,1fr));margin-top:16px}}.wide{{grid-column:1/-1}}
.card,.note{{background:var(--card,Canvas);border:1px solid var(--border,ButtonBorder);border-radius:var(--radius,12px);padding:16px;min-width:0}}
.sample{{width:100%;height:185px;object-fit:contain;display:block}}.wide .sample{{height:auto;max-height:180px}}.icons .sample{{height:135px}}
h2{{font-size:15px;line-height:1.4;margin:12px 0 0}}p{{font-size:13px;color:var(--muted-foreground,GrayText);margin:4px 0 0}}.mono{{margin-top:16px}}.mono .sample{{height:88px}}
.note{{margin-top:16px}}.note h2{{margin-top:0}}.proof{{display:flex;gap:32px;align-items:end;padding:16px;margin:12px 0 0;position:relative;isolation:isolate}}
.proof-bg{{position:absolute;inset:0;width:100%;height:100%;z-index:-1}}.size{{display:flex;align-items:center;gap:10px;flex-direction:column}}.size img{{display:block;flex:none}}.size span{{font-size:11px;color:var(--muted-foreground,GrayText);background:var(--card,Canvas);padding:1px 5px;border-radius:4px}}
.swatches{{display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:12px;margin-top:14px}}.swatch svg{{display:block;width:100%;height:30px}}.swatch span,.swatch code{{display:block;font-size:11px;margin-top:5px}}code{{font-family:var(--font-mono,monospace)}}
table{{border-collapse:collapse;width:100%;font-size:12px;margin-top:12px}}td,th{{text-align:left;padding:8px 0;border-bottom:1px solid var(--border,ButtonBorder)}}th{{font-weight:600}}td:first-child{{width:31%;font-family:var(--font-mono,monospace)}}
details{{margin-top:12px;font-size:13px}}summary{{cursor:pointer;font-weight:600}}.usage p{{margin-top:10px}}
@media(max-width:480px){{.grid{{grid-template-columns:minmax(0,1fr)}}.icons{{grid-template-columns:repeat(3,minmax(0,1fr))}}.icons .sample{{height:80px}}.icons .card{{padding:12px}}.icons p{{display:none}}.proof{{gap:22px}}.swatches{{grid-template-columns:repeat(3,minmax(0,1fr))}}}}
</style></head><body>
<main>
<div class="grid">{cards}</div>
<div class="grid icons">{icons}</div>
<div class="grid mono">{mono}</div>
<section class="note"><h2>Small sizes</h2><p>Simplified mark at 16–32 px. Detailed mark from 48 px. Samples below are shown at their actual size.</p>
<div class="proof"><svg class="proof-bg" aria-hidden="true"><rect width="100%" height="100%" rx="8" fill="{PALETTE['cream']}"/></svg>{small}</div></section>
<section class="note"><h2>Color palette</h2><p>Flat RGB colors, plus black and white monochrome exports.</p><div class="swatches">{swatches}</div></section>
<section class="note usage"><h2>Using the pack</h2>
<table><thead><tr><th>Folder</th><th>Use</th></tr></thead><tbody>
<tr><td>svg/</td><td>Editable vector paths. Choose for-light, for-dark, mono-black, or mono-white.</td></tr>
<tr><td>png/</td><td>Ready-to-use raster images. The filename gives width in pixels.</td></tr>
<tr><td>platform/linux/</td><td>App icons from 16 to 1024 px, scalable SVG, and symbolic SVG.</td></tr>
<tr><td>web/</td><td>Favicons, Apple touch icon, regular and maskable app icons, and webmanifest.</td></tr>
<tr><td>social/</td><td>Light and dark 1200 × 630 sharing images.</td></tr>
<tr><td>source/</td><td>Master SVG, outlined components, approved reference, and rebuild scripts.</td></tr>
</tbody></table>
<p>The full, horizontal, stacked, wordmark, and symbol versions have transparent backgrounds. Files in backgrounds/ have a colored canvas. Square and circle versions carry their own fields.</p>
<details><summary>Size, spacing, and source notes</summary>
<p>Suggested minimum widths: full 220 px, horizontal 320 px, stacked 200 px, wordmark 180 px. At smaller sizes use the symbol, or the small drawing at 16–32 px.</p>
<p>Preserve the proportions and supplied margins. Keep a lower-node diameter of clear space around a full logo. Dedicated small icons and tiles have fitted spacing.</p>
<p>Lettering is outlined custom artwork. You can edit its vector paths without installing a font. The approved 3C raster is included alongside the flattened imagegen master used for tracing.</p>
<p>README.txt contains the complete usage notes. asset-index.json lists dimensions and transparency. Source scripts rebuild the exports with Python 3, rsvg-convert, and ImageMagick 7.</p>
</details></section>
</main></body></html>'''
    (ROOT/'GUIDE.html').write_text(text)
    print(f'Created self-contained GUIDE.html, {len(text.encode())} bytes.')


if __name__ == '__main__':
    main()
