// Copyright (c) 2021 - for information on the respective copyright owner
// see the NOTICE file and/or the repository https://github.com/boschglobal/locator_ros_bridge.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "receiving_interface.hpp"

#include <Poco/NObserver.h>

#include <fstream>
#include <string>
#include <vector>

#include "rosmsgs_datagram_converter.hpp"

#include "bosch_locator_bridge/msg/client_global_align_visualization.hpp"
#include "bosch_locator_bridge/msg/client_localization_pose.hpp"
#include "bosch_locator_bridge/msg/client_localization_visualization.hpp"
#include "bosch_locator_bridge/msg/client_map_visualization.hpp"
#include "bosch_locator_bridge/msg/client_recording_visualization.hpp"

ReceivingInterface::ReceivingInterface(
  const Poco::Net::IPAddress & hostadress, Poco::UInt16 port,
  rclcpp::Node::SharedPtr node)
: node_(node),
  tf_broadcaster_(node),
  ccm_socket_(Poco::Net::SocketAddress(hostadress, port))
{
  binary_reader_.setExceptions(
    std::ifstream::failbit | std::ifstream::badbit |
    std::ifstream::eofbit);
  reactor_.addEventHandler(
    ccm_socket_, Poco::NObserver<ReceivingInterface, Poco::Net::ReadableNotification>(
      *this, &ReceivingInterface::onReadEvent));
}

ReceivingInterface::~ReceivingInterface()
{
  reactor_.stop();
  ccm_socket_.shutdown();
}

void ReceivingInterface::onReadEvent(
  const Poco::AutoPtr<Poco::Net::ReadableNotification> & /*notification*/)
{
  try {
    if (ccm_socket_.available() == 0) {
      std::cout << "received msg of length 0... Connection closed? \n";
    }
    while (ccm_socket_.available()) {
      tryToParseData(binary_reader_);
    }
  } catch (const std::ios_base::failure & io_failure) {
    // catching this exception is actually no error:
    // the datagram is just not yet completely transmitted could not be
    // parsed because of that. Will automatically retry after more data is available.
  } catch (...) {
    RCLCPP_ERROR_STREAM(node_->get_logger(), "Caught exception in ReceivingInterface!");
  }
}

void ReceivingInterface::run()
{
  reactor_.run();
}

void ReceivingInterface::publishTransform(
  const geometry_msgs::msg::PoseStamped & pose, const std::string & parent_frame,
  const std::string child_frame)
{
  geometry_msgs::msg::TransformStamped transform;
  transform.header.stamp = pose.header.stamp;
  transform.header.frame_id = parent_frame;

  transform.transform.translation.x = pose.pose.position.x;
  transform.transform.translation.y = pose.pose.position.y;
  transform.transform.translation.z = pose.pose.position.z;
  transform.transform.rotation = pose.pose.orientation;

  transform.child_frame_id = child_frame;

  tf_broadcaster_.sendTransform(transform);
}

ClientControlModeInterface::ClientControlModeInterface(
  const Poco::Net::IPAddress & hostadress,
  rclcpp::Node::SharedPtr node)
: ReceivingInterface(hostadress, BINARY_CLIENT_CONTROL_MODE_PORT, node)
{
  // Setup publisher (use QoS settings to emulate a latched topic (ROS 1))
  client_control_mode_pub_ = node->create_publisher<bosch_locator_bridge::msg::ClientControlMode>(
    "~/client_control_mode", rclcpp::QoS(rclcpp::KeepLast(1)).transient_local().reliable());
}

void ClientControlModeInterface::tryToParseData(Poco::BinaryReader & binary_reader)
{
  // convert datagram to ros message
  bosch_locator_bridge::msg::ClientControlMode client_control_mode;

  RosMsgsDatagramConverter::convertClientControlMode2Message(
    binary_reader,
    node_->now(), client_control_mode);
  // publish client control mode
  client_control_mode_pub_->publish(client_control_mode);
}

