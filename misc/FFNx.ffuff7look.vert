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

// Final Fantasy Universe: FF7's battle haze and cinematic bars (src/ffu_ff7look.cpp). One
// full-screen quad, its corners already in clip space.

$input a_position, a_texcoord0
$output v_texcoord0, v_ffu

#include <bgfx/bgfx_shader.sh>

SAMPLER2D(s_ff7Scene, 0);

void main()
{
    gl_Position = vec4(a_position.xy, 0.5, 1.0);
    v_texcoord0 = a_texcoord0;
    // The colour grade's middle grey (FFNx.ffugrade.sh): the mean brightness of 64 places on an 8 x 8 grid, once a picture
    float sum = 0.0;
    for (int k = 0; k < 64; k++)
        sum += dot(texture2DLod(s_ff7Scene, vec2((float(k - (k / 8) * 8) + 0.5) / 8.0, (float(k / 8) + 0.5) / 8.0), 0.0).rgb, vec3(0.299, 0.587, 0.114));
    v_ffu = vec4(sum / 64.0, 0.0, 0.0, 0.0);
}
