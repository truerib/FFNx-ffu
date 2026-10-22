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

// Final Fantasy Universe: FF8's battle lighting pass (FFNx.ffuff8look.vert, .frag): its samplers, uniforms and view
// depth, shared by both stages; the backdrop and the sky's colour are the same for the whole picture, so the vertex
// shader works them out once (fetched with texture2DLod, as a vertex shader must) and the pixels read them from v_ffu
// and v_ffucap (Richard 2026-10-22, "FF8s battles are very laggy": with every switch on FF8's battle ran at 23 a second,
// each pixel reading the backdrop's 8 places and the sky's 16 again).

SAMPLER2D(s_ff8Scene, 0);
SAMPLER2D(s_ff8Depth, 1);
SAMPLER2D(s_ff8Light, 2);   // the light FF8's effects added, blurred at a quarter (the glow's, the last frame's)

uniform vec4 u_ff8Lens;    // ssigpu scale x, y; offset x, y (FF8's screen = offset + scale * (of + H * X / Z))
uniform vec4 u_ff8Cam;     // GTE H, OFX, OFY; the wide viewport's left (game units)
uniform vec4 u_ff8Frame;   // the picture's width and height (game units); one texel (uv)
uniform vec4 u_ff8Depth;   // FF8's depth for all but its stage = x * view z + y; z: the glow's mask in alpha (1) or not (0); w: the lighting on (1)
uniform vec4 u_ff8Stage;   // x: FF8's slope within a triangle; y: the characters' farthest (view depth, a tenth on); z, w: the log's
uniform vec4 u_ff8Light;   // toward the main light (view space: x right, y down, z ahead); w: its strength
uniform vec4 u_ff8Look;    // x ambient, y 1 / (1 + wrap), z exposure, w contact strength
uniform vec4 u_ff8Spell;   // the spells' light: x its strength (0: off), yz its smoothing taps' offset (uv)
uniform vec4 u_ff8Haze;    // the haze: x where it starts (view depth), z its most (0: none); y the test split (FFU_LOOK_SPLIT); w the test view (FFU_LOOK_DEBUG: 1 what each pixel is taken for, 2 its shadow)
uniform vec4 u_ff8Shake;   // the impact's shake: xy the picture's offset (uv), z how much it is magnified (1: none)
uniform vec4 u_ff8Rim;     // the rim light: x its strength (0: off), yz how far in from an edge it reaches (uv); w the soft shadows (1: on)
uniform vec4 u_ff8Floor;   // FF8's battle floor: view depth = w / dot(xyz, the view ray); w 0: not read this frame

// The view ray through this pixel (x and y per unit of view depth), by FF8's lens
vec2 rayAt(vec2 uv)
{
    vec2 s = uv * u_ff8Frame.xy + vec2(u_ff8Cam.w, 0.0);
    return vec2(((s.x - u_ff8Lens.z) / u_ff8Lens.x - u_ff8Cam.y) / u_ff8Cam.x, ((s.y - u_ff8Lens.w) / u_ff8Lens.y - u_ff8Cam.z) / u_ff8Cam.x);
}

// The battle floor's depth along this pixel's ray (FF8's world height 0, from its camera: ffu_ff8look.cpp), or 0 where
// the ray never meets it (above the horizon) or the camera wasn't read this frame
float floorAt(vec2 uv)
{
    float down = dot(u_ff8Floor.xyz, vec3(rayAt(uv), 1.0));
    return u_ff8Floor.w > 0.0 && down > 1e-6 ? u_ff8Floor.w / down : 0.0;
}

// The view depth here. FF8 pushes its stage back behind everything, in pieces at depth offsets of their own (they overlap
// its characters' depths, so no depth tells them apart: 2026-10-22, builds 93-94); but nothing seen lies below the floor,
// so a depth read on the characters' line that stands beyond the floor along this ray is the stage's: the floor's.
float viewDepthAt(vec2 uv, float z)
{
    float own = (z - u_ff8Depth.y) / u_ff8Depth.x, floorDepth = floorAt(uv);
    if (floorDepth > 1.0 && floorDepth < own) return floorDepth;
    // (above the horizon, what reads beyond the characters' farthest is the stage's far scenery, read on their line as
    // nonsense: the backdrop; review)
    if (floorDepth <= 1.0 && u_ff8Stage.y > 0.0 && own > u_ff8Stage.y) return 1e7;
    return own;
}

// On the floor (what FF8 pushed back, read off the floor): not lit as an edge by the rim light
// FF8's backdrop (its sky dome, far 3D), from the picture's top: the farthest of 8 places there, and what stands within
// 80% of it (a dome round the camera is nearer at the picture's corners, about 0.79 of its farthest).
float backdropDepth()
{
    float farthest = 0.0;
    for (int k = 0; k < 8; k++)
    {
        vec2 at = vec2((float(k) + 0.5) / 8.0, k < 4 ? 0.06 : 0.18);
        float t = texture2DLod(s_ff8Depth, at, 0.0).x;
        if (t < 0.9999 && t > 0.0001) farthest = max(farthest, viewDepthAt(at, t));
    }
    return farthest > 0.0 ? 0.8 * farthest : 1e30;
}

bool backdropAt(vec2 uv, float z, float backdrop)
{
    return z >= 0.9999 || z <= 0.0001 || viewDepthAt(uv, z) >= backdrop;
}

// The sky's colour: the backdrop near the picture's top (8 places); w how many were the backdrop (outdoors).
vec4 skyColour(float backdrop)
{
    vec4 sum = vec4_splat(0.0);
    for (int k = 0; k < 8; k++)
    {
        vec2 at = vec2((float(k) + 0.5) / 8.0, k < 4 ? 0.06 : 0.18);
        if (backdropAt(at, texture2DLod(s_ff8Depth, at, 0.0).x, backdrop)) sum += vec4(texture2DLod(s_ff8Scene, at, 0.0).rgb, 1.0);
    }
    return sum.w > 0.0 ? vec4(sum.rgb / sum.w, sum.w) : sum;
}
