/*
 * Copyright (C) 2016 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
#include <functional>
#include <fcntl.h>
#include <cstdlib>


#ifdef _WIN32
  #include <Winsock2.h>
  #include <Ws2def.h>
  #include <Ws2ipdef.h>
  #include <Ws2tcpip.h>
  using raw_type = char;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  using raw_type = void;
#endif

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#include <mutex>
#include <string>
#include <vector>
#include <sdf/sdf.hh>
#include <boost/algorithm/string.hpp>
#include <ignition/math/Filter.hh>
#include <gazebo/common/Assert.hh>
#include <gazebo/common/Plugin.hh>
#include <gazebo/msgs/msgs.hh>
#include <gazebo/sensors/sensors.hh>
#include <gazebo/transport/transport.hh>
#include <gazebo/physics/Base.hh>

#include "FlightControllerPlugin.hh"

#include "MotorCommand.pb.h"
#include "EscSensor.pb.h"
#include "Imu.pb.h"

#include "State.pb.h"
#include "Action.pb.h"

/// \brief Obtains a parameter from sdf.
/// \param[in] _sdf Pointer to the sdf object.
/// \param[in] _name Name of the parameter.
/// \param[out] _param Param Variable to write the parameter to.
/// \param[in] _default_value Default value, if the parameter not available.
/// \param[in] _verbose If true, gzerror if the parameter is not available.
/// \return True if the parameter was found in _sdf, false otherwise.
template<class T>
bool getSdfParam(sdf::ElementPtr _sdf, const std::string &_name,
  T &_param, const T &_defaultValue, const bool &_verbose = false)
{
  if (_sdf->HasElement(_name))
  {
    _param = _sdf->GetElement(_name)->Get<T>();
    return true;
  }

  _param = _defaultValue;
  if (_verbose)
  {
    gzerr << "[FlightControllerPlugin] Please specify a value for parameter ["
      << _name << "].\n";
  }
  return false;
}

// Helper function to find link with suffix 
bool hasEnding (std::string const &fullString, std::string const &ending) {
    if (fullString.length() >= ending.length()) {
        return (0 == fullString.compare (fullString.length() - ending.length(), ending.length(), ending));
    } else {
        return false;
    }
}



using namespace gazebo;


GZ_REGISTER_WORLD_PLUGIN(FlightControllerPlugin)

boost::mutex g_CallbackMutex;

FlightControllerPlugin::FlightControllerPlugin() 
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // socket
  this->handle = socket(AF_INET, SOCK_DGRAM /*SOCK_STREAM*/, 0);
  #ifndef _WIN32
  // Windows does not support FD_CLOEXEC
  fcntl(this->handle, F_SETFD, FD_CLOEXEC);
  #endif
  int one = 1;
  setsockopt(this->handle, IPPROTO_TCP, TCP_NODELAY,
      reinterpret_cast<const char *>(&one), sizeof(one));


  setsockopt(this->handle, SOL_SOCKET, SO_REUSEADDR,
     reinterpret_cast<const char *>(&one), sizeof(one));

  #ifdef _WIN32
  u_long on = 1;
  ioctlsocket(this->handle, FIONBIO,
              reinterpret_cast<u_long FAR *>(&on));
  #else
  fcntl(this->handle, F_SETFL,
      fcntl(this->handle, F_GETFL, 0) | O_NONBLOCK);
  #endif

}
FlightControllerPlugin::~FlightControllerPlugin()
{
    // Tear down the transporter
    gazebo::transport::fini();

	  // Sleeps (pauses the destructor) until the thread has finished
	  this->callbackLoopThread.join();
}