ClientMapMapInterface::ClientMapMapInterface(
  const Poco::Net::IPAddress & hostadress,
  rclcpp::Node::SharedPtr node)
: ReceivingInterface(hostadress, BINARY_CLIENT_MAP_MAP_PORT, node)
{
  // Setup publisher
  client_map_map_pub_ =
    node->create_publisher<sensor_msgs::msg::PointCloud2>("~/client_map_map", 5);
}

void ClientMapMapInterface::tryToParseData(Poco::BinaryReader & binary_reader)
{
  // convert datagram to ros message
  sensor_msgs::msg::PointCloud2 map;
  RosMsgsDatagramConverter::convertMapDatagram2Message(
    binary_reader,
    node_->now(), map);
  // publish
  client_map_map_pub_->publish(map);
}

ClientMapVisualizationInterface::ClientMapVisualizationInterface(
  const Poco::Net::IPAddress & hostadress,
  rclcpp::Node::SharedPtr node)
: ReceivingInterface(hostadress, BINARY_CLIENT_MAP_VISUALIZATION_PORT, node)
{
  // Setup publisher
  client_map_visualization_pub_ =
    node->create_publisher<bosch_locator_bridge::msg::ClientMapVisualization>(
    "~/client_map_visualization", 5);
  client_map_visualization_pose_pub_ = node->create_publisher<geometry_msgs::msg::PoseStamped>(
    "~/client_map_visualization/pose", 5);
  client_map_visualization_scan_pub_ = node->create_publisher<sensor_msgs::msg::PointCloud2>(
    "~/client_map_visualization/scan", 5);
  client_map_visualization_path_poses_pub_ = node->create_publisher<geometry_msgs::msg::PoseArray>(
    "~/client_map_visualization/path_poses", 5);
}

void ClientMapVisualizationInterface::tryToParseData(Poco::BinaryReader & binary_reader)
{
  // convert datagram to ros messages
  bosch_locator_bridge::msg::ClientMapVisualization client_map_visualization;
  geometry_msgs::msg::PoseStamped pose;
  sensor_msgs::msg::PointCloud2 scan;
  geometry_msgs::msg::PoseArray path_poses;

  RosMsgsDatagramConverter::convertClientMapVisualizationDatagram2Message(
    binary_reader, client_map_visualization, pose, scan, path_poses);


  // publish
  publishTransform(pose, MAP_FRAME_ID, LASER_FRAME_ID);
  client_map_visualization_pub_->publish(client_map_visualization);
  client_map_visualization_pose_pub_->publish(pose);
  client_map_visualization_scan_pub_->publish(scan);
  client_map_visualization_path_poses_pub_->publish(path_poses);

}

ClientRecordingMapInterface::ClientRecordingMapInterface(
  const Poco::Net::IPAddress & hostadress,
  rclcpp::Node::SharedPtr node)
: ReceivingInterface(hostadress, BINARY_CLIENT_RECORDING_MAP_PORT, node)
{
  // Setup publisher
  client_recording_map_pub_ = node->create_publisher<sensor_msgs::msg::PointCloud2>(
    "~/client_recording_map", 5);
}

void ClientRecordingMapInterface::tryToParseData(Poco::BinaryReader & binary_reader)
{
  // convert datagram to ros message
  sensor_msgs::msg::PointCloud2 map;
  RosMsgsDatagramConverter::convertMapDatagram2Message(
    binary_reader,
    node_->now(), map);
  // publish
  client_recording_map_pub_->publish(map);
}

ClientRecordingVisualizationInterface::ClientRecordingVisualizationInterface(
  const Poco::Net::IPAddress & hostadress,
  rclcpp::Node::SharedPtr node)
