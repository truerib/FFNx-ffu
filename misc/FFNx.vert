/****************************************************************************/
//    Copyright (C) 2009 Aali132                                            //
//    Copyright (C) 2018 quantumpencil                                      //
//    Copyright (C) 2018 Maxime Bacoux                                      //
//    Copyright (C) 2020 myst6re                                            //
//    Copyright (C) 2020 Chris Rizzitello                                   //
//    Copyright (C) 2020 John Pritchard                                     //
//    Copyright (C) 2026 Julian Xhokaxhiu                                   //
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

$input a_position, a_color0, a_texcoord0, a_normal, a_indices, a_weight
$output v_color0, v_texcoord0, v_position0, v_normal0

#include <bgfx/bgfx_shader.sh>
#include "FFNx.common.sh"

uniform mat4 d3dViewport;
uniform mat4 d3dProjection;
uniform mat4 worldView;
uniform mat4 normalMatrix;
uniform mat4 invViewMatrix;
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

uniform vec4 VSFlags;
uniform vec4 WMFlags;
// Final Fantasy Universe: FF8's whole world map as a 3D mesh (src/ff8/world/renderer.cpp)
//  [0] anchor x, anchor z (camera-relative), camera height C.y, virtual altitude
//  [1] virtual scale, planet radius, mode (0 flat, 1 globe, 2 FF8's own bend, 4 other planet,
//      5 airship: worldView places the model in the camera-relative frame, 6 atmosphere shell,
//      7 cloud shell, 8 another planet's cloud shell), polar-cap draw (1)
//  [2] globe: east axis | bend: depth row R3 (xyz) + curve start (w)
//  [3] globe: up axis   | flat/bend: camera position in mesh space (xyz)
//  [4] globe: north axis
//  [5] other planet: its centre minus FF8's (solar frame, planet radii, docs/space.md), game units per radius
//      world mesh / cloud shell: x = cloud fade, w = shell height (game units)
uniform vec4 ff8Globe[6];

#define isTLVertex VSFlags.x > 0.0
#define blendMode VSFlags.y
#define isFBTexture VSFlags.z > 0.0

//#define isSmoothSkinning skinningFlags.x > 0.0

#define isApplySphericalWorld WMFlags.x > 0.0
#define sphericaWorldRadiusScale  WMFlags.x

#define gameLightingMode gameLightingFlags.x
#define isFF8WorldMesh (WMFlags.z > 0.0)

#define FF8_MAP vec2(262144.0, 196608.0)
#define FF8_PLANET_RADIUS 41721.5

// A mesh vertex (FF8's G space: x = map X, -z = map Z, +y down) -> unit vector on the planet,
// true Mercator (conformal): map X is longitude, map Z the Mercator latitude, north positive.
vec3 FF8PlanetUnit(vec3 g)
{
    // x wraps by itself (sin/cos are periodic); z must not wrap: the south edge (G.z = -196608) would
    // jump to the north pole.
    vec2 m = vec2(g.x, clamp(-g.z, 0.0, FF8_MAP.y));
    float lon = m.x / FF8_PLANET_RADIUS;
    float lat = 2.0 * atan(exp((0.5 * FF8_MAP.y - m.y) / FF8_PLANET_RADIUS)) - 1.5707963;
    return vec3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));
}

// The vertex's place in the planet textures' equirectangular layout (cloud shadows, docs/space.md "Clouds").
vec2 FF8PlanetUV(vec3 unitP)
{
    float lon = atan2(unitP.x, unitP.z);
    if (lon < 0.0) lon += 6.2831853;
    return vec2(lon / 6.2831853, 0.5 - asin(clamp(unitP.y, -1.0, 1.0)) / 3.1415927);
}

// Same as mods/ff8-autoboot/globe_ff8.c globe_point() + to_virtual(): the camera-relative point FF8
// would project (camera at the origin, +y down), so the mesh and FF8's own models agree exactly.
vec3 FF8GlobePoint(vec3 unitP, float height)
{
    float e = dot(unitP, ff8Globe[2].xyz);
    float u = dot(unitP, ff8Globe[3].xyz);
    float n = dot(unitP, ff8Globe[4].xyz);
    float sideways = sqrt(e * e + n * n);
    float theta = atan2(sideways, u);
    float radius = ff8Globe[1].y;
    float r = radius + height;
    float along = sideways > 1e-9 ? r * sin(theta) / sideways : 0.0;
    vec3 p = vec3(ff8Globe[0].x + e * along, ff8Globe[0].z + radius - r * cos(theta), ff8Globe[0].y + n * along);
    return vec3(p.x, p.y + ff8Globe[0].w, p.z) * ff8Globe[1].x;
}

// Another game's planet (docs/space.md): a point of a unit sphere around that planet's solar centre, in
// the same camera-relative, climbed and scaled frame as FF8's own globe. Planet axes = solar axes.
vec3 FF8OtherPlanetPoint(vec3 unitP)
{
    vec3 d = ff8Globe[5].xyz + unitP;
    vec3 centre = vec3(ff8Globe[0].x, ff8Globe[0].z + ff8Globe[1].y, ff8Globe[0].y);
    vec3 q = centre + ff8Globe[5].w * vec3(dot(d, ff8Globe[2].xyz), -dot(d, ff8Globe[3].xyz), dot(d, ff8Globe[4].xyz));
    return vec3(q.x, q.y + ff8Globe[0].w, q.z) * ff8Globe[1].x;
}

// The mesh-space position relative to FF8's camera, on the nearest copy of the wrapping map.
vec3 FF8CameraRelative(vec3 g)
{
    vec3 p = g - ff8Globe[3].xyz;
    p.x -= floor(p.x / FF8_MAP.x + 0.5) * FF8_MAP.x;
    p.z -= floor(p.z / FF8_MAP.y + 0.5) * FF8_MAP.y;
    return p;
}

