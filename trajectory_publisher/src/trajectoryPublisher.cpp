/****************************************************************************
 *
 *   Copyright (c) 2018-2021 Jaeyoung Lim. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
/**
 * @brief Trajectory Publisher
 *
 * @author Jaeyoung Lim <jalim@ethz.ch>
 */

#include "trajectory_publisher/trajectoryPublisher.h"

using namespace std;
using namespace Eigen;
trajectoryPublisher::trajectoryPublisher(const ros::NodeHandle& nh, const ros::NodeHandle& nh_private)
    : nh_(nh), nh_private_(nh_private), motion_selector_(0) {
  trajectoryPub_ = nh_.advertise<nav_msgs::Path>("trajectory_publisher/trajectory", 1);
  referencePub_ = nh_.advertise<geometry_msgs::TwistStamped>("reference/setpoint", 1);
  flatreferencePub_ = nh_.advertise<controller_msgs::FlatTarget>("reference/flatsetpoint", 1);
  rawreferencePub_ = nh_.advertise<mavros_msgs::PositionTarget>("mavros/setpoint_raw/local", 1);
  global_rawreferencePub_ = nh_.advertise<mavros_msgs::GlobalPositionTarget>("mavros/setpoint_raw/global", 1);
  trajectoryInfoPub = nh_.advertise<trajectory_publisher::TrajectoryInfo>("trajectory_publisher/info", 1);
  motionselectorSub_ =
      nh_.subscribe("trajectory_publisher/motionselector", 1, &trajectoryPublisher::motionselectorCallback, this,
                    ros::TransportHints().tcpNoDelay());
  mavposeSub_ = nh_.subscribe("mavros/local_position/pose", 1, &trajectoryPublisher::mavposeCallback, this,
                              ros::TransportHints().tcpNoDelay());
  mavtwistSub_ = nh_.subscribe("mavros/local_position/velocity", 1, &trajectoryPublisher::mavtwistCallback, this,
                               ros::TransportHints().tcpNoDelay());

  prev_time_ = ros::Time::now();

  trajloop_timer_ = nh_.createTimer(ros::Duration(0.1), &trajectoryPublisher::loopCallback, this);
  refloop_timer_ = nh_.createTimer(ros::Duration(0.01), &trajectoryPublisher::refCallback, this);

  trajtriggerServ_ = nh_.advertiseService("start", &trajectoryPublisher::triggerCallback, this);

  nh_private_.param<double>("initpos_x", init_pos_x_, 0.0);
  nh_private_.param<double>("initpos_y", init_pos_y_, 0.0);
  nh_private_.param<double>("initpos_z", init_pos_z_, 1.0);
  nh_private_.param<double>("updaterate", controlUpdate_dt_, 0.01);
  nh_private_.param<double>("horizon", primitive_duration_, 1.0);
  nh_private_.param<double>("maxjerk", max_jerk_, 10.0);
  nh_private_.param<double>("shape_omega", shape_omega_, 1.5);
  nh_private_.param<double>("shape_radius", shape_radius_, 1);
  nh_private_.param<int>("trajectory_type", trajectory_type_, 0);
  nh_private_.param<int>("number_of_primitives", num_primitives_, 7);
  nh_private_.param<int>("reference_type", pubreference_type_, 2);
  nh_private_.param<double>("velocity_scaler", velocity_scaler_, 2.1);
  nh_private_.param<double>("windup_ratio", windup_ratio_, 0);


  inputs_.resize(num_primitives_);

  if (trajectory_type_ == 0) {  // Polynomial Trajectory

    if (num_primitives_ == 7) {
      inputs_.at(0) << 0.0, 0.0, 0.0;  // Constant jerk inputs for minimim time trajectories
      inputs_.at(1) << 1.0, 0.0, 0.0;
      inputs_.at(2) << -1.0, 0.0, 0.0;
      inputs_.at(3) << 0.0, 1.0, 0.0;
      inputs_.at(4) << 0.0, -1.0, 0.0;
      inputs_.at(5) << 0.0, 0.0, 1.0;
      inputs_.at(6) << 0.0, 0.0, -1.0;
    }

    for (int i = 0; i < num_primitives_; i++) {
      motionPrimitives_.emplace_back(std::make_shared<polynomialtrajectory>());
      primitivePub_.push_back(
          nh_.advertise<nav_msgs::Path>("trajectory_publisher/primitiveset" + std::to_string(i), 1));
      inputs_.at(i) = inputs_.at(i) * max_jerk_;
    }
  } else {  // Shape trajectories

    num_primitives_ = 1;
    motionPrimitives_.emplace_back(std::make_shared<shapetrajectory>(trajectory_type_));
    primitivePub_.push_back(nh_.advertise<nav_msgs::Path>("trajectory_publisher/primitiveset", 1));
  }

  p_targ << init_pos_x_, init_pos_y_, init_pos_z_;
  v_targ << 0.0, 0.0, 0.0;
  shape_origin_ << init_pos_x_, init_pos_y_, init_pos_z_;
  shape_axis_ << 0.0, 0.0, 1.0;
  motion_selector_ = 0;

  initializePrimitives(trajectory_type_);
}

