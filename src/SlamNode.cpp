/*
 * SlamNode.cpp
 *
 *  Created on: 05.05.2014
 *      Author: phil
 */

#include "SlamNode.h"
#include "Localization.h"
#include "ThreadMapping.h"
#include "ThreadGrid.h"
#include "obcore/math/mathbase.h"
#include <unistd.h>
#include <omp.h>

namespace ohm_tsd_slam
{
SlamNode::SlamNode(const std::string& content, obvious::EnumTsdGridLoadSource source)
{
  ros::NodeHandle prvNh("~");
  std::string strVar;
  std::string storeMapTopic;
  int octaveFactor        = 0;
  double cellsize         = 0.0;
  double dVar             = 0;
  int iVar                = 0;
  double truncationRadius = 0.0;
  prvNh.param        ("laser_topic", strVar, std::string("simon/scan"));
  prvNh.param        ("store_map_topic", storeMapTopic, std::string("store_map"));
  prvNh.param<int>   ("cell_octave_factor", octaveFactor, 10);
  prvNh.param<double>("cellsize", cellsize, 0.025);
  prvNh.param<int>   ("truncation_radius", iVar, 3);
  truncationRadius = static_cast<double>(iVar);
  prvNh.param<double>("max_range", _maxRange, 30.0);
  prvNh.param<double>("occ_grid_time_interval", _gridPublishInterval, 2.0);
  prvNh.param<double>("loop_rate", _loopRate, 40.0);

  _laserSubs = _nh.subscribe(strVar, 1, &SlamNode::laserScanCallBack, this);

  unsigned int uiVar = static_cast<unsigned int>(octaveFactor);
  if((uiVar > 15) || (uiVar < 5))
  {
    std::cout << __PRETTY_FUNCTION__ << " error! Unknown / Invalid cell_octave_factor -> set to default!" << std::endl;
    uiVar = 10;
  }
  _initialized = false;
  if(content.size())
  {
    std::cout << __PRETTY_FUNCTION__ << " node in localize only mode" << std::endl;
    _grid = new obvious::TsdGrid(content, source);
    _localizeOnly = true;
    std::cout << __PRETTY_FUNCTION__ << "load map with " << _grid->getCellsX() << " x " << _grid->getCellsY() << " cells, " << _grid->getCellSize()
                        << " cellsize" << std::endl;
  }
  else
  {
    std::cout << __PRETTY_FUNCTION__ << " slam node started" << std::endl;
    _grid        = new obvious::TsdGrid(cellsize, obvious::LAYOUT_32x32, static_cast<obvious::EnumTsdGridLayout>(uiVar));
    _grid->setMaxTruncation(truncationRadius * cellsize);
    _localizeOnly = false;
    unsigned int cellsPerSide = std::pow(2, uiVar);
    std::cout << __PRETTY_FUNCTION__ << " creating representation with " << cellsPerSide << "x" << cellsPerSide;
    double sideLength = static_cast<double>(cellsPerSide) * cellsize;
    std::cout << " cells, representating "<< sideLength << "x" << sideLength << "m^2" << std::endl;
  }

  //  _grid        = new obvious::TsdGrid(cellsize, obvious::LAYOUT_32x32, static_cast<obvious::EnumTsdGridLayout>(uiVar));
  //  _grid->setMaxTruncation(truncationRadius * cellsize);

  unsigned int cellsPerSide = std::pow(2, uiVar);
  std::cout << __PRETTY_FUNCTION__ << " creating representation with " << cellsPerSide << "x" << cellsPerSide;
  double sideLength = static_cast<double>(cellsPerSide) * cellsize;
  std::cout << " cells, representating "<< sideLength << "x" << sideLength << "m^2" << std::endl;

  _sensor        = NULL;
  _localizer     = NULL;
  _threadMapping = NULL;
  _threadGrid    = NULL;
}

SlamNode::~SlamNode()
{
  if(_initialized)
  {
    _threadMapping->terminateThread();
    _threadGrid->terminateThread();
    delete _threadGrid;
    delete _threadMapping;
  }
  if(_localizer)
    delete _localizer;
  if(_grid)
    delete _grid;
  if(_sensor)
    delete _sensor;
}

void SlamNode::start(void)
{
  if(1)//!_localizeOnly)
    this->run();
  else
    localizeOnly();
}

void SlamNode::initialize(const sensor_msgs::LaserScan& initScan)
{
  double xOffFactor = 0.0;
  double yOffFactor = 0.0;
  double yawOffset  = 0.0;
  double minRange = 0.0;
  double maxRange = 0.0;
  double lowReflectivityRange = 0.0;
  double footPrintWidth= 0.0;
  double footPrintHeight= 0.0;
  double footPrintXoffset= 0.0;
  bool   icpSac = false;

  ros::NodeHandle prvNh("~");
  prvNh.param<double>("x_off_factor", xOffFactor, 0.2);
  prvNh.param<double>("y_off_factor", yOffFactor, 0.5);
  prvNh.param<double>("yaw_offset", yawOffset, 0.0);
  prvNh.param<double>("min_range", minRange, 0.01);
  prvNh.param<double>("low_reflectivity_range", lowReflectivityRange, 2.0);
  prvNh.param<double>("footprint_width" , footPrintWidth, 0.1);
  prvNh.param<double>("footprint_height", footPrintHeight, 0.1);
  prvNh.param<double>("footprint_x_offset", footPrintXoffset, 0.28);
  prvNh.param<bool>  ("use_icpsac", icpSac, true);

  _sensor=new obvious::SensorPolar2D(initScan.ranges.size(), initScan.angle_increment, initScan.angle_min, _maxRange, minRange, lowReflectivityRange);
  _sensor->setRealMeasurementData(initScan.ranges, 1.0);

  const double phi        = yawOffset;
  const double gridWidth  = _grid->getCellsX() * _grid->getCellSize();
  const double gridHeight = _grid->getCellsY() * _grid->getCellSize();
  const obfloat startX    = static_cast<obfloat>(gridWidth  * xOffFactor);
  const obfloat startY    = static_cast<obfloat>(gridHeight * yOffFactor);

  double tf[9] = {cos(phi), -sin(phi), gridWidth * xOffFactor,
      sin(phi),  cos(phi), gridHeight * yOffFactor,
      0,         0,                        1};
  obvious::Matrix Tinit(3, 3);
  Tinit.setData(tf);
  _sensor->transform(&Tinit);

  if(!_localizeOnly)
  {
    _threadMapping = new ThreadMapping(_grid);
    const obfloat t[2] = {startX, startY};
    if(!_grid->freeFootprint(t, footPrintWidth, footPrintHeight))
      std::cout << __PRETTY_FUNCTION__ << " warning! Footprint could not be freed!\n";
    _threadMapping->initPush(_sensor);
  }

  _localizer   = new Localization(_grid, _threadMapping, _nh, xOffFactor, yOffFactor, icpSac);
  _threadGrid  = new ThreadGrid(_grid, _nh, xOffFactor, yOffFactor, _localizeOnly);
  _initialized = true;
}

void SlamNode::run(void)
{
  ros::Time lastMap=ros::Time::now();
  ros::Duration durLastMap=ros::Duration(_gridPublishInterval);
  ros::Rate rate(_loopRate);
  std::cout << __PRETTY_FUNCTION__ << " waiting for first laser scan to initialize node...\n";
  while(ros::ok())
  {
    ros::spinOnce();
    if(_initialized)
    {
      ros::Time curTime=ros::Time::now();
      if((curTime-lastMap).toSec()>durLastMap.toSec())
      {
        _threadGrid->unblock();
        lastMap=ros::Time::now();
      }
    }
    rate.sleep();
  }
}

void SlamNode::localizeOnly(void)
{
  ros::spin();
}

void SlamNode::laserScanCallBack(const sensor_msgs::LaserScan& scan)
{
  sensor_msgs::LaserScan tmpScan = scan;
  for(std::vector<float>::iterator iter = tmpScan.ranges.begin(); iter != tmpScan.ranges.end(); iter++)
  {
    if(isnan(*iter))
      *iter = 0.0;
    else if(*iter > _maxRange)
      *iter = INFINITY;
  }
  if(!_initialized)
  {
    std::cout << __PRETTY_FUNCTION__ << " received first scan. Initialize node...\n";
    this->initialize(tmpScan);
    std::cout << __PRETTY_FUNCTION__ << " initialized -> running...\n";
    return;
  }
  _sensor->setRealMeasurementData(scan.ranges, 1.0);
  _sensor->resetMask();
  _sensor->maskZeroDepth();
  _sensor->maskInvalidDepth();
  _sensor->maskDepthDiscontinuity(obvious::deg2rad(3.0));
  _localizer->localize(_sensor);
}

bool SlamNode::storeMapServiceCallBack(std_srvs::Empty::Request& req, std_srvs::Empty::Response& res)
{
  return _threadGrid->requestStoreTsdGrid();
}

} /* namespace ohm_tsdSlam2 */
