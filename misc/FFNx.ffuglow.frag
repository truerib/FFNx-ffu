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

// Final Fantasy Universe: the battle look's glow (src/ffu_glow.cpp, docs/battle-look.md "Glow"): only the game's own
// effects glow. In a battle FFNx's frame keeps alpha only where additive draws (the effects) added it (the other draws
// clear it), so: 0 extract the bright part of what the effects added there (the light they add is the frame's alpha,
// FFNx.frag); 1 blur it along one axis (9 taps); 2 the two blurs, added onto the
// picture by the caller's ONE+ONE blend, in the frame's own gamma numbers, as the games' PlayStation frame buffers added
// (Rule 110). 3: the battle look's anti-aliasing, FXAA (the console form) on the whole frame, alpha kept.

$input v_texcoord0

#include <bgfx/bgfx_shader.sh>

SAMPLER2D(s_ffuGlowA, 0);
SAMPLER2D(s_ffuGlowB, 1);

uniform vec4 u_ffuGlow;         // x: the pass (0 extract, 1 blur, 2 add, 3 FXAA); yz: one texel along the blur (uv)
uniform vec4 u_ffuGlowParams;   // x: threshold, y: knee, z: strength of the small blur, w: of the wide one

vec3 fxaa(vec2 uv, vec2 texel)
{
    vec3 nw = texture2D(s_ffuGlowA, uv + vec2(-1.0, -1.0) * texel).rgb;
    vec3 ne = texture2D(s_ffuGlowA, uv + vec2(1.0, -1.0) * texel).rgb;
    vec3 sw = texture2D(s_ffuGlowA, uv + vec2(-1.0, 1.0) * texel).rgb;
    vec3 se = texture2D(s_ffuGlowA, uv + vec2(1.0, 1.0) * texel).rgb;
    vec3 m = texture2D(s_ffuGlowA, uv).rgb;
    vec3 toLuma = vec3(0.299, 0.587, 0.114);
    float lnw = dot(nw, toLuma), lne = dot(ne, toLuma), lsw = dot(sw, toLuma), lse = dot(se, toLuma), lm = dot(m, toLuma);
    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
    float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    float reduce = max((lnw + lne + lsw + lse) * 0.03125, 0.0078125);
    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, -8.0, 8.0) * texel;
    vec3 a = 0.5 * (texture2D(s_ffuGlowA, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture2D(s_ffuGlowA, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture2D(s_ffuGlowA, uv - dir * 0.5).rgb + texture2D(s_ffuGlowA, uv + dir * 0.5).rgb);
    float lb = dot(b, toLuma);
    return (lb < lmin || lb > lmax) ? a : b;
}

void main()
{
    vec2 uv = v_texcoord0;
    if (u_ffuGlow.x < 0.5)
    {
        // What the effects added: their light is the frame's alpha (FFNx.frag), that share of the picture, bright enough
        vec4 c = texture2D(s_ffuGlowA, uv);
        float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
        vec3 added = c.rgb * clamp(c.a / max(lum, 0.02), 0.0, 1.0);
        float peak = max(added.r, max(added.g, added.b));
        float keep = clamp((peak - u_ffuGlowParams.x) / u_ffuGlowParams.y, 0.0, 1.0);
        gl_FragColor = vec4(added * keep * keep, 1.0);
    }
    else if (u_ffuGlow.x < 1.5)
    {
        vec2 s = u_ffuGlow.yz;
        vec3 sum = texture2D(s_ffuGlowA, uv).rgb * 0.2270;
        sum += (texture2D(s_ffuGlowA, uv + s * 1.3846).rgb + texture2D(s_ffuGlowA, uv - s * 1.3846).rgb) * 0.3162;
        sum += (texture2D(s_ffuGlowA, uv + s * 3.2308).rgb + texture2D(s_ffuGlowA, uv - s * 3.2308).rgb) * 0.0703;
        gl_FragColor = vec4(sum, 1.0);
    }
    else if (u_ffuGlow.x < 2.5)
    {
        vec3 glow = texture2D(s_ffuGlowA, uv).rgb * u_ffuGlowParams.z + texture2D(s_ffuGlowB, uv).rgb * u_ffuGlowParams.w;
        if (uv.x < u_ffuGlow.w) glow = vec3_splat(0.0);   // (the test split, FFU_LOOK_SPLIT: the game's own on the left)
        gl_FragColor = vec4(glow, 0.0);
    }
    else
    {
        vec4 own = texture2D(s_ffuGlowA, uv);
        gl_FragColor = uv.x < u_ffuGlow.w ? own : vec4(fxaa(uv, u_ffuGlow.yz), own.a);   // (the test split)
    }
}
