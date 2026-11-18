// A small SVG renderer, enough for the playback buttons' icons (DAW_icons) and similar simple
// drawings: <svg> with viewBox/width/height, <g> (transform, fill, stroke, opacity inherited),
// <path d>, <rect> (rx/ry), <circle>, <ellipse>, <line>, <polyline>, <polygon>; fill / stroke /
// stroke-width / stroke-linecap / stroke-linejoin / fill-rule / opacity / fill-opacity /
// stroke-opacity as attributes or in style="…"; colours as #rgb, #rrggbb, rgb(), named basic
// colours, none, currentColor; transforms matrix / translate / scale / rotate / skewX / skewY.
// viewBox with preserveAspectRatio (none: stretched to the size asked for, each way on its own;
// xMin|xMid|xMax + YMin|YMid|YMax, meet or slice; default xMidYMid meet), on nested <svg> too.
// Gradients: fill / stroke="url(#id) [fallback colour]" naming a <linearGradient> (x1 y1 x2 y2) or
// <radialGradient> (cx cy r fx fy fr) anywhere in the file: <stop offset stop-color stop-opacity>
// (attributes or style="…"), gradientUnits (objectBoundingBox, the default, or userSpaceOnUse:
// lengths, % of the viewBox), gradientTransform, spreadMethod pad / reflect / repeat, and href /
// xlink:href to another gradient (its stops and attributes, where this one has none). One stop: its
// colour; no stops: none; a reference to something else: the fallback colour, else none.
// Unknown elements (metadata, sodipodi:namedview, defs…) are not drawn.
#pragma once
#include <string>

#include "gfx/image.h"

// Renders the drawing scaled to fit w x h (keeping its aspect ratio, centred). Returns an empty
// image if the text isn't SVG it can read.
Image renderSvg(const std::string& svgText, int w, int h);
Image renderSvgFile(const std::string& path, int w, int h);
// The same, every painted pixel in `tint` (coverage kept): icons in the skin's colours.
Image renderSvgTinted(const std::string& svgText, int w, int h, Color tint);
// Its own size (px: width and height, absolute lengths; the other from the viewBox's proportions when only one is
// given; 0: not given) and its proportions (width / height: the viewBox's, else width and height's; 0: none).
// False if the text isn't SVG.
bool svgSize(const std::string& svgText, float& w, float& h, float& ratio);
