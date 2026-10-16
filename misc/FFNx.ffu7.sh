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

// Final Fantasy Universe: FF7's world map as a planet (the project's docs/globe.md and docs/space.md).
// Used only by the FFNx.ffu7* shaders, which FFNx draws FF7's world map with instead of FFNx.* and
// FFNx.lighting.* (Vulkan only; src/ff7/world/ffu.cpp). Every other game mode, and FF8, keeps the stock shaders.

// [0] x = 1 on FF7's world map, y = cloud drift (radians, one turn east per 6 h of real time),
//     z = 1 while an airship flying with the camera is drawn, w = 1 while the airship is flown (space lens)
uniform vec4 ff7Globe[2];
#define FFU_CLOUD_DRIFT ff7Globe[0].y

// Final Fantasy Universe: the globe -------------------------------
// The world is a planet at every altitude (Richard, 2026-10-15), following the shared solar-system spec
// (docs/space.md). Every world point maps to ONE place on a sphere fixed to the world: map x ->
// longitude (one turn = the map's width), map z -> TRUE Mercator latitude (conformal: the ground keeps
// FF7's exact shapes; the map spans +-80.07 degrees). Beyond the map's north and south edges are the
// polar caps: the open sea of the map's last 8 rows (8,192 units, all Gaia deep sea at sea level) is
// bent over the pole (PolarBandSide), and drawn with the sea texture laid out in true Mercator per
// pixel (CapSeaUv), so the caps are the game's own open sea, lit by the game's own lighting.
// Cosmos Gaia draws the map up to 9 times (a 3 x 3 grid, so the flat map wraps). Terrain vertices are
// placed from their MODEL-space position, which is the same in every copy, so all copies come out
// bit-identical (position, depth and lighting) and can't fight each other; FFNx's own culling still
// decides which copies are drawn (turning it off cost 20x: 1 FPS).
// The sphere is turned so the point under the camera is on top, with the camera's own east and south
// along x and z, and shifted so that point sits at the camera's ground position: around the camera
// nothing moves, and the camera itself is never transformed. So the world axes x, y, z at the camera
// are the planet's east, up and south there (GlobeFrame), which is how the sky and the solar camera
// (mods/bridge-ff7 log_solar_camera) turn world directions into solar ones.
#define FFU_WORLD_SIZE vec2(294912.0, 229376.0)   // 36 x 28 blocks of 8192 (Cosmos Gaia's tile period)
#define FFU_GLOBE_RADIUS 46936.7026                // map width / 2 pi: 1 R (docs/space.md)
#define FFU_EDGE_LATITUDE 1.3975118                // true-Mercator latitude of the map's edges (80.0715 deg)
#define FFU_CAP_BAND 8192.0                        // open sea within this of an edge is bent over the pole
#define FFU_CAP_BAND_LATITUDE 1.3646832            // latitude of the band's inner edge (78.1906 deg)
#define FFU_SEA_CELL 1024.0                        // Gaia's deep-sea grid
#define FFU_PI 3.14159265
#define FFU_TERRAIN_Y -500.0                       // Gaia's terrain grid sits at y = -500 (FFNx renderer.cpp)
// The near-ground enlargement 1/cos(latitude) fades out between these camera altitudes: docs/space.md's
// 20,000 -> 120,000 FF8 units, in FF7 units (x 46936.70 / 41721.5, the ratio of the radii).
#define FFU_NEAR_GROUND_FROM 22500.0
#define FFU_NEAR_GROUND_TO 135000.0
#define FFU_NEAR_GROUND_MIN_COS 0.17242  // cos(FFU_EDGE_LATITUDE): exact size over the whole map, finite over the caps

float GlobeAmount(mat4 invViewM)
{
	return 1.0;   // always a globe
}

float MercatorLatitude(float mercator)
{
	return 2.0 * atan(exp(mercator)) - 0.5 * FFU_PI;
}

// Map z (from the north edge) of a latitude; the inverse of the mapping in GlobeLatLon.
float MercatorZ(float latitude)
{
	float clamped = clamp(latitude, -1.5621, 1.5621);   // 89.5 degrees: keeps the poles finite
	return 0.5 * FFU_WORLD_SIZE.y - FFU_GLOBE_RADIUS * log(tan(0.25 * FFU_PI + 0.5 * clamped));
}

vec2 GlobeLatLon(vec2 xz)
{
	float lon = xz.x / FFU_GLOBE_RADIUS;
	float mercator = (0.5 * FFU_WORLD_SIZE.y - xz.y) / FFU_GLOBE_RADIUS;   // north positive; FF7's z grows south
	return vec2(MercatorLatitude(mercator), lon);
}

vec3 GlobeUnit(vec2 latLon)
{
	return vec3(cos(latLon.x) * sin(latLon.y), sin(latLon.x), cos(latLon.x) * cos(latLon.y));
}

vec2 WrapOntoMap(vec2 xz)
{
	return xz - floor(xz / FFU_WORLD_SIZE) * FFU_WORLD_SIZE;
}

// The point under the camera. Longitude wraps; latitude does NOT: a camera beyond the map's north or
// south edge (it trails the player, so it can be) hangs over the polar cap there. Wrapping z put it at
// the other edge, which turned the whole planet over to the other pole (the airship vanished).
vec2 CameraLatLon(vec3 cameraPos)
{
	return GlobeLatLon(vec2(WrapOntoMap(cameraPos.xz).x, cameraPos.z));
}

