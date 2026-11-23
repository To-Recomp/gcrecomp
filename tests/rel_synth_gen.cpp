// SPDX-License-Identifier: MIT
//
// Writes the synthetic REL module of tests/rel_synth.h to a file (the recompiler reads it at build time).
#include <cstdio>

#include "rel_synth.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output.rel>\n", argv[0]);
        return 2;
    }
    const relsynth::Module m = relsynth::build();
    FILE* f = std::fopen(argv[1], "wb");
    if (!f || std::fwrite(m.file.data(), 1, m.file.size(), f) != m.file.size()) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    std::fclose(f);
    return 0;
}
