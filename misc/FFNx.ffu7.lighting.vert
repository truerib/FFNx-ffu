/****************************************************************************/
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

// This shader is never used for 2D elements.
// This shader is used for 3D elements when advanced lighting is enabled.

// FF7's world map (Final Fantasy Universe), advanced lighting on: FFNx.lighting.vert plus the globe (FFNx.ffu7.sh).

$input a_position, a_color0, a_texcoord0, a_normal, a_indices, a_weight
$output v_color0, v_texcoord0, v_position0, v_shadow0, v_normal0, v_ffu, v_ffucap

#include <bgfx/bgfx_shader.sh>
#include "FFNx.common.sh"
#include "FFNx.ffu7.sh"

uniform mat4 d3dViewport;
uniform mat4 d3dProjection;
uniform mat4 viewMatrix;
uniform mat4 worldView;
uniform mat4 normalMatrix;
uniform mat4 lightViewProjTexMatrix;
uniform mat4 invViewMatrix;

uniform vec4 VSFlags;
uniform vec4 WMFlags;
uniform vec4 lightingDebugData;
uniform vec4 gameLightingFlags;
uniform vec4 gameGlobalLightColor;
uniform vec4 gameLightColor1;
uniform vec4 gameLightColor2;
uniform vec4 gameLightColor3;
uniform vec4 gameLightDir1;
uniform vec4 gameLightDir2;
uniform vec4 gameLightDir3;
uniform vec4 gameScriptedLightColor;

uniform mat4 boneMatrices[MAX_BONE_MATRICES];
uniform vec4 skinningFlags;

#define isTLVertex VSFlags.x > 0.0
#define blendMode VSFlags.y
#define isFBTexture VSFlags.z > 0.0
#define isNotTexture VSFlags.w == 0.0

//#define isSmoothSkinning skinningFlags.x > 0.0

#define isApplySphericalWorld WMFlags.x > 0.0
#define sphericaWorldRadiusScale WMFlags.x

#define isHide2dEnabled lightingDebugData.x > 0.0

#define gameLightingMode gameLightingFlags.x
#define GAME_LIGHTING_PER_VERTEX 1

