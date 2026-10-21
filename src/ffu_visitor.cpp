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

// FFU (Final Fantasy Universe): a visitor from another game drawn in FF7's own battle (mixed parties). Our bridge DLL
// (mods/bridge-ff7 in the universe's repo) poses the visitor's own model, exported from its own game, on the CPU and hands
// its triangles here, one texture at a time, from inside FF7's own battle model draw, in place of the seat's stand-in body.
// They go through gl_draw_indexed_primitive like FF7's own actors: FF7's projection and battle camera, FFNx's deferred
// lighting and shadows, and depth-tested against the monsters and the stage. Generic on purpose: everything about the
// visitor (its file, axes, scale, pose) is the bridge's, so this file changes rarely.

#include <windows.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "globals.h"
#include "renderer.h"
#include "gl.h"
#include "matrix.h"
#include "log.h"

struct ffu_visitor_vertex
{
	float x, y, z;      // FF7 model space (flags 1) or view space before projection
	float nx, ny, nz;   // its normal, the same space (FFNx's lighting)
	uint32_t bgra;      // its colour, multiplied into the texture
	float u, v;
};

enum : uint32_t
{
	FFU_VISITOR_IN_BATTLE_SPACE = 1,   // `matrix` places the mesh in FF7's battle space: the world-view is it times the
	                                   // battle camera (game_object->camera_matrix), as FF7 draws its own bones
	FFU_VISITOR_ALPHA_CUT = 2,         // its texture's see-through pixels are cut out (hair, cloth edges)
};

namespace
{
	std::unordered_map<std::string, uint32_t> textures;   // PNG path -> renderer texture (kept: a visitor's few pages)
	std::vector<nvertex> vertexBuffer;
	std::vector<vector3<float>> normalBuffer;
	std::vector<WORD> indexBuffer;

	uint32_t textureOf(const char* png)
	{
		if (png == nullptr || png[0] == '\0') return 0;
		auto found = textures.find(png);
		if (found != textures.end()) return found->second;
		uint32_t width = 0, height = 0;
		std::string path = png;
		uint32_t handle = newRenderer.createTextureLibPng(path.data(), &width, &height, true);
		if (handle == 0) ffnx_error("FFU visitor: can't load the texture %s\n", png);
		else ffnx_info("FFU visitor: texture %s (%ux%u)\n", png, width, height);
		textures[png] = handle;   // (a failure is kept too: never retried every frame)
		return handle;
	}

	// The battle's projection, as FF7's own body draw sets it: the first part of the stand-in body that has one.
	struct matrix* projectionOf(struct hrc_data* hrc)
	{
		if (hrc == nullptr || hrc->bones == nullptr) return nullptr;
		for (uint32_t bone = 0; bone < hrc->num_bones; bone++)
		{
			struct hrc_bone* each = &hrc->bones[bone];
			if (each->rsd_array == nullptr) continue;
			for (uint32_t part = 0; part < each->num_rsd; part++)
			{
				struct rsd_data* data = each->rsd_array[part].rsd_data;
				if (data && data->polygon_set && data->polygon_set->matrix_set && data->polygon_set->matrix_set->matrix_projection)
					return data->polygon_set->matrix_set->matrix_projection;
			}
		}
		return nullptr;
	}
}

// One textured triangle list of a visitor, drawn now in FF7's battle as one of its own models (game thread, inside FF7's
// battle model draw). hrc: the stand-in body FF7 was about to draw (for the battle's projection; null: the current one).
// game_object: FF7's game object (its battle camera). matrix: 16 floats, row-major, FF7's row-vector convention. Returns
// 1 if drawn, 0 if not (FF8, bad arguments, no texture).
extern "C" __declspec(dllexport) int ffu_visitor_draw(void* hrc, void* game_object, const float* matrix,
	const ffu_visitor_vertex* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount, const char* png,
	uint32_t flags)
{
	if (ff8 || game_object == nullptr || matrix == nullptr || vertices == nullptr || indices == nullptr
		|| vertexCount == 0 || indexCount < 3 || vertexCount > 65535) return 0;
	struct ff7_game_obj* game = (struct ff7_game_obj*)game_object;

	struct matrix place;
	memcpy(&place.m[0][0], matrix, sizeof(float) * 16);
	struct matrix worldView;
	if ((flags & FFU_VISITOR_IN_BATTLE_SPACE) && game->camera_matrix)
	{
		multiply_matrix(&place, game->camera_matrix, &worldView);
		worldView._14 = 0.0f;   // (as FF7's own bones: graphics.cpp draw_3d_model)
		worldView._24 = 0.0f;
		worldView._34 = 0.0f;
		worldView._44 = 1.0f;
	}
	else worldView = place;

	vertexBuffer.resize(vertexCount);
	normalBuffer.resize(vertexCount);
	for (uint32_t k = 0; k < vertexCount; k++)
	{
		const ffu_visitor_vertex& from = vertices[k];
		nvertex& to = vertexBuffer[k];
		memset(&to, 0, sizeof(to));
		to._.x = from.x;
		to._.y = from.y;
		to._.z = from.z;
		to.color.w = 1.0f;
		to.color.color = from.bgra;
		to.color.specular = 0xFF000000;
		to.u = from.u;
		to.v = from.v;
		normalBuffer[k] = { from.nx, from.ny, from.nz };
	}
	indexBuffer.assign(indices, indices + indexCount - indexCount % 3);
	for (WORD& index : indexBuffer)
		if (index >= vertexCount) index = 0;   // (never past the vertices, whatever the caller sent)

	struct driver_state saved;
	gl_save_state(&saved);

	struct matrix* projection = projectionOf((struct hrc_data*)hrc);
	if (projection) gl_set_d3dprojection_matrix(projection);
	gl_set_worldview_matrix(&worldView);
	gl_set_texture(textureOf(png), nullptr);
	gl_set_blend_func(BLEND_NONE);
	internal_set_renderstate(V_NOCULL, 1, (struct game_obj*)game_object);      // (both sides: its own winding is its game's)
	internal_set_renderstate(V_DEPTHTEST, 1, (struct game_obj*)game_object);
	internal_set_renderstate(V_DEPTHMASK, 1, (struct game_obj*)game_object);
	if (flags & FFU_VISITOR_ALPHA_CUT)
	{
		internal_set_renderstate(V_ALPHAFUNC, 6, (struct game_obj*)game_object);   // GREATER
		internal_set_renderstate(V_ALPHAREF, 127, (struct game_obj*)game_object);
		internal_set_renderstate(V_ALPHATEST, 1, (struct game_obj*)game_object);
	}
	else internal_set_renderstate(V_ALPHATEST, 0, (struct game_obj*)game_object);

	gl_draw_indexed_primitive(RendererPrimitiveType::PT_TRIANGLES, LVERTEX, vertexBuffer.data(), normalBuffer.data(), vertexCount,
		indexBuffer.data(), (uint32_t)indexBuffer.size(), nullptr, nullptr, nullptr, 0, true);

	gl_load_state(&saved);
	return 1;
}
