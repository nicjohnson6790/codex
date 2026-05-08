#pragma once

#include "PineTreePackConverter.hpp"

// TODO: Add a BC7 color/alpha output option for higher-quality color capture.
// TODO: Add a BC4 depth/thickness imposter texture alongside color and normal.
// TODO: Refine the current alpha-coverage preservation pass with material-aware foliage tuning.
// TODO: Hook the generated imposter textures into the runtime imposter renderer.
bool GeneratePineImposters(
    ImportedPack* pack,
    std::string* error);
