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

// FF7's world map (Final Fantasy Universe), advanced lighting off: FFNx.frag plus the globe (FFNx.ffu7.sh).

$input v_color0, v_texcoord0, v_position0, v_normal0, v_ffu, v_ffucap

#include <bgfx/bgfx_shader.sh>
#include "FFNx.common.sh"
#include "FFNx.ffu7.sh"

SAMPLER2D(tex_0, 0);
SAMPLER2D(tex_1, 1);
SAMPLER2D(tex_2, 2);

uniform mat4 invViewMatrix;
uniform mat4 d3dProjection;   // set globally by FFNx; declared here for the space sky
uniform mat4 d3dViewport;

uniform vec4 VSFlags;
uniform vec4 FSAlphaFlags;
uniform vec4 FSMiscFlags;
uniform vec4 FSHDRFlags;
uniform vec4 FSTexFlags;
uniform vec4 WMFlags;
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

void main()
{
    // World-map 3D pixels far from the camera get logarithmic depth (FFNx.ffu7.sh WorldDepth);
    // everything else keeps the game's depth.
    gl_FragDepth = v_ffu.y > 0.5 ? WorldDepth(v_ffu.x, gl_FragCoord.z, v_ffu.z, v_ffu.w) : gl_FragCoord.z;
    vec2 capCell = CapSeaCell(v_ffucap.xyz).xy;   // outside any branch: its derivatives are needed
    // v_color0 is used for solid 2D colors (e.g., textbox backgrounds), solid-color polygon faces (e.g., all original FF7 models), and colorizing textures
    vec4 color = v_color0;
    color.rgb = toLinear(color.rgb);

    if (isTexture)
    {
        // Handling for YUV movies was moved to separate shaders

        // This stanza pertains to 2D textures (aside from YUV movies) and textures on 3D objects if advanced lighting is disabled
        vec4 texture_color = texture2D(tex_0, v_texcoord0.xy);
        // Polar caps: the open sea bent over the poles takes its texture from its true position.
        if (v_ffucap.w > 0.5) texture_color = texture2DGrad(tex_0, CapSeaUv(capCell), CapSeaGradient(dFdx(capCell)), CapSeaGradient(dFdy(capCell)));

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

#ifdef FFU_SKYUV
    if (isTLVertex && v_position0.w < 0.0) color = vec4(v_position0.x, v_position0.y, 0.0, 1.0);
#endif
    // Final Fantasy Universe: the world map's sky becomes space (FFNx.vert marks it in v_position0.w).
    if (isTLVertex && v_position0.w > 0.0)
    {
#ifdef FFU_SPACE_DEBUG
        color = vec4(1.0, 0.0, 0.0, 1.0);   // test: where does the space sky land?
#else
        // This pixel's view ray, from its screen position and the world camera's projection.
        vec2 ndc = (gl_FragCoord.xy - u_viewRect.xy) / u_viewRect.zw * 2.0 - 1.0;
        ndc.y = -ndc.y;   // gl_FragCoord y grows downwards (Vulkan)
        // Unproject through the exact matrix the planet is drawn with (viewport x projection, which
        // carries FFNx's widescreen scaling), at two depths, to get this pixel's view ray.
        mat4 unproject = FfuInverse(mul(d3dViewport, d3dProjection));
        vec3 lensCamera = mul(invViewMatrix, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
        vec4 lens = SpaceLens(d3dViewport, d3dProjection, lensCamera);   // the space lens (docs/space.md)
        vec2 pixelNdc = ndc;
        ndc = SpaceLensToGame(pixelNdc, lens);
        vec4 nearPoint = mul(unproject, vec4(ndc, 0.0, 1.0));
        vec4 farPoint = mul(unproject, vec4(ndc, 1.0, 1.0));
        // The ray runs from the depth-0 point to the depth-1 point. (It ran the other way until
        // 2026-10-16: the sky was computed for the opposite direction, found when the shared stars
        // didn't line up with the bridge's solar camera; a sky coloured by solar octant showed every
        // ray inverted. See docs/globe.md "Stars".)
        vec3 viewRay = farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w;
        vec3 worldRay = mul(invViewMatrix, vec4(viewRay, 0.0)).xyz;
        vec3 cameraPos = mul(invViewMatrix, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
        // The angle one pixel spans here (the ray one pixel to the right), for the stars' size.
        vec2 nextNdc = SpaceLensToGame(pixelNdc + vec2(2.0 / u_viewRect.z, 0.0), lens);
        vec4 nextNear = mul(unproject, vec4(nextNdc, 0.0, 1.0));
        vec4 nextFar = mul(unproject, vec4(nextNdc, 1.0, 1.0));
        vec3 nextRay = nextFar.xyz / nextFar.w - nextNear.xyz / nextNear.w;
        float pixelAngle = max(length(normalize(nextRay) - normalize(viewRay)), 1e-7);
        color.rgb = SkyFromSpace(worldRay, cameraPos, pixelAngle, u_viewRect.z);
        color.a = 1.0;
#endif
    }

    // Final Fantasy Universe: on the cloud plane (FFNx.ffu7.sh "Other planets"): FF8 and FF9 with their
    // clouds, and (draws of slot 0) FF7's own cloud shell where it shows beyond FF7's limb.
    if (!(isTLVertex) && v_ffucap.w > 1.5)
    {
        vec3 ray = normalize(v_ffucap.xyz);
        float slot = v_ffucap.w - 2.0;
        vec3 cameraPos = mul(invViewMatrix, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
        vec3 east, up, south;
        vec2 latLonQ;
        GlobeFrame(cameraPos, east, up, south, latLonQ);
        float drift = FFU_CLOUD_DRIFT;
        // FF7's shell beyond its limb: rays that miss FF7's sea but cross the shell.
        vec3 toFF7 = FFU_FF7_CENTRE - SolarCameraPosition(cameraPos);
        float sea = SeaLevelR(cameraPos);
        float hitsFF7 = RayToSphere(ray, toFF7, sea);
        float tShell = RayToSphere(ray, toFF7, FFU_CLOUD_SHELL);
        vec2 shellCoords = CloudCoords(normalize(ray * max(tShell, 0.0) - toFF7), drift);
        // The other planet (slot 0 = FF8, 1 = FF9).
        float t = RayToPlanet(ray, v_normal0);
        vec2 planetCoords = CloudCoords(normalize(ray * max(t, 0.0) - v_normal0), drift);
        vec2 mapCoords = vec2(planetCoords.x + drift / (2.0 * FFU_PI), planetCoords.y);   // the map doesn't drift
        vec2 sdx = SeamlessGradient(dFdx(shellCoords)), sdy = SeamlessGradient(dFdy(shellCoords));
        vec2 pdx = SeamlessGradient(dFdx(planetCoords)), pdy = SeamlessGradient(dFdy(planetCoords));
        // Atlas (tools/ff7planet skyatlas): thirds of the height: FF8 map, FF9 map, clouds (r FF7, g FF8, b FF9).
        vec2 third = vec2(1.0, 1.0 / 3.0);
        vec3 shellClouds = texture2DGrad(tex_0, vec2(fract(shellCoords.x), (2.0 + clamp(shellCoords.y, 0.002, 0.998)) / 3.0), sdx * third, sdy * third).rgb;
        vec3 planetClouds = texture2DGrad(tex_0, vec2(fract(planetCoords.x), (2.0 + clamp(planetCoords.y, 0.002, 0.998)) / 3.0), pdx * third, pdy * third).rgb;
        vec3 planetMap = toGamma(texture2DGrad(tex_0, vec2(fract(mapCoords.x), (slot + clamp(mapCoords.y, 0.002, 0.998)) / 3.0), pdx * third, pdy * third).rgb);
        float depthT;
        vec3 light;
        if (slot < 0.5 && hitsFF7 < 0.0 && tShell > 0.0 && CloudFade(cameraPos) > 0.0)
        {
            // Blended over the sky behind the limb: the sky there is SkyFromSpace's air glow (computed
            // again here), so adding density x (cloud - sky) gives exactly mix(sky, cloud, density) with
            // FFNx's additive blending (where the glow is brighter than the cloud, nothing is added).
            float density = shellClouds.r * CloudFade(cameraPos);
            vec3 sky = SkyAir(vec3(dot(ray, east), dot(ray, up), dot(ray, south)), cameraPos);
            light = max(FFU_CLOUD_COLOUR - sky, 0.0) * density;
            depthT = tShell;
        }
        else
        {
            if (hitsFF7 > 0.0 || t <= 0.0) discard;   // FF7's own planet (its terrain draws its clouds) or nothing
            float density = slot < 0.5 ? planetClouds.g : planetClouds.b;
            // Seen through FF7's air like the stars: the blue transmittance along the ray.
            float fade = exp(-AirToSpace(cameraPos, dot(ray, up)) * FFU_SCATTER_R.b * 3.0);
            if (fade < 0.002) discard;
            light = mix(planetMap, FFU_CLOUD_COLOUR, density) * fade;
            depthT = t;
        }
        color = vec4(light * v_ffu.x, 1.0);
        // Solar distances are in radii of the planet as drawn (R_now game units each).
        gl_FragDepth = WorldDepth(depthT * GlobeRadiusNow(latLonQ.x, cameraPos.y), gl_FragCoord.z, 0.0, v_ffu.w);
    }

    gl_FragColor = color;
}
