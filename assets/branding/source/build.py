#!/usr/bin/env python3
"""Rebuild the Branch Truce asset pack from its outlined vector components.

Needs Python 3, rsvg-convert, and ImageMagick 7. No font installation required.
Run from any directory. RSVG_CONVERT and MAGICK can specify executable paths.
"""
import concurrent.futures
import html
import json
import math
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
G = json.loads((ROOT / 'source/geometry.json').read_text())
RSVG = os.environ.get('RSVG_CONVERT', 'rsvg-convert')
MAGICK = os.environ.get('MAGICK', 'magick')
PALETTE = {
    'forest': '#124D33', 'terracotta': '#BE6041', 'cream': '#F5F3E9',
    'warm_clay': '#E49B7A', 'dark_ground': '#16281E',
    'black': '#111713', 'white': '#FFFFFF',
}
COLORS = {
    'for-light': (PALETTE['forest'], PALETTE['terracotta']),
    'for-dark': (PALETTE['cream'], PALETTE['warm_clay']),
    'mono-black': (PALETTE['black'], PALETTE['black']),
    'mono-white': (PALETTE['white'], PALETTE['white']),
}
JOBS = []
ASSETS = []


def write(relative, content):
    path = ROOT / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    return path


def component(name, x, y, scale, color):
    return (f'<g transform="translate({x} {y}) scale({scale})" fill="{color}">'
            f'{G[name]["svg"]}</g>')


def symbol(x, y, scale, ink, accent):
    return (component('symbol', x, y, scale, ink)
            + component('accent', x, y, scale, accent))


def wordmark(x, y, scale, ink):
    # Match the original x-height and baseline; keep the traced custom lettering.
    return (component('branch', x, y, scale, ink)
            + component('truce', x + 674 * scale, y + 14 * scale, scale, ink))


def micro(x, y, scale, ink, accent):
    # This is a separately drawn optical-size variant, in a 32-unit grid.
    return f'''<g transform="translate({x} {y}) scale({scale})">
      <g fill="none" stroke="{ink}" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round">
        <path d="M10.5 3.5V22 M10.5 6.5C16 3 21 11 27.5 6.5L23 12L27.5 17.5C21 21 16 13 10.5 16.5"/>
        <path d="M10.5 22C10.5 25 5 25 5 28 M10.5 22C10.5 25 16 25 16 28"/>
      </g>
      <g fill="{ink}"><circle cx="5" cy="28" r="2.4"/><circle cx="16" cy="28" r="2.4"/></g>
      <circle cx="10.5" cy="22.3" r="3.1" fill="none"/>
      <path d="M10.5 19.2a3.1 3.1 0 1 0 0 6.2a3.1 3.1 0 1 0 0-6.2" fill="{accent}"/>
    </g>'''


def logo(layout, ink, accent, tile_background=None):
    if layout == 'full':
        return 1264, 664, (symbol(80, 80, 1, ink, accent)
                          + component('branch', 555, 197, 1, ink)
                          + component('truce', 547, 385, 1, ink))
    if layout == 'horizontal':
        return 1648, 372, symbol(60, 60, .5, ink, accent) + wordmark(350, 101, 1, ink)
    if layout == 'stacked':
        return 960, 720, symbol(294.4, 54, .8, ink, accent) + wordmark(88, 550, .65, ink)
    if layout == 'wordmark':
        return 1328, 290, wordmark(60, 60, 1, ink)
    if layout == 'symbol':
        return 640, 640, symbol(88, 68, 1, ink, accent)
    if layout == 'small':
        return 32, 32, micro(0, 0, 1, ink, accent)
    if layout in ('square', 'circle'):
        bg = tile_background or PALETTE['cream']
        shape = (f'<rect width="512" height="512" rx="104" fill="{bg}"/>' if layout == 'square'
                 else f'<circle cx="256" cy="256" r="256" fill="{bg}"/>')
        # All artwork lies within the inscribed safe circle, including in avatars.
        return 512, 512, shape + symbol(119.12, 107.32, .59, ink, accent)
    raise ValueError(layout)


