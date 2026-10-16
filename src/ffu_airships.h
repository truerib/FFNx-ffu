/****************************************************************************/
//    Copyright (C) 2026 Final Fantasy Universe                             //
//                                                                          //
//    This file is part of FFNx                                             //
//                                                                          //
//    FFNx is free software: you can redistribute it and/or modify          //
//    it under the terms of the GNU General Public License as published by  //
//    the Free Software Foundation, either version 3 of the License         //
//                                                                          //
//    FFNx is distributed in the hope that it will be useful,               //
//    but WITHOUT ANY WARRANTY; without even the implied warranty of        //
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         //
//    GNU General Public License for more details.                          //
/****************************************************************************/

// Final Fantasy Universe: the airship (and character) models the fork draws from mesh/airships/<id>.gltf, for FF7
// (src/ff7/world/ffu.cpp) and FF8 (src/ff8/world/renderer.cpp). A model with <id>_frames.json also has its own
// game's animation baked frame by frame (<id>_fNN.gltf, tools/airships write_frames: the Highwind's propellers, the
// FF9 airships'), and the frame for now is drawn. Frames load one per call so the first take-off doesn't stall;
// the still model stands in until they are all in.
#pragma once

#include <cstddef>

class ExternalMesh;

namespace ffu
{
    // nullptr when there is no model for `id` (logged once, as `what`).
    ExternalMesh *airshipModel(const char *id, size_t idSize, const char *what);
}
