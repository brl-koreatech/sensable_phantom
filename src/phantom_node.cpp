/*
 * See ../LICENSE.md for original copyright and license.
 */

/*
 * Copyright (c) 2013, Boris Gromov, BioRobotics Lab at KoreaTech
 * All right reserved.
 *
 * Based on original code from Healthcare Robotics Lab at Georgia Tech
 *
 */

#include <ros/ros.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/WrenchStamped.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <sstream>

#include <HL/hl.h>
#include <HD/hd.h>
#include <HDU/hduError.h>
#include <HDU/hduVector.h>
#include <HDU/hduMatrix.h>
#include <HDU/hduQuaternion.h>

#include "sensable_phantom/PhantomButtonEvent.h"
#include <pthread.h>

float prev_time;

struct PhantomState
{
  hduVector3Dd position; //3x1 vector of position
  hduVector3Dd velocity; //3x1 vector of velocity
  hduVector3Dd inp_vel1; //3x1 history of velocity used for filtering velocity estimate
  hduVector3Dd inp_vel2;
  hduVector3Dd inp_vel3;
  hduVector3Dd out_vel1;
  hduVector3Dd out_vel2;
  hduVector3Dd out_vel3;
  hduVector3Dd pos_hist1; //3x1 history of position used for 2nd order backward difference estimate of velocity
  hduVector3Dd pos_hist2;
  hduVector3Dd rot;
  hduVector3Dd joints;
  hduVector3Dd force; //3 element double vector force[0], force[1], force[2]
  hduVector3Dd torque; //3 element double vector torque[0], torque[1], torque[2]

  hduMatrix hd_cur_transform;

  float thetas[7];
  int buttons[2];
  int buttons_prev[2];
  bool lock;
  hduVector3Dd lock_pos;
};

class PhantomROS
{

public:
  ros::NodeHandlePtr node_;
  ros::NodeHandlePtr pnode_;

  ros::Publisher pose_publisher_;

  ros::Publisher button_publisher_;
  ros::Subscriber wrench_sub_;
  std::string base_link_name_;
  std::string sensable_frame_name_;
  std::string link_names_[7];

  std::string tf_prefix_;
  double table_offset_;
  double damping_k_;
  bool locked_;
  bool calibrate_;

  PhantomState *state_;
  tf::TransformBroadcaster br_;
  tf::TransformListener ls_;

  PhantomROS() : table_offset_(0.0), damping_k_(0.0), locked_(false), calibrate_(false), state_(NULL)
  {
  }

  int init(PhantomState *s)
  {
    if(!s)
    {
      ROS_FATAL("Internal error. PhantomState is NULL.");
      return -1;
    }

    node_ = ros::NodeHandlePtr(new ros::NodeHandle);
    pnode_ = ros::NodeHandlePtr(new ros::NodeHandle("~"));
    pnode_->param(std::string("tf_prefix"), tf_prefix_, std::string(""));

    // Vertical displacement from base_link to link_0. Defaults to Omni offset.
    pnode_->param(std::string("table_offset"), table_offset_, .135);

    // Force feedback damping coefficient
    pnode_->param(std::string("damping_k"), damping_k_, 0.001);

    // On startup device will generate forces to hold end-effector at origin.
    pnode_->param(std::string("locked"), locked_, false);

    // Check calibration status on start up and calibrate if necessary.
    pnode_->param(std::string("calibrate"), calibrate_, false);

    //Frame attached to the base of the phantom (NAME/base_link)
    base_link_name_ = "base_link";

    //Publish on NAME/pose
    std::string pose_topic_name = "pose";
    pose_publisher_ = node_->advertise<geometry_msgs::PoseStamped>(pose_topic_name, 100);

    //Publish button state on NAME/button
    std::string button_topic = "button";
    button_publisher_ = node_->advertise<sensable_phantom::PhantomButtonEvent>(button_topic, 100);

    //Subscribe to NAME/force_feedback
    std::string force_feedback_topic = "force_feedback";
    wrench_sub_ = node_->subscribe(force_feedback_topic, 100, &PhantomROS::wrench_callback, this);

    //Frame of force feedback (NAME/sensable_origin)
    sensable_frame_name_ = "sensable_origin";

    for (int i = 0; i < 7; i++)
    {
      std::ostringstream stream1;
      stream1 << "link_" << i;
      link_names_[i] = std::string(stream1.str());
    }

    state_ = s;
    state_->buttons[0] = 0;
    state_->buttons[1] = 0;
    state_->buttons_prev[0] = 0;
    state_->buttons_prev[1] = 0;
    hduVector3Dd zeros(0, 0, 0);
    state_->velocity = zeros;
    state_->inp_vel1 = zeros; //3x1 history of velocity
    state_->inp_vel2 = zeros; //3x1 history of velocity
    state_->inp_vel3 = zeros; //3x1 history of velocity
    state_->out_vel1 = zeros; //3x1 history of velocity
    state_->out_vel2 = zeros; //3x1 history of velocity
    state_->out_vel3 = zeros; //3x1 history of velocity
    state_->pos_hist1 = zeros; //3x1 history of position
    state_->pos_hist2 = zeros; //3x1 history of position
    state_->lock = locked_;
    state_->lock_pos = zeros;
    state_->hd_cur_transform = hduMatrix::createTranslation(0, 0, 0);

    return 0;
  }

