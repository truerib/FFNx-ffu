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
// full-screen quad, its corners already in clip space.

$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx/bgfx_shader.sh>

void main()
{
    gl_Position = vec4(a_position.xy, 0.5, 1.0);
    v_texcoord0 = a_texcoord0;
}