// FF8's own horizon bend (0x4022D0), for checking the projection against FF8's terrain.
vec3 FF8Bend(vec3 p)
{
    float depth = clamp(floor(dot(ff8Globe[2].xyz, p) + 0.5), 0.0, 65535.0);
    float d = floor(depth / 4.0) - ff8Globe[2].w;
    if (d >= 0.0)
    {
        float relative = p.y - ff8Globe[0].z;
        p.y = floor(relative * (4096.0 - d) / 4096.0) + ff8Globe[0].z + floor(d / 64.0) * floor(d / 64.0);
    }
    return p;
}

// FF8's TL depth (0x461210): linear in view z, z = floor(round(zv) / 8) * 1.5235901e-5 + 0.0015,
// continued past FF8's 0x8000 limit by a log band, so the far side of the planet still sorts.
float FF8Depth(float zv)
{
    float d = floor(floor(zv + 0.5) * 0.125);
    if (d < 4095.0) return d * 1.5235901e-05 + 0.0015;
    float z4095 = 4095.0 * 1.5235901e-05 + 0.0015;
    return mix(z4095, 0.9999, saturate(log2(zv / 32760.0) / log2(4.0e6 / 32760.0)));
}
#define GAME_LIGHTING_PER_VERTEX 1

void main()
{
    vec4 pos = a_position;
    vec4 color = a_color0;
    vec2 coords = a_texcoord0;

    color.rgb = color.bgr;

    if (isTLVertex)
    {
        pos.w = 1.0 / pos.w;
        pos.xyz *= pos.w;
        pos = mul(u_proj, pos);
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
        // }
        
        v_position0 = mul(worldView, vec4(pos.xyz, 1.0));

        vec2 FF8MeshCloudUV = vec2_splat(0.0);
        if (isFF8WorldMesh)
        {
            vec3 p;
            if (ff8Globe[1].z > 8.5) p = FF8OtherPlanetPoint(normalize(pos.xyz) * 1.3);    // their atmosphere's shell
            else if (ff8Globe[1].z > 7.5) p = FF8OtherPlanetPoint(normalize(pos.xyz) * 1.02);   // their clouds, 0.02 R up
            else if (ff8Globe[1].z > 6.5) p = FF8GlobePoint(normalize(pos.xyz), ff8Globe[5].w);   // our cloud shell
            else if (ff8Globe[1].z > 5.5) p = FF8GlobePoint(normalize(pos.xyz), ff8Globe[5].w);   // atmosphere shell
            else if (ff8Globe[1].z > 4.5) p = v_position0.xyz * ff8Globe[1].x;   // airship: climbs with the camera
            else if (ff8Globe[1].z > 3.5) p = FF8OtherPlanetPoint(normalize(pos.xyz));
            else if (ff8Globe[1].w > 0.5) p = FF8GlobePoint(normalize(pos.xyz), 0.0);   // polar cap: unit vectors
            else if (ff8Globe[1].z > 1.5) p = FF8Bend(FF8CameraRelative(pos.xyz));
            else if (ff8Globe[1].z > 0.5)
            {
                vec3 unitP = FF8PlanetUnit(pos.xyz);
                p = FF8GlobePoint(unitP, -pos.y);
                FF8MeshCloudUV = FF8PlanetUV(unitP);
            }
            else p = FF8CameraRelative(pos.xyz);
            v_position0 = vec4(p, 1.0);
            pos = mul(mul(d3dViewport, d3dProjection), vec4(p, 1.0));   // FF8's rotation and lens; w = view z
            // Behind the camera: z = w, so a triangle crossing the camera plane is clipped where w >= 0.
            pos.z = pos.w > 0.0 ? FF8Depth(pos.w) * pos.w : pos.w;
            if (ff8Globe[1].z > 5.5 && ff8Globe[1].z < 6.5 && pos.w > 0.0) pos.z = 0.99997 * pos.w;   // atmosphere: in front of the sky only
        }
        else
        {
            if (isApplySphericalWorld) pos.xyz = ApplySphericalWorld(v_position0.xyz, sphericaWorldRadiusScale);
            else pos = v_position0;

            pos = mul(mul(d3dViewport, d3dProjection), vec4(pos.xyz, 1.0));
        }
        v_normal0 = mul(normalMatrix, vec4(a_normal, 0.0)).xyz;
        if (isFF8WorldMesh) v_normal0 = vec3(FF8MeshCloudUV, 0.0);   // FF8 has no lighting: carries the cloud map UV

        // In this default shader, lighting is applied in gamma space so that it does better match the original lighting
        if (gameLightingMode == GAME_LIGHTING_PER_VERTEX)
        {
            vec3 worldNormal = mul(invViewMatrix, vec4(v_normal0, 0)).xyz;
            float dotLight1 = saturate(dot(worldNormal, gameLightDir1.xyz));
            float dotLight2 = saturate(dot(worldNormal, gameLightDir2.xyz));
            float dotLight3 = saturate(dot(worldNormal, gameLightDir3.xyz));
            vec3 light1Ambient = gameLightColor1.rgb * dotLight1 * dotLight1;
            vec3 light2Ambient = gameLightColor2.rgb * dotLight2 * dotLight2;
            vec3 light3Ambient = gameLightColor3.rgb * dotLight3 * dotLight3;
            vec3 lightAmbient = gameScriptedLightColor.rgb * (gameGlobalLightColor.rgb + light1Ambient + light2Ambient + light3Ambient);
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

    gl_Position = pos;
    v_color0 = color;
    v_texcoord0 = coords;
}