// A world point's copy nearest the camera (the flat map repeats every FFU_WORLD_SIZE).
vec3 NearestCopy(vec3 p, vec3 cameraPos)
{
	vec2 offset = floor((p.xz - cameraPos.xz) / FFU_WORLD_SIZE + 0.5) * FFU_WORLD_SIZE;
	return vec3(p.x - offset.x, p.y, p.z - offset.y);
}

// Terrain's flat position for lighting and shadows (FFNx lights the flat world): its model position,
// moved by whole map widths to the copy nearest the camera in x only, so the lighting is continuous
// across the map's x = 0 seam (it showed as a line along that meridian) and matches FFNx's flat
// lighting near the camera. z stays the model's, in step with the latitude.
vec3 TerrainLightingPosition(vec3 p, vec3 cameraPos)
{
	return vec3(p.x - floor((p.x - cameraPos.x) / FFU_WORLD_SIZE.x + 0.5) * FFU_WORLD_SIZE.x, p.y, p.z);
}

// Gaia's tiled terrain draws have a model origin of (gx * width, -500, gz * height).
bool IsTerrainCopy(mat4 worldViewM, mat4 invViewM)
{
	vec3 origin = mul(invViewM, mul(worldViewM, vec4(0.0, 0.0, 0.0, 1.0))).xyz;
	vec2 grid = floor(origin.xz / FFU_WORLD_SIZE + 0.5);
	vec2 offGrid = abs(origin.xz - grid * FFU_WORLD_SIZE);
	return abs(origin.y - FFU_TERRAIN_Y) < 2.0 && offGrid.x < 4.0 && offGrid.y < 4.0;
}

// The planet's radius this frame: Mercator shrinks the map by cos(latitude), so near the ground that
// scale is undone at the camera's latitude (the planet is drawn larger and the ground around the
// camera is at FF7's own scale); it fades out on the way up, so in space the planet is exactly 1 R.
float GlobeRadiusNow(float cameraLatitude, float cameraHeight)
{
	float nearGround = 1.0 / max(cos(cameraLatitude), FFU_NEAR_GROUND_MIN_COS);
	return FFU_GLOBE_RADIUS * mix(nearGround, 1.0, smoothstep(FFU_NEAR_GROUND_FROM, FFU_NEAR_GROUND_TO, cameraHeight));
}

// The planet's east, up and south at the point under the camera, in planet (= solar) axes: they are
// the world's x, y and z axes there.
void GlobeFrame(vec3 cameraPos, out vec3 east, out vec3 up, out vec3 south, out vec2 latLonQ)
{
	latLonQ = CameraLatLon(cameraPos);
	up = GlobeUnit(latLonQ);
	east = vec3(cos(latLonQ.y), 0.0, -sin(latLonQ.y));
	south = vec3(sin(latLonQ.x) * sin(latLonQ.y), -cos(latLonQ.x), sin(latLonQ.x) * cos(latLonQ.y));
}

// A world direction (at the camera) in solar axes: +Y north, +Z longitude 0.
vec3 WorldToSolar(vec3 v, vec3 cameraPos)
{
	vec3 east, up, south;
	vec2 latLonQ;
	GlobeFrame(cameraPos, east, up, south, latLonQ);
	return east * v.x + up * v.y + south * v.z;
}

// Polar caps: the open sea within FFU_CAP_BAND of the map's north (+1) or south (-1) edge, or 0.
// The band holds only Gaia's deep sea at sea level (checked over every Gaia file: the first other
// triangle is 8,517 units in from the north edge and 8,947 from the south), whose texture coordinates
// are the only ones there with v < -7. The texture test keeps the snowfield map (wm3, which also
// starts at z = 0, with v >= 1) out.
float PolarBandSide(vec3 modelPos, vec2 texcoord)
{
	if (abs(modelPos.y) > 0.01 || texcoord.y > -7.0) return 0.0;
	if (modelPos.z <= FFU_CAP_BAND) return 1.0;
	if (modelPos.z >= FFU_WORLD_SIZE.y - FFU_CAP_BAND) return -1.0;
	return 0.0;
}

// The band's rows are spread evenly from the band's inner edge (its true latitude, so the seam is
// exact) to the pole: the map's edge row lands on the pole and the cap is closed.
vec2 PolarBandLatLon(vec3 modelPos, float side)
{
	float inward = side > 0.0 ? modelPos.z : FFU_WORLD_SIZE.y - modelPos.z;
	float latitude = mix(0.5 * FFU_PI, FFU_CAP_BAND_LATITUDE, inward / FFU_CAP_BAND);
	return vec2(side * latitude, modelPos.x / FFU_GLOBE_RADIUS);
}

