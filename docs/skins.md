# Skins: styling Tiny DAW with CSS

Everything Tiny DAW draws takes its look from stylesheets written in CSS: every colour, font, padding,
border, corner radius, line width and size of the layout. This page lists everything that can be styled.

- [How skins work](#how-skins-work)
- [The CSS understood](#the-css-understood)
- [What rules match: types, ids, classes, states, parts](#what-rules-match)
- [Variables](#variables): colours, shades, fonts, sizes (made from `default.css` itself)
- [Types](#types): the controls, then the program's own surfaces, with their parts, states and properties
- [Examples](#examples)

## How skins work

`data/skins/default.css` is the program's own stylesheet. It defines every colour as a variable of `:root`
(`--lane`, `--playhead`…), works out the shades from them (`--hover: color-mix(…)`), sets the fonts and the
layout's sizes, and holds the rules that draw every control and surface from those variables. The same file
is built into the program, and is used if it can't be read from disk.

A **skin** is a stylesheet put on top of `default.css`: it changes what it names and keeps the rest. A skin
that only sets some variables recolours everything that uses them; a skin can also change any rule. Its name
is its `--skin-name` (else its file's name).

- The program's own skins (Dark, Light, Midnight, OLED, High contrast, three looks of classic desktops: 90s, XP
  and 7, and Neon District: a neon-noir look with corners cut at 45° by small pictures, a font of its own and glowing
  edges) are in `data/skins`: each is a readable example of what a skin can do. Yours are in
  `~/.local/share/TinyDAW/skins` (Options → Skin → Open the skins folder).
- **Options → Skin → Edit this skin…** opens the skin's stylesheet in your text editor (the one your desktop
  opens `.css` files with). One of the program's own skins is copied into your folder first, under a name you
  choose: you edit your copy. The copy takes the files the skin uses with it (see **Copies** below).
- **Live:** Tiny DAW checks the skin's file and `default.css` twice a second (and every file they `@import`, and
  every image their `url()`s name), and puts the skin on again as soon as one is saved. The status bar says
  "Skin … reloaded" (and how many problems it has).
- **Problems:** like a browser, Tiny DAW leaves out what it can't read (a rule, a declaration, a value) and keeps
  the default there; nothing in a stylesheet can stop it from starting. Options → Skin → **Skin problems** lists
  what was left out, with the file, line and column: `Mine.css:12:5: unknown colour 'blu' (left out)`.
  A colour variable that isn't a colour is reported where it is set:
  `Mine.css:4:5: --lane: 'blu' is not a colour (the default is used)`. A value that uses `var()` can only be
  checked with the variable's value: those whose variables are set in `:root` alone are checked as soon as the
  skin is put on (`… (after var(); 'background-size' is left unset)`), the others where they are used.
- **Import skin…** copies a `.css` skin into your folder and uses it (asking before it replaces one of the same
  name); **Export this skin…** saves a copy anywhere.
- **Copies** (Edit this skin…, Import skin…, Export this skin…) take every file the stylesheet uses with it: its
  `url()` images, its `@font-face` fonts (and the licence and read-me files beside them) and the stylesheets it
  `@import`s, theirs too. They go into a folder of their own beside the copy, named after it (`My XP.css` and
  `My XP/`), placed as they were from one another, and the copy's `url()`s are changed to name them there
  (`url("My XP/xp/check.svg")`). So each copy is whole on its own: it works wherever it is moved, and changing
  its files changes no other skin. A file already in that folder that is the same is left as it is; one that
  differs is never overwritten: the files then go into `My XP (2)/` (or the first `(n)` where nothing
  differs). An export is `the name you gave.css` with its folder beside it: send both.
- **Skins of earlier versions** (JSON) in your skins folder become stylesheets the first time the skins are
  listed. The JSON files are moved into the folder's `old JSON skins`; whatever couldn't be kept is noted in a
  comment at the end of the new stylesheet.
- **Options → Rounded corners** off draws every corner square, whatever the stylesheets say.
- Sizes are in logical pixels: Options → Interface size scales them like everything else.

## The CSS understood

A defined part of CSS, read by Tiny DAW's own parser (`src/core/css.h`):

- **Selectors:** a type (`Button`), `*`, `.class`, `#id`, states (`:hover`), `:root`, a part (`::handle`, on
  the last compound only); the combinators `A B` (descendant), `A > B` (child), `A + B` (the sibling right
  before) and `A ~ B` (any sibling before); lists (`A, B`); `:not(…)`, `:is(…)` and `:where(…)` of selector
  lists (nested up to 16 deep, without parts); a node's place among its siblings: `:first-child`, `:last-child`,
  `:only-child`, `:nth-child(an+b | odd | even)`, `:nth-last-child(…)`. Specificity as in CSS: ids, then
  classes / states / place / `:root`, then types / parts; `:not` and `:is` count as their most specific
  argument, `:where` as nothing. Then source order; later stylesheets win over earlier ones at equal
  specificity; `!important` wins over all of that. A sibling's hover, press or focus restyles the siblings
  after it (`Button:hover + Button`). Not understood: attribute selectors, `:has()`, `:nth-child(… of S)`,
  nesting.
- **Colours:** `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()` / `rgba()` (commas or spaces, `/ alpha`, numbers or
  %), `hsl()` / `hsla()`, the CSS named colours, `transparent`, `currentColor`, and
  `color-mix(in srgb, A p%, B q%)`. Mixing two opaque colours is exactly the program's own mix, to the byte.
- **Lengths:** `px`, `pt`, `em` (of the node's font size), `rem` (of `:root`'s font size), `ch` (the width of
  "0" in the node's font), `vw`, `vh`, `vmin`, `vmax` (of the main window's size), `%` (of what the property
  refers to), `in`, `cm`, `mm`, `Q`, `pc` (at 96 dpi), and `calc()` of them (`+ - * /`). Where a percentage
  is of the box a background is drawn in (`background-size`, `background-position`, a gradient's colour stops,
  a radial gradient's size and `at` position), `calc()` may mix percentages and lengths, `calc(100% - 8px)`,
  also through `var()`: it is worked out against the box each time it is drawn (a size below 0 is 0).
  Multiplying or dividing by a length (`calc(100% * 2px)`) is not a value (reported). `var(--name,
  fallback)`, nested.
- **`@import`** `url("x.css")`, `url(x.css)` or `"x.css"` (with an optional media list), before the rules:
  the file is read from the importing stylesheet's folder, and its rules come where the `@import` is. Imports
  may import (8 levels); loops, missing files and remote URLs are reported. Saving an imported file reloads
  the skin too.
- **`@media`** `<query list> { rules }` (nested up to 32 deep): `[not|only] all|screen [and (feature)…]` or
  `[not] (feature) [and (feature)…]`, joined by commas. Features: `width`, `height` and their `min-` /
  `max-` forms and ranges (`(width >= 600px)`, `(400px < width < 800px)`), of the main window (px, or em /
  rem at 16 px); `orientation`; `resolution` and `min-` / `max-resolution` in `dppx`, `x`, `dpi` or `dpcm`
  (the interface size: 150 % is `1.5dppx`). Re-worked out whenever the window is resized. An unknown
  feature makes its query false (reported).
- **`@font-face`** `{ font-family: "Name"; src: url(file.ttf); font-weight: bold; font-style: italic }`: a
  font file (TrueType or OpenType) next to the skin, named in `font-family` like any installed font. The
  first local `url()` in `src` counts.
- **Shorthands:** `padding`, `margin`, `border-width`, `border-color`, `border-radius` (1–4 values); `border`,
  `border-top`… (width, style and colour in any order); `background` (per layer, in any order: an image (a
  gradient or a `url()`), a repeat, a position with an optional `/ size`, one or two boxes (the origin, then the
  clip), and on the last layer the colour: `background: url(90s/desk.png) center / cover no-repeat #008080`;
  the attachment `scroll` / `fixed` / `local` is not read, and any other word makes it a bad value, reported).
- **Fonts:** `font-family` is a list, tried in order: the first family this computer has (or a skin's
  `@font-face`) is used, so `"Tahoma", "Liberation Sans", sans-serif` falls back as in a browser. `system-ui`,
  `sans-serif` and `serif` are the interface's own font, `monospace` the numbers font. If none of the list is
  there, its first name is asked for and the nearest look-alike drawn. `font-size`, `font-weight` (bold from 600).
- **Inherited:** custom properties (`--name`), the text properties (`color`, `font-family`, `font-size`,
  `font-weight`, `font-style`, `letter-spacing`, `font-smooth`, `-webkit-font-smoothing`) and `image-rendering`.
  The rest isn't.
- **Not understood (skipped and reported):** other at-rules (`@supports`, `@keyframes`, `@layer`…;
  `@charset` is ignored), units other than those above.
- **Boxes:** a box is its `background-color`, its `background-image` over that, and its border, with
  rounded corners: each side its own width (`border-top-width`…), colour (`border-top-color`…) and style
  (`border-top-style`…), and each corner its own radius (`border-top-left-radius`…; one value per corner, not
  elliptical). The sides meet on the diagonal of each corner, anti-aliased.
- **Border styles:** `solid`, `dashed`, `dotted` (round dots from 3 px), `double` (from 3 px), `inset`, `outset`,
  `groove`, `ridge` (lighter and darker shades of the border colour; from 2 px, four tones, so `2px outset
  silver` is the classic raised bevel), `none` and `hidden` (the side has no width). Dashes and dots follow
  rounded corners.
- **Gradients** in `background-image` or `background` (the first on top, over the colour):
  `linear-gradient()`, `radial-gradient()`, `repeating-linear-gradient()`, `repeating-radial-gradient()`.
  Directions: an angle (`deg`, `rad`, `turn`, `grad`) or `to` a side or corner. Radial: `circle` or `ellipse`,
  a size (`closest-side`, `closest-corner`, `farthest-side`, `farthest-corner`, or lengths / percentages), `at`
  a position. Colour stops with none, one or two positions (`%`, lengths, or `calc()` of both: `red
  calc(100% - 1px), blue calc(100% - 1px)` is a 1 px blue line at the end). Colours are mixed premultiplied
  (a fade to `transparent` doesn't go grey). Not understood: conic gradients, transition hints.
- **Images:** `url(file.png)`, `url("file.svg")` layers in `background-image` or `background`, mixed with
  gradients (the first on top). The file is found from the folder of the stylesheet that names it (also through
  a variable: `--check: url(90s/check.svg)` in a skin in `data/skins` names `data/skins/90s/check.svg`
  wherever `var(--check)` is used). PNG (any kind) and SVG (what the program's own icons use: paths, shapes,
  groups, transforms, fills and strokes, `opacity`). An SVG's `viewBox` is fitted to the size it is drawn at
  as its `preserveAspectRatio` says: `none` stretches it each way on its own (an SVG background with
  `preserveAspectRatio="none"` and `background-size: 2px 100%` is 2 px across and as tall as the box), the
  alignments `xMinYMin` … `xMaxYMax` with `meet` (all of it, the default `xMidYMid meet`) or `slice` (all of the
  area, cut); nested `<svg>` elements too. Fills and strokes can be gradients, `fill="url(#id) [fallback]"`
  naming a `<linearGradient>` (`x1 y1 x2 y2`) or `<radialGradient>` (`cx cy r`, a focal point `fx fy`, `fr`)
  anywhere in the file: `<stop>`s with `offset` (number or %), `stop-color` and `stop-opacity` (attributes or
  in `style`), `gradientUnits` (`objectBoundingBox`, the default: of the shape's box; `userSpaceOnUse`:
  lengths, % of the viewBox), `gradientTransform`, `spreadMethod` (`pad`, `reflect`, `repeat`), and
  `href` / `xlink:href` to another gradient in the file (its stops, and the attributes this one doesn't give).
  A gradient with one stop is its colour, with none nothing; a `url()` to anything else (a pattern, another
  file) takes the fallback colour, else nothing. Not drawn: text, `<use>`, patterns, clip paths, masks,
  filters. An SVG is drawn at the screen's pixels, so it is sharp at any
  interface size; a PNG is drawn at its size times the interface size, pixel for pixel with
  `image-rendering: pixelated` (or `crisp-edges`: the nearest pixel, for dithered 90s pictures), smoothly
  scaled otherwise (`auto`). Each layer takes its value of these lists (repeated to the layers' number, as in
  CSS):
  - `background-repeat`: `repeat`, `no-repeat`, `space` (whole images, the room left spread between them),
    `round` (whole images, stretched to fill), one or two (x then y), or `repeat-x`, `repeat-y`.
  - `background-position`: one to four values: keywords (`left`, `center`, `right`, `top`, `bottom`), lengths,
    percentages (`30% 2px`), `calc()` of both (`calc(100% - 8px) 0`: 8 px left of the right edge), edges with
    offsets (`right 4px bottom 2px`, `right calc(10% + 2px) top`). Images are placed on whole pixels.
  - `background-size`: `auto` (its own size; an SVG without `width` and `height`: the box's), `cover`,
    `contain`, or one or two lengths / percentages / `calc()` of both / `auto` (one `auto`: its proportions
    kept): `2px calc(100% - 8px)`.
  - `background-origin` (what position and size are of: `padding-box`, the default, `border-box`,
    `content-box`) and `background-clip` (where it is drawn: `border-box`, the default, `padding-box`,
    `content-box`; rounded corners cut it too). Gradients take the origin and clip as well (not the size,
    position or repeat: they fill their box). The background colour is clipped as the last layer is.
  A file that is missing or isn't an image it can read is reported where the `url()` is
  (`Mine.css:8:23: 'background': can't use img/tile.png (…): the file is missing or can't be read`), and that
  layer is left out. Remote files (`http:`, `data:`) are not read. Each image is read once and kept at the
  sizes it is drawn at; saving one (in any image editor) puts the skin on again.
- **`box-shadow`**: `none`, or a list of `[inset] x y [blur [spread]] [colour]` (no colour: `currentColor`),
  the first on top. Outer shadows are drawn under the box, inset ones over its background and under its
  border; blur is soft (Gaussian-like), spread grows the corners too.
- **`outline`** (`outline-width`, `outline-style`, `outline-color`; its shorthand as one side of a border) and
  `outline-offset`: a line around a box, outside it (`outline-offset` farther out; negative: in), drawn as a border
  of that style is (`1px dotted` is every other pixel: a classic focus rectangle), in `outline-color` (else
  `color`); square corners. Drawn by `CheckBox::label` and `TreeWidget::text` (so `:focus` can show a focus
  rectangle); other boxes don't draw one.
- **`text-shadow`**: the same without `inset` and spread: labels, buttons, check boxes, group titles, the
  status bar, tabs, menus, the menu bar, lists, trees and combo boxes draw it.
- **`opacity`** (a number or %): a widget and everything in it, drawn see-through as one.
- **`font-smooth: never`** (or `-webkit-font-smoothing: none`): the text is drawn without anti-aliasing, each
  pixel of a letter fully on or off (crisp, like the bitmap fonts of old desktops), each letter on a whole pixel.
  A pixel is on where the letter covers at least half of it, and thin strokes are kept by dropout control, as
  TrueType's own monochrome rasterizers do: wherever a row or column of pixels crosses a stroke thinner than a
  pixel that none of its pixels shows, the pixel nearest the stroke's middle is turned on (the tip of a round
  stroke already drawn in the next row or column is left out, so curves don't grow bumps). So a minus sign, an
  apostrophe, the stem and serifs of an I and the thin curves of an S stay whole at 8 or 9 pt.
  Inherited: on `Window` (or `:root`) it is every text of the program that takes its font from the stylesheets
  (labels, buttons, menus, lists, title bars, the timeline's texts…); `font-smooth: auto` turns it back on below.
  Also `always` (the default) and a length (as `auto`).
- **Properties of Tiny DAW's own** (besides the CSS ones): `stroke-width` (a line's thickness), `icon-size` (a
  drawn arrow or mark), `line-width` (separators), `baseline` (where a text's baseline is in its box; % of the
  box), `fill-opacity` and `stroke-opacity` (curves and waveforms), the glyph images below, and custom
  properties a type reads, listed with it below.
- **Glyph images:** every small drawing a control makes (a check mark, an arrow, a caption button's sign) can be
  a picture instead, set on the part that draws it ([the list](#glyph-images)):
  - `-tiny-glyph: url(file)` (or `none`: the drawn glyph). PNG or SVG, as for images above (`image-rendering`
    too).
  - `-tiny-glyph-size`: one or two lengths (width, height; `auto`: from its proportions). Without it: the
    image's own size (an SVG's `width` and `height`), else as big as fits where the glyph goes.
  - `-tiny-glyph-offset`: one or two lengths (x, y; one: both), how far it is moved from centred: a pressed
    button's glyph 1 px down and right is `-tiny-glyph-offset: 1px` on `:pressed`.
  - `-tiny-glyph-tint: color` draws every painted pixel of it in the part's `color` (its coverage kept: one
    black-on-transparent picture serves every colour of every state); `none` (the default) draws it as it is.
  The image is centred where the drawn glyph would go, on whole pixels (rounding down). Each state is its own
  style, so `:hover`, `:pressed`, `:disabled`, `:checked`… can each have their own picture. Without
  `-tiny-glyph` the glyph is drawn exactly as before.

### Glyph images

The parts that draw a glyph, and so take `-tiny-glyph` (with `-tiny-glyph-size`, `-tiny-glyph-offset`,
`-tiny-glyph-tint`). The image is centred in the area given.

| Part | The glyph | Centred in |
| :--- | :--- | :--- |
| `CheckBox::indicator` (`:checked`) | the check mark | the box's inside (border and padding off) |
| `ListWidget::indicator` (`:checked`) | a row's check mark | the box's inside |
| `TreeWidget::branch` (`:open` when expanded) | the ▸ / ▾ arrow | the indent of its level, the row's height |
| `FileView::branch` (`:open`) | the Explorer tree's ▸ / ▾ | the indent, the row's height |
| `ScrollBar::sub-line`, `ScrollBar::add-line` (`:vertical`, `:horizontal`) | the arrows ▴ ▾ / ◂ ▸ | the button's inside |
| `SpinBox::up-button`, `SpinBox::down-button` | the arrows | the button's half of the column (with a margin: its own box) |
| `ComboBox::arrow` | the ▾ | the arrow box's inside |
| `Button::arrow` | the ▾ of a button with a menu | the arrow's room (split: its 12 px; else beside the text) |
| `Menu::check` | a checked item's mark | `width` across, the item's height |
| `Menu::arrow` | a submenu's ▸ | `width` across, the item's height |
| `LineEdit::clear-button` | the ✕ | the button |
| `TitleBar::minimize`, `::maximize` (`:maximized`: the restore sign), `::close` | the caption signs | the button |
| `MessageIcon` (`.information`, `.question`, `.warning`, `.critical`) | the whole icon (circle and sign) | the icon |
| `FileList::icon` (`:folder`) | the file dialog's folder and file signs | `width` across, the row's height |

```css
/* classic check boxes and scroll arrows from pictures (next to the skin: 90s/…) */
CheckBox::indicator { -tiny-glyph: url(90s/check.png); image-rendering: pixelated }
CheckBox::indicator:disabled { -tiny-glyph: url(90s/check-grey.png) }
ScrollBar::sub-line:vertical { -tiny-glyph: url(90s/up.svg); -tiny-glyph-tint: color }
ScrollBar::sub-line:vertical:pressed { -tiny-glyph-offset: 1px }
TitleBar::maximize:maximized { -tiny-glyph: url(90s/restore.svg) }
Window { font-smooth: never }
```

## What rules match

Every widget is a node, and so is every drawn thing that isn't one (a lane, a clip, the ruler).

- **Type:** `Button`, `Slider`, `Lane`… A node also counts as the types it is made from: a `PushButton` and a
  `ToolButton` are both `Button`s, so `Button` rules apply to both and `PushButton` rules to one.
- **Id:** `#name`, the program's name for a node: `#Transport`, `#lane`, `#time`, `#lufs`.
- **Classes:** the program's marks. A mark with a value gives two classes: a button marked `toggle=mute` has
  `.toggle` and `.toggle-mute`.
- **States:** `:hover`, `:pressed`, `:disabled` and `:focus` for every widget, and each type's own (`:checked`,
  `:open`, `:selected`…), listed below.
- **Parts:** `Type::part`. A part has the states of its own thing: `ListWidget::item:hover` is the row under the
  mouse, not the whole list. `PianoRoll::key:black` and `PianoRoll:black::key` mean the same.

## Variables

The colours, shades, fonts and sizes `default.css` defines, with their defaults (Dark). Every table here is
made from `default.css` by `tools/skins_reference.py`.

<!-- variables: made by tools/skins_reference.py -->

### The skin

| Name | Default | What it is |
|---|---|---|
| `--skin-name` | `"Dark"` | the skin's name in Options → Skin |

### Fonts

| Name | Default | What it is |
|---|---|---|
| `font-family` | `system-ui` | the interface font (system-ui: the system's own) |
| `font-size` | `9pt` | its size (also what em is, and the numbers font's); kept exactly as given (`11.5px` is 11.5 px) |
| `--mono-font` | `"Consolas"` | numbers: times, LUFS (Consolas falls back to a monospaced font) |

### Window

| Name | Default | What it is |
|---|---|---|
| `--window` | `#2b2d31` | Window background |
| `--window-text` | `#dcdde0` | Window text |
| `--base` | `#1e1f22` | Fields and lists |
| `--alternate-base` | `#2b2d31` | Lists, every other row |
| `--text` | `#dcdde0` | Text in fields and lists |
| `--button` | `#36383d` | Buttons |
| `--button-text` | `#dcdde0` | Button text |
| `--highlight` | `#3d7ab8` | Selected items |
| `--highlighted-text` | `#ffffff` | Selected items' text |
| `--tooltip-base` | `#1e1f22` | Tooltips |
| `--tooltip-text` | `#dcdde0` | Tooltip text |
| `--disabled-text` | `#6b6e75` | Disabled text |
| `--hint-text` | `#9a9da5` | Hints |
| `--dimmed-text` | `#70737c` | Dimmed notes (updates) |
| `--accent` | `#3d7ab8` | Lit buttons (FX with plugins, Preview, Metronome…) |
| `--accent-text` | `#ffffff` | Text on lit buttons |
| `--frame-edge` | `#111111` | Thin edges |

### Timeline

| Name | Default | What it is |
|---|---|---|
| `--lane` | `#232529` | Track lane |
| `--lane-alt` | `#282a2f` | Track lane, every other |
| `--below-lanes` | `#1d1e22` | Below the last track |
| `--master-lane` | `#1e2530` | Master lane and row |
| `--master-divider` | `#465a78` | Line above the master |
| `--group-lane` | `#33363e` | Group header (no colour of its own) |
| `--group-accent` | `#a0a0af` | Bar beside a group's tracks |
| `--ruler-bg` | `#18191c` | Ruler |
| `--ruler-text` | `#9696a0` | Ruler text |
| `--grid-fine` | `#2d2f35` | Grid: finer lines |
| `--grid` | `#373a40` | Grid: beats (and seconds) |
| `--grid-bar` | `#484c56` | Grid: bars |
| `--playhead` | `#ff5050` | Playback cursor |
| `--cursor` | `#5fafff` | Selection cursor |
| `--selection-fill` | `#78aae62d` | Selected range |
| `--selection-edge` | `#96c3f5c8` | Selected range's edges |
| `--automation-lane` | `#1c1f24` | Automation lane (and its row) |
| `--automation-line` | `#e0a84a` | Automation curve and its points |
| `--loop` | `#e0a33cb4` | Loop range on the ruler (looping on) |
| `--loop-off` | `#e0a33c46` | Loop range on the ruler (looping off) |
| `--frozen-tint` | `#bee1ff46` | Over a frozen track's clips |
| `--overview-bg` | `#141518` | Overview bar |
| `--overview-view` | `#c8d2e1c8` | Overview: outline of the view |
| `--overview-view-fill` | `#c8d2e11c` | Overview: inside the outline |
| `--overview-edge` | `#373a40` | Overview: line under it |

### Tracks and clips

| Name | Default | What it is |
|---|---|---|
| `--row` | `#232529` | Track rows in the right-hand panel |
| `--row-alt` | `#282a2f` | Track rows in the right-hand panel, every other |
| `--clip-audio` | `#326996` | Audio clips (no colour of their own) |
| `--clip-midi` | `#377d55` | MIDI clips (no colour of their own) |
| `--accent-audio` | `#5096d2` | Audio track stripe |
| `--accent-midi` | `#5abe78` | MIDI track stripe |
| `--accent-return` | `#d0913c` | Return track stripe |
| `--mute` | `#c0392b` | Mute (M) when on |
| `--solo` | `#d4a017` | Solo (S) when on |
| `--record` | `#d0433a` | Recording: ● Rec when on, the take as it grows |

### Meters

| Name | Default | What it is |
|---|---|---|
| `--meter-bg` | `#121316` | Meter background |
| `--meter-ok` | `#46c86e` | Meter: up to -12 dB (and CPU load) |
| `--meter-warn` | `#dcbe3c` | Meter: up to 0 dB |
| `--meter-hot` | `#dc3c32` | Meter: at 0 dB |

### MIDI editor

| Name | Default | What it is |
|---|---|---|
| `--keys-white` | `#e8e8ea` | White keys |
| `--keys-black` | `#1b1c1f` | Black keys |
| `--keys-line` | `#b8b9be` | Lines between keys |
| `--keys-label` | `#3a3c42` | Key names (C4…) |
| `--key-sounding` | `#5fafff96` | The key sounding |
| `--roll-shade` | `#0000006e` | Outside the clip |
| `--selected-note` | `#ffffff` | Selected notes |

### Explorer

| Name | Default | What it is |
|---|---|---|
| `--sample-wave` | `#78afe6` | Sample waveform |

### Shades

| Name | Default | What it is |
|---|---|---|
| `--hover` | `color-mix(in srgb, var(--button) 90%, var(--button-text) 10%)` | a control under the mouse |
| `--pressed` | `color-mix(in srgb, var(--button) 82%, var(--button-text) 18%)` | pressed, or switched on |
| `--disabled` | `color-mix(in srgb, var(--button) 50%, var(--window) 50%)` | a disabled button's fill |
| `--line` | `color-mix(in srgb, var(--window) 90%, var(--window-text) 10%)` | dividing lines, edges of menus |
| `--field-edge` | `color-mix(in srgb, var(--base) 86%, var(--text) 14%)` | the edge of fields |
| `--field-hover` | `color-mix(in srgb, var(--base) 74%, var(--text) 26%)` | … under the mouse |
| `--item-hover` | `color-mix(in srgb, var(--base) 93%, var(--text) 7%)` | list rows under the mouse |
| `--panel` | `color-mix(in srgb, var(--window) 55%, var(--base) 45%)` | boxes grouping controls |
| `--handle` | `color-mix(in srgb, var(--window) 78%, var(--window-text) 22%)` | scroll bar handles |
| `--handle-hover` | `color-mix(in srgb, var(--window) 62%, var(--window-text) 38%)` | … under the mouse |
| `--accent-hover` | `color-mix(in srgb, var(--accent) 85%, var(--accent-text) 15%)` | lit buttons under the mouse |

### Basic controls sizes

| Name | Default | What it is |
|---|---|---|
| `--layout-spacing` | `4px` | between the controls of a row or column made without a gap of its own |
| `--grid-spacing` | `6px` | between the cells of a grid made without one (the Pad Kit's pads…) |
| `--label-wrap-width` | `360px` | a wrapping label's width when nothing gives it one |
| `--label-min-width` | `16px` | the least width of a label that elides its text |
| `--message-box-margin` | `14px` | around its parts |
| `--message-box-spacing` | `14px` | between the sign, the text and the buttons |
| `--message-box-text-spacing` | `8px` | between the text and its details |
| `--input-dialog-margin` | `12px` | around a question for a name or a number |
| `--input-dialog-spacing` | `8px` | between its parts |
| `--input-dialog-button-gap` | `4px` | more above the buttons |
| `--input-dialog-field-width` | `280px` | the field's least width |
| `--progress-dialog-margin` | `12px` | around a progress window's parts |
| `--progress-dialog-spacing` | `10px` | between them |
| `--progress-dialog-min-width` | `320px` | its least width |

### Sliders, lists, scrolling and menus sizes

| Name | Default | What it is |
|---|---|---|
| `--scrollbar-width` | `10px` | the thickness of scroll bars (and the room lists and scroll areas keep for them) |

### Timeline sizes

| Name | Default | What it is |
|---|---|---|
| `--ruler-height` | `24px` | the timeline's ruler (and the CPU monitor beside it) |
| `--tempo-lane-height` | `56px` | the tempo lane under the ruler (View → Tempo lane; drawn as an automation lane) |
| `--master-height` | `86px` | the master lane and its row |
| `--group-height` | `52px` | a group's header lane and row |
| `--take-height` | `46px` | a take lane and its row |
| `--automation-padding` | `6px` | above and below the curve in an automation lane |
| `--panel-width` | `276px` | the track rows' panel |
| `--overview-height` | `44px` | the overview's standard height (the least it can be dragged to) |
| `--overview-max-height` | `320px` | … the most |
| `--clip-header-height` | `16px` | a clip's name strip (drag it to move the clip) |
| `--clip-edge-width` | `6px` | a clip's ends (drag them to trim it) |
| `--fade-handle-size` | `7px` | an audio clip's fade handles |
| `--link-badge-width` | `14px` | a linked clip's chain mark |
| `--link-badge-height` | `8px` | (its height) |
| `--cursor-arrow-width` | `11px` | a cursor's arrow on the ruler |
| `--cursor-arrow-height` | `7px` | (its height) |

### Panels, meters and editors sizes

| Name | Default | What it is |
|---|---|---|
| `--meter-gap` | `1px` | between a meter's bars |
| `--scope-height` | `78px` | the master scope (with the gap above it) |
| `--explorer-width` | `250px` | the Explorer's width at the start (it can be dragged) |
| `--dock-title-height` | `22px` | the Explorer's title bar |
| `--dock-close-size` | `18px` | its ✕ button |
| `--transport-button-size` | `28px` | the playback buttons (square) |
| `--transport-icon-size` | `14px` | their icons |
| `--transport-time-width` | `200px` | the time display's room (less when the bar is short of room) |
| `--transport-separator-width` | `7px` | an upright line between groups of controls, with the room around it |
| `--lufs-width` | `62px` | the master's LUFS readout |
| `--row-button-height` | `22px` | the buttons on a track's row (M, S, FX, the instrument, icons) |
| `--toggle-button-width` | `26px` | M, S, ● and the like |
| `--fx-button-width` | `28px` | FX |
| `--instrument-button-width` | `40px` | a MIDI track's instrument (with its ▾) |
| `--icon-button-width` | `22px` | a row's small icon buttons |
| `--sends-button-width` | `20px` | → (the sends) |
| `--sends-button-height` | `16px` | (its height) |
| `--output-selector-width` | `58px` | a row's outputs (1/2 ▾) |
| `--output-selector-height` | `18px` | (its height) |
| `--auto-button-width` | `58px` | a track's Auto (its automation lanes) |
| `--auto-button-short-width` | `36px` | … beside the sends button (→) |
| `--auto-button-height` | `16px` | (its height) |
| `--write-button-width` | `40px` | an automation lane's Write |
| `--group-meter-indent` | `26px` | a group row's meter, from the row's left (past the fold button) |
| `--volume-slider-height` | `14px` | a row's volume |
| `--db-reading-width` | `44px` | its dB beside it |
| `--pan-knob-size` | `22px` | a row's pan knob |
| `--send-slider-width` | `150px` | a send's slider (its least width) |
| `--pre-button-width` | `34px` | a send's Pre |
| `--curve-view-height` | `130px` | an effect window's curve (its least height) |
| `--tuner-height` | `120px` | the Tuner's display (its least height) |
| `--piano-keys-width` | `56px` | the MIDI editor's keys (and the velocity lane's label column) |
| `--velocity-lane-height` | `90px` | the lane under the notes |
| `--editor-ruler-height` | `22px` | the MIDI editor's ruler |
| `--step-row-height` | `22px` | a row of the Steps view |
| `--piano-roll-min-height` | `200px` | the notes' area (its least height) |
| `--explorer-button-size` | `24px` | the Explorer's +, −, ⟳ and ■ buttons |
| `--explorer-row-height` | `20px` | a row of the Explorer's list |
| `--explorer-indent` | `16px` | a level of its folder tree |
| `--explorer-header-height` | `22px` | its search results' header |
| `--sample-waveform-height` | `76px` | the sample's waveform (the Explorer, the sampler, the Pad Kit) |
| `--pad-width` | `116px` | a Pad Kit pad's least width |
| `--color-wheel-size` | `200px` | the colour dialog's wheel |
| `--color-swatch-size` | `22px` | its recent colours |

### Window frames sizes

| Name | Default | What it is |
|---|---|---|
| `--title-bar-height` | `30px` | the title bar of Tiny DAW's own window frames |
| `--title-buttons` | `right` | where its buttons are: `right` (the icon then on the left) or `left` (the icon on the right) |
| `--window-resize-border` | `4px` | how far in from a window's edge dragging resizes it (at least its border); reaches over what is inside |

<!-- end of variables -->

## Types

Each entry gives the type's own properties (what it reads besides its box), its parts and its states.
"A box" means `background-color`, the border and the corner radius (see [Boxes](#the-css-understood)).

### Buttons

- **`Button`**, with **`PushButton`** and **`ToolButton`**: a box; `color`, the font; `padding` (its room around
  the text and icon, which gives its size). States `:hover` (not while held off it), `:pressed`, `:checked`,
  `:default`, `:lit` (accent-coloured: lit, accent and checked, a dialog's default button…), `:toggled` (a
  switched-on toggle with a role: its `--toggle-color`), `:icon`, `:arrow-hover`, `:disabled`, `:focus`. Classes
  `.lit`, `.accent`, `.autoRaise`, `.toggle` with `.toggle-mute`, `.toggle-solo`, `.toggle-record`,
  `.toggle-accent` (each sets `--toggle-color`). Parts `::arrow` (the drop-down part of a split button: a box)
  and `::separator` (the line before it: `background-color`).
- **`IconButton`** (a `ToolButton`): `::icon` `color`, with `:checked` and `:disabled`.

### Text and boxes

- **`Label`**: `color`, the font, `padding`, `text-decoration: underline`, a box. Classes `.hint`, `.dimmed`
  (8 pt); ids `#time` (12 pt) and `#lufs` (8 pt) in `var(--mono-font)`. State `:link`. Part `::image` (`opacity`,
  0.45 when disabled).
- **`CheckBox`**: `color`, the font, `padding`, a box. States `:checked`, `:hover`, `:disabled`, `:focus`. Part
  `::indicator`: `width`, `height`, `margin-right` (the gap to the text), `padding`, a box; the check mark is its
  `color`. Part `::label`: a box shrink-wrapped around the text (its `padding` and border add to the check box's
  size; centred in its height), `color`, `text-shadow`, and an `outline`: `CheckBox::label:focus { outline: 1px
  dotted; padding: 0 1px }` is the classic dotted focus rectangle around the text.
- **`GroupBox`**: a box (the frame) starting `margin-top` down, its contents inside `padding`. State
  `:disabled`. Part `::title`: `color`, the font, `height` (its row), `margin`; with a background, a border or
  `padding`, also a box. The title is placed against the frame's top-left corner: its row starts `margin-top`
  below the frame's top (negative: above it; default.css has `margin: -19px 4px 1px`) and `margin-left` in.
  A title with a background, border or padding is drawn as a box shrink-wrapped around its text, over the
  frame, so it can sit across the frame's top line; if it reaches below the frame's top border, the contents
  start that much lower (like a fieldset's legend). Example: `GroupBox { margin-top: 7px } GroupBox::title {
  margin-top: -7px; margin-left: 8px; padding: 0 3px; background-color: var(--window) }`.
- **`TabWidget`**: a box. Parts `::tab` (a box; `padding`, `color`, the font) and `::tab-line` (`height`;
  `background-color`), both with `:selected`, `:hover`, `:disabled`; `::base` (the line under the tabs:
  `height`, `background-color`); `::pane` (a box under the tabs, around the pages: the current page goes
  inside its border and `padding`; `::base` is drawn over its top).
  Tabs take `margin`: they are placed in a row from the left, each after the one before and its margin; their
  bottoms are `margin-bottom` above the bar's bottom, and the bar is as high as the highest tab with its
  `margin-top` and `margin-bottom` (as if no tab were under the pointer, so hovering never moves the page). Each tab
  is its own `padding` and border around its title, so tabs can differ in size: the selected one larger, with a
  negative margin over its neighbours and over the pane (it is drawn last, and takes clicks there). States
  `:first` and `:last` are the row's ends. A classic row: `TabWidget::tab { margin: 2px 0 1px } TabWidget::tab:first
  { margin-left: 2px } TabWidget::tab:selected { padding: 3px 8px 5px; margin: 0 -2px -1px }`: the selected tab
  2 px wider each side and 2 px taller, its bottom over the line under the tabs (joined to its page).
- **`Separator`**: `color`, `line-width`, `margin`. States `:vertical`, `:horizontal`. With a border it is a box
  across its thickness instead of a line (`width` thick when vertical, `height` when horizontal, else as thick
  as its borders): `Separator:vertical { border-left: 1px solid #808080; border-right: 1px solid #fff; width:
  2px }` is an etched divider.
- **`SplitterHandle`**: `color`, `line-width`; `width` (`:horizontal`) or `height` (`:vertical`). States
  `:hover`, `:pressed`.
- **`StatusBar`**: `color`, the font, `padding`, a box. Parts `::separator` (the line above it: `height`,
  `background-color`) and `::permanent` (the text on its right: `padding`, `color`, the font).
- **`ProgressBar`**: `width`, `height`, a box, `color`. Part `::chunk`: a box, `color`, `min-width` (the moving
  block when the progress isn't known).

### Windows and dialogs

- **`Window`**: `background-color` (and its border), under its widgets (with its own frame, `Window:framed`:
  under the title bar too, its border around both: [Window frames](#window-frames)). **`ToolTip`** (a `Window`): a box,
  `padding`, `color`, the font. **`DragImage`**: a box; `::image` `opacity`.
- **`Dialog`** (classes `.message-box`, `.input-dialog`), **`ProgressDialog`**: their sizes are `:root`
  variables (below). **`ButtonBox`**: `column-gap`; its buttons' `min-width` (`ButtonBox > Button`).
- **`Dialog.message-box`**: a message box. Part `::buttons`: the band its buttons are in, across the whole width
  below the sign and the text (a box: `background-color`, borders…); its buttons inside its border and `padding`.
  A side its `padding` doesn't set is the message box's own size (`--message-box-spacing` above,
  `--message-box-margin` at the sides and below), so unstyled it changes nothing. `Dialog.message-box {
  background-color: #fff } Dialog.message-box::buttons { background-color: #f0f0f0; border-top: 1px solid #dfdfdf;
  padding: 11px 10px }` is a white message with a grey strip of buttons. A message box can't be resized (no
  maximize button).
- **`MessageIcon`**: `background-color` (the circle), `color` (the sign). Classes `.information`, `.question`,
  `.warning`, `.critical`.
- **`Canvas`**: a plain drawing area (the type only).
- **`MessageIcon`** also reads `width` and `height` (its drawing is scaled to them).

### Window frames

With Options → Window frames → Tiny DAW's own (the default), every window (the main window, dialogs, effect and
plugin windows; not menus, drop-down lists or tooltips) has a frame drawn by the program, in the skin's look,
instead of the window manager's. With The system's, the window manager draws them as for other programs; every
window changes at once.

- **`Window:framed`**: a window with its own frame. Its border (`border`, each side's width and colour) is the
  frame's edge, drawn around the title bar and the inside; its `background-color` (and gradients) under both.
  States **`:active`** (the keyboard focus is in it) or **`:inactive`**, and **`:maximized`** (default.css: a 1 px
  edge of `--frame-edge`, lighter while active, none while maximized). A window without a frame has none of these
  states, and its border is drawn inside it, as before.
  Part **`::frame`** (`Window:framed::frame`, with the window's states): a ring inside that border, around the title
  bar and the inside: its border and `padding` count in the frame's size (the inside keeps its size; the resize band
  reaches over it), its box (`background-color`, borders, gradients) drawn around the inside (never over it: the
  inside's background stays the Window's), under the title bar. `Window`'s own `padding` (a dialog's margins) is
  untouched. A classic frame, two tones a side and a face-coloured gap: `Window:framed { border: 2px outset
  #c0c0c0 } Window:framed::frame { padding: 2px } Window:framed:maximized::frame { padding: 0 }`. A focus change
  repaints the frame (and the window's background) whenever `:active` changes its look, and applies new sizes.
  Its corners' radii (`border-top-left-radius`…) also cut the window itself round, so the desktop shows past a
  rounded corner (not while maximized, or with Options → Rounded corners off).
- **`TitleBar`**: the bar across the top, above the window's inside: a box (`background-color`, gradients,
  borders, `box-shadow`, `padding`), `--title-bar-height` high. States `:active` / `:inactive` and `:maximized`
  (the window's), `:hover` and `:pressed` (the bar's). Its parts, each with the bar's states:
  - **`::icon`**: the window's icon, if it has one: `width`, `height`, `margin`.
  - **`::title`**: the window's title: `color`, the font, `text-align` (`left`, `center`: across the whole bar
    while that clears the buttons, or `right`), `margin`; a box.
  - **`::minimize`**, **`::maximize`** (drawn as restore while the window is maximized) and **`::close`**: each a
    box (`width`, `height`: at most the bar's, `margin`) with its glyph drawn over it in its `color`, `icon-size`
    big and `stroke-width` thick; each its own `:hover` and `:pressed`. A modal dialog has no minimize, a window
    that can't be resized (a message box…) no maximize. Vertically a button (and the icon) is centred in what its
    `margin-top` and `margin-bottom` leave of the bar (`margin: 3px 0 1px` puts it 1 px below the middle); its
    `margin-left` and `margin-right` are the room beside it. States **`:first`** and **`:last`**: the first and the
    last of the buttons it has, as shown left to right (the rounded ends of a joined group; one button alone is
    both).
- **Where things are:** `--title-buttons: right` puts the buttons on the right (close outermost, then maximize,
  then minimize) and the icon on the left; `left` mirrors that. The title fills what is between.
- **What it does:** dragging the bar moves the window (the window manager moves it, so it snaps and tiles as
  usual); a double click maximizes or restores it; a right click shows the window's menu (Minimize, Maximize or
  Restore, Close). Close is the window manager's ✕: unsaved changes are still asked about. Dragging within
  `--window-resize-border` of the edge (or the border, if wider) resizes the window; corners resize both ways.

### Dialogs

Every dialog's room is its style: `padding` (its margins), `row-gap` and `column-gap` (between its parts), and
the same on the parts named here (a part's own row or column of controls). What a dialog's layout doesn't get
from its style is `--layout-spacing` (rows and columns) and `--grid-spacing` (grids). Its buttons are
`Button.dialog-button` (`min-width`). A heading in a dialog is `Label.heading` (bold).

- **`ColorPickerDialog`** (choosing a colour): parts `::row`, `::recent`, `::brightness`, `::hex`. **`ColorWheel`**,
  **`ClipPreview`**, **`TrackPreview`** and **`Swatch`**: see below.
- **`MetronomeDialog`**: parts `::editors`, `::buttons`. **`ClickEditor`** (a `GroupBox`: one click's sound): parts
  `::column`, `::form`. **`EnvelopeGraph`**: below, and `min-width` / `min-height`.
- **`PadKitDialog`**: parts `::columns`, `::pads`, `::settings`. **`SliceDialog`** (Slice to a Pad Kit): `::count`.
  **`SamplerDialog`**.
- **`PluginsDialog`** (a track's effects or instrument): parts `::header`, `::buttons`. **`MidiEffectsDialog`**:
  parts `::settings`, `::value`. **`PluginChooser`**, **`PluginFoldersDialog`**, **`WinePrefixesDialog`**,
  **`WarpDialog`** (Warp…: warping, stretch and pitch): part `::row`. **`ParameterDialog`**, **`LinksDialog`** (MIDI learn's links).
- **`FileDialog`** (opening and saving files): `width`, `height`, `min-width`, `min-height` (its size when it
  opens, and its least); parts `::path-row`, `::nav-button` (Up and Home: `width`, `height`), `::middle`,
  `::places` (the list of places: `width`), `::form`. Its list is a **`FileList`**.
- **`AudioPropertiesDialog`**, **`RecordOffsetDialog`**, **`RenderDialog`** (render the mix),
  **`RecordSettingsDialog`**, **`TempoDialog`**, **`MidiPortsDialog`** (Options → MIDI ports…), **`ExternalSynthDialog`**.
- `Dialog.group-tracks` (grouping tracks), `Dialog.recovery` (after a crash: parts `::text`, `::buttons` with
  `margin-top`).

### Fields

- **`LineEdit`**: a box; `padding` (around the text: its size). States `:hover`, `:focus`, `:disabled`,
  `:pressed`, `:frameless` (inside a spin box or an editable combo box), `:read-only`, `:clear-hover`. Parts
  `::placeholder` (`color`), `::selection` (a box, the selected text's `color`, `padding` beyond the text),
  `::cursor` (`background-color`, `width`), `::clear-button` (`color`, `width`, `margin-left`, `margin-right`,
  `icon-size`, `stroke-width`).
- **`SpinBox`**: a box; `padding` (its size around the widest number). Its field is a `LineEdit:frameless`. States
  `:focus` (its field has the focus), `:hover`, `:disabled`, `:pressed`, `:up-hover`, `:down-hover`,
  `:up-pressed`, `:down-pressed`, `:at-maximum`, `:at-minimum`. Parts `::up-button` and `::down-button`: a box
  over the inside of its border, the arrow's `color`, `icon-size`, `stroke-width`; `width` (the buttons' column,
  from `::up-button`). With a `margin` on either (even `margin: 0`), each button is a box of its own
  instead: its half of the column less its margin (the up button's `margin-bottom` and the down button's
  `margin-top` make a gap between them), so each can have its own border (classic bevelled spin buttons with a line
  between them); its arrow centred in it.
- **`ComboBox`**: a box; `padding`. States `:hover`, `:focus`, `:disabled`, `:pressed`, `:open`, `:editable`. Part
  `::arrow`: `width`, `margin` (the room around it: `margin: 2px 2px 2px 0` puts it inside a 2 px
  border, its ▾ centred in its height; the text and the field stop at its margin), `padding-left`, a box, `color`, `icon-size`, `stroke-width`.
- **`ComboPopup`** (a combo box's list): a box, `padding`. Parts `::item` (a box, `padding`, `color`, the font;
  `:hover` and `:selected` for the highlighted row, `:disabled`) and `::separator` (`height`, `margin`, a box,
  `stroke-width`).

### Sliders, scrolling, lists and menus

- **`Slider`**: `width`, `height`, `min-width` / `min-height`. States `:horizontal`, `:vertical`. Parts
  `::groove` (`background-color`, `border-radius`, its thickness), `::fill` (the groove below the value;
  `:disabled`), `::handle` (`width`, `height`, a box; `:hover`, `:pressed`, `:disabled`; a radius of half its
  size makes it round).
- **`ScrollBar`**: `width`, `height` (its thickness: `--scrollbar-width`). States `:horizontal`, `:vertical`. Part
  `::thumb`: a box, `margin`, `min-height` / `min-width`; `:hover`, `:pressed`. Parts `::sub-line` (the arrow
  button at the top or left end) and `::add-line` (bottom or right): their length along the bar is `height` on
  a vertical bar and `width` on a horizontal one (default 0: no buttons); each a box with an arrow (`color`,
  `icon-size`, `stroke-width`), `:hover`, `:pressed`. A press steps the bar; held, it repeats. The thumb moves
  between them.
- **`ScrollArea`**: a box; its viewport and scroll bars sit inside its border and padding. The timeline's is
  `ScrollArea#timeline`: its box surrounds the ruler, the lanes, the pinned master lane and the scroll bars,
  so `border: 2px inset …` makes a sunken well (the track panel keeps its rows lined up with the lanes).
- **`ListWidget`**: a box; `width`, `height`, `min-width`, `min-height`. Parts `::item` (`height`, `padding`,
  `color`; `:selected`, `:hover`, `:alternate`, `:disabled`, `:checked`, `:checkable`, `:bold`), `::indicator`
  (a row's check box: `width`, `height`, `margin`, `padding`, a box, `color`; `:checked`), `::icon` (`width`,
  `height`, `margin`).
- **`TreeWidget`**: as `ListWidget`. `::item` states `:selected`, `:hover`, `:disabled`, `:open`,
  `:has-children`, `:bold`; `::branch` (`width`: each level's indent; `padding`, `color`); `::icon`.
  Lines joining the rows: `-tiny-tree-lines: dotted` (or `solid`; `none`, the default) on `TreeWidget::branch`, in
  `-tiny-tree-line-color` (else the branch's `color`): from each row's branch across to its text, up to the row
  above, down to its next sibling, and its ancestors' lines passing by on the left (to their next siblings); 1 px,
  through the middle pixel (rounding down) of each level's room for the arrow (`width` less `padding`) and of the
  row's height; dotted ones are every other pixel, the dots fixed to the rows as they scroll. Part `::text`: a box
  shrink-wrapped around the text (its `padding` and border; the row's height), with the row's states: a selection
  covering the text only is `TreeWidget::item:selected { background-color: transparent }
  TreeWidget::text:selected { background-color: #000080; color: #fff; padding: 0 2px }`; its `outline` is a focus
  rectangle (`TreeWidget:focus::text:selected { outline: 1px dotted }`).
- **`MenuBar`**: a box. Part `::item`: `padding`, `color`; `:hover`, `:open`, `:disabled`.
- **`Menu`**: a box, `padding`, `min-width`. Parts `::item` (`padding`, `color`, the font; `:hover`, `:disabled`,
  `:checkable`, `:checked`, `:section`, `:submenu`), `::separator` (`height`, `margin`), `::check` (`width`,
  `margin-left`), `::shortcut` (`margin-left`), `::arrow` (`width`, `margin-left`, `margin-right`).
- **`FileList`** (the file window's list): a box; `width`, `height`, `min-width`, `min-height`, `--details-from`
  (the size and date columns show when rows are wider). Its header, rows and scroll bar are inside its border
  and padding. Parts `::header` (a box, `padding`), `::item` (`height`;
  `:selected`, `:hover`, `:folder`), `::icon` (`width`, `color`), `::detail` (`padding`, `--spare`, `color`),
  `::placeholder` (`height`).

### The timeline

- **`Timeline`**: parts `::grid-fine`, `::grid`, `::grid-bar`, `::cursor`, `::playhead` (`color`,
  `stroke-width`), `::selection` and `::recording` (boxes), `::selection-edge` (`color`, `stroke-width`),
  `::slice` (`color`, `stroke-width`, `width`, `height`).
- **`Ruler`** (in the timeline; `:looping`): a box, `color`. Parts `::label` (`color`, the font, `padding-left`,
  `baseline`), `::tick`, `::tick-fine`, `::tick-beat`, `::tick-bar` (`height` as % of the ruler, `color`,
  `stroke-width`), `::loop` (a box, `height` %), `::loop-edge` (`width`, `background-color`), `::marker` (a box,
  `color`, the font, `padding-left`, `padding-right`, `height` %), `::marker-line` (a box, `width`), `::tempo`
  (`color`, the font, `margin-left`, `baseline`).
- **`Lane`** (in the timeline and the overview): a box; `padding-top` / `padding-bottom`. Classes `.track`,
  `.group`, `.automation`, `.take`, `.below`; states `:alternate`, `:armed`, `:locked`. Parts `::overlay` (a
  box over it); for automation lanes `color` (the curve), `::curve` and `::level` (`stroke-width`), `::point`
  (`width`, `height`), `::range` (a box), `::range-edge` (`color`, `stroke-width`), `::guide` (`color`),
  `::label` (`color`, the font, `padding`, `width`, `height`), `::value` (a box, `color`, the font, `padding`,
  `margin-left`), `::hint` (`color`, the font, `padding-left`, `baseline`).
- **`Clip`** (in the timeline): `stroke-width` (its outline), `padding`. Classes `.audio`, `.midi`; states
  `:selected`, `:disabled`, `:frozen`, `:muted`, `:linked`, `:looping`. Parts `::body` and `::header` (boxes in the
  clip's own colours), `::overlay` (a box), `::name` (`padding`, the font), `::fade` (`background-color`,
  `stroke-width`), `::loop-start` (`stroke-width`, `height`), `::link` (`stroke-width`).
- **`OverviewCaption`** (a `Label`: "Project overview"). **`MasterLane`**: a box. **`Overview`**: a box, `padding-top` / `padding-bottom`; parts `::view` (a box and
  `outline-*`), `::cursor`, `::playhead`; its rows are `Lane`s (`Overview Lane.group { … }`).

### The track panel and the master

- **`TrackPanel`**: the right-hand panel. **`TrackRow`** (a row in it): `background-color`, `border-left-color`
  (the stripe), `border-top-color`, `padding`, `row-gap`, `column-gap`. Classes `.track`, `.automation`, `.take`,
  `.group`, `.audio`, `.midi`, `.return`; states `:alternate`, `:armed`. Parts `::tint`, `::group-bar` (`width`,
  `background-color`), `::controls`, `::volume`, `::auto` (the room of those rows of controls). Its controls'
  sizes are `:root` variables (the Auto and Write buttons, the outputs). A row is drawn as a box in its own
  colour (a track's or group's colour wins over `background-color`): its border and border styles, radii,
  gradients over the colour, inset and outer shadows all apply, so `TrackRow { border: 2px outset #c0c0c0 }`
  gives raised rows; its controls go inside the border, and `border-left-color` is still the stripe.
- **`LaneFrame`** (`#lane`, a row's frame): parts `::stripe` (`width`), `::edge` (`background-color`, `width`),
  `::current` (the current row's outline: `border-color`, `border-width`), `::load` (a box, `height`,
  `margin-bottom`), `::load-bar` (`:ok`, `:warn`, `:hot`). Class `.master` (`background-color`, `padding`,
  `row-gap`, `column-gap`; `::title` font, `::notice` `margin-top`): the master's row, drawn as a full box the
  same way. `::load` is a box with its bar inside its border and padding; `::load-bar` may be a box.
- **`NameLabel`** (`.group`: the font), **`PanKnob`** (`background-color`, `padding`; `::arc`, `::pointer`:
  `color`, `stroke-width`), **`SendsPanel`** (a box, `padding`, `row-gap`, `column-gap`, `min-width`, `min-height`; `::title` font). Class
  `.pin-cover` (a box).

### Meters, the transport and the top bar

- **`LevelMeter`**: a box around the whole meter, the bars and number inside its border and padding. Parts
  `::track` (a box; its bar and peak are drawn inside its border and padding), `::bar` and `::peak` (`:ok`,
  `:warn`, `:hot`: a box, so a gradient makes glossy bars; `::peak` `width`), `::number` (`:clipped`: a box, `color`, `font-family`, `font-weight`).
- **`CpuMonitor`**: a box (its left border is the line by the ruler), `color`, the font, `padding`. Parts
  `::late` (`color`, `min-width`), `::track` (a box, `height`; the bar inside its border and padding), `::bar`
  (`:ok`, `:warn`, `:hot`: a box).
- **`Scope`**: a box, `padding`, `margin-top`. Parts `::divider` (`color`, `width`, `margin-left`,
  `margin-right`), `::trace` (`color`, `stroke-width`, `fill-opacity`), `::grid` (`color`), `::caption` (`color`,
  the font, `padding`, `height`).
- **`Transport`** (`#Transport`): a box (its bottom border is the line under the top bar), `padding`,
  `column-gap`. **`LoudnessDisplay`** (a `Label`: `#lufs`). **`UpdateNotice`**: `color`, the font; `::link`
  `color`.

### The MIDI editor and Steps

- **`MidiEditor`**: parts `::toolbar` (`padding`, `column-gap`), `::scale` (`margin-left`), `::hint` (`padding`).
- **`PianoRoll`**: parts `::row` (`:white`, `:black`, `:outside`, `:tonic`), `::octave`, `::below`, `::shade`,
  `::grid` (`width`; `:fine`, `:beat`, `:bar`: `color`), `::note` (`border-width`; `:selected`: `border-color`,
  `border-width`), `::slide`, `::slide-head` (`stroke-width`), `::loop-end`, `::selection-box`, `::cursor`,
  `::playhead` (`color`, `width`), `::keys` (a box), `::key` (`:black`: a box, `width` as % of the keys;
  `:folded`, `:named`, `:sounding`), `::key-line` (`background-color`, `height`), `::key-label` (`color`, the font,
  `padding-right`), `::keys-edge`.
- **`Steps`**: a box. Parts `::row`, `::pad` (`background-color`, `border-radius`, `margin`; `:odd` with
  `--darken`), `::shade`, `::note` (`:selected`), `::slide`, `::slide-head`, `::cursor`, `::playhead`, `::keys`,
  `::key` (`:black`, `:sounding`), `::key-line`, `::key-label`, `::keys-edge`, `::add-row`.
- **`VelocityLane`**: a box. Parts `::grid` (`:fine`, `:beat`, `:middle`), `::bar` (`width`; `:selected`),
  `::cap` (`width`, `height`), `::label` (a box, `color`, the font, `padding`, `height`), `::chooser`
  (`margin`), `::cursor`, `::playhead`.
- **`EditorRuler`**: a box, `color`, the font. Parts `::tick` (`height`; `:bar`, `:second`), `::label`
  (`padding-left`, `top`), `::corner`, `::cursor`, `::playhead`.

### The patch panel

- **`PatchPanel`**: a box; `--cable-1`, `--cable-2`, `--cable-3`, `--cable-4`, `--cable-5`, `--cable-6` (the cables' colours, in turn). Parts `::rail`,
  `::rail-edge`, `::screw`, `::sources` (a box), `::sources-title`, `::source` (`:blocked`: `opacity`), `::jack`,
  `::jack-ring` (`:hover`), `::end-title`, `::end-label`, `::empty-hint`, `::unit` (a box; `:hover`; `:selected`:
  `outline-color`, `outline-width`), `::picture` (`:bypassed`), `::faceplate`, `::faceplate-text`, `::wave`,
  `::reload-shade`, `::reload` (`width`, `height`; `:hover`), `::name` (`:bypassed`), `::sidechain-label`, `::led` (`:on`, `:idle`,
  `:bypassed`, `:problem`), `::status` (`:hover`), `::groove` (`height`), `::level` (`:bypassed`), `::zero-mark`, `::knob`
  (`width`, `height`; `:hover`), `::value` (`:hover`), `::mod-link`, `::mod-label`, `::cable` (`stroke-width`, `outline-width`).

### The Explorer, waveforms and the Pad Kit

- **`Explorer`**: `padding`, `row-gap`; parts `::folder-row` and `::preview-row` (`column-gap`), `::folder` (the
  folder list: `min-width`). **`ExplorerDock`** (the Explorer with its title): `::title-bar` (a box: a caption
  bar's gradient, a border; `padding`, `column-gap`; its `color` and font go to the title "Explorer").
- **`FileView`** (the Explorer's list): a box, `color`. Parts `::row` (`:hover`, `:selected`), `::icon` (`color`,
  `fill-opacity`; `:selected`), `::header` (a box).
- **`SampleWaveform`**: a box, `color`, the font. Parts `::caption` (`color`, the font, `height`, `padding`,
  `column-gap`), `::wave`, `::midline`, `::position`. **`SamplerWaveform`** (a `SampleWaveform`): `::outside`,
  `::marker`.
- **`SynthDialog`** (the built-in synth's window): `padding`, `row-gap`. **`SynthPage`** (its knobs): `color`, the
  font. Parts `::section` (a box), `::title` (`color`, the font), `::knob` (`background-color`), `::pointer` and
  `::arc` (`color`, `stroke-width`), `::choice` (a box, `color`), `::current` and `::target` (`color`), `::handle`
  (`background-color`, `color`), `::env1`, `::env2`, `::lfo` (the modulation rings: `color`, `stroke-width`).
- **`PadView`**: states `:selected`, `:dropping`; `--choke-1`, `--choke-2`, `--choke-3`, `--choke-4`, `--choke-5`, `--choke-6`, `--choke-7`, `--choke-8` (the choke groups' colours). Parts
  `::choke-tint`, `::empty`, `::name` (`:selected`), `::stripe`, `::frame`.

### Effect windows and other windows

- **`EffectWindow`**: `background-color`, `padding`, `row-gap`, `column-gap`. Parts `::slider` and `::value`
  (`min-width`), `::changed` (`color`).
- **`CurveView`**: a box. Parts `::grid` (`color`, `stroke-width`, the font), `::analyzer` (`color`,
  `fill-opacity`, `stroke-opacity`, `stroke-width`), `::curve` (`color`, `stroke-width`), `::grid-label` (the
  grid's labels: `color`, the font; by default the grid's).
- **`MeterView`**: `color`, the font, `line-height` (a row's height). Parts `::label` (`width`, `padding-right`),
  `::track` (a box, `height`; the bar inside its border and padding), `::bar` (a box), `::value` (`width`,
  `padding-left`).
- **`Tuner`** (a `MeterView`): a box. Parts `::note` (`:in-tune`, `:silent`), `::scale` (a box,
  `height`, `margin-left`, `margin-right`, `top`), `::tick` (`width`, `height`; `:centre`), `::needle` (`background-color`,
  `border-radius`, `width`, `height`, `top`; `:in-tune`), `::cents`, `::hz`, `::end-label`.
- **`EditorWindow`** (a plugin's own window): parts `::plugin` and `::stopped` (`background-color`, `padding`,
  `row-gap`; `::stopped` also `min-width`, `min-height`).
- **`HistoryWindow`**: `padding`, `row-gap`, `min-width`, `min-height`; `::undone` (`color`).
- **`EnvelopeGraph`** (the metronome's; classes `.bar`, `.beat`): a box, `color`. Parts `::grid`, `::envelope`
  (`stroke-width`, `fill-opacity`), `::label`, `::handle` (`:dragging`).
- The colour window: **`ColorWheel`** (`::marker`), **`ClipPreview`** (a box, `padding`, `width`, `height`;
  `::clip`: `min-height`, `border-top-width`), **`TrackPreview`** (`::row`: a box in the track's colour,
  as a panel row; `::stripe`, `::name`, `::readout`, `::button`, `::meter`), **`Swatch`** (`::chip`).

### What stays the program's

- **Data:** a clip's, track's, group's, pad's or choke group's own colour, and the shades worked out from it
  (notes and velocity bars take their clip's); waveforms; the overview's clip blocks.
- **What you do depends on it:** how far from an edge a drag grabs it, drag thresholds, the patch panel's unit,
  jack and card geometry (saved with projects), the fader's value layout, clip insets used for clicking.
- **Drawings:** the shapes of arrows, check marks, the Explorer's file and folder icons, the clip's chain mark,
  the patch panel's faceplates; dash patterns; text positions inside the tuner and the patch cards.
- **Placement:** tooltip and drag-picture offsets, the overlap of a submenu, the tool buttons' content insets
  (their padding sets their size); dialogs' first sizes before they fit their contents.
- **When sizes apply:** most follow a skin change at once; a dialog reads its room when it opens, and the track
  panel's rows when they are rebuilt (a skin change does that).

## Examples

A skin that only changes some colours:

```css
:root {
    --skin-name: "Warm";
    --accent: #d9822b;
    --highlight: #d9822b;
    --playhead: #ffd166;
    --meter-ok: #8ac926;
}
```

Bigger and squarer buttons, a wider track panel, a taller ruler:

```css
:root {
    --skin-name: "Roomy";
    --panel-width: 300px;
    --ruler-height: 30px;
    font-size: 10pt;
}
Button { border-radius: 0; }
PushButton { padding: 6px 18px; }
Slider::handle { width: 12px; height: 12px; border-radius: 6px; }
```

A taller title bar with its title centred, the buttons on the left as round lights, and a red edge around the
window while it has the focus:

```css
:root { --title-bar-height: 34px; --title-buttons: left; }
TitleBar { background-image: linear-gradient(to bottom, #3a3d44, #2b2d31); padding: 0 6px; }
TitleBar::title { text-align: center; font-weight: bold; }
TitleBar::close, TitleBar::maximize, TitleBar::minimize {
    width: 14px; height: 14px; margin: 0 4px; border-radius: 7px; icon-size: 6px;
}
TitleBar::close { background-color: #ff5f57; color: #4d0000; }
TitleBar::maximize { background-color: #28c840; color: #003b00; }
TitleBar::minimize { background-color: #febc2e; color: #5a3d00; }
TitleBar:inactive::close, TitleBar:inactive::maximize, TitleBar:inactive::minimize { background-color: #55585f; }
Window:framed:active { border-color: #c0392b; }
```

Solo in green, a thicker playhead, selected clips outlined:

```css
.toggle-solo { --toggle-color: #2fbf71; }
Timeline::playhead { stroke-width: 3px; }
Clip:selected::overlay { border: 2px solid #ffffff; }
```
