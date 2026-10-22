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

// Final Fantasy Universe: FF7's battle haze and cinematic bars (src/ffu_ff7look.cpp, docs/battle-look.md), run once a
// battle frame before FF7's menus, over the picture so far and FF7's own depth. With the lighting on, contact shading:
// each pixel darkened by its neighbours standing in front of the surface through it (feet on the ground, creases);
// outdoors (FF7's backdrop, its far scenery, seen near the picture's top), what stands beyond the stage fades toward
// the backdrop's colour; the backdrop itself, effect pixels (the glow's alpha mask) and nothing drawn are left as they
// are; then the bars, black.

$input v_texcoord0

#include <bgfx/bgfx_shader.sh>

SAMPLER2D(s_ff7Scene, 0);
SAMPLER2D(s_ff7Depth, 1);
SAMPLER2D(s_ff7Light, 2);   // the light FF7's effects added, blurred at a quarter (the glow's, the last frame's)

uniform vec4 u_ff7Depth;   // FF7's depth along the view axis: (z x + y) / (z z' + w), the rows FFNx drew its 3D with
uniform vec4 u_ff7Haze;    // x where the haze starts (view depth), y its most (0: none), z the bars' height (uv), w the mask
uniform vec4 u_ff7Split;   // x: the test split (FFU_LOOK_SPLIT): FF7's own left of it
uniform vec4 u_ff7Rim;     // the rim light: x its strength (0: off), yz how far in from an edge it reaches (uv)
uniform vec4 u_ff7Spell;   // the spells' light: x its strength (0: off), yz its smoothing taps' offset (uv)
uniform vec4 u_ff7Contact; // x the contact shading's strength (0: off), y its ring's radius (uv across); zw one texel (uv)

float viewDepth(float d)
{
    // (with FF7's projection both parts are negative: only a zero denominator is guarded, never its sign)
    float below = d * u_ff7Depth.z - u_ff7Depth.x;
    return (u_ff7Depth.y - d * u_ff7Depth.w) / (abs(below) < 1e-6 ? -1e-6 : below);
}

// 1 / view depth, which is affine on the screen for any flat surface
float nearness(vec2 uv)
{
    float d = texture2D(s_ff7Depth, uv).x;
    return d < 0.9999 && d > 0.0001 ? 1.0 / max(viewDepth(d), 1.0) : 0.0;
}

// Contact shading: 8 places on a ring (two radii) against the plane through this pixel (in nearness, from its nearer
// neighbours on each axis); those standing in front of it by 0.4-15% of its depth shade it (nearer than that: another
// thing in front, a silhouette, never a shadow on this one).
// The plane through this pixel, in nearness per texel along x and y (from its nearer neighbours on each axis)
vec2 nearSlope(vec2 uv, float here)
{
    vec2 texel = u_ff7Contact.zw;
    float px = nearness(uv + vec2(texel.x, 0.0)), nx = nearness(uv - vec2(texel.x, 0.0));
    float py = nearness(uv + vec2(0.0, texel.y)), ny = nearness(uv - vec2(0.0, texel.y));
    return vec2(abs(px - here) < abs(here - nx) ? px - here : here - nx, abs(py - here) < abs(here - ny) ? py - here : here - ny);
}

float contactAt(vec2 uv, float here)
{
    vec2 texel = u_ff7Contact.zw;
    vec2 g = nearSlope(uv, here);
    float gx = g.x, gy = g.y;
    vec2 ring = vec2(u_ff7Contact.y, u_ff7Contact.y * texel.y / texel.x);   // (round on the screen)
    float occlusion = 0.0;
    for (int k = 0; k < 8; k++)
    {
        float a = float(k) * 0.785398;
        vec2 o = vec2(cos(a), sin(a)) * ring * (mod(float(k), 2.0) < 0.5 ? 1.0 : 0.55);
        vec2 at = uv + o;
        if (at.x < 0.0 || at.x > 1.0 || at.y < 0.0 || at.y > 1.0) continue;   // (off the picture: nothing known there)
        float there = nearness(at);
        if (there <= 0.0) continue;
        float plane = here + gx * o.x / texel.x + gy * o.y / texel.y;
        float ahead = (there - plane) / here;   // (in front of the plane: nearer)
        occlusion += smoothstep(0.004, 0.02, ahead) * (1.0 - smoothstep(0.08, 0.15, ahead));
    }
    return 1.0 - u_ff7Contact.x * occlusion / 8.0;
}

// The spells' light round this pixel: the effects' light, blurred so a spell lights what stands about a tenth of the
// picture round it.
vec3 spellLight(vec2 uv)
{
    // (the light comes blurred about a tenth of the picture wide, at a sixteenth of its size: ffu_glow.cpp; four more
    // taps a little apart smooth its texels)
    vec2 o = u_ff7Spell.yz;
    return (2.0 * texture2D(s_ff7Light, uv).rgb + texture2D(s_ff7Light, uv + vec2(o.x, 0.0)).rgb + texture2D(s_ff7Light, uv - vec2(o.x, 0.0)).rgb
        + texture2D(s_ff7Light, uv + vec2(0.0, o.y)).rgb + texture2D(s_ff7Light, uv - vec2(0.0, o.y)).rgb) / 6.0;
}

