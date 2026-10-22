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
// battle frame before FF7's menus, over the picture so far and FF7's own depth. Outdoors (FF7's backdrop, its far
// scenery, seen near the picture's top), what stands beyond the stage fades toward the backdrop's colour; the backdrop
// itself, effect pixels (the glow's alpha mask) and nothing drawn are left as they are; then the bars, black.

$input v_texcoord0

#include <bgfx/bgfx_shader.sh>

SAMPLER2D(s_ff7Scene, 0);
SAMPLER2D(s_ff7Depth, 1);

uniform vec4 u_ff7Depth;   // FF7's depth along the view axis: (z x + y) / (z z' + w), the rows FFNx drew its 3D with
uniform vec4 u_ff7Haze;    // x where the haze starts (view depth), y its most (0: none), z the bars' height (uv), w the mask

float viewDepth(float d)
{
    // (with FF7's projection both parts are negative: only a zero denominator is guarded, never its sign)
    float below = d * u_ff7Depth.z - u_ff7Depth.x;
    return (u_ff7Depth.y - d * u_ff7Depth.w) / (abs(below) < 1e-6 ? -1e-6 : below);
}

void main()
{
    vec2 uv = v_texcoord0;
    vec4 scene = texture2D(s_ff7Scene, uv);
    vec3 shown = scene.rgb;
    float d = texture2D(s_ff7Depth, uv).x;
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
            float effect = u_ff7Haze.w > 0.5 ? clamp(scene.a * 3.0, 0.0, 1.0) : 0.0;
            float haze = u_ff7Haze.y * outdoors * clamp((z - u_ff7Haze.x) / (backdrop - u_ff7Haze.x), 0.0, 1.0) * (1.0 - effect);
            shown = mix(shown, sky.rgb / sky.w, haze);
        }
    }
    shown *= step(u_ff7Haze.z, uv.y) * step(uv.y, 1.0 - u_ff7Haze.z);   // (the bars: black)
    gl_FragColor = vec4(shown, scene.a);
}
