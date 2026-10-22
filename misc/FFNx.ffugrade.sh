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

// Final Fantasy Universe: the battle look's colour grade (Richard's next list, item 4: "per-stage colour grading, each
// world's palette kept"; docs/battle-look.md "Colour grade"), shared by FF7's and FF8's passes (FFNx.ffuff7look.frag,
// FFNx.ffuff8look.frag) and, in HLSL, FF9's (mods/ff9-globe hlsl/look_glow.hlsl). No tint: each stage keeps its own
// colours. Its tones are given more shape about its own middle grey (the mean brightness of 16 places of its picture:
// a dark cave keeps its darkness, a bright beach its light), and its duller colours are made a little richer than its
// already rich ones (vibrance). In display (gamma) numbers, as the games draw; k the strength (the slider: 0-2).
// The middle is 64 places on an 8 x 8 grid, worked out once a picture in the passes' vertex shaders (a moving character
// or spark changing one of 16 made the whole grade pulse: review).
float ffuGradeMiddleOf(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}
vec3 ffuGrade(vec3 c, float k, float middle)
{
    float l = max(dot(c, vec3(0.299, 0.587, 0.114)), 1e-4);
    float p = clamp(middle, 0.12, 0.6);
    float g = 1.0 + 0.3 * k;   // (the curve's steepness about the middle)
    float lc = l < p ? p * pow(l / p, g) : 1.0 - (1.0 - p) * pow(max(1.0 - l, 0.0) / (1.0 - p), g);
    // (the ratio held so no channel passes 1: a bright saturated colour keeps its hue rather than clipping; review)
    float most = max(c.r, max(c.g, c.b));
    vec3 o = c * min(lc / l, 1.0 / max(most, 1e-4));
    float saturation = max(o.r, max(o.g, o.b)) - min(o.r, min(o.g, o.b));
    return clamp(vec3_splat(lc) + (o - vec3_splat(lc)) * (1.0 + 0.3 * k * (1.0 - saturation)), 0.0, 1.0);
}
