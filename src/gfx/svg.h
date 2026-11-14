// A small SVG renderer, enough for the playback buttons' icons (DAW_icons) and similar simple
// drawings: <svg> with viewBox/width/height, <g> (transform, fill, stroke, opacity inherited),
// <path d>, <rect> (rx/ry), <circle>, <ellipse>, <line>, <polyline>, <polygon>; fill / stroke /
// stroke-width / stroke-linecap / stroke-linejoin / fill-rule / opacity / fill-opacity /
// stroke-opacity as attributes or in style="…"; colours as #rgb, #rrggbb, rgb(), named basic
// colours, none, currentColor; transforms matrix / translate / scale / rotate / skewX / skewY.
// Unknown elements (metadata, sodipodi:namedview, defs…) are skipped.
#pragma once
#include <string>

#include "gfx/image.h"

// Renders the drawing scaled to fit w x h (keeping its aspect ratio, centred). Returns an empty
// image if the text isn't SVG it can read.
Image renderSvg(const std::string& svgText, int w, int h);
Image renderSvgFile(const std::string& path, int w, int h);
// The same, every painted pixel in `tint` (coverage kept): icons in the skin's colours.
Image renderSvgTinted(const std::string& svgText, int w, int h, Color tint);