  /*******************************************************************************
   ROS node callback.
   *******************************************************************************/
  void wrench_callback(const geometry_msgs::WrenchStampedConstPtr& wrench)
  {
    // Both force and torque supplied in the same coordinate frame
    geometry_msgs::Vector3Stamped f_in, f_out;
    geometry_msgs::Vector3Stamped t_in, t_out;
    std_msgs::Header h;

    h = wrench->header;
    h.stamp = ros::Time(0);

    t_in.header = f_in.header = h;

    f_in.vector = wrench->wrench.force;
    t_in.vector = wrench->wrench.torque;

    try
    {
      ls_.transformVector(sensable_frame_name_, f_in, f_out);
      ls_.transformVector(sensable_frame_name_, t_in, t_out);
    }
    catch(tf::TransformException& ex)
    {
      ROS_ERROR("%s", ex.what());
    }

    ////////////////////Some people might not like this extra damping, but it
    ////////////////////helps to stabilize the overall force feedback. It isn't
    ////////////////////like we are getting direct impedance matching from the
    ////////////////////omni anyway
    state_->force[0] = f_out.vector.x - damping_k_ * state_->velocity[0];
    state_->force[1] = f_out.vector.y - damping_k_ * state_->velocity[1];
    state_->force[2] = f_out.vector.z - damping_k_ * state_->velocity[2];

    // TODO torque should be split back to gimbal axes
    state_->torque[0] = t_out.vector.x;
    state_->torque[1] = t_out.vector.y;
    state_->torque[2] = t_out.vector.z;
  }

  void publish_phantom_state()
  {
    // Construct transforms
    tf::Transform l0, sensable;
    // Distance from table top to first intersection of the axes
    l0.setOrigin(tf::Vector3(0, 0, table_offset_)); // .135 - Omni, .155 - Premium 1.5, .345 - Premium 3.0
    l0.setRotation(tf::createQuaternionFromRPY(0, 0, 0));
    br_.sendTransform(tf::StampedTransform(l0, ros::Time::now(), base_link_name_.c_str(), link_names_[0].c_str()));

    // Displacement from vertical axis towards user.
    // Frame in which OpenHaptics report Phantom coordinates. Valid and useful
    // for Omni only, since other devices do not do calibration.
    sensable.setOrigin(tf::Vector3(-0.2, 0, 0));
    sensable.setRotation(tf::createQuaternionFromRPY(M_PI / 2, 0, -M_PI / 2));
    br_.sendTransform(
        tf::StampedTransform(sensable, ros::Time::now(), link_names_[0].c_str(), sensable_frame_name_.c_str()));

    tf::Transform tf_cur_transform;
    geometry_msgs::PoseStamped phantom_pose;

    // Convert column-major matrix to row-major
    tf_cur_transform.setFromOpenGLMatrix(state_->hd_cur_transform);
    // Scale from mm to m
    tf_cur_transform.setOrigin(tf_cur_transform.getOrigin() / 1000.0);
    // Since hd_cur_transform is defined w.r.t. sensable_frame
    tf_cur_transform = sensable * tf_cur_transform;
    // Rotate end-effector back to base
    tf_cur_transform.setRotation(tf_cur_transform.getRotation() * sensable.getRotation().inverse());

    // Publish pose in link_0
    phantom_pose.header.frame_id = tf::resolve(tf_prefix_, link_names_[0]);
    phantom_pose.header.stamp = ros::Time::now();
    tf::poseTFToMsg(tf_cur_transform, phantom_pose.pose);
    pose_publisher_.publish(phantom_pose);

    if ((state_->buttons[0] != state_->buttons_prev[0]) or (state_->buttons[1] != state_->buttons_prev[1]))
    {
      if ((state_->buttons[0] == state_->buttons[1]) and (state_->buttons[0] == 1))
      {
        state_->lock = !(state_->lock);
      }
      sensable_phantom::PhantomButtonEvent button_event;
      button_event.grey_button = state_->buttons[0];
      button_event.white_button = state_->buttons[1];
      state_->buttons_prev[0] = state_->buttons[0];
      state_->buttons_prev[1] = state_->buttons[1];
      button_publisher_.publish(button_event);
    }
  }
};

