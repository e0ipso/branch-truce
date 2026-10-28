Branch Truce logo asset pack
Selected identity: 3C, soft pennant

START HERE

Open GUIDE.html to see the layouts, color variants, and small icons.
For a general-purpose logo on a light background, use:
  svg/for-light/branch-truce-full.svg
For a navigation bar or README header, use:
  svg/for-light/branch-truce-horizontal.svg
For an application launcher, use the files in platform/linux/hicolor/.
For a website, copy the web/ directory and adapt web/integration.html.txt.

WHAT IS INCLUDED

Eight layouts, each in four color variants:
  full        Flag beside the original two-line lettering.
  horizontal  Flag beside the name on one line.
  stacked     Flag above the name, for taller spaces.
  wordmark    Name on one line, without the flag.
  symbol      Detailed flag without lettering, on a square transparent canvas.
  small       A simpler flag drawn for 16-32 px, with heavier lines.
  square      Rounded square tile with an opaque colored field.
  circle      Circular avatar with transparent outer corners.

The four color folders describe the intended placement:
  for-light   Forest green and terracotta for light backgrounds.
  for-dark    Cream and warm clay for dark backgrounds.
  mono-black  One dark ink for monochrome reproduction.
  mono-white  White for reversed monochrome reproduction.

The square and circle versions carry their own backgrounds. The other layouts
have transparent backgrounds. Additional full, horizontal, and stacked logos
with opaque cream or dark backgrounds are in svg/backgrounds/ and png/backgrounds/.

FORMATS AND SIZES

SVG: Genuine scalable paths, including the lettering. No embedded raster images,
external files, font downloads, or installed fonts are required. The letter shapes
can be edited as vector paths; the wordmark is not live editable text.

PNG: Transparent or opaque as described above. The number in each filename is
its width in pixels; height keeps the SVG's proportions. Full layouts are exported
at 1024 and 2048 px wide. Symbols and tiles are 256 and 1024 px square. Small marks
are 16, 24, and 32 px square. Use SVG when another size is needed.

Linux: hicolor icons at 16, 22, 24, 32, 48, 64, 128, 256, 512, and 1024 px, plus
scalable and symbolic SVG files. Icons up to 32 px use the small drawing. The
files are named after the application ID, com.canpicasoft.branch-truce, which
is also the desktop entry's icon name. For example:
  platform/linux/hicolor/48x48/apps/com.canpicasoft.branch-truce.png
  platform/linux/hicolor/scalable/apps/com.canpicasoft.branch-truce.svg
  platform/linux/hicolor/symbolic/apps/com.canpicasoft.branch-truce-symbolic.svg
This pack does not install or replace files in the application.

Web: SVG favicon, a multi-size ICO containing 16/32/48 px, matching PNG favicons,
180 px Apple touch icon, 192/512 px regular and maskable app icons, webmanifest,
and a head-tag example. Adjust start_url and scope for the deployed website.
Maskable icons have a full opaque square and artwork inside the central safe area.

Social: Light and dark 1200 x 630 PNGs with SVG sources.

COLORS

Forest green   #124D33   Main ink and filled app tiles
Terracotta     #BE6041   Merge junction on light backgrounds
Cream          #F5F3E9   Light field and reversed lettering
Warm clay      #E49B7A   Merge junction on dark backgrounds
Dark ground    #16281E   Dark presentation background
Black          #111713   Monochrome dark ink
White          #FFFFFF  Monochrome reverse

All files use RGB colors. palette.json and palette.gpl contain the swatches.
For commercial printing, give the printer the SVG and agree on an output profile.
This pack does not claim a calibrated CMYK or spot-color match.

USAGE

Use the small mark for 16-32 px. Use the detailed symbol from 48 px upward.
Suggested minimum widths: full 220 px, horizontal 320 px, stacked 200 px,
and wordmark 180 px. Below those sizes, prefer a symbol-only version.

Preserve the supplied aspect ratios and margins. Keep at least one lower-node
diameter of clear space around a full logo. The dedicated small icons and tiles
already have their own fitted spacing. Use the circle version for circular crops.

Place for-light files on light, quiet backgrounds and for-dark files on dark ones.
Use monochrome files when only one ink is available. Keep the pennant, branches,
merge dot, and lettering together in their supplied proportions. Avoid stretching,
shadows, gradients, glows, and recoloring individual letters.

SOURCE AND REBUILDING

source/master.svg is the default full logo. source/components/ and geometry.json
contain separate outlined shapes for the flag, accent dot, and the two words.
source/selected-3c.png preserves the approved concept. transparent-master.png is
the imagegen cleanup used for tracing. generation.json contains the exact prompt.
The production SVGs use flat colors and smoothed paths derived from that master.
The small mark is a separate vector drawing for legibility at small sizes.

Rebuild the pack with Python 3, librsvg's rsvg-convert, and ImageMagick 7.
From this directory, run:
  python3 source/rebuild.py

The script runs source/build.py for the exports, then source/make_guide.py for
GUIDE.html, and then rewrites SHA256SUMS.txt. Run it after any change to the
pack, including edits to this file, so that the checksums stay current.
RSVG_CONVERT and MAGICK may specify executable paths. Change the PALETTE values
in source/build.py to adjust colors consistently. No network access is required
after these tools are installed. Rebuilding intentionally replaces the pack's
generated exports. The original concept and source geometry stay intact.

asset-index.json lists export dimensions and transparency expectations.
VERIFICATION.txt records the checks performed for this pack.
SHA256SUMS.txt covers every file in the pack except itself. To verify the pack,
run this from this directory:
  sha256sum -c SHA256SUMS.txt
