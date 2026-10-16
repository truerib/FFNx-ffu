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

// FF7's world map (Final Fantasy Universe), advanced lighting on: FFNx.lighting.frag plus the globe (FFNx.ffu7.sh).

$input v_color0, v_texcoord0, v_position0, v_shadow0, v_normal0, v_ffu, v_ffucap

#include <bgfx/bgfx_shader.sh>
#include "FFNx.lighting.sh"
#include "FFNx.ffu7.sh"

// TEX_YUV
SAMPLER2D(tex_0, 0); // Y
SAMPLER2D(tex_1, 1); // U
SAMPLER2D(tex_2, 2); // V
// TEX_NML
SAMPLER2D(tex_5, 5);
// TEX_PBR
SAMPLER2D(tex_6, 6);

uniform vec4 VSFlags;
uniform vec4 FSAlphaFlags;
uniform vec4 FSMiscFlags;
uniform vec4 FSHDRFlags;
uniform vec4 FSTexFlags;
uniform vec4 WMFlags;

uniform vec4 lightingSettings;
uniform vec4 lightingDebugData;
uniform vec4 materialData;
uniform vec4 materialScaleData;
uniform vec4 iblData;
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

// ---
#define debugOutput lightingDebugData.z
#define DEBUG_OUTPUT_DISABLED 0
#define DEBUG_OUTPUT_COLOR 1
#define DEBUG_OUTPUT_NORMAL 2
#define DEBUG_OUTPUT_ROUGHNESS 3
#define DEBUG_OUTPUT_METALLIC 4
#define DEBUG_OUTPUT_AO 5
#define DEBUG_OUTPUT_SPECULAR 6
#define DEBUG_OUTPUT_IBL_SPECULAR 7
#define DEBUG_OUTPUT_IBL_DIFFUSE 8

#define isPbrTextureEnabled lightingSettings.x > 0.0
#define isEnvironmentLightingEnabled lightingSettings.y > 0.0

#define isFogEnabled WMFlags.y > 0.0

#define isNmlTextureLoaded FSTexFlags.x > 0.0
#define isPbrTextureLoaded FSTexFlags.y > 0.0
#define isIblTextureLoaded FSTexFlags.z > 0.0

#define gameLightingMode gameLightingFlags.x
#define GAME_LIGHTING_PER_PIXEL 2