void FlightControllerPlugin::Load(physics::WorldPtr _world, sdf::ElementPtr _sdf)
{

  this->world = _world;
  this->ProcessSDF(_sdf);

  this->LoadVars();
  this->CalculateCallbackCount();

  this->nodeHandle = transport::NodePtr(new transport::Node());
  this->nodeHandle->Init(this->robotNamespace);

  //Subscribe to all the sensors that are
  //enabled
  for (auto  sensor : this->supportedSensors)
  {
    switch(sensor)
    {
      case IMU:
        this->imuSub = this->nodeHandle->Subscribe<sensor_msgs::msgs::Imu>(this->imuSubTopic, &FlightControllerPlugin::ImuCallback, this);
        break;
      case ESC:
        //Each defined motor will have a unique index, since they are indpendent they must come in 
        //as separate messages
        for (unsigned int i = 0; i < this->numActuators; i++)
        {
          this->escSub.push_back(this->nodeHandle->Subscribe<sensor_msgs::msgs::EscSensor>(this->escSubTopic + "/" + std::to_string(i) , &FlightControllerPlugin::EscSensorCallback, this));
        }
        break;
    }
  }
  this->InitState();

  this->cmdPub = this->nodeHandle->Advertise<cmd_msgs::msgs::MotorCommand>(this->cmdPubTopic);
  // Force pause because we drive the simulation steps
  this->world->SetPaused(TRUE);

  this->callbackLoopThread = boost::thread( boost::bind( &FlightControllerPlugin::LoopThread, this) );
}
bool FlightControllerPlugin::SensorEnabled(Sensors _sensor)
{
  for (auto  sensor : this->supportedSensors)
  {
    if (sensor == _sensor)
    {
      return true;
    }
  }
  return false;

}
void FlightControllerPlugin::LoadVars()
{

  //XXX This is getting messy, if there is anything the plugin needs
  //we need to switch to a config file

  // Default port can be read in from an environment variable
  // This allows multiple instances to be run
  int port = 9002;
  if(const char* env_p =  std::getenv(ENV_SITL_PORT))
  {
		  port = std::stoi(env_p);
  }
  gzdbg << "Binding on port " << port << "\n";
  if (!this->Bind("127.0.0.1", port))
  {
    gzerr << "failed to bind with 127.0.0.1:" << port <<", aborting plugin.\n";
    return;
  }

  if(const char* env_p =  std::getenv(ENV_DIGITAL_TWIN_SDF))
  {
    this->digitalTwinSDF = env_p;
  } else 
  {
    gzerr << "Could not load digital twin model from environment variable " << ENV_DIGITAL_TWIN_SDF << "\n";
    return;
  }

  if(const char* env_p =  std::getenv(ENV_NUM_MOTORS))
  {
    this->numActuators = std::stoi(env_p);
  } else 
  {
    gzerr << "Environment variable " << ENV_NUM_MOTORS << " not set.\n";
    return;
  }

  if(const char* env_p =  std::getenv(ENV_SUPPORTED_SENSORS)) 
  {
    std::vector<std::string> results;
    boost::split(results, env_p, [](char c){return c == ',';});
    for (auto  s : results) 
    {
      if (boost::iequals(s, "imu"))
      {
        this->supportedSensors.push_back(IMU);
      } 
      else if (boost::iequals(s, "esc"))
      {
        this->supportedSensors.push_back(ESC);
      }

    }
  }
  else 
  {
    gzerr << "Environment variable " << ENV_SUPPORTED_SENSORS << " not set.\n";
    return;
  }

}

void FlightControllerPlugin::InitState()
{
  // Initialize the state of the senors to a value
  // that reflect the aircraft in an active state thus 
  // forcing the sensors to be flushed.
  for (unsigned int i = 0; i < 3; i++)
  {
    this->state.add_imu_angular_velocity_rpy(1);
    // TODO 
    this->state.add_imu_linear_acceleration_xyz(0);
  }
  for (unsigned int i = 0; i < 4; i++)
  {
    // TODO 
    this->state.add_imu_orientation_quat(0);
  }


  // ESC sensor 
  for (unsigned int i = 0; i < this->numActuators; i++)
  {
    this->state.add_esc_motor_angular_velocity(100);
    this->state.add_esc_temperature(10000);
    this->state.add_esc_current(-1);
    this->state.add_esc_voltage(-1);
  }

}

