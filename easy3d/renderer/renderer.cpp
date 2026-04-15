/********************************************************************
 * Copyright (C) 2015 Liangliang Nan <liangliang.nan@gmail.com>
 * https://3d.bk.tudelft.nl/liangliang/
 *
 * This file is part of Easy3D. If it is useful in your research/work,
 * I would be grateful if you show your appreciation by citing it:
 * ------------------------------------------------------------------
 *      Liangliang Nan.
 *      Easy3D: a lightweight, easy-to-use, and efficient C++ library
 *      for processing and rendering 3D data.
 *      Journal of Open Source Software, 6(64), 3255, 2021.
 * ------------------------------------------------------------------
 *
 * Easy3D is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3
 * as published by the Free Software Foundation.
 *
 * Easy3D is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 ********************************************************************/

#include <algorithm>
#include <limits>
#include <typeinfo>
#include <unordered_set>

#include <easy3d/renderer/renderer.h>
#include <easy3d/core/graph.h>
#include <easy3d/core/point_cloud.h>
#include <easy3d/core/surface_mesh.h>
#include <easy3d/core/poly_mesh.h>
#include <easy3d/renderer/drawable_points.h>
#include <easy3d/renderer/drawable_lines.h>
#include <easy3d/renderer/drawable_triangles.h>
#include <easy3d/renderer/texture_manager.h>
#include <easy3d/util/file_system.h>
#include <easy3d/util/logging.h>
#include <easy3d/util/setting.h>


namespace easy3d {

    namespace {
        template <typename T>
        bool collect_face_texture_indices(const SurfaceMesh *model,
                                          const std::string& property_name,
                                          std::vector<int>& indices) {
            auto prop = model->get_face_property<T>(property_name);
            if (!prop)
                return false;

            indices.clear();
            indices.reserve(model->n_faces());
            for (auto face : model->faces())
                indices.push_back(static_cast<int>(prop[face]));
            return true;
        }

        template <typename FT>
        void clamp_scalar_field(const std::vector<FT> &property,
                                float &min_value,
                                float &max_value,
                                float dummy_lower_percent,
                                float dummy_upper_percent) {
            if (property.empty()) {
                LOG(WARNING) << "empty property";
                return;
            }

            std::vector<FT> values = property;
            std::sort(values.begin(), values.end());

            const std::size_t n = values.size() - 1;
            const std::size_t index_lower = static_cast<std::size_t>(n * dummy_lower_percent);
            const std::size_t index_upper = n - static_cast<std::size_t>(n * dummy_upper_percent);

            min_value = static_cast<float>(values[index_lower]);
            max_value = static_cast<float>(values[index_upper]);
            if (min_value >= max_value) {
                min_value = static_cast<float>(values.front());
                max_value = static_cast<float>(values.back());
            }

            if (min_value >= max_value && typeid(FT) == typeid(bool)) {
                min_value = 0.0f;
                max_value = 1.0f;
            }
        }

        template <typename FT>
        float scalar_coord(FT value, float min_value, float max_value) {
            const auto range = max_value - min_value;
            if (range <= std::numeric_limits<float>::epsilon())
                return 0.5f;

            return (static_cast<float>(value) - min_value) / range;
        }

        Texture* request_mesh_texture(const SurfaceMesh *model, const std::string& texture_name = "") {
            std::vector<std::string> candidates;
            if (!texture_name.empty())
                candidates.push_back(texture_name);
            else if (model)
                candidates = model->textures;

            for (const auto& candidate : candidates) {
                std::string resolved = file_system::convert_to_native_style(candidate);
                if (!file_system::is_file(resolved) && model)
                    resolved = file_system::convert_to_native_style(file_system::parent_directory(model->name()) + "/" + candidate);

                if (!file_system::is_file(resolved))
                    continue;

                if (auto tex = TextureManager::request(resolved, Texture::REPEAT))
                    return tex;
            }

            return nullptr;
        }

        bool prepare_subset_geometry(SurfaceMesh *mesh,
                                     const TrianglesDrawable *drawable,
                                     SurfaceMesh::VertexProperty<vec3>& points,
                                     SurfaceMesh::VertexProperty<vec3>& normals) {
            points = mesh->get_vertex_property<vec3>("v:point");
            if (!points) {
                LOG(WARNING) << "missing geometry for drawable '" << drawable->name() << "'";
                return false;
            }

            mesh->update_vertex_normals();
            normals = mesh->get_vertex_property<vec3>("v:normal");
            if (!normals) {
                LOG(WARNING) << "missing vertex normals for drawable '" << drawable->name() << "'";
                return false;
            }

            return true;
        }

        void update_triangle_range(SurfaceMesh *mesh, const std::vector<int>& face_indices) {
            auto triangle_range = mesh->face_property<std::pair<int, int> >("f:triangle_range");
            int triangle_index = 0;
            for (const auto face_index : face_indices) {
                auto face = SurfaceMesh::Face(face_index);
                triangle_range[face] = std::make_pair(triangle_index, triangle_index);
                ++triangle_index;
            }
        }

