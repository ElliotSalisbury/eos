/*
* eos - A 3D Morphable Model fitting library written in modern C++11/14.
*
* File: include/eos/fitting/linear_shape_fitting.hpp
*
* Copyright 2014, 2015 Patrik Huber
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
#pragma once

#ifndef LINEARSHAPEFITTING_HPP_
#define LINEARSHAPEFITTING_HPP_

#include "eos/morphablemodel/MorphableModel.hpp"

#include "Eigen/QR"
#include "Eigen/Sparse"

#include "opencv2/core/core.hpp"

#include "boost/optional.hpp"

#include <vector>
#include <cassert>

namespace eos {
	namespace fitting {

/**
 * Fits the shape of a Morphable Model to given 2D landmarks (i.e. estimates the maximum likelihood solution of the shape coefficients) as proposed in [1].
 * It's a linear, closed-form solution fitting of the shape, with regularisation (prior towards the mean).
 *
 * [1] O. Aldrian & W. Smith, Inverse Rendering of Faces with a 3D Morphable Model, PAMI 2013.
 *
 * Note: Using less than the maximum number of coefficients to fit is not thoroughly tested yet and may contain an error.
 * Note: Returns coefficients following standard normal distribution (i.e. all have similar magnitude). Why? Because we fit using the normalised basis?
 * Note: The standard deviations given should be a vector, i.e. different for each landmark. This is not implemented yet.
 *
 * @param[in] morphable_model The Morphable Model whose shape (coefficients) are estimated.
 * @param[in] affine_camera_matrix A 3x4 affine camera matrix from model to screen-space (should probably be of type CV_32FC1 as all our calculations are done with float).
 * @param[in] landmarks 2D landmarks from an image to fit the model to.
 * @param[in] vertex_ids The vertex ids in the model that correspond to the 2D points.
 * @param[in] base_face The base or reference face from where the fitting is started. Usually this would be the models mean face, which is what will be used if the parameter is not explicitly specified.
 * @param[in] lambda The regularisation parameter (weight of the prior towards the mean). Gets normalized by the number of images given.
 * @param[in] num_coefficients_to_fit How many shape-coefficients to fit (all others will stay 0). Should be bigger than zero, or boost::none to fit all coefficients.
 * @param[in] detector_standard_deviation The standard deviation of the 2D landmarks given (e.g. of the detector used), in pixels.
 * @param[in] model_standard_deviation The standard deviation of the 3D vertex points in the 3D model, projected to 2D (so the value is in pixels).
 * @return The estimated shape-coefficients (alphas).
 */
