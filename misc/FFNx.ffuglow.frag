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
// clear it), so: 0 extract the bright part there; 1 blur it along one axis (9 taps); 2 the two blurs, added onto the
// picture by the caller's ONE+ONE blend, in the frame's own gamma numbers, as the games' PlayStation frame buffers added
// (Rule 110).

$input v_texcoord0

#include <bgfx/bgfx_shader.sh>

SAMPLER2D(s_ffuGlowA, 0);
SAMPLER2D(s_ffuGlowB, 1);

uniform vec4 u_ffuGlow;         // x: the pass (0 extract, 1 blur, 2 add); yz: one texel along the blur (uv)
uniform vec4 u_ffuGlowParams;   // x: threshold, y: knee, z: strength of the small blur, w: of the wide one

void main()
{
    vec2 uv = v_texcoord0;
    if (u_ffuGlow.x < 0.5)
    {
        vec4 c = texture2D(s_ffuGlowA, uv);
        float effect = clamp(c.a * 3.0, 0.0, 1.0);
        float peak = max(c.r, max(c.g, c.b));
        float keep = clamp((peak - u_ffuGlowParams.x) / u_ffuGlowParams.y, 0.0, 1.0);
        gl_FragColor = vec4(c.rgb * effect * keep * keep, 1.0);
    }
    else if (u_ffuGlow.x < 1.5)
    {
        vec2 s = u_ffuGlow.yz;
        vec3 sum = texture2D(s_ffuGlowA, uv).rgb * 0.2270;
        sum += (texture2D(s_ffuGlowA, uv + s * 1.3846).rgb + texture2D(s_ffuGlowA, uv - s * 1.3846).rgb) * 0.3162;
        sum += (texture2D(s_ffuGlowA, uv + s * 3.2308).rgb + texture2D(s_ffuGlowA, uv - s * 3.2308).rgb) * 0.0703;
        gl_FragColor = vec4(sum, 1.0);
    }
    else
    {
        vec3 glow = texture2D(s_ffuGlowA, uv).rgb * u_ffuGlowParams.z + texture2D(s_ffuGlowB, uv).rgb * u_ffuGlowParams.w;
        gl_FragColor = vec4(glow, 0.0);
    }
}