        void update_subset_uniform_colors(SurfaceMesh *mesh,
                                          TrianglesDrawable *drawable,
                                          const std::vector<int>& face_indices) {
            SurfaceMesh::VertexProperty<vec3> points;
            SurfaceMesh::VertexProperty<vec3> normals;
            if (!prepare_subset_geometry(mesh, drawable, points, normals))
                return;

            std::vector<vec3> d_points, d_normals;
            d_points.reserve(face_indices.size() * 3);
            d_normals.reserve(face_indices.size() * 3);
            for (const auto face_index : face_indices) {
                auto face = SurfaceMesh::Face(face_index);
                for (auto h : mesh->halfedges(face)) {
                    auto v = mesh->target(h);
                    d_points.push_back(points[v]);
                    d_normals.push_back(normals[v]);
                }
            }

            drawable->update_vertex_buffer(d_points);
            drawable->update_normal_buffer(d_normals);
            drawable->disable_element_buffer();
            update_triangle_range(mesh, face_indices);
        }

        void update_subset_colors_on_faces(SurfaceMesh *mesh,
                                           TrianglesDrawable *drawable,
                                           const std::vector<int>& face_indices,
                                           SurfaceMesh::FaceProperty<vec3> colors) {
            SurfaceMesh::VertexProperty<vec3> points;
            SurfaceMesh::VertexProperty<vec3> normals;
            if (!prepare_subset_geometry(mesh, drawable, points, normals))
                return;

            std::vector<vec3> d_points, d_normals, d_colors;
            d_points.reserve(face_indices.size() * 3);
            d_normals.reserve(face_indices.size() * 3);
            d_colors.reserve(face_indices.size() * 3);
            for (const auto face_index : face_indices) {
                auto face = SurfaceMesh::Face(face_index);
                const auto& color = colors[face];
                for (auto h : mesh->halfedges(face)) {
                    auto v = mesh->target(h);
                    d_points.push_back(points[v]);
                    d_normals.push_back(normals[v]);
                    d_colors.push_back(color);
                }
            }

            drawable->update_vertex_buffer(d_points);
            drawable->update_normal_buffer(d_normals);
            drawable->update_color_buffer(d_colors);
            drawable->disable_element_buffer();
            update_triangle_range(mesh, face_indices);
        }

        void update_subset_colors_on_vertices(SurfaceMesh *mesh,
                                              TrianglesDrawable *drawable,
                                              const std::vector<int>& face_indices,
                                              SurfaceMesh::VertexProperty<vec3> colors) {
            SurfaceMesh::VertexProperty<vec3> points;
            SurfaceMesh::VertexProperty<vec3> normals;
            if (!prepare_subset_geometry(mesh, drawable, points, normals))
                return;

            std::vector<vec3> d_points, d_normals, d_colors;
            d_points.reserve(face_indices.size() * 3);
            d_normals.reserve(face_indices.size() * 3);
            d_colors.reserve(face_indices.size() * 3);
            for (const auto face_index : face_indices) {
                auto face = SurfaceMesh::Face(face_index);
                for (auto h : mesh->halfedges(face)) {
                    auto v = mesh->target(h);
                    d_points.push_back(points[v]);
                    d_normals.push_back(normals[v]);
                    d_colors.push_back(colors[v]);
                }
            }

            drawable->update_vertex_buffer(d_points);
            drawable->update_normal_buffer(d_normals);
            drawable->update_color_buffer(d_colors);
            drawable->disable_element_buffer();
            update_triangle_range(mesh, face_indices);
        }

        void update_subset_texcoords_on_vertices(SurfaceMesh *mesh,
                                                 TrianglesDrawable *drawable,
                                                 const std::vector<int>& face_indices,
                                                 SurfaceMesh::VertexProperty<vec2> texcoords) {
            SurfaceMesh::VertexProperty<vec3> points;
            SurfaceMesh::VertexProperty<vec3> normals;
            if (!prepare_subset_geometry(mesh, drawable, points, normals))
                return;

            std::vector<vec3> d_points, d_normals;
            std::vector<vec2> d_texcoords;
            d_points.reserve(face_indices.size() * 3);
            d_normals.reserve(face_indices.size() * 3);
            d_texcoords.reserve(face_indices.size() * 3);
            for (const auto face_index : face_indices) {
                auto face = SurfaceMesh::Face(face_index);
                for (auto h : mesh->halfedges(face)) {
                    auto v = mesh->target(h);
                    d_points.push_back(points[v]);
                    d_normals.push_back(normals[v]);
                    d_texcoords.push_back(texcoords[v]);
                }
            }

            drawable->update_vertex_buffer(d_points);
            drawable->update_normal_buffer(d_normals);
            drawable->update_texcoord_buffer(d_texcoords);
            drawable->disable_element_buffer();
            update_triangle_range(mesh, face_indices);
        }

