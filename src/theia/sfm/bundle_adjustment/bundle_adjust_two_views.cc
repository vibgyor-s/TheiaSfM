// Copyright (C) 2015 The Regents of the University of California (Regents).
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//
//     * Neither the name of The Regents or University of California nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Please contact the author of this library if you have any questions.
// Author: Chris Sweeney (cmsweeney@cs.ucsb.edu)

#include "theia/sfm/bundle_adjustment/bundle_adjust_two_views.h"

#include <ceres/ceres.h>
#include <Eigen/Core>
#include <vector>

#include "theia/util/timer.h"
#include "theia/sfm/bundle_adjustment/angular_epipolar_error.h"
#include "theia/sfm/bundle_adjustment/bundle_adjustment.h"
#include "theia/sfm/bundle_adjustment/unit_norm_three_vector_parameterization.h"
#include "theia/sfm/camera/camera.h"
#include "theia/sfm/camera/camera_intrinsics.h"
#include "theia/sfm/camera/reprojection_error.h"
#include "theia/matching/feature_correspondence.h"
#include "theia/sfm/triangulation/triangulation.h"
#include "theia/sfm/twoview_info.h"
#include "theia/sfm/types.h"

namespace theia {

namespace {

void SetSolverOptions(const BundleAdjustmentOptions& options,
                      ceres::Solver::Options* solver_options) {
  CHECK_NOTNULL(solver_options);
  solver_options->linear_solver_type = ceres::DENSE_SCHUR;
  solver_options->visibility_clustering_type = ceres::SINGLE_LINKAGE;
  solver_options->logging_type = ceres::SILENT;
  solver_options->num_threads = options.num_threads;
  solver_options->num_linear_solver_threads = options.num_threads;
  solver_options->max_num_iterations = 200;
  // Solver options takes ownership of the ordering so that we can order the BA
  // problem by points and cameras.
  solver_options->linear_solver_ordering.reset(
      new ceres::ParameterBlockOrdering);
}

void SetCamera(const double focal_length,
               const double principal_point[2],
               const Eigen::Vector3d& position,
               const Eigen::Vector3d& rotation,
               Camera* camera) {
  camera->SetFocalLength(focal_length);
  camera->SetPrincipalPoint(principal_point[0],
                             principal_point[1]);
  camera->SetPosition(position);
  camera->SetOrientationFromAngleAxis(rotation);
}

void AddCameraParametersToProblem(const bool constant_camera_parameters,
                                  double* camera_parameters,
                                  ceres::Problem* problem) {
  // Keep the intrinsic parameters constant.
  std::vector<int> constant_intrinsics;
  for (int i = 0; i < Camera::kIntrinsicsSize; i++) {
    constant_intrinsics.push_back(Camera::kExtrinsicsSize + i);
  }
  ceres::SubsetParameterization* subset_parameterization =
      new ceres::SubsetParameterization(Camera::kParameterSize,
                                        constant_intrinsics);
  problem->AddParameterBlock(camera_parameters, Camera::kParameterSize,
                             subset_parameterization);

  if (constant_camera_parameters) {
    problem->SetParameterBlockConstant(camera_parameters);
  }
}

void TriangulatePoints(
    const Camera& camera1,
    const Camera& camera2,
    const std::vector<FeatureCorrespondence>& correspondences,
    std::vector<FeatureCorrespondence>* triangulated_matches,
    std::vector<Eigen::Vector4d>* tracks) {
  Matrix3x4d projection_matrix1, projection_matrix2;
  camera1.GetProjectionMatrix(&projection_matrix1);
  camera2.GetProjectionMatrix(&projection_matrix2);

  triangulated_matches->reserve(correspondences.size());
  tracks->reserve(correspondences.size());
  for (int i = 0; i < correspondences.size(); i++) {
    Eigen::Vector4d point;

    if (Triangulate(projection_matrix1,
                    projection_matrix2,
                    correspondences[i].feature1,
                    correspondences[i].feature2,
                    &point)) {
      tracks->emplace_back(point);
      triangulated_matches->emplace_back(correspondences[i]);
    }
  }
}

}  // namespace

// Triangulates all 3d points and performs standard bundle adjustment on the
// points and cameras.
BundleAdjustmentSummary BundleAdjustTwoViews(
    const BundleAdjustmentOptions& options,
    const std::vector<FeatureCorrespondence>& correspondences,
    const CameraIntrinsics& intrinsics1,
    const CameraIntrinsics& intrinsics2,
    TwoViewInfo* info) {
  CHECK_NOTNULL(info);

  BundleAdjustmentSummary summary;

  // Start setup timer.
  Timer timer;

  // Set problem options.
  ceres::Problem::Options problem_options;
  ceres::Problem problem(problem_options);

  // Set solver options.
  ceres::Solver::Options solver_options;
  SetSolverOptions(options, &solver_options);
  ceres::ParameterBlockOrdering* parameter_ordering =
      solver_options.linear_solver_ordering.get();

  // Set cameras from two view info.
  Camera camera1, camera2;
  SetCamera(info->focal_length_1,
            intrinsics1.principal_point,
            Eigen::Vector3d::Zero(),
            Eigen::Vector3d::Zero(),
            &camera1);
  SetCamera(info->focal_length_2,
            intrinsics2.principal_point,
            info->position_2,
            info->rotation_2,
            &camera2);

  // Add the two cameras as parameter blocks.
  AddCameraParametersToProblem(true, camera1.mutable_parameters(), &problem);
  AddCameraParametersToProblem(false, camera2.mutable_parameters(), &problem);
  parameter_ordering->AddElementToGroup(camera1.mutable_parameters(), 1);
  parameter_ordering->AddElementToGroup(camera2.mutable_parameters(), 1);

  // Triangulate all features.
  std::vector<FeatureCorrespondence> triangulated_matches;
  std::vector<Eigen::Vector4d> tracks;
  TriangulatePoints(camera1,
                    camera2,
                    correspondences,
                    &triangulated_matches,
                    &tracks);

  // Add triangulated points to the problem.
  for (int i = 0; i < triangulated_matches.size(); i++) {
    const FeatureCorrespondence& match = triangulated_matches[i];
    problem.AddResidualBlock(
        ReprojectionError::Create(match.feature1),
        NULL,
        camera1.mutable_parameters(),
        tracks[i].data());
    problem.AddResidualBlock(
        ReprojectionError::Create(match.feature2),
        NULL,
        camera2.mutable_parameters(),
        tracks[i].data());

    parameter_ordering->AddElementToGroup(tracks[i].data(), 0);
  }

  // End setup time.
  summary.setup_time_in_seconds = timer.ElapsedTimeInSeconds();

  // Solve the problem.
  ceres::Solver::Summary solver_summary;
  ceres::Solve(solver_options, &problem, &solver_summary);
  LOG_IF(INFO, options.verbose) << solver_summary.FullReport();

  // Set the BundleAdjustmentSummary.
  summary.solve_time_in_seconds = solver_summary.total_time_in_seconds;
  summary.initial_cost = solver_summary.initial_cost;
  summary.final_cost = solver_summary.final_cost;

  // This only indicates whether the optimization was successfully run and makes
  // no guarantees on the quality or convergence.
  summary.success = solver_summary.termination_type != ceres::FAILURE;

  // Update the relative pose.
  info->rotation_2 = camera2.GetOrientationAsAngleAxis();
  info->position_2 = camera2.GetPosition();
  info->position_2.normalize();

  return summary;
}

BundleAdjustmentSummary BundleAdjustTwoViewsAngular(
    const BundleAdjustmentOptions& options,
    const std::vector<FeatureCorrespondence>& correspondences,
    TwoViewInfo* info) {
  CHECK_NOTNULL(info);

  BundleAdjustmentSummary summary;

  // Start setup timer.
  Timer timer;

  // Set problem options.
  ceres::Problem::Options problem_options;

  ceres::Problem problem(problem_options);

  // Set solver options.
  ceres::Solver::Options solver_options;
  SetSolverOptions(options, &solver_options);
  // Allow Ceres to determine the ordering.
  solver_options.linear_solver_ordering.reset();

  // Add the relative rotation as a parameter block.
  const int kParameterBlockSize = 3;
  problem.AddParameterBlock(info->rotation_2.data(), kParameterBlockSize);
  // Add the position as a parameter block, ensuring that the norm is 1.
  ceres::LocalParameterization* position_parameterization =
      new ceres::AutoDiffLocalParameterization<
          UnitNormThreeVectorParameterization, 3, 3>;
  problem.AddParameterBlock(info->position_2.data(),
                            kParameterBlockSize,
                            position_parameterization);

  // Add all the epipolar constraints from feature matches.
  for (const FeatureCorrespondence& match : correspondences) {
    problem.AddResidualBlock(
        AngularEpipolarError::Create(match.feature1, match.feature2),
        NULL,
        info->rotation_2.data(),
        info->position_2.data());
  }

  // End setup time.
  summary.setup_time_in_seconds = timer.ElapsedTimeInSeconds();

  // Solve the problem.
  ceres::Solver::Summary solver_summary;
  ceres::Solve(solver_options, &problem, &solver_summary);
  LOG_IF(INFO, options.verbose) << solver_summary.FullReport();

  // Set the BundleAdjustmentSummary.
  summary.solve_time_in_seconds = solver_summary.total_time_in_seconds;
  summary.initial_cost = solver_summary.initial_cost;
  summary.final_cost = solver_summary.final_cost;

  // This only indicates whether the optimization was successfully run and makes
  // no guarantees on the quality or convergence.
  summary.success = solver_summary.termination_type != ceres::FAILURE;
  return summary;
}

}  // namespace theia