def svg(width, height, body, title, background=None):
    backing = f'<rect width="{width}" height="{height}" fill="{background}"/>' if background else ''
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" role="img" aria-labelledby="title">
  <title id="title">{html.escape(title)}</title>
  <desc>Branch Truce. Soft pennant above two joining branches. Outlined custom lettering where present.</desc>
  {backing}{body}
</svg>
'''


def asset(relative, width, height, body, title, sizes=(), background=None, alpha=True):
    content = svg(width, height, body, title, background)
    path = write(relative, content)
    ASSETS.append({'path': relative, 'format': 'svg', 'width': width, 'height': height,
                   'transparent_corners': alpha, 'title': title})
    for size in sizes:
        target_width = size
        target_height = round(size * height / width)
        raster_relative = relative.replace('svg/', 'png/', 1).removesuffix('.svg') + f'-{size}.png'
        queue_png(path, raster_relative, target_width, target_height, title, alpha)
    return path


def queue_png(source, relative, width, height, title, alpha):
    target = ROOT / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    JOBS.append((source, target, width, height))
    ASSETS.append({'path': relative, 'format': 'png', 'width': width, 'height': height,
                   'transparent_corners': alpha, 'title': title})


def render(job):
    source, target, width, height = job
    subprocess.run([RSVG, '--width', str(width), '--height', str(height),
                    '--output', str(target), str(source)], check=True, capture_output=True)


def build():
    layouts = ('full', 'horizontal', 'stacked', 'wordmark', 'symbol', 'small', 'square', 'circle')
    for scheme, (ink, accent) in COLORS.items():
        tile_bg = {'for-light': PALETTE['cream'], 'for-dark': PALETTE['forest'],
                   'mono-black': PALETTE['white'], 'mono-white': PALETTE['black']}[scheme]
        for layout in layouts:
            width, height, body = logo(layout, ink, accent, tile_bg)
            sizes = (16, 24, 32) if layout == 'small' else ((256, 1024) if layout in ('symbol', 'square', 'circle') else (1024, 2048))
            asset(f'svg/{scheme}/branch-truce-{layout}.svg', width, height, body,
                  f'Branch Truce / {layout} / {scheme}', sizes)
    for scheme, background in [('for-light', PALETTE['cream']), ('for-dark', PALETTE['dark_ground'])]:
        for layout in ('full', 'horizontal', 'stacked'):
            width, height, body = logo(layout, *COLORS[scheme])
            asset(f'svg/backgrounds/branch-truce-{layout}-{scheme}.svg', width, height, body,
                  f'Branch Truce / {layout} / {scheme} background', (2048,), background, False)

    # Editable master and component files, all containing genuine vector paths.
    shutil.copy2(ROOT / 'svg/for-light/branch-truce-full.svg', ROOT / 'source/master.svg')
    for name, record in G.items():
        _, _, width, height = map(float, record['viewBox'].split())
        ink = PALETTE['terracotta'] if name == 'accent' else PALETTE['forest']
        write(f'source/components/{name}.svg', svg(width, height, component(name, 0, 0, 1, ink), f'Branch Truce / {name} component'))

    # Linux hicolor structure is ready to copy into an install prefix.
    desktop_name = 'local.branch_truce.BranchTruce'
    for size in (16, 22, 24, 32, 48, 64, 128, 256, 512, 1024):
        layout = 'small' if size <= 32 else 'symbol'
        queue_png(ROOT / f'svg/for-light/branch-truce-{layout}.svg',
                  f'platform/linux/hicolor/{size}x{size}/apps/{desktop_name}.png',
                  size, size, 'Branch Truce Linux application icon', True)
    write(f'platform/linux/hicolor/scalable/apps/{desktop_name}.svg',
          (ROOT / 'svg/for-light/branch-truce-symbol.svg').read_text())
    symbolic = (ROOT / 'svg/mono-black/branch-truce-small.svg').read_text().replace(PALETTE['black'], 'currentColor')
    write(f'platform/linux/hicolor/symbolic/apps/{desktop_name}-symbolic.svg', symbolic)

    # Web app icons have an opaque square canvas. The maskable content stays
    # inside the platform's central safe area rather than baking in a crop.
    web_ink, web_accent = COLORS['for-dark']
    web_body = symbol(119.12, 107.32, .59, web_ink, web_accent)
    web_svg = asset('web/app-icon.svg', 512, 512, web_body, 'Branch Truce web application icon',
                    background=PALETTE['forest'], alpha=False)
    for name, size in [('apple-touch-icon.png', 180), ('icon-192.png', 192), ('icon-512.png', 512),
                       ('maskable-192.png', 192), ('maskable-512.png', 512)]:
        queue_png(web_svg, f'web/{name}', size, size, name, False)
    favicon_svg = asset('web/favicon.svg', 32, 32, micro(0, 0, 1, web_ink, web_accent),
                        'Branch Truce favicon', background=PALETTE['forest'], alpha=False)
    for size in (16, 32, 48):
        queue_png(favicon_svg, f'web/favicon-{size}.png', size, size, f'Favicon {size}px', False)
    write('web/site.webmanifest', json.dumps({
        'name': 'Branch Truce', 'short_name': 'Branch Truce', 'start_url': '.',
        'display': 'standalone', 'background_color': PALETTE['forest'], 'theme_color': PALETTE['forest'],
        'icons': [
            {'src': f'icon-{s}.png', 'sizes': f'{s}x{s}', 'type': 'image/png', 'purpose': 'any'} for s in (192, 512)
        ] + [
            {'src': f'maskable-{s}.png', 'sizes': f'{s}x{s}', 'type': 'image/png', 'purpose': 'maskable'} for s in (192, 512)
        ]}, indent=2) + '\n')
    write('web/integration.html.txt', '''<!-- Copy this directory to /brand, then add to the document head. -->