void FlightControllerPlugin::EscSensorCallback(EscSensorPtr &_escSensor)
{
  boost::mutex::scoped_lock lock(g_CallbackMutex);

  uint32_t id = _escSensor->id();  
  this->state.set_esc_motor_angular_velocity(id, _escSensor->motor_speed());
  this->state.set_esc_temperature(id, _escSensor->temperature());
  this->state.set_esc_current(id, _escSensor->current());
  this->state.set_esc_voltage(id, _escSensor->voltage());
  this->sensorCallbackCount++;
  this->callbackCondition.notify_all();

}
void FlightControllerPlugin::ImuCallback(ImuPtr &_imu)
{
  boost::mutex::scoped_lock lock(g_CallbackMutex);
  //gzdbg << "Received IMU" << std::endl;

  this->state.set_imu_angular_velocity_rpy(0, _imu->angular_velocity().x());
  this->state.set_imu_angular_velocity_rpy(1, _imu->angular_velocity().y());
  this->state.set_imu_angular_velocity_rpy(2, _imu->angular_velocity().z());

  this->state.set_imu_orientation_quat(0, _imu->orientation().w());
  this->state.set_imu_orientation_quat(1, _imu->orientation().x());
  this->state.set_imu_orientation_quat(2, _imu->orientation().y());
  this->state.set_imu_orientation_quat(3, _imu->orientation().z());

  this->state.set_imu_linear_acceleration_xyz(0, _imu->linear_acceleration().x());
  this->state.set_imu_linear_acceleration_xyz(1, _imu->linear_acceleration().y());
  this->state.set_imu_linear_acceleration_xyz(2, _imu->linear_acceleration().z());

  this->sensorCallbackCount++;
  this->callbackCondition.notify_all();

}
void FlightControllerPlugin::ProcessSDF(sdf::ElementPtr _sdf)
{
  this->cmdPubTopic = kDefaultCmdPubTopic;
  if (_sdf->HasElement("commandPubTopic")){
      this->cmdPubTopic = _sdf->GetElement("commandPubTopic")->Get<std::string>();
  }
  this->imuSubTopic = kDefaultImuSubTopic;
  if (_sdf->HasElement("imuSubTopic")){
      this->imuSubTopic = _sdf->GetElement("imuSubTopic")->Get<std::string>();
  }
  this->escSubTopic = kDefaultEscSubTopic;
  if (_sdf->HasElement("escSubTopicPrefix")){
      this->escSubTopic = _sdf->GetElement("escSubTopicPrefix")->Get<std::string>();
  }


  if (_sdf->HasElement("robotNamespace"))
    this->robotNamespace = _sdf->GetElement("robotNamespace")->Get<std::string>();
  else
    gzerr << "[gazebo_motor_model] Please specify a robotNamespace.\n";


}

void FlightControllerPlugin::SoftReset()
{
  this->world->ResetTime();
  this->world->ResetEntities(gazebo::physics::Base::BASE);
	this->world->ResetPhysicsStates();
}