void trajectoryPublisher::dynamicReconfigureCallback(trajectory_publisher::TrajectoryPublisherConfig &config,
                                               uint32_t level)
{
  if (velocity_scaler_ != config.velocity_scaler)
  {
    velocity_scaler_ = config.velocity_scaler;
    ROS_INFO("Reconfigure request : velocity_scaler = %.2f ", config.velocity_scaler);
    pubTrajectoryInfo();
  }
}

void trajectoryPublisher::updateReference() {
  curr_time_ = ros::Time::now();

  ros::Duration time_delta;
  time_delta = (curr_time_ - prev_time_) * (windup_ratio_ * velocity_scaler_); // Scale time delta based on windup speed
  prev_time_ = curr_time_;

  if (started_ ) {
        prev_simulated_time_ += time_delta;
  }
  
  trigger_time_ = prev_simulated_time_.toSec();

  // Keep track of laps flown to stop automatically
  if ((trigger_time_ * shape_omega_) - ((lap_ + 1) * 2 * 3.14) > 0) {
    lap_ += 1;
    pubTrajectoryInfo();
  }

  // Slowly speed up
  if (started_) {
    if (windup_ratio_ < 1) {
      pubTrajectoryInfo();
      windup_ratio_ += 0.001;
      initializePrimitives(trajectory_type_);

      if (windup_ratio_ >= 1) {
        ROS_INFO("Done speeding up");
      }
    }
  }

  p_targ = motionPrimitives_.at(motion_selector_)->getPosition(trigger_time_);
  v_targ = motionPrimitives_.at(motion_selector_)->getVelocity(trigger_time_);
  if (pubreference_type_ != 0) a_targ = motionPrimitives_.at(motion_selector_)->getAcceleration(trigger_time_);

  // Prevent jerk movement at the start of trajectory
  a_targ = a_targ * windup_ratio_;
  v_targ = v_targ * windup_ratio_;
}

void trajectoryPublisher::initializePrimitives(int type) {
  if (type == 0) {
    for (int i = 0; i < motionPrimitives_.size(); i++)
      motionPrimitives_.at(i)->generatePrimitives(p_mav_, v_mav_, inputs_.at(i));
  } else {
    for (int i = 0; i < motionPrimitives_.size(); i++)
      motionPrimitives_.at(i)->initPrimitives(shape_origin_, shape_axis_, shape_omega_, shape_radius_);
    // TODO: Pass in parameters for primitive trajectories
  }
}

void trajectoryPublisher::updatePrimitives() {
  for (int i = 0; i < motionPrimitives_.size(); i++) motionPrimitives_.at(i)->generatePrimitives(p_mav_, v_mav_);
}

void trajectoryPublisher::pubrefTrajectory(int selector) {
  // Publish current trajectory the publisher is publishing
  refTrajectory_ = motionPrimitives_.at(selector)->getSegment();
  refTrajectory_.header.stamp = ros::Time::now();
  refTrajectory_.header.frame_id = "map";
  trajectoryPub_.publish(refTrajectory_);
}

void trajectoryPublisher::pubprimitiveTrajectory() {
  for (int i = 0; i < motionPrimitives_.size(); i++) {
    primTrajectory_ = motionPrimitives_.at(i)->getSegment();
    primTrajectory_.header.stamp = ros::Time::now();
    primTrajectory_.header.frame_id = "map";
    primitivePub_.at(i).publish(primTrajectory_);
  }
}