HDCallbackCode HDCALLBACK phantom_state_callback(void *pUserData)
{
  static bool lock_flag = true;
  PhantomState *phantom_state = static_cast<PhantomState *>(pUserData);

  hdBeginFrame(hdGetCurrentDevice());
  //Get angles, set forces
  hdGetDoublev(HD_CURRENT_GIMBAL_ANGLES, phantom_state->rot);
  hdGetDoublev(HD_CURRENT_POSITION, phantom_state->position);
  hdGetDoublev(HD_CURRENT_JOINT_ANGLES, phantom_state->joints);
  hdGetDoublev(HD_CURRENT_TRANSFORM, phantom_state->hd_cur_transform);

  hduVector3Dd vel_buff(0, 0, 0);
  vel_buff = (phantom_state->position * 3 - 4 * phantom_state->pos_hist1 + phantom_state->pos_hist2) / 0.002; //mm/s, 2nd order backward dif
  //	phantom_state->velocity = 0.0985*(vel_buff+phantom_state->inp_vel3)+0.2956*(phantom_state->inp_vel1+phantom_state->inp_vel2)-(-0.5772*phantom_state->out_vel1+0.4218*phantom_state->out_vel2 - 0.0563*phantom_state->out_vel3);    //cutoff freq of 200 Hz
  phantom_state->velocity = (.2196 * (vel_buff + phantom_state->inp_vel3)
      + .6588 * (phantom_state->inp_vel1 + phantom_state->inp_vel2)) / 1000.0
      - (-2.7488 * phantom_state->out_vel1 + 2.5282 * phantom_state->out_vel2 - 0.7776 * phantom_state->out_vel3); //cutoff freq of 20 Hz
  phantom_state->pos_hist2 = phantom_state->pos_hist1;
  phantom_state->pos_hist1 = phantom_state->position;
  phantom_state->inp_vel3 = phantom_state->inp_vel2;
  phantom_state->inp_vel2 = phantom_state->inp_vel1;
  phantom_state->inp_vel1 = vel_buff;
  phantom_state->out_vel3 = phantom_state->out_vel2;
  phantom_state->out_vel2 = phantom_state->out_vel1;
  phantom_state->out_vel1 = phantom_state->velocity;
  //	printf("position x, y, z: %f %f %f \node_", phantom_state->position[0], phantom_state->position[1], phantom_state->position[2]);
  //	printf("velocity x, y, z, time: %f %f %f \node_", phantom_state->velocity[0], phantom_state->velocity[1],phantom_state->velocity[2]);
  if (phantom_state->lock == true)
  {
    lock_flag = true;
    phantom_state->force = 0.04 * (phantom_state->lock_pos - phantom_state->position) - 0.001 * phantom_state->velocity;
  }
  else
  {
    if(lock_flag == true)
    {
      phantom_state->force.set(0.0, 0.0, 0.0);
      lock_flag = false;
    }
  }

  // Set force
  hdSetDoublev(HD_CURRENT_FORCE, phantom_state->force);
  // Set torque
  hdSetDoublev(HD_CURRENT_TORQUE, phantom_state->torque);

  //Get buttons
  int nButtons = 0;
  hdGetIntegerv(HD_CURRENT_BUTTONS, &nButtons);
  phantom_state->buttons[0] = (nButtons & HD_DEVICE_BUTTON_1) ? 1 : 0;
  phantom_state->buttons[1] = (nButtons & HD_DEVICE_BUTTON_2) ? 1 : 0;

  hdEndFrame(hdGetCurrentDevice());

  HDErrorInfo error;
  if (HD_DEVICE_ERROR(error = hdGetError()))
  {
    hduPrintError(stderr, &error, "Error during main scheduler callback\n");
    if (hduIsSchedulerError(&error))
      return HD_CALLBACK_DONE;
  }

  float t[7] = {0., phantom_state->joints[0], phantom_state->joints[1], phantom_state->joints[2] - phantom_state->joints[1],
                phantom_state->rot[0], phantom_state->rot[1], phantom_state->rot[2]};
  for (int i = 0; i < 7; i++)
    phantom_state->thetas[i] = t[i];
  return HD_CALLBACK_CONTINUE;
}

