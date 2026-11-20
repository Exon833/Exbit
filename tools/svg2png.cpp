// svg2png <in.svg> <out.png> <size>: an SVG drawn as a square PNG by Tiny DAW's own renderer (gfx/svg.h), so
// no converter needs to be installed (tools/appimage.sh: the AppImage's .DirIcon, which must be a PNG).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "gfx/image.h"
#include "gfx/svg.h"

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: svg2png <in.svg> <out.png> <size>\n");
        return 2;
    }
    int size = std::atoi(argv[3]);
    Image img = size > 0 ? renderSvgFile(argv[1], size, size) : Image();
    if (img.empty()) {
        fprintf(stderr, "svg2png: can't draw %s\n", argv[1]);
        return 1;
    }
    std::string error;
    if (!savePng(argv[2], img, &error)) {
        fprintf(stderr, "svg2png: can't write %s: %s\n", argv[2], error.c_str());
        return 1;
    }
    return 0;
}