        void update_subset_texcoords_on_halfedges(SurfaceMesh *mesh,
                                                  TrianglesDrawable *drawable,
                                                  const std::vector<int>& face_indices,
                                                  SurfaceMesh::HalfedgeProperty<vec2> texcoords) {
            SurfaceMesh::VertexProperty<vec3> points;
            SurfaceMesh::VertexProperty<vec3> normals;
            if (!prepare_subset_geometry(mesh, drawable, points, normals))
                return;

            std::vector<vec3> d_points, d_normals;
            std::vector<vec2> d_texcoords;
            d_points.reserve(face_indices.size() * 3);
            d_normals.reserve(face_indices.size() * 3);
            d_texcoords.reserve(face_indices.size() * 3);
            for (const auto face_index : face_indices) {
                auto face = SurfaceMesh::Face(face_index);
                for (auto h : mesh->halfedges(face)) {
                    auto v = mesh->target(h);
                    d_points.push_back(points[v]);
                    d_normals.push_back(normals[v]);
                    d_texcoords.push_back(texcoords[h]);
                }
            }

            drawable->update_vertex_buffer(d_points);
            drawable->update_normal_buffer(d_normals);
            drawable->update_texcoord_buffer(d_texcoords);
            drawable->disable_element_buffer();
            update_triangle_range(mesh, face_indices);
        }

        template <typename FT>
        void update_subset_scalar_on_faces(SurfaceMesh *mesh,
                                           TrianglesDrawable *drawable,
                                           const std::vector<int>& face_indices,
                                           SurfaceMesh::FaceProperty<FT> prop) {
            SurfaceMesh::VertexProperty<vec3> points;
            SurfaceMesh::VertexProperty<vec3> normals;
            if (!prepare_subset_geometry(mesh, drawable, points, normals))
                return;

            const float dummy_lower = drawable->clamp_range() ? drawable->clamp_lower() : 0.0f;
            const float dummy_upper = drawable->clamp_range() ? drawable->clamp_upper() : 0.0f;
            float min_value = std::numeric_limits<float>::max();
            float max_value = -std::numeric_limits<float>::max();
            clamp_scalar_field(prop.vector(), min_value, max_value, dummy_lower, dummy_upper);

            std::vector<vec3> d_points, d_normals;
            std::vector<vec2> d_texcoords;
            d_points.reserve(face_indices.size() * 3);
            d_normals.reserve(face_indices.size() * 3);
            d_texcoords.reserve(face_indices.size() * 3);
            for (const auto face_index : face_indices) {
                auto face = SurfaceMesh::Face(face_index);
                const float coord = scalar_coord(prop[face], min_value, max_value);
                for (auto h : mesh->halfedges(face)) {
                    auto v = mesh->target(h);
                    d_points.push_back(points[v]);
                    d_normals.push_back(normals[v]);
                    d_texcoords.emplace_back(coord, 0.5f);
                }
            }

            drawable->update_vertex_buffer(d_points);
            drawable->update_normal_buffer(d_normals);
            drawable->update_texcoord_buffer(d_texcoords);
            drawable->disable_element_buffer();
            update_triangle_range(mesh, face_indices);
        }

        template <typename FT>
        void update_subset_scalar_on_vertices(SurfaceMesh *mesh,
                                              TrianglesDrawable *drawable,
                                              const std::vector<int>& face_indices,
                                              SurfaceMesh::VertexProperty<FT> prop) {
            SurfaceMesh::VertexProperty<vec3> points;
            SurfaceMesh::VertexProperty<vec3> normals;
            if (!prepare_subset_geometry(mesh, drawable, points, normals))
                return;

            const float dummy_lower = drawable->clamp_range() ? drawable->clamp_lower() : 0.0f;
            const float dummy_upper = drawable->clamp_range() ? drawable->clamp_upper() : 0.0f;
            float min_value = std::numeric_limits<float>::max();
            float max_value = -std::numeric_limits<float>::max();
            clamp_scalar_field(prop.vector(), min_value, max_value, dummy_lower, dummy_upper);

            std::vector<vec3> d_points, d_normals;
            std::vector<vec2> d_texcoords;
            d_points.reserve(face_indices.size() * 3);
            d_normals.reserve(face_indices.size() * 3);
            d_texcoords.reserve(face_indices.size() * 3);
            for (const auto face_index : face_indices) {
                auto face = SurfaceMesh::Face(face_index);
                for (auto h : mesh->halfedges(face)) {
                    auto v = mesh->target(h);
                    d_points.push_back(points[v]);
                    d_normals.push_back(normals[v]);
                    d_texcoords.emplace_back(scalar_coord(prop[v], min_value, max_value), 0.5f);
                }
            }

            drawable->update_vertex_buffer(d_points);
            drawable->update_normal_buffer(d_normals);
            drawable->update_texcoord_buffer(d_texcoords);
            drawable->disable_element_buffer();
            update_triangle_range(mesh, face_indices);
        }