void trajectoryPublisher::pubrefState() {
  geometry_msgs::TwistStamped msg;

  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = "map";
  msg.twist.angular.x = p_targ(0);
  msg.twist.angular.y = p_targ(1);
  msg.twist.angular.z = p_targ(2);
  msg.twist.linear.x = v_targ(0);
  msg.twist.linear.y = v_targ(1);
  msg.twist.linear.z = v_targ(2);
  referencePub_.publish(msg);
}

void trajectoryPublisher::pubflatrefState() {
  controller_msgs::FlatTarget msg;

  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = "map";
  msg.type_mask = pubreference_type_;
  msg.position.x = p_targ(0);
  msg.position.y = p_targ(1);
  msg.position.z = p_targ(2);
  msg.velocity.x = v_targ(0);
  msg.velocity.y = v_targ(1);
  msg.velocity.z = v_targ(2);
  msg.acceleration.x = a_targ(0);
  msg.acceleration.y = a_targ(1);
  msg.acceleration.z = a_targ(2);
  flatreferencePub_.publish(msg);
}

void trajectoryPublisher::pubrefSetpointRaw() {
  mavros_msgs::PositionTarget msg;
  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = "map";
  msg.type_mask = 0;
  msg.position.x = p_targ(0);
  msg.position.y = p_targ(1);
  msg.position.z = p_targ(2);
  msg.velocity.x = v_targ(0);
  msg.velocity.y = v_targ(1);
  msg.velocity.z = v_targ(2);
  msg.acceleration_or_force.x = a_targ(0);
  msg.acceleration_or_force.y = a_targ(1);
  msg.acceleration_or_force.z = a_targ(2);
  rawreferencePub_.publish(msg);
}

void trajectoryPublisher::pubrefSetpointRawGlobal() {
  mavros_msgs::GlobalPositionTarget msg;

  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = "map";
  msg.type_mask = 0;
  msg.coordinate_frame = 5;
  msg.latitude = 47.397742;
  msg.longitude = 8.545594;
  msg.altitude = 500.0;
  msg.velocity.x = v_targ(0);
  msg.velocity.y = v_targ(1);
  msg.velocity.z = v_targ(2);
  msg.acceleration_or_force.x = a_targ(0);
  msg.acceleration_or_force.y = a_targ(1);
  msg.acceleration_or_force.z = a_targ(2);
  global_rawreferencePub_.publish(msg);
}

void trajectoryPublisher::pubTrajectoryInfo() {
  trajectory_publisher::TrajectoryInfo msg;
  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = "map";
  msg.lap = lap_;
  msg.windup_ratio = windup_ratio_;
  msg.velocity_scaler = velocity_scaler_;
  trajectoryInfoPub.publish(msg);
}

void trajectoryPublisher::loopCallback(const ros::TimerEvent& event) {
  // Slow Loop publishing trajectory information
  pubrefTrajectory(motion_selector_);
  pubprimitiveTrajectory();
}

void trajectoryPublisher::refCallback(const ros::TimerEvent& event) {
  // Fast Loop publishing reference states
  updateReference();
  switch (pubreference_type_) {
    case REF_TWIST:
      pubrefState();
      break;
    case REF_SETPOINTRAW:
      pubrefSetpointRaw();
      // pubrefSetpointRawGlobal();
      break;
    default:
      pubflatrefState();
      break;
  }
}

bool trajectoryPublisher::triggerCallback(std_srvs::SetBool::Request& req, std_srvs::SetBool::Response& res) {
  unsigned char mode = req.data;

  prev_simulated_time_, start_time_ = ros::Time::now();
  res.success = true;
  res.message = "trajectory triggered";
  started_ = true;

  return true;
}

void trajectoryPublisher::motionselectorCallback(const std_msgs::Int32& selector_msg) {
  motion_selector_ = selector_msg.data;
  updatePrimitives();
  start_time_ = ros::Time::now();
}

void trajectoryPublisher::mavposeCallback(const geometry_msgs::PoseStamped& msg) {
  p_mav_(0) = msg.pose.position.x;
  p_mav_(1) = msg.pose.position.y;
  p_mav_(2) = msg.pose.position.z;
  updatePrimitives();
}

void trajectoryPublisher::mavtwistCallback(const geometry_msgs::TwistStamped& msg) {
  v_mav_(0) = msg.twist.linear.x;
  v_mav_(1) = msg.twist.linear.y;
  v_mav_(2) = msg.twist.linear.z;
  updatePrimitives();
}