inline std::vector<float> fit_shape_to_landmarks_linear_multi(const morphablemodel::MorphableModel& morphable_model, std::vector<cv::Mat> affine_camera_matrix, std::vector<std::vector<cv::Vec2f>>& landmarks, std::vector<std::vector<int>>& vertex_ids, std::vector<Eigen::VectorXf> base_face=std::vector<Eigen::VectorXf>(), float lambda=3.0f, boost::optional<int> num_coefficients_to_fit=boost::optional<int>(), boost::optional<float> detector_standard_deviation=boost::optional<float>(), boost::optional<float> model_standard_deviation=boost::optional<float>())
{

	assert(affine_camera_matrix.size() == landmarks.size() && landmarks.size() == vertex_ids.size()); // same number of instances (i.e. images/frames) for each of them
	assert(landmarks.size() == vertex_ids.size());

	using Eigen::VectorXf;
	using Eigen::MatrixXf;

	int num_coeffs_to_fit = num_coefficients_to_fit.get_value_or(morphable_model.get_shape_model().get_num_principal_components());
	int num_images = affine_camera_matrix.size();

    // the regularisation has to be adjusted when more than one image is given
    lambda *= num_images;

	int total_num_landmarks_dimension = 0;
	for (auto&& l : landmarks) {
		total_num_landmarks_dimension += l.size();
	}

	// $\hat{V} \in R^{3N\times m-1}$, subselect the rows of the eigenvector matrix $V$ associated with the $N$ feature points
	// And we insert a row of zeros after every third row, resulting in matrix $\hat{V}_h \in R^{4N\times m-1}$:
    MatrixXf V_hat_h = MatrixXf::Zero(4 * total_num_landmarks_dimension, num_coeffs_to_fit);
	int V_hat_h_row_index = 0;
	// Form a block diagonal matrix $P \in R^{3N\times 4N}$ in which the camera matrix C (P_Affine, affine_camera_matrix) is placed on the diagonal:
    Eigen::SparseMatrix<float> P (3 * total_num_landmarks_dimension, 4 * total_num_landmarks_dimension);
    std::vector<Eigen::Triplet<float>> P_coefficients; // list of non-zeros coefficients
	int P_index = 0;
	// The variances: Add the 2D and 3D standard deviations.
	// If the user doesn't provide them, we choose the following:
	// 2D (detector) standard deviation: In pixel, we follow [1] and choose sqrt(3) as the default value.
	// 3D (model) variance: 0.0f. It only makes sense to set it to something when we have a different variance for different vertices.
	// The 3D variance has to be projected to 2D (for details, see paper [1]) so the units do match up.
	float sigma_squared_2D = std::pow(detector_standard_deviation.get_value_or(std::sqrt(3.0f)), 2) + std::pow(model_standard_deviation.get_value_or(0.0f), 2);
	// We use a VectorXf, and later use .asDiagonal():
	VectorXf Omega = VectorXf::Constant(3 * total_num_landmarks_dimension, 1.0f / sigma_squared_2D);
	// The landmarks in matrix notation (in homogeneous coordinates), $3N\times 1$
	VectorXf y = VectorXf::Ones(3 * total_num_landmarks_dimension);
	int y_index = 0; // also runs the same as P_index. Should rename to "running_index"?
	// The mean, with an added homogeneous coordinate (x_1, y_1, z_1, 1, x_2, ...)^t
	VectorXf v_bar = VectorXf::Ones(4 * total_num_landmarks_dimension);
	int v_bar_index = 0; // also runs the same as P_index. But be careful, if I change it to be only 1 variable, only increment it once! :-)
						 // Well I think that would make it a bit messy since we need to increment inside the for (landmarks...) loop. Try to refactor some other way.

    for (int k = 0; k < num_images; ++k)
    {
        // For each image we have, set up the equations and add it to the matrices:
        assert(landmarks[k].size() == vertex_ids[k].size()); // has to be valid for each img

        int num_landmarks = static_cast<int>(landmarks[k].size());

        if (base_face[k].size()==0)
        {
            base_face[k] = morphable_model.get_shape_model().get_mean();
        }

        // $\hat{V} \in R^{3N\times m-1}$, subselect the rows of the eigenvector matrix $V$ associated with the $N$ feature points
        // And we insert a row of zeros after every third row, resulting in matrix $\hat{V}_h \in R^{4N\times m-1}$:
        //Mat V_hat_h = Mat::zeros(4 * num_landmarks, num_coeffs_to_fit, CV_32FC1);
        for (int i = 0; i < num_landmarks; ++i) {
            MatrixXf basis_rows_ = morphable_model.get_shape_model().get_rescaled_pca_basis_at_point(vertex_ids[k][i]); // In the paper, the orthonormal basis might be used? I'm not sure, check it. It's even a mess in the paper. PH 26.5.2014: I think the rescaled basis is fine/better.
            V_hat_h.block(V_hat_h_row_index, 0, 3, V_hat_h.cols()) = basis_rows_.block(0, 0, basis_rows_.rows(), num_coeffs_to_fit);
            V_hat_h_row_index += 4; // replace 3 rows and skip the 4th one, it has all zeros
        }

        // Form a block diagonal matrix $P \in R^{3N\times 4N}$ in which the camera matrix C (P_Affine, affine_camera_matrix) is placed on the diagonal:
        for (int i = 0; i < num_landmarks; ++i) {
            for (int x = 0; x < affine_camera_matrix[k].rows; ++x){
                for (int y = 0; y < affine_camera_matrix[k].cols; ++y){
                    P_coefficients.push_back(Eigen::Triplet<float>(3 * P_index + x, 4 * P_index + y, affine_camera_matrix[k].at<float>(x,y)));
                }
            }
            ++P_index;
        }


        // The landmarks in matrix notation (in homogeneous coordinates), $3N\times 1$
        //Mat y = Mat::ones(3 * num_landmarks, 1, CV_32FC1);
        for (int i = 0; i < num_landmarks; ++i) {
            y(3 * y_index) = landmarks[k][i][0];
            y((3 * y_index) + 1) = landmarks[k][i][1];
            //y((3 * i) + 2) = 1; // already 1, stays (homogeneous coordinate)
            ++y_index;
        }
        // The mean, with an added homogeneous coordinate (x_1, y_1, z_1, 1, x_2, ...)^t
        //Mat v_bar = Mat::ones(4 * num_landmarks, 1, CV_32FC1);
        for (int i = 0; i < num_landmarks; ++i) {            
            v_bar(4 * v_bar_index) = base_face[k](vertex_ids[k][i] * 3);
            v_bar((4 * v_bar_index) + 1) = base_face[k](vertex_ids[k][i] * 3 + 1);
            v_bar((4 * v_bar_index) + 2) = base_face[k](vertex_ids[k][i] * 3 + 2);
            //v_bar.at<float>((4 * i) + 3) = 1; // already 1, stays (homogeneous coordinate)
            ++v_bar_index;
        }
    }

    // fill P with coefficients
    P.setFromTriplets(P_coefficients.begin(), P_coefficients.end());

	// Bring into standard regularised quadratic form with diagonal distance matrix Omega:
	const MatrixXf A = P * V_hat_h; // camera matrix times the basis
	const MatrixXf b = P * v_bar - y; // camera matrix times the mean, minus the landmarks
	const MatrixXf AtOmegaAReg = A.transpose() * Omega.asDiagonal() * A + lambda * Eigen::MatrixXf::Identity(num_coeffs_to_fit, num_coeffs_to_fit);
	const MatrixXf rhs = -A.transpose() * Omega.asDiagonal() * b; // It's -A^t*Omega^t*b, but we don't need to transpose Omega, since it's a diagonal matrix, and Omega^t = Omega.
	// c_s: The 'x' that we solve for. (The variance-normalised shape parameter vector, $c_s = [a_1/sigma_{s,1} , ..., a_m-1/sigma_{s,m-1}]^t$.)
	// We get coefficients ~ N(0, 1), because we're fitting with the rescaled basis. The coefficients are not multiplied with their eigenvalues.
	const VectorXf c_s = AtOmegaAReg.colPivHouseholderQr().solve(rhs);

    return std::vector<float>(c_s.data(), c_s.data() + c_s.size());
};


