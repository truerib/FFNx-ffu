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

#include "ffu_airships.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "external_mesh.h"
#include "globals.h"
#include "cfg.h"
#include "log.h"

namespace ffu
{
    namespace
    {
        struct Model
        {
            std::unique_ptr<ExternalMesh> still;
            std::vector<std::unique_ptr<ExternalMesh>> frames;
            std::vector<bool> failed;
            uint32_t loaded = 0;
            double seconds = 0.0;
        };
        std::map<std::string, Model> models;

        std::filesystem::path meshDir() { return std::filesystem::path(basedir) / external_mesh_path / "airships"; }

        std::unique_ptr<ExternalMesh> load(const std::filesystem::path &file)
        {
            auto mesh = std::make_unique<ExternalMesh>();
            std::string path = file.generic_string();
            std::string textures = (std::filesystem::path(basedir) / mod_path / "world").generic_string() + "/";
            if (std::filesystem::exists(file) && mesh->importExternalMeshGltfFile(path.data(), textures.data(), true) && !mesh->shapes.empty())
                return mesh;
            return nullptr;
        }

        // <id>_frames.json from tools/airships: {"animation": ..., "count": N, "seconds": s}.
        void readFrames(const std::string &key, Model &model)
        {
            const std::filesystem::path file = meshDir() / (key + "_frames.json");
            FILE *in = fopen(file.generic_string().c_str(), "rb");
            if (!in) return;
            char text[512] = {};
            fread(text, 1, sizeof(text) - 1, in);
            fclose(in);
            const char *count = strstr(text, "\"count\""), *seconds = strstr(text, "\"seconds\"");
            unsigned frames = 0;
            double each = 0.0;
            if (!count || !seconds || sscanf(strchr(count, ':') + 1, "%u", &frames) != 1 || sscanf(strchr(seconds, ':') + 1, "%lf", &each) != 1
                || frames < 2 || frames > 120 || each <= 0.0) return;
            model.frames.resize(frames);
            model.failed.assign(frames, false);
            model.seconds = each;
        }
    }

    ExternalMesh *airshipModel(const char *id, size_t idSize, const char *what)
    {
        const std::string key(id, strnlen(id, idSize));
        auto found = models.find(key);
        if (found == models.end())
        {
            Model &model = models[key];
            model.still = load(meshDir() / (key + ".gltf"));
            if (!model.still) ffnx_error("%s: no airship model %s\n", what, (meshDir() / (key + ".gltf")).generic_string().c_str());
            else
            {
                readFrames(key, model);
                ffnx_info("%s: airship %s loaded (%u shapes%s)\n", what, key.c_str(), (uint32_t)model.still->shapes.size(),
                          model.frames.empty() ? "" : ", its animation's frames to follow");
            }
            found = models.find(key);
        }
        Model &model = found->second;
        if (!model.still || model.frames.empty()) return model.still.get();
        if (model.loaded < model.frames.size())
        {
            for (size_t i = 0; i < model.frames.size(); ++i)
                if (!model.frames[i] && !model.failed[i])
                {
                    char name[96];
                    _snprintf(name, sizeof(name), "%s_f%02u.gltf", key.c_str(), (unsigned)i);
                    model.frames[i] = load(meshDir() / name);
                    model.failed[i] = !model.frames[i];
                    if (++model.loaded == model.frames.size())
                        ffnx_info("%s: airship %s animated (%u frames of %.0f ms)\n", what, key.c_str(), (unsigned)model.frames.size(), model.seconds * 1000.0);
                    break;
                }
            return model.still.get();
        }
        const size_t frame = (size_t)((double)GetTickCount64() / (model.seconds * 1000.0)) % model.frames.size();
        return model.frames[frame] ? model.frames[frame].get() : model.still.get();
    }
}