        void update_subset_face_drawable(SurfaceMesh *mesh,
                                         TrianglesDrawable *drawable,
                                         const std::vector<int>& face_indices) {
            if (!mesh || !drawable || mesh->empty())
                return;

            const auto& name = drawable->property_name();
            switch (drawable->coloring_method()) {
                case State::TEXTURED: {
                    switch (drawable->property_location()) {
                        case State::VERTEX: {
                            auto texcoord = mesh->get_vertex_property<vec2>(name);
                            if (texcoord)
                                update_subset_texcoords_on_vertices(mesh, drawable, face_indices, texcoord);
                            else {
                                LOG(WARNING) << "texcoord property '" << name
                                             << "' not found on vertices (use uniform coloring)";
                                drawable->set_coloring_method(State::UNIFORM_COLOR);
                                update_subset_uniform_colors(mesh, drawable, face_indices);
                            }
                            return;
                        }
                        case State::HALFEDGE: {
                            auto texcoord = mesh->get_halfedge_property<vec2>(name);
                            if (texcoord)
                                update_subset_texcoords_on_halfedges(mesh, drawable, face_indices, texcoord);
                            else {
                                LOG(WARNING) << "texcoord property '" << name
                                             << "' not found on halfedges (use uniform coloring)";
                                drawable->set_coloring_method(State::UNIFORM_COLOR);
                                update_subset_uniform_colors(mesh, drawable, face_indices);
                            }
                            return;
                        }
                        case State::FACE:
                        case State::EDGE:
                            LOG(WARNING) << "should not happen" << name;
                            break;
                    }
                    break;
                }

                case State::COLOR_PROPERTY: {
                    switch (drawable->property_location()) {
                        case State::FACE: {
                            auto colors = mesh->get_face_property<vec3>(name);
                            if (colors)
                                update_subset_colors_on_faces(mesh, drawable, face_indices, colors);
                            else {
                                LOG(WARNING) << "color property '" << name
                                             << "' not found on faces (use uniform coloring)";
                                drawable->set_coloring_method(State::UNIFORM_COLOR);
                                update_subset_uniform_colors(mesh, drawable, face_indices);
                            }
                            return;
                        }
                        case State::VERTEX: {
                            auto colors = mesh->get_vertex_property<vec3>(name);
                            if (colors)
                                update_subset_colors_on_vertices(mesh, drawable, face_indices, colors);
                            else {
                                LOG(WARNING) << "color property '" << name
                                             << "' not found on vertices (use uniform coloring)";
                                drawable->set_coloring_method(State::UNIFORM_COLOR);
                                update_subset_uniform_colors(mesh, drawable, face_indices);
                            }
                            return;
                        }
                        case State::EDGE:
                        case State::HALFEDGE:
                            LOG(WARNING) << "should not happen" << name;
                            break;
                    }
                    break;
                }

                case State::SCALAR_FIELD: {
                    switch (drawable->property_location()) {
                        case State::FACE: {
                            if (auto prop = mesh->get_face_property<float>(name))
                                update_subset_scalar_on_faces(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_face_property<double>(name))
                                update_subset_scalar_on_faces(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_face_property<int>(name))
                                update_subset_scalar_on_faces(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_face_property<unsigned int>(name))
                                update_subset_scalar_on_faces(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_face_property<char>(name))
                                update_subset_scalar_on_faces(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_face_property<unsigned char>(name))
                                update_subset_scalar_on_faces(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_face_property<bool>(name))
                                update_subset_scalar_on_faces(mesh, drawable, face_indices, prop);
                            else {
                                LOG(WARNING) << "scalar field '" << name
                                             << "' not found on faces (use uniform coloring)";
                                drawable->set_coloring_method(State::UNIFORM_COLOR);
                                update_subset_uniform_colors(mesh, drawable, face_indices);
                            }
                            return;
                        }
                        case State::VERTEX: {
                            if (auto prop = mesh->get_vertex_property<float>(name))
                                update_subset_scalar_on_vertices(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_vertex_property<double>(name))
                                update_subset_scalar_on_vertices(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_vertex_property<int>(name))
                                update_subset_scalar_on_vertices(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_vertex_property<unsigned int>(name))
                                update_subset_scalar_on_vertices(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_vertex_property<char>(name))
                                update_subset_scalar_on_vertices(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_vertex_property<unsigned char>(name))
                                update_subset_scalar_on_vertices(mesh, drawable, face_indices, prop);
                            else if (auto prop = mesh->get_vertex_property<bool>(name))
                                update_subset_scalar_on_vertices(mesh, drawable, face_indices, prop);
                            else {
                                LOG(WARNING) << "scalar field '" << name
                                             << "' not found on vertices (use uniform coloring)";
                                drawable->set_coloring_method(State::UNIFORM_COLOR);
                                update_subset_uniform_colors(mesh, drawable, face_indices);
                            }
                            return;
                        }
                        case State::EDGE:
                        case State::HALFEDGE:
                            LOG(WARNING) << "should not happen" << name;
                            break;
                    }
                    break;
                }

                case State::UNIFORM_COLOR:
                default:
                    drawable->set_coloring_method(State::UNIFORM_COLOR);
                    update_subset_uniform_colors(mesh, drawable, face_indices);
                    return;
            }

            drawable->set_coloring_method(State::UNIFORM_COLOR);
            update_subset_uniform_colors(mesh, drawable, face_indices);
        }