// Places a point at (lat, lon) and `height` above the sphere, returns its view-space position.
vec3 ApplyGlobeAt(vec2 latLonP, float height, mat4 viewM, mat4 invViewM)
{
	vec3 cameraPos = mul(invViewM, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	vec3 east, up, south;
	vec2 latLonQ;
	GlobeFrame(cameraPos, east, up, south, latLonQ);
	vec3 unitP = GlobeUnit(latLonP);
	vec3 n = vec3(dot(unitP, east), dot(unitP, up), dot(unitP, south));
	float radius = GlobeRadiusNow(latLonQ.x, cameraPos.y);

	// atan2 rather than acos: exact for points close to the camera.
	float sideways = length(n.xz);
	float theta = atan2(sideways, n.y);
	vec2 direction = sideways > 1e-9 ? n.xz / sideways : vec2(0.0, 0.0);
	// Things high above the terrain (the Highwind) keep their true sideways offset from the camera's
	// axis instead of being pushed out by (R + h) / R.
	theta *= mix(1.0, radius / (radius + max(height, 0.0)), smoothstep(5000.0, 15000.0, height));
	vec3 globeWorld = vec3(cameraPos.x, -radius, cameraPos.z)
		+ (radius + height) * vec3(sin(theta) * direction.x, cos(theta), sin(theta) * direction.y);
	return mul(viewM, vec4(globeWorld, 1.0)).xyz;
}

// Models and effects: their world position, taken at the copy nearest the camera (so something just
// across the map's edge from the camera stays next to it).
vec3 ApplyGlobeWorld(vec3 worldPos, mat4 viewM, mat4 invViewM)
{
	vec3 cameraPos = mul(invViewM, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	return ApplyGlobeAt(GlobeLatLon(NearestCopy(worldPos, cameraPos).xz), worldPos.y, viewM, invViewM);
}

// Where terrain is SHADED from (view space): FFNx lights and shadows the flat world, so near the camera
// terrain keeps its flat position (FFNx's exact lighting and shadows around the player); further out it
// moves to its true place on the planet, which has no seam (the flat map has one at the camera's
// antimeridian, where the specular light jumped: a line from the pole to the horizon). The switch
// happens well before that seam, whose nearest point is the pole.
vec3 TerrainShadingPosition(vec3 flatView, vec3 globeView, mat4 invViewM)
{
	vec3 cameraPos = mul(invViewM, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	vec2 latLonQ = CameraLatLon(cameraPos);
	float radius = GlobeRadiusNow(latLonQ.x, cameraPos.y);
	float seam = radius * max(0.5 * FFU_PI - abs(latLonQ.x), 0.0);
	float far = max(min(40000.0, 0.9 * seam), 1.0);
	return mix(flatView, globeView, smoothstep(0.5 * far, far, length(globeView)));
}

// Terrain: its model position (identical in every copy, and always inside the map).
vec3 ApplyGlobeTerrain(vec3 modelWorldPos, mat4 viewM, mat4 invViewM)
{
	return ApplyGlobeAt(GlobeLatLon(modelWorldPos.xz), modelWorldPos.y, viewM, invViewM);
}

// The deep sea's texture coordinates for a point of a polar band, from its true position (planet
// unit vector), so the cap shows the sea exactly as the map would continue it in true Mercator.
// Gaia's deep sea is a grid of 1,024-unit cells, each showing the texture's [0, 0.9375] square, u
// running west from 0.9375 and v from -7.0625 down to -8 (sampled with mirror wrap); measured on
// wm0.gltf. Explicit gradients from the unwrapped cell coordinates keep the mip level smooth across
// cell borders (the per-cell restart would pick the finest mip there).
vec4 CapSeaCell(vec3 planetUnit)
{
	vec3 n = normalize(planetUnit);
	float latitude = asin(clamp(n.y, -1.0, 1.0));
	float longitude = atan2(n.x, n.z);
	return vec4(longitude * FFU_GLOBE_RADIUS, MercatorZ(latitude), 0.0, 0.0) / FFU_SEA_CELL;
}

vec2 CapSeaUv(vec2 cell)
{
	vec2 inCell = fract(cell);
	return vec2(0.9375 * (1.0 - inCell.x), -7.0625 - 0.9375 * inCell.y);
}

vec2 CapSeaGradient(vec2 gradient)
{
	float turn = FFU_WORLD_SIZE.x / FFU_SEA_CELL;   // longitude wraps at +-pi: take the short way round
	gradient.x -= turn * floor(gradient.x / turn + 0.5);
	return -0.9375 * gradient;
}

// The shared solar system (docs/space.md): FF7's planet is centred at (0, 0, 20) R, the other two
// planets (slot 0 = FF8, slot 1 = FF9) at their solar centres, each 1 R in radius.
#define FFU_FF7_CENTRE vec3(0.0, 0.0, 20.0)

vec3 OtherPlanetCentre(float slot)
{
	return slot < 0.5 ? vec3(17.3205, 0.0, -10.0) : vec3(-17.3205, 0.0, -10.0);
}

// The camera's position in the solar frame (R), as mods/bridge-ff7/solar_ff7.c computes it.
vec3 SolarCameraPosition(vec3 cameraPos)
{
	vec3 east, up, south;
	vec2 latLonQ;
	GlobeFrame(cameraPos, east, up, south, latLonQ);
	return FFU_FF7_CENTRE + up * (1.0 + cameraPos.y / GlobeRadiusNow(latLonQ.x, cameraPos.y));
}

// Distance along a unit solar ray to a unit sphere whose centre is `toCentre` from the camera, or -1.
// The camera's height above the planet as drawn, in R (d - 1).
float CameraHeightR(vec3 cameraPos)
{
	vec2 latLonQ = CameraLatLon(cameraPos);
	return max(cameraPos.y, 0.0) / GlobeRadiusNow(latLonQ.x, cameraPos.y);
}

float RayToSphere(vec3 ray, vec3 toCentre, float radius)
{
	float b = dot(ray, toCentre);
	float h = b * b - (dot(toCentre, toCentre) - radius * radius);
	return (h < 0.0 || b <= 0.0) ? -1.0 : b - sqrt(h);
}

float RayToPlanet(vec3 ray, vec3 toCentre)
{
	return RayToSphere(ray, toCentre, 1.0);
}

// docs/space.md "Clouds": a shell 0.02 R above sea level, alpha-blended (0.96, 0.97, 1.0) x density,
// turning east one turn per 6 h of real time (FFNx writes the offset into ff7Globe[0].y from the system clock, src/ff7/world/ffu.cpp), faded in by
// the camera's height (smoothstep 0.25 -> 0.6 R); the terrain under a cloud darkened by 1 - 0.25 x
// density. FF7's map reaches the terrain as FFNx's environment-BRDF texture (tex_9, lit draws only) and
// the sky atlas's cloud band (the cloud billboards), both made by tools/ff7planet skyatlas.
#define FFU_CLOUD_SHELL 1.02   // the shell's radius in R (FF7's sea is 500 units, 0.011 R, below the sphere)
#define FFU_CLOUD_COLOUR vec3(0.96, 0.97, 1.0)
#define FFU_CLOUD_SHADOW 0.25

float CloudFade(vec3 cameraPos)
{
	return smoothstep(0.25, 0.6, CameraHeightR(cameraPos));
}

// Sea level and the cloud shell, in planet radii of the planet as drawn (FF7's sea is 500 units down).
float SeaLevelR(vec3 cameraPos)
{
	vec2 latLonQ = CameraLatLon(cameraPos);
	return 1.0 + FFU_TERRAIN_Y / GlobeRadiusNow(latLonQ.x, cameraPos.y);
}

// A planet-axes direction -> the cloud map's unwrapped coordinates (u in turns, v from the north pole),
// with the drift: the map has turned east by `drift`, so it is read `drift` further west.
vec2 CloudCoords(vec3 n, float drift)
{
	return vec2((atan2(n.x, n.z) - drift) / (2.0 * FFU_PI), 0.5 - asin(clamp(n.y, -1.0, 1.0)) / FFU_PI);
}

// Gradients of unwrapped longitude across the turn's seam: take the short way round.
vec2 SeamlessGradient(vec2 gradient)
{
	gradient.x -= floor(gradient.x + 0.5);
	return gradient;
}

// A point of the planet (lat/lon, height above the sphere in game units) in the planet's own axes, in
// R of the planet as drawn: what the terrain passes to its fragments for the cloud shell's parallax.
vec3 PlanetPoint(vec2 latLon, float height, vec3 cameraPos)
{
	vec2 latLonQ = CameraLatLon(cameraPos);
	return GlobeUnit(latLon) * (1.0 + height / GlobeRadiusNow(latLonQ.x, cameraPos.y));
}

// The camera in the planet's own axes (R of the planet as drawn), centre at the origin.
vec3 CameraInPlanet(vec3 cameraPos)
{
	vec3 east, up, south;
	vec2 latLonQ;
	GlobeFrame(cameraPos, east, up, south, latLonQ);
	return up * (1.0 + CameraHeightR(cameraPos));
}

// Other planets in FF7's sky (docs/space.md "Other planets"). FFNx 1.24.3 can't bind a texture of ours,
// so they come in through FFNx's own cloud billboards: Gaia's 11 x 11 cloud plane, drawn 5 times
// (additively, depth-tested, no depth write), whose texture is replaced by an atlas of FF8's and FF9's
// world maps (tools/ff7planet skyatlas). The 3 draws with a mirrored world matrix... see OtherPlanetSlot.
// Each draw becomes a camera-facing square exactly bounding one planet's disc; the fragment shader
// ray-traces the sphere, samples its map and writes its true depth.
// FFNx alternates the sign of the billboards' x axis (draws 0, 2, 4 vs 1, 3), which flips the sign of the
// model matrix's determinant: positive -> slot 0 (drawn 3 times), negative -> slot 1 (twice). Each draw
// adds 1/3 or 1/2 of the colour, so the sum is the planet once.
float OtherPlanetSlot(mat4 worldViewM, mat4 invViewM)
{
	vec3 ax = mul(invViewM, mul(worldViewM, vec4(1.0, 0.0, 0.0, 0.0))).xyz;
	vec3 ay = mul(invViewM, mul(worldViewM, vec4(0.0, 1.0, 0.0, 0.0))).xyz;
	vec3 az = mul(invViewM, mul(worldViewM, vec4(0.0, 0.0, 1.0, 0.0))).xyz;
	return dot(ax, cross(ay, az)) > 0.0 ? 0.0 : 1.0;
}

// In front of the camera = positive clip w through FFNx's own projection (whichever way its view looks).
vec4 OtherPlanetSquare(vec2 corner, vec3 directionV, float distance, mat4 viewport, mat4 projection, mat4 invViewM,
	vec3 east, vec3 up, vec3 south, out vec3 solarRay)
{
	solarRay = vec3_splat(0.0);
	vec4 clip = mul(mul(viewport, projection), vec4(directionV * 1000.0, 1.0));
	if (clip.w <= 0.0) return vec4(0.0, 0.0, 0.0, 0.0);   // behind the camera
	vec3 side = normalize(abs(directionV.y) < 0.99 ? cross(directionV, vec3(0.0, 1.0, 0.0)) : cross(directionV, vec3(1.0, 0.0, 0.0)));
	vec3 other = cross(side, directionV);
	// A square perpendicular to the view of the centre, at depth 50,000 (the depth written per pixel is
	// the true one), round the disc and its air: half-angle asin(1.3 / distance), as FF8's 1.3 R shell.
	float depth = 50000.0;
	float halfSize = depth * tan(asin(min(1.3 / distance, 0.999))) * 1.02;
	vec3 view = directionV * depth + (side * corner.x + other * corner.y) * halfSize;
	vec3 world = mul(invViewM, vec4(view, 0.0)).xyz;
	solarRay = east * world.x + up * world.y + south * world.z;
	return vec4(view, 1.0);
}

// 4x4 inverse (bgfx's Vulkan path compiles HLSL, which has no inverse()). Reads and rebuilds the
// matrix with the same [row][column] indexing, so mul() treats the result consistently.
mat4 FfuInverse(mat4 m)
{
	float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2], a03 = m[0][3];
	float a10 = m[1][0], a11 = m[1][1], a12 = m[1][2], a13 = m[1][3];
	float a20 = m[2][0], a21 = m[2][1], a22 = m[2][2], a23 = m[2][3];
	float a30 = m[3][0], a31 = m[3][1], a32 = m[3][2], a33 = m[3][3];
	float b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10;
	float b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
	float b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30;
	float b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
	float invDet = 1.0 / (b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06);
	return mtxFromRows(
		vec4(a11 * b11 - a12 * b10 + a13 * b09, a02 * b10 - a01 * b11 - a03 * b09, a31 * b05 - a32 * b04 + a33 * b03, a22 * b04 - a21 * b05 - a23 * b03) * invDet,
		vec4(a12 * b08 - a10 * b11 - a13 * b07, a00 * b11 - a02 * b08 + a03 * b07, a32 * b02 - a30 * b05 - a33 * b01, a20 * b05 - a22 * b02 + a23 * b01) * invDet,
		vec4(a10 * b10 - a11 * b08 + a13 * b06, a01 * b08 - a00 * b10 - a03 * b06, a30 * b04 - a31 * b02 + a33 * b00, a21 * b02 - a20 * b04 - a23 * b00) * invDet,
		vec4(a11 * b07 - a10 * b09 - a12 * b06, a00 * b09 - a01 * b07 + a02 * b06, a31 * b01 - a30 * b03 - a32 * b00, a20 * b03 - a21 * b01 + a22 * b00) * invDet);
}

// The whole view (half again as wide, for the space lens) at view depth 50,000: the slot-0 draws use it
// while FF7's own clouds can show beyond its limb.
vec4 FullViewCorner(vec2 corner, mat4 viewport, mat4 projection, mat4 invViewM, out vec3 solarRay)
{
	mat4 m = mul(viewport, projection);
	float ahead = mul(m, vec4(0.0, 0.0, 1000.0, 1.0)).w > 0.0 ? 50000.0 : -50000.0;
	vec4 clip = mul(m, vec4(0.0, 0.0, ahead, 1.0));
	vec4 unprojected = mul(FfuInverse(m), vec4(corner * 1.5, clip.z / clip.w, 1.0));
	vec3 view = unprojected.xyz / unprojected.w;
	vec3 cameraPos = mul(invViewM, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	vec3 east, up, south;
	vec2 latLonQ;
	GlobeFrame(cameraPos, east, up, south, latLonQ);
	vec3 world = mul(invViewM, vec4(view, 0.0)).xyz;
	solarRay = east * world.x + up * world.y + south * world.z;
	return vec4(view, 1.0);
}

// corner: the cloud plane's vertex in [-1, 1]^2. Returns its view-space position (w = 0: hide it),
// its solar view direction and the camera -> planet centre vector (R).
vec4 OtherPlanetCorner(vec2 corner, float slot, mat4 viewM, mat4 invViewM, mat4 viewport, mat4 projection,
	out vec3 solarRay, out vec3 toCentre)
{
	vec3 cameraPos = mul(invViewM, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	vec3 east, up, south;
	vec2 latLonQ;
	GlobeFrame(cameraPos, east, up, south, latLonQ);
	toCentre = OtherPlanetCentre(slot) - SolarCameraPosition(cameraPos);
	float distance = length(toCentre);
	vec3 directionS = toCentre / distance;
	vec3 directionV = mul(viewM, vec4(dot(directionS, east), dot(directionS, up), dot(directionS, south), 0.0)).xyz;
	return OtherPlanetSquare(corner, directionV, distance, viewport, projection, invViewM, east, up, south, solarRay);
}


// Space: above FFU_SPACE_START the world map's sky quad (the only 2D quad at far depth) is stretched
// to the whole screen and turns into black space with stars fixed in the world, fully by FFU_SPACE_FULL.
#define FFU_SPACE_START 55000.0
#define FFU_SPACE_FULL 85000.0

// The sky is drawn by SkyFromSpace from a little below FFU_SPACE_START, so the moment the bridge
// stretches the quad (its own estimate of 70000) the sky is already the computed one.
#define FFU_SKY_TAKEOVER 55000.0
bool IsSkyFromSpace(mat4 invViewM)
{
	return true;   // the world is always a globe, so the computed sky is used at every altitude
}

float SpaceAmount(mat4 invViewM)
{
	vec3 cameraPos = mul(invViewM, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	return smoothstep(FFU_SPACE_START, FFU_SPACE_FULL, cameraPos.y);
}

// The shared stars (docs/space.md "Stars"): the same 700 stars as FF8 and FF9, fixed in the solar frame
// (the sky shader turns its world ray into solar axes with WorldToSolar). The list is binned into a cube
// map by tools/ff7planet/src/ff7planet/stars.py (FFNx.stars.sh), so a pixel only tests the few stars of
// its own cell. Must match stars.py's cube_cell.
#include "FFNx.stars.sh"

float FfuStarCell(vec3 d)
{
	vec3 a = abs(d);
	float face;
	vec2 uv;
	float major;
	if (a.x >= a.y && a.x >= a.z) { face = d.x > 0.0 ? 0.0 : 1.0; uv = d.yz; major = a.x; }
	else if (a.y >= a.z) { face = d.y > 0.0 ? 2.0 : 3.0; uv = d.xz; major = a.y; }
	else { face = d.z > 0.0 ? 4.0 : 5.0; uv = d.xy; major = a.z; }
	vec2 c = clamp(floor((uv / major * 0.5 + 0.5) * FFU_STAR_CELLS), 0.0, FFU_STAR_CELLS - 1.0);
	return face * FFU_STAR_CELLS * FFU_STAR_CELLS + c.y * FFU_STAR_CELLS + c.x;
}

// Each star is a round disc with the same area as FF8's square of `size` 1/2560ths of the screen width,
// with a one-pixel antialiased edge measured at sub-pixel accuracy (so a star moving across pixels keeps
// its brightness: no shimmer), colour = brightness x fade, and every 7th-ish one bluer (FF8: blue + 40).
// pixelAngle: the angle one pixel spans at this pixel; viewWidth in pixels.
vec3 SpaceStars(vec3 solarDir, float pixelAngle, float viewWidth, float fade)
{
	int cell = int(FfuStarCell(solarDir));
	int first = int(FFU_STAR_CELL_START[cell]);
	int last = int(FFU_STAR_CELL_START[cell + 1]);
	float subPixel = viewWidth / 2560.0;
	vec3 light = vec3_splat(0.0);
	for (int k = first; k < last; k++)
	{
		vec4 entry = FFU_STAR_ENTRY[k];
		float distancePx = length(solarDir - entry.xyz) / pixelAngle;
		float packed = entry.w;
		float blue = packed >= 1024.0 ? 1.0 : 0.0;
		packed -= 1024.0 * blue;
		float size = 1.0 + floor(packed / 256.0);
		float brightness = (packed - 256.0 * (size - 1.0)) / 255.0 * fade;
		float radiusPx = size * subPixel * 0.5641896;   // 1 / sqrt(pi): a disc as big as the square
		float coverage = saturate(radiusPx + 0.5 - distancePx);
		light += coverage * vec3(brightness, brightness, min(1.0, brightness + blue * 40.0 / 255.0));
	}
	return light;
}


// The sky for one view ray, from a real atmosphere around the planet: air density falls off
// exponentially with height (scale height ~13% of the radius). The air along the ray (its optical
// depth) comes from Schueler's closed-form Chapman approximation (GPU Pro 3) instead of a per-pixel
// ray march, which cost 40% of the frame at 4608 x 2880. Blue scatters most; long paths through the
// air whiten towards the horizon; above it the sky goes black with stars, and from space the
// atmosphere shows as a glowing rim around the planet.
// Tuned so FF7's normal camera (~7000 up) has a blue sky, the Highwind's cruise (~27000) a deep navy
// one, and space is black from ~40000 (about one planet radius up).
// docs/space.md "Atmosphere", computed in planet radii of the planet as drawn (R_now: the near-ground
// enlargement scales the air with the planet), so it is exactly the written spec.
// Scale height 0.12783 R (6,000 FF7 units), thinning to 0.025566 R (1,200) between camera heights
// 0.63916 R and 3.19579 R (30,000 and 150,000): an atmosphere 13% of the radius deep would glow from
// space as a halo half a radius wide; thinned, the limb stays a band.
#define FFU_SCALE_HEIGHT_R 0.127832
#define FFU_SPACE_SCALE_HEIGHT_R 0.025566
#define FFU_THIN_FROM_R 0.639159
#define FFU_THIN_TO_R 3.195793
#define FFU_SCATTER_R vec3(14.0810, 32.8557, 70.4051)   // per R (0.0003, 0.0007, 0.0015 per FF7 unit)
#define FFU_DEPTH_CLAMP_R 2.130530                       // optical depths are clamped to 1e5 FF7 units
// Long paths through the air (the horizon) go pale blue rather than white.
#define FFU_SKY_TINT vec3(0.62, 0.80, 1.0)

// Optical depth (R) from height h (R) along a ray with cosine cosChi to the local vertical, out to space,
// for an exponential atmosphere of scale height H (R) round a sphere of radius 1: Schueler's Chapman
// approximation. For rays heading down, the term through the ray's lowest point is exp((1 - p) / H) with
// p = (1 + h) sin(chi) the ray's closest approach to the centre; it is computed as one exponent
// (clamped), because as two factors (exp(x - x sin chi) clamped, times exp(-h/H)) it lost the limb
// entirely from high up: e^80 x e^-125.
float AirAlongRay(float h, float cosChi, float H)
{
	float x = (1.0 + h) / H;
	float c = sqrt(0.5 * FFU_PI * x);
	float above = exp(-max(h, 0.0) / H);
	if (cosChi >= 0.0) return H * above * c / ((c - 1.0) * cosChi + 1.0);
	float sinChi = sqrt(saturate(1.0 - cosChi * cosChi));
	float lowest = min((1.0 - (1.0 + h) * sinChi) / H, 80.0);
	// (rays that hit the planet come out meaningless, even negative: the terrain covers them)
	return H * max(above * c / ((c - 1.0) * cosChi - 1.0) + 2.0 * c * exp(lowest) * sqrt(sinChi), 0.0);
}


// Optical depth (R) from the camera out along a world ray (y = up at the camera).
float AirToSpace(vec3 cameraPos, float cosChi)
{
	float h = CameraHeightR(cameraPos);
	float H = mix(FFU_SCALE_HEIGHT_R, FFU_SPACE_SCALE_HEIGHT_R, smoothstep(FFU_THIN_FROM_R, FFU_THIN_TO_R, h));
	return min(AirAlongRay(h, cosChi, H), FFU_DEPTH_CLAMP_R);
}

// docs/space.md "Lens": from 2 R up (d >= 3) every game draws space with a 30.0 degree vertical field
// of view over the whole frame, centred; below d = 1.5 FF7's own lens (whose axis sits above the
// frame's middle); in between the vertical field of view blends with smoothstep(1.5, 3.0, d). FF7's
// projection comes from the game, so the lens is applied in clip space to every 3D draw (ApplySpaceLens)
// and undone for the sky's view rays (SpaceLensToGame).
// The game's own lens: (focal x, focal y, centre x, centre y) in NDC.
vec4 GameLens(mat4 viewport, mat4 projection)
{
	mat4 m = mul(viewport, projection);
	float ahead = mul(m, vec4(0.0, 0.0, 1000.0, 1.0)).w > 0.0 ? 1000.0 : -1000.0;   // whichever way the view looks
	vec4 a = mul(m, vec4(0.0, 0.0, ahead, 1.0));
	vec4 bx = mul(m, vec4(1.0, 0.0, ahead, 1.0));
	vec4 by = mul(m, vec4(0.0, 1.0, ahead, 1.0));
	vec2 centre = a.xy / a.w;
	return vec4(abs(bx.x / bx.w - centre.x) * 1000.0, abs(by.y / by.w - centre.y) * 1000.0, centre);
}

// (scale about the game's centre, the game's centre x/y, share of the space lens)
vec4 SpaceLens(mat4 viewport, mat4 projection, vec3 cameraPos)
{
	vec4 lens = GameLens(viewport, projection);
	// While the airship is flown the shared 30-degree lens holds at every height (docs/space.md "Chase camera").
	float share = max(smoothstep(1.5, 3.0, 1.0 + CameraHeightR(cameraPos)), ff7Globe[0].w);
	float fieldOfView = mix(2.0 * atan(1.0 / lens.y), 30.0 * FFU_PI / 180.0, share);
	return vec4(1.0 / (tan(0.5 * fieldOfView) * lens.y), lens.zw, share);
}

vec4 ApplySpaceLens(vec4 clip, vec4 lens)
{
	clip.xy = (clip.xy - lens.yz * clip.w) * lens.x + lens.yz * (1.0 - lens.w) * clip.w;
	return clip;
}

vec2 SpaceLensToGame(vec2 ndc, vec4 lens)
{
	return (ndc - lens.yz * (1.0 - lens.w)) / lens.x + lens.yz;
}

// The air's own glow along a world ray from the camera (SkyFromSpace without the stars).
vec3 SkyAir(vec3 ray, vec3 cameraPos)
{
	return (1.0 - exp(-AirToSpace(cameraPos, normalize(ray).y) * FFU_SCATTER_R)) * FFU_SKY_TINT;
}

// pixelAngle: the angle one pixel spans (for the stars' size); viewWidth: the view's width in pixels.
vec3 SkyFromSpace(vec3 ray, vec3 cameraPos, float pixelAngle, float viewWidth)
{
#ifdef FFU_CHEAP_SKY
	return vec3(0.2, 0.4, 0.8);   // test: cost of the atmosphere
#endif
	vec3 d = normalize(ray);
	float height = max(cameraPos.y, 0.0);
	float opticalDepth = AirToSpace(cameraPos, d.y);
	vec3 scattered = 1.0 - exp(-opticalDepth * FFU_SCATTER_R);
	vec3 transmitted = exp(-opticalDepth * FFU_SCATTER_R * 3.0);
	vec3 sky = scattered * FFU_SKY_TINT;
	// The stars' "space fade" (docs/space.md) is the air's own transmittance along the ray (its blue,
	// the colour the sky scatters most): they come out as the sky above goes dark, and stay faint
	// near the horizon, where the path through the air is long.
	float fade = transmitted.b;
	vec3 solarRay = normalize(WorldToSolar(d, cameraPos));
	vec3 solarCamera = SolarCameraPosition(cameraPos);
	bool behindPlanet = RayToPlanet(solarRay, OtherPlanetCentre(0.0) - solarCamera) > 0.0
		|| RayToPlanet(solarRay, OtherPlanetCentre(1.0) - solarCamera) > 0.0;   // FF8 or FF9 is in front
	if (fade > 0.002 && !behindPlanet) sky += SpaceStars(solarRay, pixelAngle, viewWidth, fade);
#ifdef FFU_SOLAR_DEBUG
	// test: the sky coloured by solar octant, to check the sky's solar frame against the bridge's.
	vec3 solar = normalize(WorldToSolar(d, cameraPos));
	sky = 0.25 + 0.5 * step(vec3_splat(0.0), solar);   // the solar octant this ray points into
	// and a magenta ring 15% wider than the planet, round the planet's centre as the sky sees it:
	// concentric with the drawn planet only if the sky's projection is the planet's.
	float planetAngle = asin(FFU_GLOBE_RADIUS / (FFU_GLOBE_RADIUS + height));
	if (abs(acos(clamp(-d.y, -1.0, 1.0)) - 1.15 * planetAngle) < 0.003) sky = vec3(1.0, 0.0, 1.0);
#endif
	return sky;
}

// Logarithmic depth for the world's 3D geometry. With the camera far above the planet, FF7's depth
// range can't separate Gaia's nearly coplanar layers (shallow and deep sea, coastlines), and they
// flickered. log2(1 + w) keeps precision from metres to space. It is written PER PIXEL (FFNx.frag,
// FFNx.lighting.frag): computed per vertex and interpolated, it was wrong across Gaia's large flat sea
// triangles and a whole sea quad lost to the seabed (a square on the planet). This projection uses
// reversed depth (z 1 = near, 0 = far).
#define FFU_DEPTH_FAR 4000000.0
#define FFU_DEPTH_NEAR_LIMIT 30000.0   // up to this distance the game's own depth is kept unchanged

// The game's depth at a view distance, through FFNx's real projection (whatever its convention).
float GameDepthAt(mat4 viewport, mat4 projection, float w)
{
	vec4 clip = mul(mul(viewport, projection), vec4(0.0, 0.0, w, 1.0));
	return clip.z / clip.w;
}

// Depth for every 3D draw (world map AND models, so their ordering stays consistent), written per pixel:
// - near the camera (<= FFU_DEPTH_NEAR_LIMIT) the game's own depth, squeezed linearly into the near half
//   of the range: exactly the game's ordering (pure log depth here broke the chunk under the camera,
//   a square on the planet);
// - beyond it a logarithmic band filling the far half. With the far band squeezed into the sliver the
//   game's projection leaves there (~0.2% of the range), Gaia's seabed shelf, just under the sea, poked
//   through the water as dark strips far from the camera.
// atLimit = the game's depth at the limit; farEnd = 0 for reversed depth (this projection), else 1.
float WorldDepth(float w, float gameDepth, float atLimit, float farEnd)
{
	float nearEnd = 1.0 - farEnd;
	if (w <= FFU_DEPTH_NEAR_LIMIT)
		return mix(0.5, nearEnd, saturate((gameDepth - atLimit) / (nearEnd - atLimit)));
	float t = log2(w / FFU_DEPTH_NEAR_LIMIT) / log2(FFU_DEPTH_FAR / FFU_DEPTH_NEAR_LIMIT);
	return mix(0.5, farEnd, saturate(t) * 0.999);
}

// FF7's own 2D world objects (the Midgar Zolom, dust, splashes and wakes: world_draw_snake_texture_75D544,
// world_submit_draw_effects_75C283) come projected by FF7 itself, with its own depth (0 near, 1 far, one value
// per quad), while every 3D draw writes WorldDepth: with the game's depth they lost to the terrain everywhere
// (docs/world-map-audit.md #1). They get the depth a 3D pixel at the same view distance gets. A perspective
// depth is a + b / w; a and b come from two samples of FFNx's projection, whose convention may be reversed
// (farEnd 0, as here). Returns the quad's depth in FFNx's convention (.x) and its view distance (.y).
vec2 Ff7WorldQuadDepth(float tlDepth, float atLimit, float further)
{
	float depth = further < atLimit ? 1.0 - tlDepth : tlDepth;
	float b = 2.0 * FFU_DEPTH_NEAR_LIMIT * (atLimit - further);
	float a = atLimit - b / FFU_DEPTH_NEAR_LIMIT;
	return vec2(depth, b / (depth - a));
}

#define MAX_BONE_MATRICES 128
