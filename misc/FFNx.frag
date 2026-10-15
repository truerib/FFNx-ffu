/****************************************************************************/
//    Copyright (C) 2009 Aali132                                            //
//    Copyright (C) 2018 quantumpencil                                      //
//    Copyright (C) 2018 Maxime Bacoux                                      //
//    Copyright (C) 2020 myst6re                                            //
//    Copyright (C) 2020 Chris Rizzitello                                   //
//    Copyright (C) 2020 John Pritchard                                     //
//    Copyright (C) 2026 Julian Xhokaxhiu                                   //
//    Copyright (C) 2023 Cosmos                                             //
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

// This shader is always used for 2D elements.
// This shader is used for 3D elements when advanced lighting is disabled.

$input v_color0, v_texcoord0, v_position0, v_normal0

#include <bgfx/bgfx_shader.sh>
#include "FFNx.common.sh"

SAMPLER2D(tex_0, 0);
SAMPLER2D(tex_1, 1);
SAMPLER2D(tex_2, 2);

uniform mat4 invViewMatrix;

uniform vec4 VSFlags;
uniform vec4 FSAlphaFlags;
uniform vec4 FSMiscFlags;
uniform vec4 FSHDRFlags;
uniform vec4 FSTexFlags;
uniform vec4 WMFlags;
uniform vec4 ff8Globe[6];   // Final Fantasy Universe: FF8 world-mesh globe state (FFNx.vert)
uniform vec4 TimeColor;
uniform vec4 TimeData;
uniform vec4 gameLightingFlags;
uniform vec4 gameGlobalLightColor;
uniform vec4 gameLightColor1;
uniform vec4 gameLightColor2;
uniform vec4 gameLightColor3;
uniform vec4 gameLightDir1;
uniform vec4 gameLightDir2;
uniform vec4 gameLightDir3;
uniform vec4 gameScriptedLightColor;

#define isTLVertex VSFlags.x > 0.0
#define isFBTexture VSFlags.z > 0.0
#define isNotFBTexture VSFlags.z < 0.00001
#define isTexture VSFlags.w > 0.0
// ---
#define inAlphaRef FSAlphaFlags.x

#define isAlphaNever abs(FSAlphaFlags.y - 0.0) < 0.00001
#define isAlphaLess abs(FSAlphaFlags.y - 1.0) < 0.00001
#define isAlphaEqual abs(FSAlphaFlags.y - 2.0) < 0.00001
#define isAlphaLEqual abs(FSAlphaFlags.y - 3.0) < 0.00001
#define isAlphaGreater abs(FSAlphaFlags.y - 4.0) < 0.00001
#define isAlphaNotEqual abs(FSAlphaFlags.y - 5.0) < 0.00001
#define isAlphaGEqual abs(FSAlphaFlags.y - 6.0) < 0.00001

#define doAlphaTest FSAlphaFlags.z > 0.0


// ---
#define modulateAlpha FSMiscFlags.z > 0.0

#define isHDR FSHDRFlags.x > 0.0
#define monitorNits FSHDRFlags.y

#define isTimeEnabled TimeData.x > 0.0
#define isTimeFilterEnabled TimeData.x > 0.0 && TimeData.y > 0.0

#define gameLightingMode gameLightingFlags.x
#define GAME_LIGHTING_PER_PIXEL 2

#define isFogEnabled WMFlags.y > 0.0

// Final Fantasy Universe: the shared atmosphere (docs/space.md "## Atmosphere"), seen from FF8's camera.
// ray: view direction in FF8's camera-relative frame; the planet's centre and the camera's height come from
// the globe state (FFNx.vert FF8GlobePoint's frame, climbed and scaled the same way).
vec3 FF8Atmosphere(vec3 ray)
{
    float scale = ff8Globe[1].x;
    float radius = ff8Globe[1].y * scale;
    vec3 centre = vec3(ff8Globe[0].x, ff8Globe[0].z + ff8Globe[1].y + ff8Globe[0].w, ff8Globe[0].y) * scale;
    float d = length(centre) / radius;
    float h = max(d - 1.0, 0.0);
    vec3 up = -normalize(centre);
    float cosChi = dot(ray, up);
    float H = mix(0.127830, 0.025566, smoothstep(0.63916, 3.19579, h));
    float x = (1.0 + h) / H;
    float c = sqrt(1.5707963 * x);
    float above = exp(-h / H);
    float tau;
    if (cosChi >= 0.0) tau = H * above * c / ((c - 1.0) * cosChi + 1.0);
    else
    {
        float sinChi = sqrt(saturate(1.0 - cosChi * cosChi));
        tau = H * max(above * c / ((c - 1.0) * cosChi - 1.0)
            + 2.0 * c * sqrt(sinChi) * exp(min((1.0 - (1.0 + h) * sinChi) / H, 80.0)), 0.0);
    }
    tau = min(tau, 2.13053);
    vec3 beta = vec3(14.0810, 32.8557, 70.4051);
    return (vec3_splat(1.0) - exp(-beta * tau)) * vec3(0.62, 0.80, 1.00);
}