        std::string face_texture_drawable_name(const SurfaceMesh *model,
                                              std::size_t texture_index,
                                              std::unordered_set<std::string>& used_names) {
            std::string texture_name;
            if (model && texture_index < model->textures.size())
                texture_name = file_system::simple_name(model->textures[texture_index]);

            if (texture_name.empty())
                texture_name = "texture_" + std::to_string(texture_index);

            std::string name = "faces: " + texture_name;
            if (used_names.insert(name).second)
                return name;

            name += " (" + std::to_string(texture_index) + ")";
            used_names.insert(name);
            return name;
        }

        bool create_textured_face_drawables(Renderer *renderer, SurfaceMesh *model) {
            if (!renderer || !model || !model->is_triangle_mesh())
                return false;

            auto texcoords = model->get_halfedge_property<vec2>("h:texcoord");
            if (!texcoords || model->textures.empty())
                return false;

            std::vector<int> texture_indices;
            const std::vector<std::string> candidates = {
                    "f:texnumber", "f:texture_number", "f:texture_id", "f:textureid", "f:texid"
            };
            bool has_texture_index = false;
            for (const auto& property_name : candidates) {
                if (collect_face_texture_indices<int>(model, property_name, texture_indices) ||
                    collect_face_texture_indices<unsigned int>(model, property_name, texture_indices) ||
                    collect_face_texture_indices<char>(model, property_name, texture_indices) ||
                    collect_face_texture_indices<unsigned char>(model, property_name, texture_indices)) {
                    has_texture_index = true;
                    break;
                }
            }

            if (!has_texture_index) {
                if (model->textures.size() > 1) {
                    LOG(WARNING) << "multiple texture images found on surface mesh '" << model->name()
                                 << "' but no supported face texture index property (e.g. 'texnumber') exists";
                }
                return false;
            }

            bool zero_based = true;
            bool one_based = true;
            for (const auto idx : texture_indices) {
                zero_based = zero_based && idx >= 0 && idx < static_cast<int>(model->textures.size());
                one_based = one_based && idx >= 1 && idx <= static_cast<int>(model->textures.size());
            }

            int index_offset = 0;
            if (!zero_based && one_based)
                index_offset = 1;
            else if (!zero_based && !one_based) {
                LOG(WARNING) << "invalid face texture indices on surface mesh '" << model->name() << "'";
                return false;
            }

            std::vector<std::vector<int> > groups(model->textures.size());
            std::size_t face_pos = 0;
            for (auto face : model->faces()) {
                const int texture_index = texture_indices[face_pos++] - index_offset;
                if (texture_index >= 0 && texture_index < static_cast<int>(groups.size()))
                    groups[texture_index].push_back(face.idx());
            }

            std::unordered_set<std::string> used_drawable_names;
            std::size_t drawable_count = 0;
            for (std::size_t texture_index = 0; texture_index < groups.size(); ++texture_index) {
                const auto& faces = groups[texture_index];
                if (faces.empty())
                    continue;

                const std::string drawable_name = face_texture_drawable_name(model, texture_index, used_drawable_names);
                auto drawable = renderer->add_triangles_drawable(drawable_name);
                drawable->set_smooth_shading(setting::surface_mesh_faces_phong_shading);
                drawable->set_visible(setting::surface_mesh_faces_visible);
                drawable->set_color(setting::surface_mesh_faces_color);
                drawable->set_opacity(setting::surface_mesh_faces_opacity);

                auto face_indices = std::make_shared<std::vector<int> >(faces);
                drawable->set_update_func([face_indices](Model *m, Drawable *d) {
                    auto mesh = dynamic_cast<SurfaceMesh *>(m);
                    auto drawable = dynamic_cast<TrianglesDrawable *>(d);
                    if (!mesh || !drawable)
                        return;

                    update_subset_face_drawable(mesh, drawable, *face_indices);
                });

                if (auto tex = request_mesh_texture(model, model->textures[texture_index]))
                    drawable->set_texture_coloring(State::HALFEDGE, "h:texcoord", tex);
                else
                    drawable->set_uniform_coloring(setting::surface_mesh_faces_color);

                ++drawable_count;
            }

            return drawable_count > 0;
        }
    }

    Renderer::Renderer(Model* model, bool create)
            : visible_(true)
            , selected_(false)
    {
        model_ = model;
        if (model_ && create)
            create_default_drawables();
    }


