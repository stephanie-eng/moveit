/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2020, KU Leuven
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Jeroen De Maeyer */

#include <moveit/ompl_interface/detail/ompl_constraints.h>

#include <moveit/ompl_interface/detail/threadsafe_state_storage.h>
#include <moveit/robot_model/robot_model.h>
#include <moveit_msgs/Constraints.h>
#include <eigen_conversions/eigen_msg.h>

namespace ompl_interface
{
constexpr char LOGNAME[] = "ompl_constraints";

inline double Bounds::penalty(double value) const
{
  if (value < lower)
    return lower - value;
  else if (value > upper)
    return value - upper;
  else
    return 0.0;
}

inline double Bounds::derivative(double value) const
{
  if (value < lower)
    return -1.0;
  else if (value > upper)
    return 1.0;
  else
    return 0.0;
}

std::ostream& operator<<(std::ostream& os, const ompl_interface::Bounds& bound)
{
  os << "Bounds: (" << bound.lower;
  os << ", " << bound.upper << " )";
  return os;
}

/****************************
 * Base class for constraints
 * **************************/
BaseConstraint::BaseConstraint(const robot_model::RobotModelConstPtr& robot_model, const std::string& group,
                               const unsigned int num_dofs, const unsigned int num_cons_)
  : ompl::base::Constraint(num_dofs, num_cons_)
  , state_storage_(robot_model)
  , joint_model_group_(robot_model->getJointModelGroup(group))

{
}

void BaseConstraint::init(const moveit_msgs::Constraints& constraints)
{
  parseConstraintMsg(constraints);
}

Eigen::Isometry3d BaseConstraint::forwardKinematics(const Eigen::Ref<const Eigen::VectorXd>& joint_values) const
{
  moveit::core::RobotState* robot_state = state_storage_.getStateStorage();
  robot_state->setJointGroupPositions(joint_model_group_, joint_values);
  return robot_state->getGlobalLinkTransform(link_name_);
}

Eigen::MatrixXd BaseConstraint::robotGeometricJacobian(const Eigen::Ref<const Eigen::VectorXd>& joint_values) const
{
  moveit::core::RobotState* robot_state = state_storage_.getStateStorage();
  robot_state->setJointGroupPositions(joint_model_group_, joint_values);
  Eigen::MatrixXd jacobian;
  // return value (success) not used, could return a garbage jacobian.
  robot_state->getJacobian(joint_model_group_, joint_model_group_->getLinkModel(link_name_),
                           Eigen::Vector3d(0.0, 0.0, 0.0), jacobian);
  return jacobian;
}

void BaseConstraint::function(const Eigen::Ref<const Eigen::VectorXd>& joint_values,
                              Eigen::Ref<Eigen::VectorXd> out) const
{
  Eigen::VectorXd current_values = calcError(joint_values);
  for (std::size_t i{ 0 }; i < bounds_.size(); ++i)
  {
    out[i] = bounds_[i].penalty(current_values[i]);
  }
}

void BaseConstraint::jacobian(const Eigen::Ref<const Eigen::VectorXd>& joint_values,
                              Eigen::Ref<Eigen::MatrixXd> out) const
{
  Eigen::VectorXd constraint_error = calcError(joint_values);
  Eigen::MatrixXd robot_jacobian = calcErrorJacobian(joint_values);
  for (std::size_t i{ 0 }; i < bounds_.size(); ++i)
  {
    out.row(i) = bounds_[i].derivative(constraint_error[i]) * robot_jacobian.row(i);
  }
}

/******************************************
 * Position constraints
 * ****************************************/
PositionConstraint::PositionConstraint(const robot_model::RobotModelConstPtr& robot_model, const std::string& group,
                                       const unsigned int num_dofs)
  : BaseConstraint(robot_model, group, num_dofs)
{
}

void PositionConstraint::parseConstraintMsg(const moveit_msgs::Constraints& constraints)
{
  ROS_INFO_STREAM_NAMED(LOGNAME, "Parsing position constraint for OMPL constrained state space.");
  bounds_.clear();
  bounds_ = positionConstraintMsgToBoundVector(constraints.position_constraints.at(0));
  ROS_INFO_STREAM_NAMED(LOGNAME, "Parsed x constraints" << bounds_[0]);
  ROS_INFO_STREAM_NAMED(LOGNAME, "Parsed y constraints" << bounds_[1]);
  ROS_INFO_STREAM_NAMED(LOGNAME, "Parsed z constraints" << bounds_[2]);

  // extract target position and orientation
  geometry_msgs::Point position =
      constraints.position_constraints.at(0).constraint_region.primitive_poses.at(0).position;
  target_position_ << position.x, position.y, position.z;
  tf::quaternionMsgToEigen(constraints.position_constraints.at(0).constraint_region.primitive_poses.at(0).orientation,
                           target_orientation_);

  link_name_ = constraints.position_constraints.at(0).link_name;
  ROS_INFO_STREAM_NAMED(LOGNAME, "Position constraints applied to link: " << link_name_);
}

Eigen::VectorXd PositionConstraint::calcError(const Eigen::Ref<const Eigen::VectorXd>& x) const
{
  return target_orientation_.matrix().transpose() * (forwardKinematics(x).translation() - target_position_);
}

Eigen::MatrixXd PositionConstraint::calcErrorJacobian(const Eigen::Ref<const Eigen::VectorXd>& x) const
{
  return target_orientation_.matrix().transpose() * robotGeometricJacobian(x).topRows(3);
}

/******************************************
 * Equality constraints
 * ****************************************/
EqualityPositionConstraint::EqualityPositionConstraint(const robot_model::RobotModelConstPtr& robot_model,
                                                       const std::string& group, const unsigned int num_dofs)
  : BaseConstraint(robot_model, group, num_dofs)
{
}

void EqualityPositionConstraint::parseConstraintMsg(const moveit_msgs::Constraints& constraints)
{
  ROS_INFO_STREAM_NAMED(LOGNAME, "Parsing equality position constraint for OMPL constrained state space.");
  bounds_.clear();

  std::vector<double> dims = constraints.position_constraints.at(0).constraint_region.primitives.at(0).dimensions;

  is_dim_constrained_ = { false, false, false };
  for (std::size_t i{ 0 }; i < dims.size(); ++i)
  {
    if (dims[i] < equality_constraint_threshold_)
    {
      if (dims[i] < getTolerance())
      {
        ROS_ERROR_NAMED(
            LOGNAME,
            "Dimension %li of position constraint is smaller than the tolerance used to evaluate the constraints. "
            "This will make all states invalid and planning will fail :( Please use a value between %f and %f. ",
            i, getTolerance(), equality_constraint_threshold_);
      }
      is_dim_constrained_.at(i) = true;
    }
  }

  ROS_INFO_STREAM_NAMED(LOGNAME, "X dimension constraint? " << is_dim_constrained_[0]);
  ROS_INFO_STREAM_NAMED(LOGNAME, "Y dimension constraint? " << is_dim_constrained_[1]);
  ROS_INFO_STREAM_NAMED(LOGNAME, "Z dimension constraint? " << is_dim_constrained_[2]);

  // extract target position and orientation
  geometry_msgs::Point position =
      constraints.position_constraints.at(0).constraint_region.primitive_poses.at(0).position;
  target_position_ << position.x, position.y, position.z;
  tf::quaternionMsgToEigen(constraints.position_constraints.at(0).constraint_region.primitive_poses.at(0).orientation,
                           target_orientation_);

  link_name_ = constraints.position_constraints.at(0).link_name;
  ROS_INFO_STREAM_NAMED(LOGNAME, "Position constraints applied to link: " << link_name_);
}

void EqualityPositionConstraint::function(const Eigen::Ref<const Eigen::VectorXd>& joint_values,
                                          Eigen::Ref<Eigen::VectorXd> out) const
{
  Eigen::Vector3d error =
      target_orientation_.matrix().transpose() * (forwardKinematics(joint_values).translation() - target_position_);
  for (std::size_t dim{ 0 }; dim < 3; ++dim)
  {
    if (is_dim_constrained_[dim])
      out[dim] = error[dim];  // equality constraint dimension
    else
      out[dim] = 0.0;  // unbounded dimension
  }
}

void EqualityPositionConstraint::jacobian(const Eigen::Ref<const Eigen::VectorXd>& joint_values,
                                          Eigen::Ref<Eigen::MatrixXd> out) const
{
  out.setZero();
  Eigen::MatrixXd jac = target_orientation_.matrix().transpose() * robotGeometricJacobian(joint_values).topRows(3);
  for (std::size_t dim{ 0 }; dim < 3; ++dim)
  {
    if (is_dim_constrained_[dim])
      out.row(dim) = jac.row(dim);  // equality constraint dimension
  }
}

/******************************************
 * Linear System constraints
 * ****************************************/
LinearSystemPositionConstraint::LinearSystemPositionConstraint(const robot_model::RobotModelConstPtr& robot_model,
                                                               const std::string& group, const unsigned int num_dofs)
  : BaseConstraint(robot_model, group, num_dofs)
{
}

void LinearSystemPositionConstraint::parseConstraintMsg(const moveit_msgs::Constraints& constraints)
{
  ROS_INFO_STREAM_NAMED(LOGNAME, "Parsing linear system position constraint for OMPL constrained state space.");
  bounds_.clear();

  std::vector<double> dims = constraints.position_constraints.at(0).constraint_region.primitives.at(0).dimensions;

  is_dim_constrained_ = { false, false, false };
  for (std::size_t i{ 0 }; i < dims.size(); ++i)
  {
    if (dims[i] < equality_constraint_threshold_)
    {
      if (dims[i] < getTolerance())
      {
        ROS_ERROR_NAMED(
            LOGNAME,
            "Dimension %li of position constraint is smaller than the tolerance used to evaluate the constraints. "
            "This will make all states invalid and planning will fail :( Please use a value between %f and %f. ",
            i, getTolerance(), equality_constraint_threshold_);
      }
      is_dim_constrained_.at(i) = true;
    }
  }

  ROS_INFO_STREAM_NAMED(LOGNAME, "X constrained? " << is_dim_constrained_[0]);
  ROS_INFO_STREAM_NAMED(LOGNAME, "Y constrained? " << is_dim_constrained_[1]);
  ROS_INFO_STREAM_NAMED(LOGNAME, "Z constrained? " << is_dim_constrained_[2]);

  // extract target position and orientation
  geometry_msgs::Point position =
      constraints.position_constraints.at(0).constraint_region.primitive_poses.at(0).position;
  start_position_ << position.x, position.y, position.z;
  position = constraints.position_constraints.at(0).constraint_region.primitive_poses.at(1).position;
  end_position_ << position.x, position.y, position.z;
  tf::quaternionMsgToEigen(constraints.position_constraints.at(0).constraint_region.primitive_poses.at(0).orientation,
                           target_orientation_);

  link_name_ = constraints.position_constraints.at(0).link_name;
  ROS_INFO_STREAM_NAMED(LOGNAME, "Position constraints applied to link: " << link_name_);
}

void LinearSystemPositionConstraint::function(const Eigen::Ref<const Eigen::VectorXd>& joint_values,
                                              Eigen::Ref<Eigen::VectorXd> out) const
{
  Eigen::Vector3d cartesianPosition =
      target_orientation_.matrix().transpose() * forwardKinematics(joint_values).translation();
  Eigen::Vector2d residual;
  residual[0] = (end_position_.x() - start_position_.x()) * (cartesianPosition.y() - start_position_.y()) -
                (end_position_.y() - start_position_.y()) * (cartesianPosition.x() - start_position_.x());
  residual[1] = (end_position_.y() - start_position_.y()) * (cartesianPosition.z() - start_position_.z()) -
                (end_position_.z() - start_position_.z()) * (cartesianPosition.y() - start_position_.y());
  out = residual;
}

void LinearSystemPositionConstraint::jacobian(const Eigen::Ref<const Eigen::VectorXd>& joint_values,
                                              Eigen::Ref<Eigen::MatrixXd> out) const
{
  out.setZero();
  Eigen::MatrixXd jac = target_orientation_.matrix().transpose() * robotGeometricJacobian(joint_values).topRows(3);
  Eigen::MatrixXd dresidual_dcartesianPosition;
  // d residual[0] / d x
  dresidual_dcartesianPosition(0, 0) = start_position_.y() - end_position_.y();
  // d residual[1] / d x
  dresidual_dcartesianPosition(1, 0) = 0;
  // d residual[0] / d y
  dresidual_dcartesianPosition(0, 1) = end_position_.x() - start_position_.x();
  // d residual[1] / d y
  dresidual_dcartesianPosition(1, 1) = start_position_.z() - end_position_.z();
  // d residual[0] / d z
  dresidual_dcartesianPosition(0, 1) = 0;
  // d residual[1] / d z
  dresidual_dcartesianPosition(1, 1) = end_position_.y() - start_position_.y();
  out = dresidual_dcartesianPosition * jac;
}

/******************************************
 * Orientation constraints
 * ****************************************/
void OrientationConstraint::parseConstraintMsg(const moveit_msgs::Constraints& constraints)
{
  bounds_.clear();
  bounds_ = orientationConstraintMsgToBoundVector(constraints.orientation_constraints.at(0));
  ROS_INFO_STREAM("Parsing orientation constraints");
  ROS_INFO_STREAM("Parsed rx / roll constraints" << bounds_[0]);
  ROS_INFO_STREAM("Parsed ry / pitch constraints" << bounds_[1]);
  ROS_INFO_STREAM("Parsed rz / yaw constraints" << bounds_[2]);

  tf::quaternionMsgToEigen(constraints.orientation_constraints.at(0).orientation, target_orientation_);

  link_name_ = constraints.orientation_constraints.at(0).link_name;
}

Eigen::VectorXd OrientationConstraint::calcError(const Eigen::Ref<const Eigen::VectorXd>& x) const
{
  Eigen::Matrix3d orientation_difference = forwardKinematics(x).linear().transpose() * target_orientation_;
  Eigen::AngleAxisd aa(orientation_difference);
  return aa.axis() * aa.angle();
}

Eigen::MatrixXd OrientationConstraint::calcErrorJacobian(const Eigen::Ref<const Eigen::VectorXd>& x) const
{
  Eigen::Matrix3d orientation_difference = forwardKinematics(x).linear().transpose() * target_orientation_;
  Eigen::AngleAxisd aa{ orientation_difference };
  return -angularVelocityToAngleAxis(aa.angle(), aa.axis()) * robotGeometricJacobian(x).bottomRows(3);
}

/************************************
 * MoveIt constraint message parsing
 * **********************************/
std::vector<Bounds> positionConstraintMsgToBoundVector(const moveit_msgs::PositionConstraint& pos_con)
{
  auto dims = pos_con.constraint_region.primitives.at(0).dimensions;

  // dimension of -1 signifies unconstrained parameter, so set to infinity
  for (auto& dim : dims)
  {
    if (dim == -1)
      dim = std::numeric_limits<double>::infinity();
  }

  return { { -dims[0] / 2, dims[0] / 2 }, { -dims[1] / 2, dims[1] / 2 }, { -dims[2] / 2, dims[2] / 2 } };
}

std::vector<Bounds> orientationConstraintMsgToBoundVector(const moveit_msgs::OrientationConstraint& ori_con)
{
  std::vector<double> dims{ ori_con.absolute_x_axis_tolerance, ori_con.absolute_y_axis_tolerance,
                            ori_con.absolute_z_axis_tolerance };

  // dimension of -1 signifies unconstrained parameter, so set to infinity
  for (auto& dim : dims)
  {
    if (dim == -1)
      dim = std::numeric_limits<double>::infinity();
  }
  return { { -dims[0], dims[0] }, { -dims[1], dims[1] }, { -dims[2], dims[2] } };
}

/******************************************
 * OMPL Constraints Factory
 * ****************************************/
std::shared_ptr<BaseConstraint> createOMPLConstraint(robot_model::RobotModelConstPtr robot_model,
                                                     const std::string& group,
                                                     const moveit_msgs::Constraints& constraints)
{
  std::size_t num_dofs = robot_model->getJointModelGroup(group)->getVariableCount();
  std::size_t num_pos_con = constraints.position_constraints.size();
  std::size_t num_ori_con = constraints.orientation_constraints.size();

  // Only positions constraints are actually created below.
  // All the other code is just given feedback to the user.

  if (num_pos_con > 1)
  {
    ROS_WARN_NAMED(LOGNAME, "Only a single position constraints supported. Using the first one.");
  }
  if (num_ori_con > 1)
  {
    ROS_WARN_NAMED(LOGNAME, "Only a single orientation constraints supported. Using the first one.");
  }

  if (num_pos_con > 0 && num_ori_con > 0)
  {
    ROS_ERROR_NAMED(LOGNAME, "Combining position and orientation constraints not implemented yet for OMPL's "
                             "constrained state space.");
    return nullptr;
  }
  else if (num_pos_con > 0)
  {
    ROS_INFO_STREAM("Constraint name: " << constraints.name);
    BaseConstraintPtr pos_con;
    if (constraints.name == "use_equality_constraints")
    {
      ROS_INFO_STREAM("Using equality position constraints.");
      pos_con = std::make_shared<EqualityPositionConstraint>(robot_model, group, num_dofs);
    }
    else if (constraints.name == "linear_system_constraints")
    {
      ROS_INFO_STREAM("Using position constraints from a linear system.");
      pos_con = std::make_shared<LinearSystemPositionConstraint>(robot_model, group, num_dofs);
    }
    else
    {
      ROS_INFO_STREAM("Using bounded position constraints.");
      pos_con = std::make_shared<PositionConstraint>(robot_model, group, num_dofs);
    }
    pos_con->init(constraints);
    return pos_con;
  }
  else if (num_ori_con > 0)
  {
    ROS_ERROR_NAMED(LOGNAME, "Orientation constraints are not yet supported.");
    auto ori_con = std::make_shared<OrientationConstraint>(robot_model, group, num_dofs);
    ori_con->init(constraints);
    return ori_con;
  }
  else
  {
    ROS_ERROR_NAMED(LOGNAME, "No path constraints found in planning request.");
    return nullptr;
  }
}
}  // namespace ompl_interface