/**
 * Fits the shape of a Morphable Model to given 2D landmarks (i.e. estimates the maximum likelihood solution of the shape coefficients) as proposed in [1].
 * It's a linear, closed-form solution fitting of the shape, with regularisation (prior towards the mean).
 *
 * [1] O. Aldrian & W. Smith, Inverse Rendering of Faces with a 3D Morphable Model, PAMI 2013.
 *
 * Note: Using less than the maximum number of coefficients to fit is not thoroughly tested yet and may contain an error.
 * Note: Returns coefficients following standard normal distribution (i.e. all have similar magnitude). Why? Because we fit using the normalised basis?
 * Note: The standard deviations given should be a vector, i.e. different for each landmark. This is not implemented yet.
 *
 * @param[in] morphable_model The Morphable Model whose shape (coefficients) are estimated.
 * @param[in] affine_camera_matrix A 3x4 affine camera matrix from model to screen-space (should probably be of type CV_32FC1 as all our calculations are done with float).
 * @param[in] landmarks 2D landmarks from an image to fit the model to.
 * @param[in] vertex_ids The vertex ids in the model that correspond to the 2D points.
 * @param[in] base_face The base or reference face from where the fitting is started. Usually this would be the models mean face, which is what will be used if the parameter is not explicitly specified.
 * @param[in] lambda The regularisation parameter (weight of the prior towards the mean).
 * @param[in] num_coefficients_to_fit How many shape-coefficients to fit (all others will stay 0). Should be bigger than zero, or boost::none to fit all coefficients.
 * @param[in] detector_standard_deviation The standard deviation of the 2D landmarks given (e.g. of the detector used), in pixels.
 * @param[in] model_standard_deviation The standard deviation of the 3D vertex points in the 3D model, projected to 2D (so the value is in pixels).
 * @return The estimated shape-coefficients (alphas).
 */
inline std::vector<float> fit_shape_to_landmarks_linear(const morphablemodel::MorphableModel& morphable_model, cv::Mat affine_camera_matrix, std::vector<cv::Vec2f> landmarks, std::vector<int> vertex_ids, Eigen::VectorXf base_face=Eigen::VectorXf(), float lambda=3.0f, boost::optional<int> num_coefficients_to_fit=boost::optional<int>(), boost::optional<float> detector_standard_deviation=boost::optional<float>(), boost::optional<float> model_standard_deviation=boost::optional<float>())
{
    std::vector<std::vector<cv::Vec2f>> all_landmarks = {landmarks};
    std::vector<std::vector<int>> all_vertex_ids = {vertex_ids};
    return fit_shape_to_landmarks_linear_multi(morphable_model, { affine_camera_matrix }, all_landmarks, all_vertex_ids, { base_face }, lambda, num_coefficients_to_fit, detector_standard_deviation, model_standard_deviation );
}

	} /* namespace fitting */
} /* namespace eos */

#endif /* LINEARSHAPEFITTING_HPP_ */