/*******************************************************************************
 Automatic Calibration of Phantom Device - No character inputs
 *******************************************************************************/
void HHD_Auto_Calibration()
{
  int calibrationStyle;
  int supportedCalibrationStyles;
  HDErrorInfo error;

  hdGetIntegerv(HD_CALIBRATION_STYLE, &supportedCalibrationStyles);
  if (supportedCalibrationStyles & HD_CALIBRATION_ENCODER_RESET)
  {
    calibrationStyle = HD_CALIBRATION_ENCODER_RESET;
    ROS_INFO("HD_CALIBRATION_ENCODER_RESET...");
  }
  if (supportedCalibrationStyles & HD_CALIBRATION_INKWELL)
  {
    calibrationStyle = HD_CALIBRATION_INKWELL;
    ROS_INFO("HD_CALIBRATION_INKWELL...");
  }
  if (supportedCalibrationStyles & HD_CALIBRATION_AUTO)
  {
    calibrationStyle = HD_CALIBRATION_AUTO;
    ROS_INFO("HD_CALIBRATION_AUTO...");
  }

  do
  {
    hdUpdateCalibration(calibrationStyle);
    ROS_INFO("Calibrating... (put stylus in well)");
    if (HD_DEVICE_ERROR(error = hdGetError()))
    {
      hduPrintError(stderr, &error, "Reset encoders reset failed.");
      break;
    }
  } while (hdCheckCalibration() != HD_CALIBRATION_OK);

  ROS_INFO("Calibration complete.");
}

void *ros_publish(void *ptr)
{
  PhantomROS *phantom_ros = (PhantomROS *)ptr;
  int publish_rate;

  // reading param from private namespace
  phantom_ros->pnode_->param(std::string("publish_rate"), publish_rate, 100);

  ros::Rate loop_rate(publish_rate);
  ros::AsyncSpinner spinner(2);
  spinner.start();

  while (ros::ok())
  {
    phantom_ros->publish_phantom_state();
    loop_rate.sleep();
  }
  return NULL;
}

int main(int argc, char** argv)
{
  ////////////////////////////////////////////////////////////////
  // Init ROS
  ////////////////////////////////////////////////////////////////
  ros::init(argc, argv, "phantom_node");
  PhantomState state;
  PhantomROS phantom_ros;

  ////////////////////////////////////////////////////////////////
  // Init Phantom
  ////////////////////////////////////////////////////////////////
  HDErrorInfo error;
  HHD hHD;
  hHD = hdInitDevice(HD_DEFAULT_DEVICE);
  if (HD_DEVICE_ERROR(error = hdGetError()))
  {
    //hduPrintError(stderr, &error, "Failed to initialize haptic device");
    ROS_ERROR("Failed to initialize haptic device");
    //: %s", &error);
    return -1;
  }

  ROS_INFO("Found %s", hdGetString(HD_DEVICE_MODEL_TYPE));
  hdEnable(HD_FORCE_OUTPUT);
//   hdEnable(HD_MAX_FORCE_CLAMPING);
  hdStartScheduler();
  if (HD_DEVICE_ERROR(error = hdGetError()))
  {
    ROS_ERROR("Failed to start the scheduler");
    //, &error);
    return -1;
  }

  if(phantom_ros.init(&state))
  {
    hdStopScheduler();
    hdDisableDevice(hHD);
    return -1;
  }
  
  if(phantom_ros.calibrate_)
  {
    HHD_Auto_Calibration();
  }

  hdScheduleAsynchronous(phantom_state_callback, &state, HD_MAX_SCHEDULER_PRIORITY);

  ////////////////////////////////////////////////////////////////
  // Loop and publish
  ////////////////////////////////////////////////////////////////
  pthread_t publish_thread;
  pthread_create(&publish_thread, NULL, ros_publish, (void*)&phantom_ros);
  pthread_join(publish_thread, NULL);

  ROS_INFO("Ending Session...");
  hdStopScheduler();
  hdDisableDevice(hHD);

  return 0;
}