physics::LinkPtr FlightControllerPlugin::FindLinkByName(physics::ModelPtr _model, std::string _linkName)
{
  for (auto link : _model->GetLinks())
  {
    //gzdbg << "Link name: " << link->GetName() << std::endl;
    if (hasEnding(link->GetName(), _linkName))
    {
      return link;
    }

  }
  return NULL;

}
void FlightControllerPlugin::LoadDigitalTwin()
{
  gzdbg << "Inserting digital twin from, " << this->digitalTwinSDF << ".\n";
   // Load the root digital twin sdf file
  const std::string sdfPath(this->digitalTwinSDF);

  sdf::SDFPtr sdfElement(new sdf::SDF());
  sdf::init(sdfElement);
  if (!sdf::readFile(sdfPath, sdfElement))
  {
    gzerr << sdfPath << " is not a valid SDF file!" << std::endl;
    return;
  }

  // start parsing model
  const sdf::ElementPtr rootElement = sdfElement->Root();
  if (!rootElement->HasElement("model"))
  {
    gzerr << sdfPath << " is not a model SDF file!" << std::endl;
    return;
  }
  const sdf::ElementPtr modelElement = rootElement->GetElement("model");
  const std::string modelName = modelElement->Get<std::string>("name");
  //gzdbg << "Found " << modelName << " model!" << std::endl;

  unsigned int startModelCount = this->world->ModelCount();
  //this->world->InsertModelFile(sdfElement);
  
  this->world->InsertModelSDF(*sdfElement);

  // TODO Better way to do this?
  // It appears the inserted model is not available in the world
  // right away, maybe due to message passing?
  // Poll until its there
  while (1)
  {
    unsigned int modelCount = this->world->ModelCount();
    if (modelCount >= startModelCount + 1)
    {
      break;
    } else {
      gazebo::common::Time::MSleep(1000);
    }
  }
  
  //gzdbg << "Num models=" << this->world->ModelCount() << std::endl;
  for (unsigned int i=0; i<this->world->ModelCount(); i++)
  {
    //gzdbg << "Model " << i << ":" << this->world->ModelByIndex(i)->GetScopedName() << std::endl;
  }

  // Now get a pointer to the model
  physics::ModelPtr model = this->world->ModelByName(modelName);
  if (!model){
    gzerr << "Could not access model " << modelName <<" from world"<<std::endl;
    return;
  }

  //Find the base link to attached to the world
  physics::LinkPtr digitalTwinCoMLink = FindLinkByName(model, DIGITAL_TWIN_ATTACH_LINK);
  if (!link)
  {
    gzerr << "Could not find link '" << DIGITAL_TWIN_ATTACH_LINK <<" from model " << modelName <<std::endl;
    return;
  } 

  
  physics::ModelPtr trainingRigModel = this->world->ModelByName(kTrainingRigModelName);
  if (!trainingRigModel){
    gzerr << "Could not find training rig"<<std::endl;
    return;
  }

  


  // Create the actual ball link, connecting the digital twin to the sim world
  physics::JointPtr joint = trainingRigModel->CreateJoint("ball_joint", "ball", trainingRigModel->GetLink("pivot"), digitalTwinCoMLink);
  if (!joint)
  {
    gzerr << "Could not create joint"<<std::endl;
    return;
  }
  joint->Init();
  
  // This is actually great because we've removed the ground plane so there is no possible collision
  //gzdbg << "Ball joint created\n";
}

void FlightControllerPlugin::FlushSensors()
{
  // Make sure we do a reset on the time even if sensors are within range 
  // XXX The first episode the sensor values are set to some active value
  // to force plugins to send action state.
  this->SoftReset();
  double error = 0.017;// About 1 deg/s
  while (1)
  {
      // Pitch and Yaw are negative
      //gzdbg << " Size =" << this->state.imu_angular_velocity_rpy_size() << std::endl;
      //gzdbg << "IMU [" << this->state.imu_angular_velocity_rpy(0) << "," << this->state.imu_angular_velocity_rpy(1) << "," << this->state.imu_angular_velocity_rpy(2) << "]" << std::endl;
      if (this->state.imu_angular_velocity_rpy_size() < 2 ||
          (
          std::abs(this->state.imu_angular_velocity_rpy(0)) > error || 
          std::abs(this->state.imu_angular_velocity_rpy(1)) > error || 
          std::abs(this->state.imu_angular_velocity_rpy(2)) > error)){
        //gzdbg << "Gyro r=" << rates.X() << " p=" << rates.Y() << " y=" << rates.Z() << "\n";
        //
        // Trigger all plugins to publish their values
        this->world->Step(1);
        this->SoftReset();
        //boost::this_thread::sleep(boost::posix_time::milliseconds(100));
      } else {
          //gzdbg << "Target velocity reached! r=" << rates.X() << " p=" << rates.Y() << " y=" << rates.Z() << "\n";
        break;
      }
  }

}
void FlightControllerPlugin::LoopThread()
{

  this->LoadDigitalTwin();

	while (1){

		bool ac_received = this->ReceiveAction();

		if (!ac_received){
        continue;
    }
    //gzdbg << "Action = " << ac->motor(0) << "," << ac->motor(1) << "," << ac->motor(2) << "," << ac->motor(3) << std::endl;

    // Handle reset command
    if (this->action.world_control() == gymfc::msgs::Action::RESET)
    {
      //gzdbg << " Flushing sensors..." << std::endl;
      // Block until we get respone from sensors
      this->FlushSensors();
      //gzdbg << " Sensors flushed." << std::endl;
      this->state.set_sim_time(this->world->SimTime().Double());
      this->state.set_status_code(gymfc::msgs::State_StatusCode_OK);
      this->SendState();
      continue;
    }

    this->ResetCallbackCount();

    //Forward the motor commands from the agent to each motor
    cmd_msgs::msgs::MotorCommand cmd;
    //gzdbg << "Sending motor commands to digital twin" << std::endl;
    for (unsigned int i = 0; i < this->numActuators; i++)
    {
      //gzdbg << i << "=" << this->motor[i] << std::endl;
      cmd.add_motor(this->action.motor(i));
    }
    this->cmdPub->Publish(cmd);
    // Triggers other plugins to publish
    this->world->Step(1);
    this->WaitForSensorsThenSend();
	}
}
void FlightControllerPlugin::ResetCallbackCount()
{
  boost::mutex::scoped_lock lock(g_CallbackMutex);
  this->sensorCallbackCount = -1 * this->numSensorCallbacks;
}

