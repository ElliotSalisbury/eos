/*
 * eos - A 3D Morphable Model fitting library written in modern C++11/14.
 *
 * File: python/generate-python-bindings.cpp
 *
 * Copyright 2016 Patrik Huber
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "eos/core/LandmarkMapper.hpp"
#include "eos/core/Mesh.hpp"
#include "eos/morphablemodel/PcaModel.hpp"
#include "eos/morphablemodel/MorphableModel.hpp"
#include "eos/morphablemodel/Blendshape.hpp"
#include "eos/morphablemodel/EdgeTopology.hpp"
#include "eos/fitting/contour_correspondence.hpp"
#include "eos/fitting/fitting.hpp"
#include "eos/fitting/orthographic_camera_estimation_linear.hpp"
#include "eos/fitting/RenderingParameters.hpp"
#include "eos/render/texture_extraction.hpp"

#include "opencv2/core/core.hpp"

#include "pybind11/pybind11.h"
#include "pybind11/stl.h"
#include "pybind11/eigen.h"
#include "pybind11_glm.hpp"
#include "pybind11_opencv.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cassert>

namespace py = pybind11;
using namespace eos;

/**
 * Generate python bindings for the eos library using pybind11.
 */
PYBIND11_PLUGIN(eos) {
    py::module eos_module("eos", "Python bindings for the eos 3D Morphable Face Model fitting library.\n\nFor an overview of the functionality, see the documentation of the submodules. For the full documentation, see the C++ doxygen documentation.");

	/**
	 * Bindings for the eos::core namespace:
	 *  - LandmarkMapper
	 *  - Mesh
	 *  - write_obj()
	 */
	py::module core_module = eos_module.def_submodule("core", "Essential functions and classes to work with 3D face models and landmarks.");
	py::class_<core::LandmarkMapper>(core_module, "LandmarkMapper", "Represents a mapping from one kind of landmarks to a different format(e.g.model vertices).")
		.def(py::init<>(), "Constructs a new landmark mapper that performs an identity mapping, that is, its output is the same as the input.")
		.def("__init__", [](core::LandmarkMapper& instance, std::string filename) { // wrap the fs::path c'tor with std::string
				new (&instance) core::LandmarkMapper(filename);
			}, "Constructs a new landmark mapper from a file containing mappings from one set of landmark identifiers to another.", py::arg("filename"))
		.def("convert", [](const core::LandmarkMapper& instance, std::string landmarkname) {
				return instance.convert(landmarkname).get_value_or("");
			}, "Converts the given landmark name to the mapped name")
		;

	py::class_<core::Mesh>(core_module, "Mesh", "This class represents a 3D mesh consisting of vertices, vertex colour information and texture coordinates.")
		.def_readwrite("vertices", &core::Mesh::vertices, "Vertices")
		.def_readwrite("tvi", &core::Mesh::tvi, "Triangle vertex indices")
		.def_readwrite("colors", &core::Mesh::colors, "Colour data")
		.def_readwrite("tci", &core::Mesh::tci, "Triangle colour indices (usually the same as tvi)")
		.def_readwrite("texcoords", &core::Mesh::texcoords, "Texture coordinates")
		.def("__getstate__", [](const core::Mesh &p) {
			/* Return a tuple that fully encodes the state of the object */
			return py::make_tuple(p.vertices, p.colors, p.texcoords, p.tvi, p.tci);
		})
		.def("__setstate__", [](core::Mesh &p, py::tuple t) {
			if (t.size() != 5)
				throw std::runtime_error("Invalid state!");
			/* Invoke the in-place constructor. Note that this is needed even
			when the object just has a trivial default constructor */
			new (&p) core::Mesh();
			p.vertices = t[0].cast<std::vector<glm::vec4>>();
			p.colors = t[1].cast<std::vector<glm::vec3>>();
			p.texcoords = t[2].cast<std::vector<glm::vec2>>();

			p.tvi = t[3].cast<std::vector<std::array<int, 3>>>();
			p.tci = t[4].cast<std::vector<std::array<int, 3>>>();
		});

	core_module.def("write_obj", &core::write_obj, "Writes the given Mesh to an obj file.", py::arg("mesh"), py::arg("filename"));

	/**
	 * Bindings for the eos::morphablemodel namespace:
	 *  - PcaModel
	 *  - MorphableModel
	 *  - load_model(), save_model()
	 *  - load_pca_model(), save_pca_model()
	 */
	py::module morphablemodel_module = eos_module.def_submodule("morphablemodel", "Functionality to represent a Morphable Model, its PCA models, and functions to load models and blendshapes.");

	py::class_<morphablemodel::PcaModel>(morphablemodel_module, "PcaModel", "Class representing a PcaModel with a mean, eigenvectors and eigenvalues, as well as a list of triangles to build a mesh.")
		.def(py::init<>(), "Creates an empty model.")
		.def(py::init<Eigen::VectorXf, Eigen::MatrixXf, Eigen::VectorXf, std::vector<std::array<int, 3>>>(), "Construct a PCA model from given mean, orthonormal PCA basis, eigenvalues and triangle list.", py::arg("mean"), py::arg("orthonormal_pca_basis"), py::arg("eigenvalues"), py::arg("triangle_list"))
		.def("get_num_principal_components", &morphablemodel::PcaModel::get_num_principal_components, "Returns the number of principal components in the model.")
		.def("get_data_dimension", &morphablemodel::PcaModel::get_data_dimension, "Returns the dimension of the data, i.e. the number of shape dimensions.")
		.def("get_triangle_list", &morphablemodel::PcaModel::get_triangle_list, "Returns a list of triangles on how to assemble the vertices into a mesh.")
		.def("get_mean", &morphablemodel::PcaModel::get_mean, "Returns the mean of the model.")
		.def("get_mean_at_point", &morphablemodel::PcaModel::get_mean_at_point, "Return the value of the mean at a given vertex index.", py::arg("vertex_index"))
		.def("get_orthonormal_pca_basis", [](const morphablemodel::PcaModel& m) { return m.get_orthonormal_pca_basis(); }, "Returns the orthonormal PCA basis matrix, i.e. the eigenvectors. Each column of the matrix is an eigenvector.") // use py::overload in VS2017
		.def("get_rescaled_pca_basis", [](const morphablemodel::PcaModel& m) { return m.get_rescaled_pca_basis(); }, "Returns the rescaled PCA basis matrix, i.e. the eigenvectors. Each column of the matrix is an eigenvector, and each eigenvector has been rescaled by multiplying it with the square root of its eigenvalue.") // use py::overload in VS2017
		.def("get_eigenvalues", &morphablemodel::PcaModel::get_eigenvalues, "Returns the models eigenvalues.")
		.def("draw_sample", (Eigen::VectorXf(morphablemodel::PcaModel::*)(std::vector<float>) const)&morphablemodel::PcaModel::draw_sample, "Returns a sample from the model with the given PCA coefficients. The given coefficients should follow a standard normal distribution, i.e. not be scaled with their eigenvalues/variances.", py::arg("coefficients"))
		;

	py::class_<morphablemodel::MorphableModel>(morphablemodel_module, "MorphableModel", "A class representing a 3D Morphable Model, consisting of a shape- and colour (albedo) PCA model, as well as texture (uv) coordinates.")
		.def(py::init<morphablemodel::PcaModel, morphablemodel::PcaModel, std::vector<std::array<double, 2>>>(), "Create a Morphable Model from a shape and a colour PCA model, and optional texture coordinates.", py::arg("shape_model"), py::arg("color_model"), py::arg("texture_coordinates") = std::vector<std::array<double, 2>>())
		.def("get_shape_model", [](const morphablemodel::MorphableModel& m) { return m.get_shape_model(); }, "Returns the PCA shape model of this Morphable Model.") // Not sure if that'll really be const in Python? I think Python does a copy each time this gets called?
		.def("get_color_model", [](const morphablemodel::MorphableModel& m) { return m.get_color_model(); }, "Returns the PCA colour (albedo) model of this Morphable Model.") // (continued from above:) We may want to use py::overload, but in any case, we need to tell pybind11 if it should use the const or non-const overload.
		.def("get_mean", &morphablemodel::MorphableModel::get_mean, "Returns the mean of the shape- and colour model as a Mesh.")
		.def("draw_sample", (core::Mesh(morphablemodel::MorphableModel::*)(std::vector<float>, std::vector<float>) const)&morphablemodel::MorphableModel::draw_sample, "Returns a sample from the model with the given shape- and colour PCA coefficients.", py::arg("shape_coefficients"), py::arg("color_coefficients"))
		.def("has_color_model", &morphablemodel::MorphableModel::has_color_model, "Returns true if this Morphable Model contains a colour model, and false if it is a shape-only model.")
		.def("get_texture_coordinates", &morphablemodel::MorphableModel::get_texture_coordinates, "Returns the texture coordinates for all the vertices in the model.")
		;

	morphablemodel_module.def("load_model", &morphablemodel::load_model, "Load a Morphable Model from a cereal::BinaryInputArchive (.bin) from the harddisk.", py::arg("filename"));
	morphablemodel_module.def("save_model", &morphablemodel::save_model, "Save a Morphable Model as cereal::BinaryOutputArchive.", py::arg("model"), py::arg("filename"));
	morphablemodel_module.def("load_pca_model", &morphablemodel::load_pca_model, "Load a PCA model from a cereal::BinaryInputArchive (.bin) from the harddisk.", py::arg("filename"));
	morphablemodel_module.def("save_pca_model", &morphablemodel::save_pca_model, "Save a PCA model as cereal::BinaryOutputArchive.", py::arg("model"), py::arg("filename"));

	/**
	 *  - Blendshape
	 *  - load_blendshapes()
     *  - draw_sample()
	 */
	py::class_<morphablemodel::Blendshape>(morphablemodel_module, "Blendshape", "A class representing a 3D blendshape.")
		.def_readwrite("name", &morphablemodel::Blendshape::name, "Name of the blendshape.")
		.def_readwrite("deformation", &morphablemodel::Blendshape::deformation, "A 3m x 1 col-vector (xyzxyz...)', where m is the number of model-vertices. Has the same format as PcaModel::mean.")
		;

	morphablemodel_module.def("load_blendshapes", &morphablemodel::load_blendshapes, "Load a file with blendshapes from a cereal::BinaryInputArchive (.bin) from the harddisk.", py::arg("filename"));

    morphablemodel_module.def("draw_sample", [](const morphablemodel::MorphableModel& morphable_model, const std::vector<morphablemodel::Blendshape>& blendshapes, const std::vector<float>& shape_coefficients, const std::vector<float>& blendshape_coefficients, const std::vector<float>& color_coefficients) {
            // Helper function - draws a sample with given shape, blendshape and colour coefficients, and
            // returns a mesh. This is quite useful and would be worth having in the C++ API too.
            // If no colour coefficients are given, the resulting mesh won't have colour information.
            // Add expressions if both blendshapes and coefficients are given, otherwise just use the PCA model sample:
            Eigen::VectorXf shape;
            if (blendshape_coefficients.size() > 0 && blendshapes.size() > 0)
            {
                shape = morphable_model.get_shape_model().draw_sample(shape_coefficients) + morphablemodel::to_matrix(blendshapes) * Eigen::Map<const Eigen::VectorXf>(blendshape_coefficients.data(), blendshape_coefficients.size());
            }
            else {
                shape = morphable_model.get_shape_model().draw_sample(shape_coefficients);
            }
            // Draw sample from colour model if color_coefficients given, otherwise set to empty:
            const Eigen::VectorXf albedo = color_coefficients.size() > 0 ? morphable_model.get_color_model().draw_sample(color_coefficients) : Eigen::VectorXf();
            return morphablemodel::sample_to_mesh(shape, albedo, morphable_model.get_shape_model().get_triangle_list(), {}, morphable_model.get_texture_coordinates());
        }, "Draws a sample with given shape, blendshape and colour coeffs, and returns a mesh.", py::arg("morphable_model"), py::arg("blendshapes"), py::arg("shape_coefficients"), py::arg("blendshape_coefficients"), py::arg("color_coefficients"));

	/**
	 *  - EdgeTopology
	 *  - load_edge_topology()
	 */
	py::class_<morphablemodel::EdgeTopology>(morphablemodel_module, "EdgeTopology", "A struct containing a 3D shape model's edge topology.");

	morphablemodel_module.def("load_edge_topology", &morphablemodel::load_edge_topology, "Load a 3DMM edge topology file from a json file.", py::arg("filename"));

	/**
	 * Bindings for the eos::fitting namespace:
	 *  - ScaledOrthoProjectionParameters
	 *  - RenderingParameters
	 *  - estimate_orthographic_projection_linear()
	 *  - ContourLandmarks
	 *  - ModelContour
	 *  - fit_shape_and_pose()
	 */
	py::module fitting_module = eos_module.def_submodule("fitting", "Pose and shape fitting of a 3D Morphable Model.");

	py::class_<fitting::ScaledOrthoProjectionParameters>(fitting_module, "ScaledOrthoProjectionParameters", "Parameters of an estimated scaled orthographic projection.")
		.def_readwrite("R", &fitting::ScaledOrthoProjectionParameters::R, "Rotation matrix")
		.def_readwrite("s", &fitting::ScaledOrthoProjectionParameters::s, "Scale")
		.def_readwrite("tx", &fitting::ScaledOrthoProjectionParameters::tx, "x translation")
		.def_readwrite("ty", &fitting::ScaledOrthoProjectionParameters::ty, "y translation")
		;

	py::class_<fitting::RenderingParameters>(fitting_module, "RenderingParameters", "Represents a set of estimated model parameters (rotation, translation) and camera parameters (viewing frustum).")
		.def(py::init<fitting::ScaledOrthoProjectionParameters, int, int>(), "Create a RenderingParameters object from an instance of estimated ScaledOrthoProjectionParameters.")
		.def("get_rotation", [](const fitting::RenderingParameters& p) { return glm::vec4(p.get_rotation().x, p.get_rotation().y, p.get_rotation().z, p.get_rotation().w); }, "Returns the rotation quaternion [x y z w].")
		.def("get_rotation_euler_angles", [](const fitting::RenderingParameters& p) { return glm::eulerAngles(p.get_rotation()); }, "Returns the rotation's Euler angles (in radians) as [pitch, yaw, roll].")
		.def("get_modelview", &fitting::RenderingParameters::get_modelview, "Returns the 4x4 model-view matrix.")
		.def("get_projection", &fitting::RenderingParameters::get_projection, "Returns the 4x4 projection matrix.")
		.def("__getstate__", [](const fitting::RenderingParameters &p) {
			/* Return a tuple that fully encodes the state of the object */			
			int cameraType = 0;
			if (p.get_camera_type() == fitting::CameraType::Perspective) {
				cameraType = 1;
			}

			fitting::Frustum frustum = p.get_frustum();
			pybind11::tuple frustumTuple = py::make_tuple(frustum.l, frustum.r, frustum.b, frustum.t);

			glm::quat rotation = p.get_rotation();
			pybind11::tuple rotationTuple = py::make_tuple(rotation.x, rotation.y, rotation.z, rotation.w);

			return py::make_tuple(cameraType, frustumTuple, rotationTuple, p.get_t_x(), p.get_t_y(), p.get_screen_width(), p.get_screen_height());
		})
			.def("__setstate__", [](fitting::RenderingParameters &p, py::tuple t) {
			if (t.size() != 7)
				throw std::runtime_error("Invalid state!");
			/* Invoke the in-place constructor. Note that this is needed even
			when the object just has a trivial default constructor */

			fitting::CameraType cameraType = fitting::CameraType::Orthographic;
			if (t[0].cast<int>() == 1) {
				cameraType = fitting::CameraType::Perspective;
			}

			pybind11::tuple frustumTuple = t[1].cast<pybind11::tuple>();
			fitting::Frustum frustum(frustumTuple[0].cast<float>(), frustumTuple[1].cast<float>(), frustumTuple[2].cast<float>(), frustumTuple[3].cast<float>());

			pybind11::tuple rotationTuple = t[2].cast<pybind11::tuple>();
			glm::quat rotation(rotationTuple[3].cast<float>(), rotationTuple[0].cast<float>(), rotationTuple[1].cast<float>(), rotationTuple[2].cast<float>());

			new (&p) fitting::RenderingParameters(cameraType, frustum, rotation, t[3].cast<float>(), t[4].cast<float>(), t[5].cast<int>(), t[6].cast<int>());
		});
		;

	fitting_module.def("estimate_orthographic_projection_linear", [](std::vector<cv::Vec2f> image_points, std::vector<cv::Vec4f> model_points, bool is_viewport_upsidedown, int viewport_height) {
			const boost::optional<int> viewport_height_opt = viewport_height == 0 ? boost::none : boost::optional<int>(viewport_height);
			return fitting::estimate_orthographic_projection_linear(image_points, model_points, is_viewport_upsidedown, viewport_height_opt);
		}, "This algorithm estimates the parameters of a scaled orthographic projection, given a set of corresponding 2D-3D points.", py::arg("image_points"), py::arg("model_points"), py::arg("is_viewport_upsidedown"), py::arg("viewport_height") = 0)
		;

	py::class_<fitting::ContourLandmarks>(fitting_module, "ContourLandmarks", "Defines which 2D landmarks comprise the right and left face contour.")
		.def_static("load", &fitting::ContourLandmarks::load, "Helper method to load contour landmarks from a text file with landmark mappings, like ibug_to_sfm.txt.", py::arg("filename"))
		;

	py::class_<fitting::ModelContour>(fitting_module, "ModelContour", "Definition of the vertex indices that define the right and left model contour.")
		.def_static("load", &fitting::ModelContour::load, "Helper method to load a ModelContour from a json file from the hard drive.", py::arg("filename"))
		;
	
	fitting_module.def("fit_shape_and_pose", [](const morphablemodel::MorphableModel& morphable_model, const std::vector<morphablemodel::Blendshape>& blendshapes, const std::vector<std::vector<glm::vec2>>& landmarkss, const std::vector<std::string>& landmark_ids, const core::LandmarkMapper& landmark_mapper, std::vector<int> image_widths, std::vector<int> image_heights, const morphablemodel::EdgeTopology& edge_topology, const fitting::ContourLandmarks& contour_landmarks, const fitting::ModelContour& model_contour, int num_iterations, int num_shape_coefficients_to_fit, float lambda, std::vector<float> pca_shape_coefficients, std::vector<std::vector<float>> blendshape_coefficients) {
		assert(landmarkss.size() > 0 && landmarkss[0].size() == landmark_ids.size());
		std::vector<std::vector<cv::Vec2f>> fitted_image_points;
		// We can change this to std::optional as soon as we switch to VS2017 and pybind supports std::optional
		const boost::optional<int> num_shape_coefficients_opt = num_shape_coefficients_to_fit == -1 ? boost::none : boost::optional<int>(num_shape_coefficients_to_fit);
		std::vector<core::LandmarkCollection<cv::Vec2f>> landmark_collections;
		for (int j = 0; j < landmarkss.size(); ++j) {
			core::LandmarkCollection<cv::Vec2f > landmark_collection;
			for (int i = 0; i < landmarkss[j].size(); ++i)
			{
				landmark_collection.push_back(core::Landmark<cv::Vec2f>{ landmark_ids[i], cv::Vec2f(landmarkss[j][i].x, landmarkss[j][i].y) });
			}
			landmark_collections.push_back(landmark_collection);
		}

		auto result = fitting::fit_shape_and_pose_multi(morphable_model, blendshapes, landmark_collections, landmark_mapper, image_widths, image_heights, edge_topology, contour_landmarks, model_contour, num_iterations, num_shape_coefficients_opt, lambda, boost::none, pca_shape_coefficients, blendshape_coefficients, fitted_image_points);
		return std::make_tuple(result.first, result.second, pca_shape_coefficients, blendshape_coefficients);
	}, "Fit the pose (camera), shape model, and expression blendshapes to landmarks, in an iterative way. Returns a tuple (mesh, rendering_parameters, shape_coefficients, blendshape_coefficients).", py::arg("morphable_model"), py::arg("blendshapes"), py::arg("landmarks"), py::arg("landmark_ids"), py::arg("landmark_mapper"), py::arg("image_width"), py::arg("image_height"), py::arg("edge_topology"), py::arg("contour_landmarks"), py::arg("model_contour"), py::arg("num_iterations") = 5, py::arg("num_shape_coefficients_to_fit") = -1, py::arg("lambda") = 30.0f, py::arg("pca_shape_coefficients") = std::vector<float>(), py::arg("blendshape_coefficients") = std::vector<std::vector<float>>())
		;
	fitting_module.def("fit_pose", [](const morphablemodel::MorphableModel& morphable_model, std::vector<glm::vec2>& landmarks, const std::vector<std::string>& landmark_ids, const core::LandmarkMapper& landmark_mapper, int image_width, int image_height, std::vector<float> pca_shape_coefficients, const std::vector<morphablemodel::Blendshape>& blendshapes, std::vector<float> blendshape_coefficients) {
		assert(landmarks.size() == landmark_ids.size());
		
		core::LandmarkCollection<cv::Vec2f > landmark_collection;
		for (int i = 0; i < landmarks.size(); ++i)
		{
			landmark_collection.push_back(core::Landmark<cv::Vec2f>{ landmark_ids[i], cv::Vec2f(landmarks[i].x, landmarks[i].y) });
		}

		auto result = fitting::fit_pose(morphable_model, blendshapes, landmark_collection, landmark_mapper, image_width, image_height, pca_shape_coefficients, blendshape_coefficients);
		return result;
	}, "Fit the pose (camera) to landmarks. Returns the rendering_parameters.", py::arg("morphable_model"), py::arg("landmarks"), py::arg("landmark_ids"), py::arg("landmark_mapper"), py::arg("image_width"), py::arg("image_height"), py::arg("pca_shape_coefficients"), py::arg("blendshapes") = std::vector<morphablemodel::Blendshape>(), py::arg("blendshape_coefficients") = std::vector<float>())
		;

	/**
	 * Bindings for the eos::render namespace:
	 *  - extract_texture()
	 */
	py::module render_module = eos_module.def_submodule("render", "3D mesh and texture extraction functionality.");
        
	render_module.def("extract_texture", [](const core::Mesh& mesh, const fitting::RenderingParameters& rendering_params, cv::Mat image, bool compute_view_angle, int isomap_resolution) {
		cv::Mat affine_from_ortho = fitting::get_3x4_affine_camera_matrix(rendering_params, image.cols, image.rows);
		return render::extract_texture(mesh, affine_from_ortho, image, compute_view_angle, render::TextureInterpolation::NearestNeighbour, isomap_resolution);
	}, "Extracts the texture of the face from the given image and stores it as isomap (a rectangular texture map).", py::arg("mesh"), py::arg("rendering_params"), py::arg("image"), py::arg("compute_view_angle") = false, py::arg("isomap_resolution") = 512);

    return eos_module.ptr();
};
