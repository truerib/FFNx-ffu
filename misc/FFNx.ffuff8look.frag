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

// Final Fantasy Universe: FF8's battle lighting (src/ffu_ff8look.cpp, docs/battle-look.md "FF8 lighting"), the battle
// look's shared light for a game whose battles reach FFNx only as screen-space triangles. Run once a battle frame, after
// FF8's 3D and before its first flat 2D draw (the HUD), over a copy of the picture so far and FF8's own depth: each 3D
// pixel's view-space place is rebuilt from FF8's linear depth and its lens, its normal from its neighbours, then lit as
// FF9's battles are (look_lit.hlsl): the main light with a soft wrap and the ambient in linear light, as a factor on
// FF8's own (already lit) colour; the main light's shadow ray-marched through the depth toward it; contact shading
// from the depth round it. The light stands in FF8's battle world, turned into the view by FF8's own camera each frame
// (ffu_ff8look.cpp). Effect pixels (the glow's alpha mask) and the sky are left as they are; alpha is kept.

$input v_texcoord0

#include <bgfx/bgfx_shader.sh>

SAMPLER2D(s_ff8Scene, 0);
SAMPLER2D(s_ff8Depth, 1);

uniform vec4 u_ff8Lens;    // ssigpu scale x, y; offset x, y (FF8's screen = offset + scale * (of + H * X / Z))
uniform vec4 u_ff8Cam;     // GTE H, OFX, OFY; the wide viewport's left (game units)
uniform vec4 u_ff8Frame;   // the picture's width and height (game units); one texel (uv)
uniform vec4 u_ff8Depth;   // FF8's depth = x * view z + y; z: the glow's mask in alpha (1) or not (0)
uniform vec4 u_ff8Light;   // toward the main light (view space: x right, y down, z ahead); w: its strength
uniform vec4 u_ff8Look;    // x ambient, y 1 / (1 + wrap), z exposure, w contact strength
uniform vec4 u_ff8Haze;    // the haze: x where it starts (view depth), z its most (0: none); y the test split (FFU_LOOK_SPLIT)

vec3 viewAt(vec2 uv)
{
    float z = texture2D(s_ff8Depth, uv).x;
    float Z = (z - u_ff8Depth.y) / u_ff8Depth.x;
    vec2 s = uv * u_ff8Frame.xy + vec2(u_ff8Cam.w, 0.0);
    float xz = ((s.x - u_ff8Lens.z) / u_ff8Lens.x - u_ff8Cam.y) / u_ff8Cam.x;
    float yz = ((s.y - u_ff8Lens.w) / u_ff8Lens.y - u_ff8Cam.z) / u_ff8Cam.x;
    return vec3(xz * Z, yz * Z, Z);
}

// FF8's backdrop (its sky dome, far 3D), from the picture's top: the farthest of 8 places there, and what stands within
// 80% of it (a dome round the camera is nearer at the picture's corners, about 0.79 of its farthest).
float backdropDepth()
{
    float farthest = 0.0;
    for (int k = 0; k < 8; k++)
    {
        float t = texture2D(s_ff8Depth, vec2((float(k) + 0.5) / 8.0, k < 4 ? 0.06 : 0.18)).x;
        if (t < 0.9999 && t > 0.0001) farthest = max(farthest, (t - u_ff8Depth.y) / u_ff8Depth.x);
    }
    return farthest > 0.0 ? 0.8 * farthest : 1e30;
}

bool backdropAt(float z, float backdrop)
{
    return z >= 0.9999 || z <= 0.0001 || (z - u_ff8Depth.y) / u_ff8Depth.x >= backdrop;
}

// The sky's colour: the backdrop near the picture's top (8 places); w how many were the backdrop (outdoors).
vec4 skyColour(float backdrop)
{
    vec4 sum = vec4_splat(0.0);
    for (int k = 0; k < 8; k++)
    {
        vec2 at = vec2((float(k) + 0.5) / 8.0, k < 4 ? 0.06 : 0.18);
        if (backdropAt(texture2D(s_ff8Depth, at).x, backdrop)) sum += vec4(texture2D(s_ff8Scene, at).rgb, 1.0);
    }
    return sum.w > 0.0 ? vec4(sum.rgb / sum.w, sum.w) : sum;
}

vec2 uvAt(vec3 p)
{
    vec2 s = vec2(u_ff8Lens.z + u_ff8Lens.x * (u_ff8Cam.y + u_ff8Cam.x * p.x / p.z),
                  u_ff8Lens.w + u_ff8Lens.y * (u_ff8Cam.z + u_ff8Cam.x * p.y / p.z));
    return (s - vec2(u_ff8Cam.w, 0.0)) / u_ff8Frame.xy;
}