<link rel="icon" href="/brand/favicon.svg" type="image/svg+xml">
<link rel="icon" href="/brand/favicon.ico" sizes="16x16 32x32 48x48">
<link rel="apple-touch-icon" href="/brand/apple-touch-icon.png" sizes="180x180">
<link rel="manifest" href="/brand/site.webmanifest">
<meta name="theme-color" content="#124D33">
<!-- Set start_url and scope in the manifest for the site being deployed. -->
''')

    # Standard social sharing images, with only the approved logo.
    for scheme, background in [('for-light', PALETTE['cream']), ('for-dark', PALETTE['dark_ground'])]:
        ink, accent = COLORS[scheme]
        _, _, body = logo('full', ink, accent)
        body = f'<g transform="translate(126 66) scale(.75)">{body}</g>'
        source = asset(f'social/branch-truce-og-{scheme}.svg', 1200, 630, body,
                       f'Branch Truce social share / {scheme}', background=background, alpha=False)
        queue_png(source, f'social/branch-truce-og-{scheme}.png', 1200, 630, 'Social sharing image', False)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(render, JOBS))
    subprocess.run([MAGICK] + [str(ROOT / f'web/favicon-{s}.png') for s in (16, 32, 48)]
                   + [str(ROOT / 'web/favicon.ico')], check=True, capture_output=True)
    write('palette.json', json.dumps(PALETTE, indent=2) + '\n')
    write('palette.gpl', 'GIMP Palette\nName: Branch Truce\nColumns: 7\n#\n' + '\n'.join(
        f'{int(color[1:3],16):3} {int(color[3:5],16):3} {int(color[5:7],16):3}\t{name}'
        for name, color in PALETTE.items()) + '\n')
    write('asset-index.json', json.dumps({'project': 'Branch Truce', 'selection': '3C', 'version': 1,
           'lettering': 'Custom outlined shapes; no font dependency', 'color_space': 'sRGB',
           'assets': ASSETS}, indent=2) + '\n')
    print(f'Built {len(ASSETS)} indexed assets, including {len(JOBS)} PNG renders.')


if __name__ == '__main__':
    build()