void main()
{
    v_ffu = vec4_splat(0.0);
    v_ffucap = vec4_splat(0.0);
    vec4 pos = a_position;
    vec3 nrm = a_normal;
    vec4 color = a_color0;
    vec2 coords = a_texcoord0;

    color.rgb = toLinear(color.bgr);

    if (isTLVertex)
    {
        pos.w = 1.0 / pos.w;
        pos.xyz *= pos.w;
        pos = mul(u_proj, pos);

        if (isHide2dEnabled) pos = vec4_splat(0.0);
    }
    else
    {
        // if (isSmoothSkinning)
        // {
        //     int boneIndex = a_indices.x;
        //     vec3 avgPos = vec3(0.0, 0.0, 0.0);
        //     avgPos += a_weight.x * mul(boneMatrices[a_indices.x], vec4(pos.xyz, 1.0)).xyz;
        //     avgPos += a_weight.y * mul(boneMatrices[a_indices.y], vec4(pos.xyz, 1.0)).xyz;
        //     avgPos += a_weight.z * mul(boneMatrices[a_indices.z], vec4(pos.xyz, 1.0)).xyz;
        //     avgPos += a_weight.w * mul(boneMatrices[a_indices.w], vec4(pos.xyz, 1.0)).xyz;

        //     pos = vec4(avgPos, 1.0);

        //     vec3 avgNrm = vec3(0.0, 0.0, 0.0);
        //     avgNrm += a_weight.x * mul(boneMatrices[a_indices.x], vec4(nrm.xyz, 0.0)).xyz;
        //     avgNrm += a_weight.y * mul(boneMatrices[a_indices.y], vec4(nrm.xyz, 0.0)).xyz;
        //     avgNrm += a_weight.z * mul(boneMatrices[a_indices.z], vec4(nrm.xyz, 0.0)).xyz;
        //     avgNrm += a_weight.w * mul(boneMatrices[a_indices.w], vec4(nrm.xyz, 0.0)).xyz;

        //     nrm = avgNrm;
        // }

        v_position0 = mul(worldView, vec4(pos.xyz, 1.0));
        vec4 shadowSource = v_position0;   // flat position for the shadow-map lookup
        float planetSlot = -1.0;           // (only FFNx.vert draws other planets)

        if (isApplySphericalWorld)
        {
            vec3 worldPos = mul(invViewMatrix, vec4(v_position0.xyz, 1.0)).xyz;
            float polarSide = 0.0;
            bool isTerrain = IsTerrainCopy(worldView, invViewMatrix);
            if (isTerrain)
            {
                // Every copy of a terrain vertex gets the same position, view position and lighting.
                worldPos = vec3(pos.x, pos.y + FFU_TERRAIN_Y, pos.z);
                polarSide = PolarBandSide(pos.xyz, coords);
                if (polarSide != 0.0)
                {
                    // Open sea bent over a pole (the polar caps). Lighting and shadows see it where
                    // the flat map would have it in true Mercator; the fragment shader takes its
                    // texture from its true place on the planet.
                    vec2 latLon = PolarBandLatLon(pos.xyz, polarSide);
                    worldPos.z = MercatorZ(latLon.x);
                    v_ffucap = vec4(PlanetPoint(latLon, worldPos.y, mul(invViewMatrix, vec4(0.0, 0.0, 0.0, 1.0)).xyz), 1.0);
                    pos.xyz = ApplyGlobeAt(latLon, worldPos.y, viewMatrix, invViewMatrix);
                }
                vec3 cameraPos = mul(invViewMatrix, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
                v_position0 = mul(viewMatrix, vec4(TerrainLightingPosition(worldPos, cameraPos), 1.0));
                if (polarSide == 0.0)
                {
                    pos.xyz = ApplyGlobeTerrain(worldPos, viewMatrix, invViewMatrix);
                    // Its true place too (flag 0): a triangle reaching from the band's inner edge into
                    // the map must interpolate real positions, or its texture is sampled from the wrong
                    // place (a row of dark triangles along the band's edge); the clouds need it for the
                    // shell's parallax (its length is the point's radius in R).
                    v_ffucap = vec4(PlanetPoint(GlobeLatLon(worldPos.xz), worldPos.y, mul(invViewMatrix, vec4(0.0, 0.0, 0.0, 1.0)).xyz), 0.0);
                }
                shadowSource = v_position0;
                v_position0.xyz = TerrainShadingPosition(v_position0.xyz, pos.xyz, invViewMatrix);
            }
            else pos.xyz = ApplyGlobeWorld(worldPos, viewMatrix, invViewMatrix);
            // FFNx's cloud backdrop (5 billboards at height 20000, the only draw with sphere rate 1.5)
            // belongs to the sky, not the planet: it fades out as the globe takes over, then goes away.
            if (sphericaWorldRadiusScale > 1.25)
            {
                float cloudFade = saturate(GlobeAmount(invViewMatrix) * 8.0);
                color.rgb *= 1.0 - cloudFade;   // drawn with additive blending
                if (cloudFade >= 1.0) pos.xyz = vec3(0.0, 0.0, -1.0e7);
            }
        }
        else pos = v_position0;

        v_shadow0 = mul(lightViewProjTexMatrix, shadowSource);
        v_normal0 = mul(normalMatrix, vec4(nrm, 0.0)).xyz;
        pos = mul(mul(d3dViewport, d3dProjection), vec4(pos.xyz, 1.0));
        if (isApplySphericalWorld || planetSlot >= 0.0)
            pos = ApplySpaceLens(pos, SpaceLens(d3dViewport, d3dProjection, mul(invViewMatrix, vec4(0.0, 0.0, 0.0, 1.0)).xyz));
#ifndef FFU_NO_LOGDEPTH
        {
            // Every 3D draw: the fragment shader writes the depth (WorldDepth). Pass the view distance,
            // the game's exact depth at the near limit, and which end is far (falling = reversed).
            float atLimit = GameDepthAt(d3dViewport, d3dProjection, FFU_DEPTH_NEAR_LIMIT);
            float further = GameDepthAt(d3dViewport, d3dProjection, 2.0 * FFU_DEPTH_NEAR_LIMIT);
            v_ffu = vec4(pos.w, 1.0, atLimit, further < atLimit ? 0.0 : 1.0);
        }
#endif
#ifdef FFU_HIDE_RATES
        if (isApplySphericalWorld && abs(sphericaWorldRadiusScale - 1.0) > 0.01) pos = vec4_splat(0.0);   // test: clouds, meteor
#endif
#ifdef FFU_HIDE_FLAT
        if (!(isApplySphericalWorld)) pos = vec4_splat(0.0);   // test: hide every unflagged 3D draw
#endif

        if (gameLightingMode == GAME_LIGHTING_PER_VERTEX)
        {
            vec3 worldNormal = mul(invViewMatrix, vec4(v_normal0, 0)).xyz;
            float dotLight1 = saturate(dot(worldNormal, gameLightDir1.xyz));
            float dotLight2 = saturate(dot(worldNormal, gameLightDir2.xyz));
            float dotLight3 = saturate(dot(worldNormal, gameLightDir3.xyz));

            vec3 light1Ambient = toLinear(gameLightColor1.rgb) * dotLight1 * dotLight1;
            vec3 light2Ambient = toLinear(gameLightColor2.rgb) * dotLight2 * dotLight2;
            vec3 light3Ambient = toLinear(gameLightColor3.rgb) * dotLight3 * dotLight3;
            vec3 lightAmbient = toLinear(gameScriptedLightColor.rgb) * (toLinear(gameGlobalLightColor.rgb) + light1Ambient + light2Ambient + light3Ambient);

            color.rgb *= gameGlobalLightColor.w * lightAmbient;
        }

        if (color.a > 0.5) color.a = 0.5;
    }

    if (blendMode == 4.0) color.a = 1.0;
    else if (blendMode == 3.0) color.a = 0.25;

#if BGFX_SHADER_LANGUAGE_HLSL
#else
    #if BGFX_SHADER_LANGUAGE_SPIRV
    #else
        if (isFBTexture) coords.y = 1.0 - coords.y;
    #endif
#endif

#ifdef FFU_GLOBE_DEBUG
    // Red = share of globe applied; blue = camera below y = 0 (only shows where the fragment shader uses vertex colour).
    if (!(isTLVertex) && isApplySphericalWorld)
    {
        vec3 debugCamera = mul(invViewMatrix, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
        color.rgb = mix(color.rgb, vec3(1.0, 0.0, 0.0), 0.7 * GlobeAmount(invViewMatrix));
        if (debugCamera.y < 0.0) color.b = 1.0;
    }
#endif
    gl_Position = pos;
    v_color0 = color;
    v_texcoord0 = coords;
}