    Renderer::~Renderer() {
        points_drawables_.clear();
        lines_drawables_.clear();
        triangles_drawables_.clear();
    }

    
    void Renderer::create_default_drawables() {
        if (dynamic_cast<PointCloud *>(model_)) {;
            auto vertices = add_points_drawable("vertices");
			vertices->set_visible(setting::point_cloud_vertices_visible);
			vertices->set_color(setting::point_cloud_vertices_color);
            vertices->set_impostor_type(setting::point_cloud_vertices_impostors ? PointsDrawable::SPHERE : PointsDrawable::PLAIN);
            vertices->set_point_size(setting::point_cloud_vertices_size);
            set_default_rendering_state(dynamic_cast<PointCloud *>(model_), vertices);
        } else if (dynamic_cast<SurfaceMesh *>(model_)) {
            auto mesh = dynamic_cast<SurfaceMesh *>(model_);
            if (!create_textured_face_drawables(this, mesh)) {
                auto faces = add_triangles_drawable("faces");
                faces->set_smooth_shading(setting::surface_mesh_faces_phong_shading);
                faces->set_visible(setting::surface_mesh_faces_visible);
                faces->set_color(setting::surface_mesh_faces_color);
                faces->set_opacity(setting::surface_mesh_faces_opacity);
                set_default_rendering_state(mesh, faces);
            }

			// vertices
			auto vertices = add_points_drawable("vertices");
			vertices->set_visible(setting::surface_mesh_vertices_visible);
			vertices->set_uniform_coloring(setting::surface_mesh_vertices_color);
			vertices->set_impostor_type(setting::surface_mesh_vertices_imposters ? PointsDrawable::SPHERE : PointsDrawable::PLAIN);
			vertices->set_point_size(setting::surface_mesh_vertices_size);

            // edges
            auto edges = add_lines_drawable("edges");
            edges->set_visible(setting::surface_mesh_edges_visible);
            edges->set_uniform_coloring(setting::surface_mesh_edges_color);
            edges->set_impostor_type(setting::surface_mesh_edges_imposters ? LinesDrawable::CYLINDER : LinesDrawable::PLAIN);
            edges->set_line_width(setting::surface_mesh_edges_size);

            // borders
            auto borders = add_lines_drawable("borders");
            borders->set_visible(setting::surface_mesh_borders_visible);
            borders->set_uniform_coloring(setting::surface_mesh_borders_color);
            borders->set_impostor_type(setting::surface_mesh_borders_imposters ? LinesDrawable::CYLINDER : LinesDrawable::PLAIN);
            borders->set_line_width(setting::surface_mesh_borders_size);

            auto locks_prop = dynamic_cast<SurfaceMesh *>(model_)->get_vertex_property<bool>("v:locked");
            if (locks_prop) {
                auto locks = add_points_drawable("locks");
                locks->set_uniform_coloring(vec4(1, 1, 0, 1.0f));
                locks->set_impostor_type(PointsDrawable::SPHERE);
                locks->set_point_size(setting::surface_mesh_vertices_size + 5);
            }
        } else if (dynamic_cast<Graph *>(model_)) {
            // create points drawable for the edges
            auto vertices = add_points_drawable("vertices");
            vertices->set_visible(setting::graph_vertices_visible);
            vertices->set_color(setting::graph_vertices_color);
            vertices->set_impostor_type(setting::graph_vertices_imposters ? PointsDrawable::SPHERE : PointsDrawable::PLAIN);
			vertices->set_point_size(setting::graph_vertices_size);
            set_default_rendering_state(dynamic_cast<Graph *>(model_), vertices);

            // create lines drawable for the edges
            auto edges = add_lines_drawable("edges");
            edges->set_visible(setting::graph_edges_visible);
            edges->set_color(setting::graph_edges_color);
            edges->set_impostor_type(setting::graph_edges_imposters ? LinesDrawable::CYLINDER : LinesDrawable::PLAIN);
			edges->set_line_width(setting::graph_edges_size);
            set_default_rendering_state(dynamic_cast<Graph *>(model_), edges);
		} else if (dynamic_cast<PolyMesh *>(model_)) {
            // we have two faces drawables for polyhedral meshes
            // border faces
            auto border_faces = add_triangles_drawable("faces:border");
			border_faces->set_visible(setting::poly_mesh_faces_visible);
            border_faces->set_uniform_coloring(setting::poly_mesh_faces_color);
            border_faces->set_distinct_back_color(true);
            border_faces->set_lighting_two_sides(true);

            // interior faces
            auto interior_faces = add_triangles_drawable("faces:interior");
			interior_faces->set_visible(setting::poly_mesh_faces_visible);
            interior_faces->set_uniform_coloring(setting::triangles_drawable_backside_color);
            interior_faces->set_distinct_back_color(true);
            interior_faces->set_lighting_two_sides(true);

			// vertices
			auto vertices = add_points_drawable("vertices");
			vertices->set_visible(setting::poly_mesh_vertices_visible);
			vertices->set_uniform_coloring(setting::poly_mesh_vertices_color);
			vertices->set_impostor_type(setting::poly_mesh_vertices_imposters ? PointsDrawable::SPHERE : PointsDrawable::PLAIN);
			vertices->set_point_size(setting::poly_mesh_vertices_size);

            // edges
            auto edges = add_lines_drawable("edges");
			edges->set_visible(setting::poly_mesh_edges_visible);
            edges->set_uniform_coloring(setting::poly_mesh_edges_color);
            edges->set_impostor_type(setting::poly_mesh_edges_imposters ? LinesDrawable::CYLINDER : LinesDrawable::PLAIN);
			edges->set_line_width(setting::poly_mesh_edges_size);
        }
    }


