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

// Final Fantasy Universe: the battle look's cinematics (Richard's next list, item 3; src/ffu_ff7look.cpp,
// docs/battle-look.md "Summons and Limits"): while FF7 plays a summon or a Limit, after the look's own pass and before
// FF7's menus, over the picture so far and FF7's own depth:
// - depth of field, in two passes (across, then down): each pixel blurred by how far it stands from what the camera
//   looks at, from the pixels round it whose own blur reaches it (so a sharp thing never bleeds into the blur round it);
//   the effects (the glow's mask) kept sharp;
// - motion blur: each pixel blurred along the way the camera's move carried it since the last frame (its place then,
//   from its depth and the last frame's camera), the effects less.
// The bars and the test split (FFU_LOOK_SPLIT: FF7's own left of it) are kept.

$input v_texcoord0

#include <bgfx/bgfx_shader.sh>

SAMPLER2D(s_cineScene, 0);
SAMPLER2D(s_cineDepth, 1);

uniform mat4 u_cineReproject;   // this frame's (x, y, depth, 1) on the screen to the last frame's clip (as FFNx.vert's mul)
uniform vec4 u_cineDepth;       // FF7's depth along the view axis: (z x + y) / (z z' + w), as the look's pass
uniform vec4 u_cineFocus;       // x the view depth looked at, y the blur at infinity, z its most (shares of the width), w the pass: 0 across, 1 down, 2 motion
uniform vec4 u_cineFrame;       // x the bars' height (uv), y the test split, z the effects' mask (1: in alpha), w the picture's width over its height
uniform vec4 u_cineShake;       // the impact's shake as the look's pass drew it: xy offset, z magnification (depth read where its picture came from)
uniform vec4 u_cineMotion;      // x the motion's share of a frame's move, y its most (uv), zw one texel (uv)

float viewDepth(float d)
{
    float below = d * u_cineDepth.z - u_cineDepth.x;
    return (u_cineDepth.y - d * u_cineDepth.w) / (abs(below) < 1e-6 ? -1e-6 : below);
}

// Where the picture at uv came from (the impact's shake), so its depth is read there.
vec2 source(vec2 uv)
{
    return (uv - 0.5) / u_cineShake.z + 0.5 + u_cineShake.xy;
}

float effectAt(vec4 scene)
{
    return u_cineFrame.z > 0.5 ? clamp(scene.a / max(dot(scene.rgb, vec3(0.299, 0.587, 0.114)), 0.02), 0.0, 1.0) : 0.0;
}

// This pixel's blur (a share of the picture's width): thin-lens, |z - focus| / z, at most u_cineFocus.z.
float blurAt(vec2 uv, vec4 scene)
{
    float d = texture2D(s_cineDepth, source(uv)).x;
    float z = d < 0.9999 && d > 0.0001 ? max(viewDepth(d), 1.0) : 1e7;   // (nothing drawn: far)
    return min(u_cineFocus.y * abs(z - u_cineFocus.x) / z, u_cineFocus.z) * (1.0 - effectAt(scene));
}

vec2 inPicture(vec2 at)
{
    return vec2(clamp(at.x, 0.0, 1.0), clamp(at.y, u_cineFrame.x + 0.001, 1.0 - u_cineFrame.x - 0.001));
}

vec3 depthOfField(vec2 uv, vec4 centre)
{
    float blur = blurAt(uv, centre);
    if (blur * (1.0 / u_cineMotion.z) < 0.5) return centre.rgb;   // (under half a pixel)
    vec2 along = u_cineFocus.w < 0.5 ? vec2(1.0, 0.0) : vec2(0.0, u_cineFrame.w);   // (the same reach in pixels down)
    vec3 sum = centre.rgb;
    float weights = 1.0;
    for (int k = 1; k <= 6; k++)
    {
        float reach = blur * float(k) / 6.0;
        float shape = exp(-2.0 * float(k * k) / 36.0);   // (a soft disc's edge)
        for (int side = 0; side < 2; side++)
        {
            vec2 at = inPicture(uv + along * (side == 0 ? reach : -reach));
            vec4 there = texture2D(s_cineScene, at);
            // (only what is blurred at least this far reaches here: a sharp thing beside a blurred one stays its own)
            float w = shape * clamp((blurAt(at, there) - reach) / (2.0 * u_cineMotion.z) + 1.0, 0.0, 1.0);
            sum += there.rgb * w;
            weights += w;
        }
    }
    return sum / weights;
}

vec3 motionBlur(vec2 uv, vec4 centre)
{
    vec2 from = source(uv);
    float d = texture2D(s_cineDepth, from).x;
    d = d < 0.9999 && d > 0.0001 ? d : 0.9999;   // (nothing drawn: as far as the camera sees)
    vec4 before = mul(u_cineReproject, vec4(2.0 * from.x - 1.0, 1.0 - 2.0 * from.y, d, 1.0));
    if (before.w <= 0.0) return centre.rgb;   // (behind the camera then)
    vec2 then = vec2(before.x / before.w * 0.5 + 0.5, 0.5 - before.y / before.w * 0.5);
    vec2 move = (from - then) * u_cineMotion.x * (1.0 - 0.7 * effectAt(centre));
    float length_ = length(move * vec2(1.0, 1.0 / u_cineFrame.w));   // (in widths)
    if (length_ > u_cineMotion.y) move *= u_cineMotion.y / length_;
    vec3 sum = vec3_splat(0.0);
    for (int k = 0; k < 10; k++)
        sum += texture2D(s_cineScene, inPicture(uv + move * ((float(k) + 0.5) / 10.0 - 0.5))).rgb;
    return sum / 10.0;
}

void main()
{
    vec2 uv = v_texcoord0;
    vec4 centre = texture2D(s_cineScene, uv);
    if (uv.x < u_cineFrame.y || uv.y < u_cineFrame.x || uv.y > 1.0 - u_cineFrame.x)
    {
        gl_FragColor = centre;   // (FF7's own picture left of the split; the bars)
        return;
    }
    gl_FragColor = vec4(u_cineFocus.w < 1.5 ? depthOfField(uv, centre) : motionBlur(uv, centre), centre.a);
}