: ReceivingInterface(hostadress, BINARY_CLIENT_RECORDING_VISUALIZATION_PORT, node)
{
  // Setup publisher
  client_recording_visualization_pub_ =
    node->create_publisher<bosch_locator_bridge::msg::ClientRecordingVisualization>(
    "~/client_recording_visualization", 5);
  client_recording_visualization_pose_pub_ =
    node->create_publisher<geometry_msgs::msg::PoseStamped>(
    "~/client_recording_visualization/pose",
    5);
  client_recording_visualization_scan_pub_ = node->create_publisher<sensor_msgs::msg::PointCloud2>(
    "~/client_recording_visualization/scan", 5);
  client_recording_visualization_path_poses_pub_ =
    node->create_publisher<geometry_msgs::msg::PoseArray>(
    "~/client_recording_visualization/path_poses", 5);
}

void ClientRecordingVisualizationInterface::tryToParseData(Poco::BinaryReader & binary_reader)
{
  // convert datagram to ros messages
  bosch_locator_bridge::msg::ClientRecordingVisualization client_recording_visualization;
  geometry_msgs::msg::PoseStamped pose;
  sensor_msgs::msg::PointCloud2 scan;
  geometry_msgs::msg::PoseArray path_poses;

  RosMsgsDatagramConverter::convertClientRecordingVisualizationDatagram2Message(
    binary_reader, client_recording_visualization, pose, scan, path_poses);

  // publish
  publishTransform(pose, MAP_FRAME_ID, LASER_FRAME_ID);
  client_recording_visualization_pub_->publish(client_recording_visualization);
  client_recording_visualization_pose_pub_->publish(pose);
  client_recording_visualization_scan_pub_->publish(scan);
  client_recording_visualization_path_poses_pub_->publish(path_poses);
}

ClientLocalizationMapInterface::ClientLocalizationMapInterface(
  const Poco::Net::IPAddress & hostadress,
  rclcpp::Node::SharedPtr node)
: ReceivingInterface(hostadress, BINARY_CLIENT_LOCALIZATION_MAP_PORT, node)
{
  // Setup publisher (use QoS settings to emulate a latched topic (ROS 1))
  client_localization_map_pub_ = node->create_publisher<sensor_msgs::msg::PointCloud2>(
    "~/client_localization_map", rclcpp::QoS(rclcpp::KeepLast(1)).transient_local().reliable());
}

void ClientLocalizationMapInterface::tryToParseData(Poco::BinaryReader & binary_reader)
{
  // convert datagram to ros message
  sensor_msgs::msg::PointCloud2 map;
  RosMsgsDatagramConverter::convertMapDatagram2Message(
    binary_reader,
    node_->now(), map);
  // publish
  client_localization_map_pub_->publish(map);
}

ClientLocalizationVisualizationInterface::ClientLocalizationVisualizationInterface(
  const Poco::Net::IPAddress & hostadress, rclcpp::Node::SharedPtr node)
: ReceivingInterface(hostadress, BINARY_CLIENT_LOCALIZATION_VISUALIZATION_PORT, node)
{
  // Setup publisher
  client_localization_visualization_pub_ =
    node->create_publisher<bosch_locator_bridge::msg::ClientLocalizationVisualization>(
    "~/client_localization_visualization", 5);
  client_localization_visualization_pose_pub_ =
    node->create_publisher<geometry_msgs::msg::PoseStamped>(
    "~/client_localization_visualization/pose", 5);
  client_localization_visualization_scan_pub_ =
    node->create_publisher<sensor_msgs::msg::PointCloud2>(
    "~/client_localization_visualization/scan", 5);
}

void ClientLocalizationVisualizationInterface::tryToParseData(Poco::BinaryReader & binary_reader)
{
  // convert datagram to ros messages
  bosch_locator_bridge::msg::ClientLocalizationVisualization client_localization_visualization;
  geometry_msgs::msg::PoseStamped pose;
  sensor_msgs::msg::PointCloud2 scan;

  RosMsgsDatagramConverter::convertClientLocalizationVisualizationDatagram2Message(
    binary_reader, client_localization_visualization, pose, scan);

  // publish
  client_localization_visualization_pub_->publish(client_localization_visualization);
  client_localization_visualization_pose_pub_->publish(pose);
  client_localization_visualization_scan_pub_->publish(scan);
}