    void Renderer::set_default_rendering_state(PointCloud *model, PointsDrawable *drawable) {
        assert(model);
        assert(drawable);

        // Priorities:
        //     1. per-vertex color: in "v:color";
        //     2. per-vertex texture coordinates: in "v:texcoord";
        //     3. segmentation: in "v:primitive_index";
        //     4. scalar field;
        //     5: uniform color.

        // 1. per-vertex color: in "v:color"
        auto colors = model->get_vertex_property<vec3>("v:color");
        if (colors) {
            drawable->set_property_coloring(State::VERTEX, "v:color");
            return;
        }

        // 2. per-vertex texture coordinates: in "v:texcoord"
        auto texcoord = model->get_vertex_property<vec2>("v:texcoord");
        if (texcoord) {
            drawable->set_texture_coloring(State::VERTEX, "v:texcoord");
            return;
        }

        // 3. segmentation: in "v:primitive_index"
        auto primitive_index = model->get_vertex_property<int>("v:primitive_index");
        if (primitive_index) { // model has segmentation information
            drawable->set_scalar_coloring(State::VERTEX, "v:primitive_index");
            return;
        }

        // 4. scalar field
        const auto properties = model->vertex_properties();
        for (const auto& name : properties) {
            if (model->get_vertex_property<int>(name) || model->get_vertex_property<unsigned int>(name) ||
                model->get_vertex_property<float>(name)) {
                drawable->set_scalar_coloring(State::VERTEX, name);
                return;
            }
        }

        // 5: uniform color
        drawable->set_uniform_coloring(setting::point_cloud_vertices_color);
    }


    void Renderer::set_default_rendering_state(SurfaceMesh *model, TrianglesDrawable *drawable) {
        assert(model);
        assert(drawable);

        //  Priorities:
        //      1: per-face color
        //      2: per-vertex color
        //      3. per-halfedge texture coordinates
        //      4. per-vertex texture coordinates
        //      5. segmentation
        //      6. scalar field on faces
        //      7. scalar field on vertices
        //      8. uniform color

        // 1: per-face color
        auto face_colors = model->get_face_property<vec3>("f:color");
        if (face_colors) {
            drawable->set_property_coloring(State::FACE, "f:color");
            return;
        }

        // 2: per-vertex color
        auto vertex_colors = model->get_vertex_property<vec3>("v:color");
        if (vertex_colors) {
            drawable->set_property_coloring(State::VERTEX, "v:color");
            return;
        }

        // 3. per-halfedge texture coordinates
        auto halfedge_texcoords = model->get_halfedge_property<vec2>("h:texcoord");
        if (halfedge_texcoords) {
            drawable->set_texture_coloring(State::HALFEDGE, "h:texcoord", request_mesh_texture(model));
            return;
        }

        // 4. per-vertex texture coordinates
        auto vertex_texcoords = model->get_vertex_property<vec2>("v:texcoord");
        if (vertex_texcoords) {
            drawable->set_texture_coloring(State::VERTEX, "v:texcoord", request_mesh_texture(model));
            return;
        }

        // 5. segmentation
        auto segmentation = model->get_face_property<int>("f:chart");
        if (segmentation) {
            drawable->set_scalar_coloring(State::FACE, "f:chart");
            return;
        }

        // 6. scalar field on faces
        const auto face_properties = model->face_properties();
        for (const auto& name : face_properties) {
            if (model->get_face_property<int>(name) || model->get_face_property<unsigned int>(name) ||
                model->get_face_property<float>(name)) {
                drawable->set_scalar_coloring(State::FACE, name);
                return;
            }
        }

        // 7. scalar field on vertices
        const auto vertex_properties = model->vertex_properties();
        for (const auto& name : vertex_properties) {
            if (model->get_vertex_property<int>(name) || model->get_vertex_property<unsigned int>(name) ||
                model->get_vertex_property<float>(name)) {
                drawable->set_scalar_coloring(State::VERTEX, name);
                return;
            }
        }

        // 8. uniform color
        drawable->set_uniform_coloring(setting::surface_mesh_faces_color);
    }


    void Renderer::set_default_rendering_state(Graph *model, PointsDrawable *drawable) {
        assert(model);
        assert(drawable);

        // Priorities:
        //     1. per-vertex color: in "v:color";
        //     2. per-vertex texture coordinates: in "v:texcoord";
        //     3. scalar field;
        //     4: uniform color.

        // 1. per-vertex color: in "v:color"
        auto colors = model->get_vertex_property<vec3>("v:color");
        if (colors) {
            drawable->set_property_coloring(State::VERTEX, "v:color");
            return;
        }

        // 2. per-vertex texture coordinates: in "v:texcoord"
        auto texcoord = model->get_vertex_property<vec2>("v:texcoord");
        if (texcoord) {
            drawable->set_texture_coloring(State::VERTEX, "v:texcoord");
            return;
        }

        // 3. scalar field
        const auto properties = model->vertex_properties();
        for (const auto& name : properties) {
            if (model->get_vertex_property<int>(name) || model->get_vertex_property<unsigned int>(name) ||
                model->get_vertex_property<float>(name)) {
                drawable->set_scalar_coloring(State::VERTEX, name);
                return;
            }
        }

        // 4: uniform color
        drawable->set_uniform_coloring(setting::graph_vertices_color);
    }


