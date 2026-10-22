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

$input v_texcoord0, v_ffu, v_ffucap

#include <bgfx/bgfx_shader.sh>

#include "FFNx.ffuff8look.sh"

bool onFloor(vec2 uv, float z)
{
    float floorDepth = floorAt(uv);
    return floorDepth > 1.0 && floorDepth < (z - u_ff8Depth.y) / u_ff8Depth.x;
}

vec3 viewFrom(vec2 uv, float Z)
{
    return vec3(rayAt(uv) * Z, Z);
}

vec3 viewAt(vec2 uv)
{
    return viewFrom(uv, viewDepthAt(uv, texture2D(s_ff8Depth, uv).x));
}

// A neighbour of a pixel at depth zc (view depth Zc): the floor's off the floor; a character's on the same triangle (its
// depth a little apart) by FF8's slope within a triangle (its characters' line holds across them only: review), else
// on its own line.
vec3 viewNear(vec2 uv, float zc, float Zc)
{
    float z = texture2D(s_ff8Depth, uv).x;
    if (onFloor(uv, z))
        return viewFrom(uv, viewDepthAt(uv, z));
    return viewFrom(uv, abs(z - zc) < 2e-4 && u_ff8Stage.x > 0.0 ? Zc + (z - zc) / u_ff8Stage.x : viewDepthAt(uv, z));
}

// The spells' light round this pixel (as FF7's pass: FFNx.ffuff7look.frag): the effects' light, blurred wide.
vec3 spellLight(vec2 uv)
{
    // (the light comes blurred about a tenth of the picture wide, at a sixteenth of its size: ffu_glow.cpp; four more
    // taps a little apart smooth its texels)
    vec2 o = u_ff8Spell.yz;
    return (2.0 * texture2D(s_ff8Light, uv).rgb + texture2D(s_ff8Light, uv + vec2(o.x, 0.0)).rgb + texture2D(s_ff8Light, uv - vec2(o.x, 0.0)).rgb
        + texture2D(s_ff8Light, uv + vec2(0.0, o.y)).rgb + texture2D(s_ff8Light, uv - vec2(0.0, o.y)).rgb) / 6.0;
}

// 1 / view depth (0: nothing drawn: farthest)
float nearAt(vec2 uv)
{
    float z = texture2D(s_ff8Depth, uv).x;
    return z < 0.9999 && z > 0.0001 ? 1.0 / max(viewDepthAt(uv, z), 1.0) : 0.0;
}

// The rim light here (as FF7's pass): how strongly this pixel stands at the edge of something well in front of what lies
// behind it, 8 directions at two distances.
float rimAt(vec2 uv, float here)
{
    // (against the plane through this pixel, in nearness, from its nearer neighbours: a floor receding into the distance
    // is no edge, only what stands in front of what lies behind it)
    vec2 t = u_ff8Frame.zw;
    float px = nearAt(uv + vec2(t.x, 0.0)), nx = nearAt(uv - vec2(t.x, 0.0));
    float py = nearAt(uv + vec2(0.0, t.y)), ny = nearAt(uv - vec2(0.0, t.y));
    vec2 g = vec2(abs(px - here) < abs(here - nx) ? px - here : here - nx, abs(py - here) < abs(here - ny) ? py - here : here - ny);
    float rim = 0.0;
    // (one ring, at three quarters of the reach and full weight: two rings, at half and the whole reach, the second at
    // half weight, cost 8 more fetches a pixel; FF8 ran at 23 a second with every switch on)
    for (int ring = 1; ring <= 1; ring++)
        for (int k = 0; k < 8; k++)
        {
            float a = float(k) * 0.785398 + 0.39 * float(ring);
            vec2 o = vec2(cos(a), sin(a)) * u_ff8Rim.yz * 0.75;
            vec2 at = uv + o;
            if (at.x < 0.0 || at.x > 1.0 || at.y < 0.0 || at.y > 1.0) continue;
            float plane = here + g.x * o.x / t.x + g.y * o.y / t.y;
            // (most where the step lies sideways, half straight up or down: a head's top keeps some)
            rim = max(rim, smoothstep(0.15, 0.45, (plane - nearAt(at)) / here) * (ring == 1 ? 1.0 : 0.5) * (0.5 + 0.5 * abs(cos(a))));
        }
    return rim;
}

