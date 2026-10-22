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

// Final Fantasy Universe: FF8's battle lighting (src/ffu_ff8look.cpp, docs/battle-look.md "FF8 lighting"). One
// full-screen quad, its corners already in clip space; the backdrop and the sky's colour worked out here, once.

$input a_position, a_texcoord0
$output v_texcoord0, v_ffu, v_ffucap

#include <bgfx/bgfx_shader.sh>
#include "FFNx.ffuff8look.sh"

void main()
{
    gl_Position = vec4(a_position.xy, 0.5, 1.0);
    v_texcoord0 = a_texcoord0;
    // The backdrop and the sky's colour, the same for every pixel: once here (FFNx.ffuff8look.sh)
    float backdrop = backdropDepth();
    vec4 sky = backdrop < 1e29 ? skyColour(backdrop) : vec4_splat(0.0);
    // The colour grade's middle grey (FFNx.ffugrade.sh): the mean brightness of 64 places on an 8 x 8 grid
    float sum = 0.0;
    for (int k = 0; k < 64; k++)
        sum += dot(texture2DLod(s_ff8Scene, vec2((float(k - (k / 8) * 8) + 0.5) / 8.0, (float(k / 8) + 0.5) / 8.0), 0.0).rgb, vec3(0.299, 0.587, 0.114));
    v_ffu = vec4(backdrop, sky.w, sum / 64.0, 0.0);
    v_ffucap = vec4(sky.rgb, 0.0);
}