    void Renderer::set_default_rendering_state(Graph *model, LinesDrawable *drawable) {
        assert(model);
        assert(drawable);

        // Priorities:
        //     1. per-edge color: in "e:color";
        //     2. per-edge texture coordinates: in "e:texcoord";
        //     3. scalar field on edges;
        //     4. uniform color.

        // 1. per-edge color: in "e:color"
        auto colors = model->get_edge_property<vec3>("e:color");
        if (colors) {
            drawable->set_property_coloring(State::EDGE, "e:color");
            return;
        }

        // 2. per-edge texture coordinates: in "e:texcoord"
        auto texcoord = model->get_edge_property<vec2>("e:texcoord");
        if (texcoord) {
            drawable->set_texture_coloring(State::EDGE, "e:texcoord");
            return;
        }

        // 3. scalar field on edges
        const auto properties = model->edge_properties();
        for (const auto& name : properties) {
            if (model->get_edge_property<int>(name) || model->get_edge_property<unsigned int>(name) ||
                model->get_edge_property<float>(name) || model->get_edge_property<double>(name) ||
                model->get_edge_property<char>(name) || model->get_edge_property<unsigned char>(name)) {
                drawable->set_scalar_coloring(State::EDGE, name);
                return;
            }
        }

        // 4: uniform color
        drawable->set_uniform_coloring(setting::graph_edges_color);
    }


    void Renderer::set_selected(bool b) {
        for (auto d : points_drawables_)
            d->set_selected(b);
        for (auto d : lines_drawables_)
            d->set_selected(b);
        for (auto d : triangles_drawables_)
            d->set_selected(b);
        selected_ = b;
    }


    void Renderer::update() {
        for (auto d : points_drawables_)
            d->update();
        for (auto d : lines_drawables_)
            d->update();
        for (auto d : triangles_drawables_)
            d->update();
    }


    PointsDrawable* Renderer::get_points_drawable(const std::string& name, bool warning_not_found) const {
        for (auto d : points_drawables_) {
            if (d->name() == name)
                return d.get();
        }
        LOG_IF(warning_not_found, WARNING) << "the requested drawable '" << name << "' does not exist (or not created)";
        return nullptr;
    }


    LinesDrawable* Renderer::get_lines_drawable(const std::string& name, bool warning_not_found) const {
        for (auto d : lines_drawables_) {
            if (d->name() == name)
                return d.get();
        }
        LOG_IF(warning_not_found, WARNING) << "the requested drawable '" << name << "' does not exist (or not created)";
        return nullptr;
    }


    TrianglesDrawable* Renderer::get_triangles_drawable(const std::string &name, bool warning_not_found) const {
        for (auto d : triangles_drawables_) {
            if (d->name() == name)
                return d.get();
        }
        LOG_IF(warning_not_found, WARNING) << "the requested drawable '" << name << "' does not exist (or not created)";
        return nullptr;
    }


    PointsDrawable* Renderer::add_points_drawable(const std::string& name) {
        for (auto d : points_drawables_) {
            if (d->name() == name) {
                LOG(ERROR) << "drawable already exists: " << name;
                return d.get();
            }
        }
        auto d = std::make_shared<PointsDrawable>(name);
        d->set_model(model_);
        points_drawables_.push_back(d);
        return d.get();
    }


    LinesDrawable* Renderer::add_lines_drawable(const std::string& name) {
        for (auto d : lines_drawables_) {
            if (d->name() == name) {
                LOG(ERROR) << "drawable already exists: " << name;
                return d.get();
            }
        }
        auto d = std::make_shared<LinesDrawable>(name);
        d->set_model(model_);
        lines_drawables_.push_back(d);

        // for PolyMesh, we want to completely discard vertices in the vertex buffer.
        if (dynamic_cast<PolyMesh*>(model_))
            d->set_plane_clip_discard_primitive(true);

        return d.get();
    }


    TrianglesDrawable* Renderer::add_triangles_drawable(const std::string& name) {
        for (auto d : triangles_drawables_) {
            if (d->name() == name) {
                LOG(ERROR) << "drawable already exists: " << name;
                return d.get();
            }
        }
        auto d = std::make_shared<TrianglesDrawable>(name);
        d->set_model(model_);
        triangles_drawables_.push_back(d);

        // for PolyMesh, we want to completely discard vertices in the vertex buffer.
        if (dynamic_cast<PolyMesh*>(model_))
            d->set_plane_clip_discard_primitive(true);

        return d.get();
    }

}