void main()
{
    if (WMFlags.z > 2.5)   // FF8 cloud shells (docs/space.md "Clouds"): the map turned east by the drift
    {
        float density = texture2D(tex_0, vec2(fract(v_texcoord0.x - WMFlags.w), v_texcoord0.y)).a;
        float fade = ff8Globe[1].z > 7.5 ? 1.0 : ff8Globe[5].x;
        gl_FragColor = vec4(0.96, 0.97, 1.0, density * fade);
        return;
    }
    if (WMFlags.z > 1.5)   // FF8 atmosphere shell: display colour, added over FF8's (black) space sky
    {
        gl_FragColor = vec4(FF8Atmosphere(normalize(v_position0.xyz)) * ff8Globe[5].x, 1.0);
        return;
    }

    // v_color0 is used for solid 2D colors (e.g., textbox backgrounds), solid-color polygon faces (e.g., all original FF7 models), and colorizing textures
    vec4 color = v_color0;
    color.rgb = toLinear(color.rgb);

    if (isTexture)
    {
        // Handling for YUV movies was moved to separate shaders

        // This stanza pertains to 2D textures (aside from YUV movies) and textures on 3D objects if advanced lighting is disabled
        vec4 texture_color = texture2D(tex_0, v_texcoord0.xy);

        if (doAlphaTest)
        {
            //NEVER
            if (isAlphaNever) discard;

            //LESS
            if (isAlphaLess)
            {
                if (!(texture_color.a < inAlphaRef)) discard;
            }

            //EQUAL
            if (isAlphaEqual)
            {
                if (!(texture_color.a == inAlphaRef)) discard;
            }

            //LEQUAL
            if (isAlphaLEqual)
            {
                if (!(texture_color.a <= inAlphaRef)) discard;
            }

            //GREATER
            if (isAlphaGreater)
            {
                if (!(texture_color.a > inAlphaRef)) discard;
            }

            //NOTEQUAL
            if (isAlphaNotEqual)
            {
                if (!(texture_color.a != inAlphaRef)) discard;
            }

            //GEQUAL
            if (isAlphaGEqual)
            {
                if (!(texture_color.a >= inAlphaRef)) discard;
            }
        }

        // check for some discard conditions
        if (isNotFBTexture && texture_color.a == 0.0) discard;
        if (isFBTexture)
        {
            if(all(equal(texture_color.rgb,vec3_splat(0.0)))) discard;

            // This was previously in gamma space, so linearize again.
            texture_color.rgb = toLinear(texture_color.rgb);
        }

        // multiply by v_color0
        if (modulateAlpha) color *= texture_color;
        else
        {
            color.rgb *= texture_color.rgb;
            color.a = texture_color.a;
        }

    } //end if (isTexture)

    if (isTimeFilterEnabled) color.rgb *= TimeColor.rgb;

    if (!(isTLVertex) && isFogEnabled) color.rgb = ApplyWorldFog(color.rgb, v_position0.xyz);

    // return to sRGB gamma space so we can do alpha blending the same way FF7/8 did.
    color.rgb = toGamma(color.rgb);

    // In this default shader, lighting is applied in gamma space so that it does better match the original lighting
    if (gameLightingMode == GAME_LIGHTING_PER_PIXEL)
    {
        vec3 normal = normalize(v_normal0);
        vec3 worldNormal = mul(invViewMatrix, vec4(normal, 0)).xyz;
        float dotLight1 = saturate(dot(worldNormal, gameLightDir1.xyz));
        float dotLight2 = saturate(dot(worldNormal, gameLightDir2.xyz));
        float dotLight3 = saturate(dot(worldNormal, gameLightDir3.xyz));
        vec3 light1Ambient = gameLightColor1.rgb * dotLight1 * dotLight1;
        vec3 light2Ambient = gameLightColor2.rgb * dotLight2 * dotLight2;
        vec3 light3Ambient = gameLightColor3.rgb * dotLight3 * dotLight3;
        vec3 lightAmbient = gameScriptedLightColor.rgb * (gameGlobalLightColor.rgb + light1Ambient + light2Ambient + light3Ambient);
        color.rgb *= gameGlobalLightColor.w * lightAmbient;
    }
    
    // FF8's world mesh: cloud shadows (docs/space.md "Clouds"); v_normal0.xy is the vertex's map UV.
    if (WMFlags.z > 0.5 && WMFlags.z < 1.5 && ff8Globe[5].x > 0.0)
    {
        float shadow = texture2D(tex_1, vec2(fract(v_normal0.x - WMFlags.w), v_normal0.y)).a;
        color.rgb *= 1.0 - 0.25 * shadow * ff8Globe[5].x;
    }

    gl_FragColor = color;
}
