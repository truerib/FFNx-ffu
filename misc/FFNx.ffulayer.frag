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

// Final Fantasy Universe: the live layer's compositor (src/ffu_layer.cpp, docs/crossover-design.md). Another
// game's own pixels of what it draws for us (a visiting airship) are laid into this game's world pass: each
// pixel keeps the guest's colour, and its depth is turned back into a view distance, scaled from the guest's
// units to ours (both per planet radius), and written as the depth this game's own 3D draws give that distance,
// so our hills and clouds hide the ship where they stand in front of it.
// Depth formats (u_ffuLayerGuest.x / u_ffuLayerHost.x, ffu_layer.cpp FFU_LAYER_DEPTH_*):
//   0  FF7's WorldDepth (FFNx.ffu7.sh): yzw = the game's depth at 30,000 and 60,000 units, and the far end
//   1  packed: rgb = t in 24 bits, view distance = near * (far / near)^t; yz = near, far (FF9's layer)
//   2  FF8Depth (FFNx.vert): FF8's linear TL depth, then a log band

$input v_texcoord0

#include <bgfx/bgfx_shader.sh>

SAMPLER2D(s_ffuColour, 0);   // RGBA8, straight alpha
SAMPLER2D(s_ffuDepth, 1);    // R32F raw (formats 0, 2) or RGBA8 packed (format 1)

uniform vec4 u_ffuLayerGuest;   // format, parameters
uniform vec4 u_ffuLayerHost;    // format, parameters
uniform vec4 u_ffuLayerScale;   // guest units per planet radius, ours

#define FFU_NEAR_LIMIT 30000.0
#define FFU_DEPTH_FAR 4000000.0
#define FF8_DEPTH_STEP 1.5235901e-05
#define FF8_DEPTH_BASE 0.0015

// FFNx.ffu7.sh WorldDepth backwards: a perspective depth is a + b / w, a and b from the two samples.
float Ff7ViewZ(float d, vec3 p)
{
    float atLimit = p.x, further = p.y, farEnd = p.z, nearEnd = 1.0 - farEnd;
    if ((d - 0.5) * (nearEnd - 0.5) >= 0.0)
    {
        float game = atLimit + saturate((d - 0.5) / (nearEnd - 0.5)) * (nearEnd - atLimit);
        float b = 2.0 * FFU_NEAR_LIMIT * (atLimit - further);
        float a = atLimit - b / FFU_NEAR_LIMIT;
        return b / (game - a);
    }
    float t = saturate((d - 0.5) / ((farEnd - 0.5) * 0.999));
    return FFU_NEAR_LIMIT * exp2(t * log2(FFU_DEPTH_FAR / FFU_NEAR_LIMIT));
}

// FFNx.ffu7.sh WorldDepth forwards, for a 3D pixel at view distance w.
float Ff7Depth(float w, vec3 p)
{
    float atLimit = p.x, further = p.y, farEnd = p.z, nearEnd = 1.0 - farEnd;
    if (w <= FFU_NEAR_LIMIT)
    {
        float b = 2.0 * FFU_NEAR_LIMIT * (atLimit - further);
        float a = atLimit - b / FFU_NEAR_LIMIT;
        return mix(0.5, nearEnd, saturate(((a + b / w) - atLimit) / (nearEnd - atLimit)));
    }
    float t = log2(w / FFU_NEAR_LIMIT) / log2(FFU_DEPTH_FAR / FFU_NEAR_LIMIT);
    return mix(0.5, farEnd, saturate(t) * 0.999);
}

// FFNx.vert FF8Depth forwards and backwards (the middle of FF8's 8-unit steps).
float Ff8Depth(float zv)
{
    float d = floor(floor(zv + 0.5) * 0.125);
    if (d < 4095.0) return d * FF8_DEPTH_STEP + FF8_DEPTH_BASE;
    float z4095 = 4095.0 * FF8_DEPTH_STEP + FF8_DEPTH_BASE;
    return mix(z4095, 0.9999, saturate(log2(zv / 32760.0) / log2(4.0e6 / 32760.0)));
}

float Ff8ViewZ(float d)
{
    float z4095 = 4095.0 * FF8_DEPTH_STEP + FF8_DEPTH_BASE;
    if (d < z4095) return (d - FF8_DEPTH_BASE) / FF8_DEPTH_STEP * 8.0 + 4.0;
    return 32760.0 * exp2(saturate((d - z4095) / (0.9999 - z4095)) * log2(4.0e6 / 32760.0));
}

float PackedViewZ(vec4 c, vec2 range)
{
    float t = dot(c.rgb, vec3(1.0, 1.0 / 255.0, 1.0 / 65025.0));
    return range.x * exp2(t * log2(range.y / range.x));
}

void main()
{
    vec4 colour = texture2D(s_ffuColour, v_texcoord0);
    if (colour.a < 0.5) discard;
    vec4 raw = texture2D(s_ffuDepth, v_texcoord0);
    float z = u_ffuLayerGuest.x < 0.5 ? Ff7ViewZ(raw.r, u_ffuLayerGuest.yzw)
            : u_ffuLayerGuest.x < 1.5 ? PackedViewZ(raw, u_ffuLayerGuest.yz) : Ff8ViewZ(raw.r);
    float w = max(z / u_ffuLayerScale.x * u_ffuLayerScale.y, 1.0);
    gl_FragDepth = u_ffuLayerHost.x < 0.5 ? Ff7Depth(w, u_ffuLayerHost.yzw) : Ff8Depth(w);
    gl_FragColor = vec4(colour.rgb, 1.0);
}