void main()
{
    vec2 uv = v_texcoord0;
    vec4 scene = texture2D(s_ff8Scene, uv);
    if (uv.x < u_ff8Haze.y)
    {
        gl_FragColor = u_ff8Haze.y - uv.x < 0.0012 ? vec4(0.0, 0.0, 0.0, scene.a) : scene;   // (the test split: FF8's own on the left)
        return;
    }
    float z = texture2D(s_ff8Depth, uv).x;
    float backdrop = backdropDepth();
    if (backdropAt(z, backdrop))
    {
        gl_FragColor = scene;   // FF8's sky or nothing of its 3D: as FF8 drew it
        return;
    }
    vec2 t = u_ff8Frame.zw;
    vec3 p = viewAt(uv);
    vec3 px = viewAt(uv + vec2(t.x, 0.0)), nx = viewAt(uv - vec2(t.x, 0.0));
    vec3 py = viewAt(uv + vec2(0.0, t.y)), ny = viewAt(uv - vec2(0.0, t.y));
    vec3 dx = abs(px.z - p.z) < abs(p.z - nx.z) ? px - p : p - nx;
    vec3 dy = abs(py.z - p.z) < abs(p.z - ny.z) ? py - p : p - ny;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n;   // toward the camera

    vec3 l = normalize(u_ff8Light.xyz);
    float ndl = dot(n, l);
    float diffuse = clamp(ndl * u_ff8Look.y + (1.0 - u_ff8Look.y), 0.0, 1.0);

    // The main light's shadow: 16 steps toward it through FF8's depth (what stands nearer the camera on that path,
    // and not far behind it, is in the way), fading with the step.
    float shadow = 0.0;
    vec3 start = p + n * (0.004 * p.z);
    for (int k = 1; k <= 16; k++)
    {
        vec3 q = start + l * (float(k) * 0.0035 * p.z);
        vec2 quv = uvAt(q);
        if (q.z <= 1.0 || quv.x < 0.0 || quv.x > 1.0 || quv.y < 0.0 || quv.y > 1.0) break;
        float there = viewAt(quv).z;
        float ahead = q.z - there;
        if (ahead > 0.002 * q.z && ahead < 0.06 * q.z) shadow = max(shadow, 1.0 - float(k) / 17.0);
    }
    float lit = mix(1.0, 1.0 - shadow, clamp(ndl * 5.0, 0.0, 1.0));   // (past the terminator the wrap shades it)

    // Contact shading: 8 neighbours on a ring about 1.5% of the depth across; those in front of the surface darken it.
    float occlusion = 0.0;
    float radius = 0.015 * p.z;
    vec2 ruv = abs(uvAt(p + vec3(radius, 0.0, 0.0)) - uv);
    float ring = max(ruv.x, 2.0 * t.x);
    for (int k = 0; k < 8; k++)
    {
        float a = float(k) * 0.785398;
        vec2 o = vec2(cos(a), sin(a)) * ring * (mod(float(k), 2.0) < 0.5 ? 1.0 : 0.55);
        vec3 s = viewAt(uv + o * vec2(1.0, u_ff8Frame.x / u_ff8Frame.y));
        vec3 d = s - p;
        float dist = length(d);
        occlusion += clamp(dot(n, d / max(dist, 1e-3)) - 0.15, 0.0, 1.0) * clamp(1.0 - dist / (3.0 * radius), 0.0, 1.0);
    }
    float contact = 1.0 - u_ff8Look.w * occlusion / 8.0;

    float light = (u_ff8Look.x * contact + u_ff8Light.w * diffuse * lit) * u_ff8Look.z;
    vec3 shaded = scene.rgb * pow(max(vec3_splat(light), vec3_splat(1e-4)), vec3_splat(0.4545));
    // The haze (outdoors only: two or more of the sky's places seen), beyond the stage, toward the sky's colour.
    if (u_ff8Haze.z > 0.0 && backdrop < 1e29)
    {
        vec4 sky = skyColour(backdrop);
        float outdoors = smoothstep(1.6, 2.4, backdrop / max(u_ff8Haze.x, 1.0));   // (as FF7's: no step as cameras move)
        if (sky.w >= 2.0 && backdrop > u_ff8Haze.x)
            shaded = mix(shaded, sky.rgb, u_ff8Haze.z * outdoors * clamp((p.z - u_ff8Haze.x) / (backdrop - u_ff8Haze.x), 0.0, 1.0));
    }
    float effect = u_ff8Depth.z > 0.5 ? clamp(scene.a * 3.0, 0.0, 1.0) : 0.0;
    gl_FragColor = vec4(mix(shaded, scene.rgb, effect), scene.a);
}