vec2 uvAt(vec3 p)
{
    vec2 s = vec2(u_ff8Lens.z + u_ff8Lens.x * (u_ff8Cam.y + u_ff8Cam.x * p.x / p.z),
                  u_ff8Lens.w + u_ff8Lens.y * (u_ff8Cam.z + u_ff8Cam.x * p.y / p.z));
    return (s - vec2(u_ff8Cam.w, 0.0)) / u_ff8Frame.xy;
}

void main()
{
    // (the impact's shake: FF8's 3D picture moved and magnified a little, so its edges stay in; its HUD comes after)
    vec2 uv = (v_texcoord0 - 0.5) / u_ff8Shake.z + 0.5 + u_ff8Shake.xy;
    vec4 scene = texture2D(s_ff8Scene, uv);
    if (v_texcoord0.x < u_ff8Haze.y)
    {
        gl_FragColor = u_ff8Haze.y - v_texcoord0.x < 0.0012 ? vec4(0.0, 0.0, 0.0, scene.a) : scene;   // (the test split: FF8's own on the left)
        return;
    }
    float z = texture2D(s_ff8Depth, uv).x;
    float backdrop = v_ffu.x;   // (the vertex shader's: FFNx.ffuff8look.sh)
    if (u_ff8Haze.w > 0.5 && u_ff8Haze.w < 1.5)
    {
        // (the test view: blue nothing drawn (far), magenta nothing (near), red the backdrop, green the floor (what FF8
        // pushed back, read off the floor plane), yellow the characters' line; over the picture dimmed)
        vec3 code = z >= 0.9999 ? vec3(0.0, 0.0, 1.0) : z <= 0.0001 ? vec3(1.0, 0.0, 1.0) : backdropAt(uv, z, backdrop) ? vec3(1.0, 0.0, 0.0)
            : onFloor(uv, z) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 1.0, 0.0);
        gl_FragColor = vec4(mix(scene.rgb * 0.3, code, 0.6), scene.a);
        return;
    }
    if (backdropAt(uv, z, backdrop))
    {
        if (u_ff8Haze.w > 1.5) { gl_FragColor = vec4(0.0, 0.0, 0.3, scene.a); return; }   // (the shadow view: the backdrop dark blue)
        gl_FragColor = scene;   // FF8's sky or nothing of its 3D: as FF8 drew it
        return;
    }
    vec2 t = u_ff8Frame.zw;
    vec3 p = viewAt(uv);
    vec3 shaded = scene.rgb;   // (the haze alone: FF8's own colour)
    float shadowSeen = 1.0;   // (the shadow view)
    // (the battle and its ground, faded out from twice to three times the depth its 3D stands at: FF8's far scenery
    // keeps its own light, as its pieces stand on lines of their own; beyond that nothing of the lighting is worked out)
    float onStage = 1.0 - smoothstep(1.333 * u_ff8Haze.x, 2.0 * u_ff8Haze.x, p.z);
    if (u_ff8Depth.w > 0.5 && onStage > 0.0)
    {
        // (two texels across: one step of FF8's stage depth is 10-20 units of view depth, a floor's pixel about as much)
        vec3 px = viewNear(uv + vec2(2.0 * t.x, 0.0), z, p.z), nx = viewNear(uv - vec2(2.0 * t.x, 0.0), z, p.z);
        vec3 py = viewNear(uv + vec2(0.0, 2.0 * t.y), z, p.z), ny = viewNear(uv - vec2(0.0, 2.0 * t.y), z, p.z);
        vec3 dx = abs(px.z - p.z) < abs(p.z - nx.z) ? px - p : p - nx;
        vec3 dy = abs(py.z - p.z) < abs(p.z - ny.z) ? py - p : p - ny;
        vec3 n = normalize(cross(dx, dy));
        if (dot(n, p) > 0.0) n = -n;   // toward the camera

        vec3 l = normalize(u_ff8Light.xyz);
        float ndl = dot(n, l);
        float diffuse = clamp(ndl * u_ff8Look.y + (1.0 - u_ff8Look.y), 0.0, 1.0);

        // The main light's shadow: 16 steps toward it through FF8's depth (what stands nearer the camera on that path,
        // and not far behind it, is in the way), fading with the step.
        // (the soft shadows switch: three rays, the light spread a little either way across, averaged: a softer edge)
        float shadow = 0.0;
        vec3 start = p + n * (0.004 * p.z);
        vec3 across = normalize(cross(l, n) + vec3(1e-4, 0.0, 0.0));
        int rays = u_ff8Rim.w > 0.5 ? 3 : 1;
        for (int r = 0; r < 3; r++)
        {
            if (r >= rays) break;
            vec3 lr = rays > 1 ? normalize(l + across * (float(r) - 1.0) * 0.14) : l;
            float rayShadow = 0.0;
            // (the first hit is the ray's strongest, 1 - k/17 falling with k, so the ray ends there; the soft shadows' two
            // side rays take every other step, the same reach: FF8 ran at 23 a second with every switch on)
            int stride = rays > 1 && r != 1 ? 2 : 1;
            for (int k = 1; k <= 16; k++)
            {
                if (k - (k / stride) * stride != 0) continue;
                vec3 q = start + lr * (float(k) * 0.0035 * p.z);
                vec2 quv = uvAt(q);
                if (q.z <= 1.0 || quv.x < 0.0 || quv.x > 1.0 || quv.y < 0.0 || quv.y > 1.0) break;
                float there = viewAt(quv).z;
                float ahead = q.z - there;
                if (ahead > 0.002 * q.z && ahead < 0.06 * q.z)
                {
                    rayShadow = 1.0 - float(k) / 17.0;
                    break;
                }
            }
            shadow += rayShadow / float(rays);
        }
        float lit = mix(1.0, 1.0 - shadow, clamp(ndl * 5.0, 0.0, 1.0));   // (past the terminator the wrap shades it)
        shadowSeen = lit * clamp(ndl * 2.0 + 0.5, 0.0, 1.0);

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
        shaded = scene.rgb * pow(max(vec3_splat(mix(1.0, light, onStage)), vec3_splat(1e-4)), vec3_splat(0.4545));
    }
    float effect = u_ff8Depth.z > 0.5 ? clamp(scene.a / max(dot(scene.rgb, vec3(0.299, 0.587, 0.114)), 0.02), 0.0, 1.0) : 0.0;   // (alpha: the light effects added)
    if (u_ff8Rim.x > 0.0 && onStage > 0.0 && !onFloor(uv, z))
    {
        // (the rim light: a light edge, a factor on the pixel's own colour and a touch of light, the battle only)
        float rim = rimAt(uv, 1.0 / max(p.z, 1.0)) * u_ff8Rim.x * onStage;
        shaded += rim * (shaded + vec3_splat(0.08));
    }
    // The spells' light: what stands round an effect takes its light, as a factor on its own colour (the battle and its
    // ground: faded out beyond them as the lighting is)
    if (u_ff8Spell.x > 0.0 && onStage > 0.0)
        shaded += shaded * (vec3_splat(1.0) - exp(-spellLight(uv) * u_ff8Spell.x)) * onStage;   // (soft: at most twice)
    // The haze (outdoors only: two or more of the sky's places seen), beyond the stage, toward the sky's colour.
    if (u_ff8Haze.z > 0.0 && backdrop < 1e29)
    {
        vec4 sky = vec4(v_ffucap.rgb, v_ffu.y);
        float outdoors = smoothstep(1.6, 2.4, backdrop / max(u_ff8Haze.x, 1.0));   // (as FF7's: no step as cameras move)
        if (sky.w >= 2.0 && backdrop > u_ff8Haze.x)
            shaded = mix(shaded, sky.rgb, u_ff8Haze.z * outdoors * clamp((p.z - u_ff8Haze.x) / (backdrop - u_ff8Haze.x), 0.0, 1.0));
    }
    gl_FragColor = u_ff8Haze.w > 1.5 ? vec4(vec3_splat(shadowSeen), scene.a) : vec4(mix(shaded, scene.rgb, effect), scene.a);
}