void main()
{
    // v_color0 is used for solid-color polygon faces (e.g., all original FF7 models) and colorizing textures
    // (Not used for solid-color 2D elements b/c this shader isn't used for 2D elements.)
    // World-map 3D pixels far from the camera get logarithmic depth (FFNx.ffu7.sh WorldDepth);
    // everything else keeps the game's depth.
    gl_FragDepth = v_ffu.y > 0.5 ? WorldDepth(v_ffu.x, gl_FragCoord.z, v_ffu.z, v_ffu.w) : gl_FragCoord.z;
    vec4 color = v_color0; //previously linearized in vertex shader
    vec2 capCell = CapSeaCell(v_ffucap.xyz).xy;   // outside any branch: its derivatives are needed
    vec4 color_nml = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 color_pbr = vec4(0.0, 0.0, 0.0, 0.0);

    if (isTexture)
    {
        // if (isYUV) stanza removed because unreachable

        // This stanza pertains to textures on 3D objects if advanced lighting is enabled
        vec4 texture_color = texture2D(tex_0, v_texcoord0.xy);
        // Polar caps: the open sea bent over the poles takes its texture from its true position.
        if (v_ffucap.w > 0.5) texture_color = texture2DGrad(tex_0, CapSeaUv(capCell), CapSeaGradient(dFdx(capCell)), CapSeaGradient(dFdy(capCell)));

        if (isNmlTextureLoaded) color_nml = texture2D(tex_5, v_texcoord0.xy);
        if (isPbrTextureLoaded) color_pbr = texture2D(tex_6, v_texcoord0.xy);

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


    }

    vec3 normal = vec3(0.0, 0.0, 0.0);
    if(isTLVertex)
    {
        gl_FragColor = color;
        if(isTimeFilterEnabled)
        {
            gl_FragColor.rgb *= TimeColor.rgb;
        }
    }
    else
    {
        // Shadow UV
        vec3 shadowUv = v_shadow0.xyz / v_shadow0.w;

        // View Direction
        vec3 viewDir = normalize(-v_position0.xyz);

        // Normal
        normal = normalize(v_normal0);
        if(isNmlTextureLoaded && isPbrTextureEnabled) normal = perturb_normal(normal, v_position0.xyz, color_nml.rgb, v_texcoord0.xy );

        // Roughness
        float perceptualRoughness = materialData.x;
        if(isPbrTextureLoaded && isPbrTextureEnabled) perceptualRoughness = color_pbr.r * materialScaleData.x;
        float roughness = min(max(0.001, perceptualRoughness * perceptualRoughness), 1.0);

        // Metallic
        float metallic = materialData.y;
        if(isPbrTextureLoaded && isPbrTextureEnabled) metallic = color_pbr.g * materialScaleData.y;
        metallic = min(1.0, metallic);

        // Specular (dielectric)
        float specular = materialData.z;
        if(isPbrTextureLoaded && isPbrTextureEnabled) specular = color_pbr.b * materialScaleData.z;
        specular = min(1.0, specular);

        // Ambient Occlusion
        float ao = 1.0;
        if(isPbrTextureLoaded && isPbrTextureEnabled) ao = color_pbr.a;

        // Luminance
        vec3 luminance = calcLuminance(color.rgb, v_position0.xyz, viewDir, normal, perceptualRoughness, roughness, metallic, specular, shadowUv);

        // Indirect Luminance
        vec3 indirectLuminance = vec3_splat(0.0);
        vec3 specularIbl = vec3_splat(0.0);
        vec3 diffuseIbl = vec3_splat(0.0);
        if(isIblTextureLoaded && isEnvironmentLightingEnabled)
        {
            // Specular IBL
            vec3 R = mul(invViewMatrix, vec4(reflect(-viewDir, normal), 0)).xyz;
            float iblMipCount = iblData.x;
            float iblLod = CalcMipmapFromRoughness(roughness, iblMipCount);
            specularIbl = textureCubeLod(tex_7, R, iblLod).rgb;

            // Diffuse IBL
            vec3 worldNormal = mul(invViewMatrix, vec4(normal, 0)).xyz;
            diffuseIbl = textureCube(tex_8, worldNormal).rgb;

            indirectLuminance = CalcIblIndirectLuminance(color.rgb, specularIbl, diffuseIbl, viewDir, normal, roughness, metallic, specular, ao);
        }
        else
        {
            indirectLuminance = CalcConstIndirectLuminance(color.rgb);
        }

        if(debugOutput == DEBUG_OUTPUT_COLOR)
        {
            gl_FragColor = color;
        }
        else if(debugOutput == DEBUG_OUTPUT_NORMAL)
        {
            gl_FragColor = vec4(0.5 * normal + 0.5, 1.0);
        }
        else if(debugOutput == DEBUG_OUTPUT_ROUGHNESS)
        {
            gl_FragColor = vec4(vec3_splat(perceptualRoughness), 1.0);
        }
        else if(debugOutput == DEBUG_OUTPUT_METALLIC)
        {
            gl_FragColor = vec4(vec3_splat(metallic), 1.0);
        }
        else if(debugOutput == DEBUG_OUTPUT_AO)
        {
            gl_FragColor = vec4(vec3_splat(ao), 1.0);
        }
        else if(debugOutput == DEBUG_OUTPUT_SPECULAR)
        {
            gl_FragColor = vec4(vec3_splat(specular), 1.0);
        }
        else if(debugOutput == DEBUG_OUTPUT_IBL_SPECULAR)
        {
            gl_FragColor = vec4(specularIbl, 1.0);
        }
        else if(debugOutput == DEBUG_OUTPUT_IBL_DIFFUSE)
        {
            gl_FragColor = vec4(diffuseIbl, 1.0);
        }
        else
        {
            gl_FragColor = vec4(luminance + indirectLuminance, color.a);
        }

        if (isFogEnabled && debugOutput == DEBUG_OUTPUT_DISABLED ) gl_FragColor.rgb = ApplyWorldFog(gl_FragColor.rgb, v_position0.xyz);
    }

    if(!(isTLVertex) && gameLightingMode == GAME_LIGHTING_PER_PIXEL && debugOutput == DEBUG_OUTPUT_DISABLED)
    {
        vec3 worldNormal = mul(invViewMatrix, vec4(normal, 0)).xyz;
        float dotLight1 = saturate(dot(worldNormal, gameLightDir1.xyz));
        float dotLight2 = saturate(dot(worldNormal, gameLightDir2.xyz));
        float dotLight3 = saturate(dot(worldNormal, gameLightDir3.xyz));


        vec3 light1Ambient = toLinear(gameLightColor1.rgb) * dotLight1 * dotLight1;
        vec3 light2Ambient = toLinear(gameLightColor2.rgb) * dotLight2 * dotLight2;
        vec3 light3Ambient = toLinear(gameLightColor3.rgb) * dotLight3 * dotLight3;
        vec3 lightAmbient = toLinear(gameScriptedLightColor.rgb) * (toLinear(gameGlobalLightColor.rgb) + light1Ambient + light2Ambient + light3Ambient);

        gl_FragColor.rgb *= gameGlobalLightColor.w * lightAmbient;
    }

    // return to sRGB gamma space so we can do alpha blending the same way FF7/8 did.
    gl_FragColor.rgb = toGamma(gl_FragColor.rgb);

    // Final Fantasy Universe: clouds over the terrain, and their shadows (docs/space.md "Clouds",
    // FFNx.ffu7.sh). Terrain fragments carry their own position in the planet's axes (v_ffucap.xyz, R,
    // w < 1.5). The cloud is read where the view ray crosses the shell (radius 1.02 R) on its way to
    // the fragment, so it sits on the shell with true parallax; a fragment above the shell (a mountain
    // top) or with the shell behind it stays clear. The shadow is read straight above the fragment
    // (no sun). FF7's cloud map is FFNx's environment-BRDF texture (tex_9), unused otherwise here.
    // Computed before the branch, so the gradients are well defined.
    vec3 cloudCamera = mul(invViewMatrix, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    float cloudFade = CloudFade(cloudCamera);
    vec3 here = v_ffucap.xyz + vec3(1e-6, 0.0, 0.0);
    vec3 eye = CameraInPlanet(cloudCamera);
    vec3 towards = normalize(here - eye);
    float tCloud = RayToSphere(towards, -eye, FFU_CLOUD_SHELL);
    vec2 shellCoords = CloudCoords(normalize(eye + towards * max(tCloud, 0.0)), FFU_CLOUD_DRIFT);
    vec2 groundCoords = CloudCoords(normalize(here), FFU_CLOUD_DRIFT);
    vec2 sdx = SeamlessGradient(dFdx(shellCoords)), sdy = SeamlessGradient(dFdy(shellCoords));
    vec2 gdx = SeamlessGradient(dFdx(groundCoords)), gdy = SeamlessGradient(dFdy(groundCoords));
    float cloud = texture2DGrad(tex_9, vec2(fract(shellCoords.x), clamp(shellCoords.y, 0.0, 1.0)), sdx, sdy).r;
    float shadow = texture2DGrad(tex_9, vec2(fract(groundCoords.x), clamp(groundCoords.y, 0.0, 1.0)), gdx, gdy).r;
    if (!(isTLVertex) && cloudFade > 0.0 && length(v_ffucap.xyz) > 0.5 && v_ffucap.w < 1.5)
    {
        gl_FragColor.rgb *= 1.0 - FFU_CLOUD_SHADOW * shadow * cloudFade;
        // the shell lies between the camera and this point only if the point is under it
        bool underShell = dot(here, here) < FFU_CLOUD_SHELL * FFU_CLOUD_SHELL && tCloud > 0.0 && tCloud < length(here - eye);
        // Rays that pass beyond the sea's limb (mountains standing up past it) get their cloud from the
        // limb pass on the cloud plane (FFNx.frag), which uses the same test: never both.
        underShell = underShell && RayToSphere(towards, -eye, SeaLevelR(cloudCamera)) > 0.0;
        if (underShell) gl_FragColor.rgb = mix(gl_FragColor.rgb, FFU_CLOUD_COLOUR, cloud * cloudFade);
    }
}