// The rim light here: how strongly this pixel stands at the edge of something well in front of what lies behind it (8
// directions, two distances: full at the edge, half a reach in), 0 within a surface or where the step behind is small
// (a character's feet on the floor), 1 at a character against the far ground or the sky (nothing drawn: farthest).
float rimAt(vec2 uv, float here)
{
    // (against the plane through this pixel: ground receding into the distance is no edge, only what stands in front of
    // what lies behind it)
    vec2 g = nearSlope(uv, here);
    vec2 texel = u_ff7Contact.zw;
    float rim = 0.0;
    for (int ring = 1; ring <= 2; ring++)
        for (int k = 0; k < 8; k++)
        {
            float a = float(k) * 0.785398 + 0.39 * float(ring);
            vec2 o = vec2(cos(a), sin(a)) * u_ff7Rim.yz * (float(ring) * 0.5);
            vec2 at = uv + o;
            if (at.x < 0.0 || at.x > 1.0 || at.y < 0.0 || at.y > 1.0) continue;
            float plane = here + g.x * o.x / texel.x + g.y * o.y / texel.y;
            float farther = (plane - nearness(at)) / here;   // (0: on the plane; 1: nothing there)
            rim = max(rim, smoothstep(0.15, 0.45, farther) * (ring == 1 ? 1.0 : 0.5));
        }
    return rim;
}

void main()
{
    vec2 uv = v_texcoord0;
    vec4 scene = texture2D(s_ff7Scene, uv);
    if (uv.x < u_ff7Split.x)
    {
        gl_FragColor = u_ff7Split.x - uv.x < 0.0012 ? vec4(0.0, 0.0, 0.0, scene.a) : scene;
        return;
    }
    vec3 shown = scene.rgb;
    float d = texture2D(s_ff7Depth, uv).x;
    float effect = u_ff7Haze.w > 0.5 ? clamp(scene.a / max(dot(scene.rgb, vec3(0.299, 0.587, 0.114)), 0.02), 0.0, 1.0) : 0.0;   // (alpha: the light effects added)
    if (u_ff7Contact.x > 0.0 && d < 0.9999 && d > 0.0001)
    {
        // (the stage and what stands on it: faded out from 2.25 to 4.5 times the camera's distance, so FF7's far
        // scenery keeps its own look)
        float here = nearness(uv);
        float onStage = 1.0 - smoothstep(1.5 * u_ff7Haze.x, 3.0 * u_ff7Haze.x, 1.0 / here);
        if (onStage > 0.0) shown *= mix(1.0, contactAt(uv, here), onStage * (1.0 - effect));
    }
    if (u_ff7Rim.x > 0.0 && d < 0.9999 && d > 0.0001)
    {
        // (a light edge, as a factor on the pixel's own colour and a touch of light, on the stage only)
        float here = nearness(uv);
        float onStage = 1.0 - smoothstep(1.5 * u_ff7Haze.x, 3.0 * u_ff7Haze.x, 1.0 / here);
        float rim = rimAt(uv, here) * u_ff7Rim.x * onStage * (1.0 - effect);
        shown += rim * (shown * 0.6 + vec3_splat(0.04));
    }
    if (u_ff7Spell.x > 0.0 && d < 0.9999 && d > 0.0001)
    {
        // (what stands round an effect takes its light, as a factor on its own colour; the effect's own pixels and FF7's
        // far scenery as they are)
        float here = nearness(uv);
        float onStage = 1.0 - smoothstep(1.5 * u_ff7Haze.x, 3.0 * u_ff7Haze.x, 1.0 / here);
        // (a soft curve: a small hit's light doubled, a summon's never past twice the picture's own brightness)
        shown += shown * (vec3_splat(1.0) - exp(-spellLight(uv) * u_ff7Spell.x)) * onStage * (1.0 - effect);
    }
    if (u_ff7Haze.y > 0.0 && d < 0.9999 && d > 0.0001)
    {
        // The backdrop: the farthest of 8 places near the top, and what is within 10% of it.
        float farthest = 0.0;
        for (int k = 0; k < 8; k++)
        {
            float t = texture2D(s_ff7Depth, vec2((float(k) + 0.5) / 8.0, k < 4 ? 0.06 : 0.18)).x;
            if (t < 0.9999 && t > 0.0001) farthest = max(farthest, viewDepth(t));
        }
        float backdrop = 0.9 * farthest;
        vec4 sky = vec4_splat(0.0);
        for (int k = 0; k < 8; k++)
        {
            vec2 at = vec2((float(k) + 0.5) / 8.0, k < 4 ? 0.06 : 0.18);
            float t = texture2D(s_ff7Depth, at).x;
            if (t >= 0.9999 || t <= 0.0001 || viewDepth(t) >= backdrop) sky += vec4(texture2D(s_ff7Scene, at).rgb, 1.0);
        }
        float z = viewDepth(d);
        // Outdoors: two or more of those places are the backdrop and it stands well beyond the stage.
        // (eased in as the backdrop stands farther beyond the stage: no step as FF7's cameras move)
        float outdoors = smoothstep(1.6, 2.4, backdrop / max(u_ff7Haze.x, 1.0));
        if (sky.w >= 2.0 && outdoors > 0.0 && z < backdrop)
        {
            float haze = u_ff7Haze.y * outdoors * clamp((z - u_ff7Haze.x) / (backdrop - u_ff7Haze.x), 0.0, 1.0) * (1.0 - effect);
            shown = mix(shown, sky.rgb / sky.w, haze);
        }
    }
    shown *= step(u_ff7Haze.z, uv.y) * step(uv.y, 1.0 - u_ff7Haze.z);   // (the bars: black)
    gl_FragColor = vec4(shown, scene.a);
}