ClientLocalizationPoseInterface::ClientLocalizationPoseInterface(
  const Poco::Net::IPAddress & hostadress,
  rclcpp::Node::SharedPtr node)
: ReceivingInterface(hostadress, BINARY_CLIENT_LOCALIZATION_POSE_PORT, node)
{
  // Setup publisher
  client_localization_pose_pub_ =
    node->create_publisher<bosch_locator_bridge::msg::ClientLocalizationPose>(
    "~/client_localization_pose", 5);
  client_localization_pose_pose_pub_ =
    node->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(
    "~/client_localization_pose/pose", 5);
  client_localization_pose_lidar_odo_pose_pub_ =
    node->create_publisher<geometry_msgs::msg::PoseStamped>(
    "~/client_localization_pose/lidar_odo_pose", 5);
}

void ClientLocalizationPoseInterface::tryToParseData(Poco::BinaryReader & binary_reader)
{
  // convert datagram to ros messages
  bosch_locator_bridge::msg::ClientLocalizationPose client_localization_pose;
  geometry_msgs::msg::PoseStamped pose;
  geometry_msgs::msg::PoseWithCovarianceStamped poseWithCov;
  geometry_msgs::msg::PoseStamped lidar_odo_pose;

  double covariance[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

  RosMsgsDatagramConverter::convertClientLocalizationPoseDatagram2Message(
    binary_reader, client_localization_pose, pose, covariance, lidar_odo_pose);

  poseWithCov.pose.pose = pose.pose;
  poseWithCov.header = pose.header;
  // assign right triangle of 3x3 matrix to 6x6 matrix
  poseWithCov.pose.covariance[0] = covariance[0];
  poseWithCov.pose.covariance[1] = covariance[1];
  poseWithCov.pose.covariance[5] = covariance[2];
  poseWithCov.pose.covariance[7] = covariance[3];
  poseWithCov.pose.covariance[11] = covariance[4];
  poseWithCov.pose.covariance[35] = covariance[5];

  // publish
  publishTransform(pose, MAP_FRAME_ID, LASER_FRAME_ID);
  client_localization_pose_pub_->publish(client_localization_pose);
  client_localization_pose_pose_pub_->publish(poseWithCov);
  client_localization_pose_lidar_odo_pose_pub_->publish(lidar_odo_pose);
}

ClientGlobalAlignVisualizationInterface::ClientGlobalAlignVisualizationInterface(
  const Poco::Net::IPAddress & hostadress,
  rclcpp::Node::SharedPtr node)
: ReceivingInterface(hostadress, BINARY_CLIENT_GLOBAL_ALIGN_VISUALIZATION_PORT, node)
{
  // Setup publisher
  client_global_align_visualization_pub_ =
    node->create_publisher<bosch_locator_bridge::msg::ClientGlobalAlignVisualization>(
    "~/client_global_align_visualization", 5);
  client_global_align_visualization_poses_pub_ =
    node->create_publisher<geometry_msgs::msg::PoseArray>(
    "~/client_global_align_visualization/poses", 5);
  client_global_align_visualization_landmarks_poses_pub_ =
    node->create_publisher<geometry_msgs::msg::PoseArray>(
    "~/client_global_align_visualization/landmarks/poses", 5);
}

void ClientGlobalAlignVisualizationInterface::tryToParseData(Poco::BinaryReader & binary_reader)
{
  // convert datagram to ros messages
  bosch_locator_bridge::msg::ClientGlobalAlignVisualization client_global_align_visualization;
  geometry_msgs::msg::PoseArray poses;
  geometry_msgs::msg::PoseArray landmark_poses;

  RosMsgsDatagramConverter::convertClientGlobalAlignVisualizationDatagram2Message(
    binary_reader, client_global_align_visualization, poses, landmark_poses);

  // publish
  client_global_align_visualization_pub_->publish(client_global_align_visualization);
  client_global_align_visualization_poses_pub_->publish(poses);
  client_global_align_visualization_landmarks_poses_pub_->publish(landmark_poses);
}