void FlightControllerPlugin::CalculateCallbackCount()
{
  // Reset the callback count, once we step the sim all the new
  // vales will be published

  for (auto  sensor : this->supportedSensors)
  {
    switch(sensor){
      case IMU:
        this->numSensorCallbacks += 1;
        break;
      case ESC:
        this->numSensorCallbacks += this->numActuators;
        break;
    }
  }

} 
void FlightControllerPlugin::WaitForSensorsThenSend()
{
  this->state.set_sim_time(this->world->SimTime().Double());
  this->state.set_status_code(gymfc::msgs::State_StatusCode_OK);

  boost::mutex::scoped_lock lock(g_CallbackMutex);
  while (this->sensorCallbackCount < 0)
  {
    //gzdbg << "Callback count = " << this->sensorCallbackCount << std::endl;
    this->callbackCondition.wait(lock);
  }
  //gzdbg << "Sending state"<<std::endl;
  this->SendState();
}

bool FlightControllerPlugin::Bind(const char *_address, const uint16_t _port)
{
  struct sockaddr_in sockaddr;
  this->MakeSockAddr(_address, _port, sockaddr);

  if (bind(this->handle, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) != 0)
  {
    shutdown(this->handle, 0);
    #ifdef _WIN32
    closesocket(this->handle);
    #else
    close(this->handle);
    #endif
    return false;
  }
  return true;
}

void FlightControllerPlugin::MakeSockAddr(const char *_address, const uint16_t _port,
  struct sockaddr_in &_sockaddr)
{
  memset(&_sockaddr, 0, sizeof(_sockaddr));

  #ifdef HAVE_SOCK_SIN_LEN
    _sockaddr.sin_len = sizeof(_sockaddr);
  #endif

  _sockaddr.sin_port = htons(_port);
  _sockaddr.sin_family = AF_INET;
  _sockaddr.sin_addr.s_addr = inet_addr(_address);
}

bool FlightControllerPlugin::ReceiveAction()
{

  //TODO What should the buf size be? How do we estimate the protobuf size?
  unsigned int buf_size = 1024;
  char buf[buf_size];

	int recvSize;
  recvSize = recvfrom(this->handle, buf, buf_size , 0, (struct sockaddr *)&this->remaddr, &this->remaddrlen);

  if (recvSize < 0)
  {
    return false;
  }
  //gzdbg << "Size " << recvSize << " Data " << buf << std::endl;
  std::string msg;
  // Do the reassignment because protobuf needs string
  msg.assign(buf, recvSize);
  this->action.ParseFromString(msg);
  //gzdbg << " Motor Size " << ac.motor_size() << std::endl;
  //gzdbg << " World Control " << ac.world_control() << std::endl;

  return true; 
}

/////////////////////////////////////////////////
void FlightControllerPlugin::SendState() const
{
  std::string buf;
  this->state.SerializeToString(&buf);

  //gzdbg << " Buf data= " << buf.data() << std::endl;
  //gzdbg << " Buf size= " << buf.size() << std::endl;
  ::sendto(this->handle,
           buf.data(),
           buf.size(), 0,
		   (struct sockaddr *)&this->remaddr, this->remaddrlen); 
}



