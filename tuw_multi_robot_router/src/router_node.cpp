/*
 * Copyright (c) 2017, <copyright holder> <email>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY <copyright holder> <email> ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <copyright holder> <email> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#define POT_HIGH 1.0e10

#include <tuw_global_router/router_node.h>
#include <tuw_global_router/srr_utils.h>
#include <tuw_multi_robot_msgs/Route.h>
#include <chrono>
#include <boost/functional/hash.hpp>
#include <boost/regex.hpp>
#include <tf/tf.h>

//TODO add Weights from robots...

int main ( int argc, char **argv ) {

    ros::init ( argc, argv, "tuw_multi_robot_router" ); /// initializes the ros node with default name
    ros::NodeHandle n;

    ros::Rate r ( 5 );

    multi_robot_router::Router_Node node ( n );

    while ( ros::ok() ) {
        r.sleep();
        ros::spinOnce();
        node.updateTimeout ( r.expectedCycleTime().toSec() );
    }

    return 0;
}


namespace multi_robot_router {

Router_Node::Router_Node ( ros::NodeHandle &_n ) : Router(),
    n_ ( _n ),
    n_param_ ( "~" ) {
    id_ = 0;

    planner_status_topic_ = "planner_status";
    n_param_.param ( "planner_status_topic", planner_status_topic_, planner_status_topic_ );

    goal_topic_ = "goals";
    n_param_.param ( "goal_topic", goal_topic_, goal_topic_ );

    map_topic_ = "/map";
    n_param_.param ( "map_topic", map_topic_, map_topic_ );

    voronoi_topic_ = "segments";
    n_param_.param ( "graph_topic", voronoi_topic_, voronoi_topic_ );

    robot_info_topic_ = "/robot_info";
    n_param_.param ( "robot_info", robot_info_topic_, robot_info_topic_ );

    singleRobotName_ = "";
    n_param_.param ( "robot_name", singleRobotName_, singleRobotName_ );

    singleRobotGoalTopic_ = "/goal";
    n_param_.param ( "robot_goal", singleRobotGoalTopic_, singleRobotGoalTopic_ );

    // static subscriptions
    subGoalSet_ = n_.subscribe ( goal_topic_, 1, &Router_Node::goalsCallback, this );
    subMap_ = n_.subscribe ( map_topic_, 1, &Router_Node::mapCallback, this );
    subVoronoiGraph_ = n_.subscribe ( voronoi_topic_, 1, &Router_Node::graphCallback, this );
    subRobotInfo_ = n_.subscribe ( robot_info_topic_, 10000, &Router_Node::robotInfoCallback, this );

    if ( !singleRobotName_.size() == 0 ) {
        subSingleRobotGoal_ = n_.subscribe ( singleRobotGoalTopic_, 1, &Router_Node::goalCallback, this );
    }

    //static publishers
    pubPlannerStatus_ = n_.advertise<tuw_multi_robot_msgs::RouterStatus> ( planner_status_topic_, 1 );

    //dynamic reconfigure
    call_type = boost::bind ( &Router_Node::parametersCallback, this, _1, _2 );
    param_server.setCallback ( call_type );
}

void Router_Node::goalCallback ( const geometry_msgs::PoseStamped &_goal ) {
    tuw_multi_robot_msgs::RobotGoals goal;
    goal.robot_name = singleRobotName_;
    goal.destinations.push_back ( _goal.pose );

    tuw_multi_robot_msgs::RobotGoalsArray goals;
    goals.robots.push_back ( goal );

    goalsCallback ( goals );
}

void Router_Node::updateTimeout ( const float _secs ) {
    //Todo update timeouts and clear old messages
    for ( auto it = subscribed_robots_.begin(); it != subscribed_robots_.end(); it++ ) {
        ( *it )->updateStatus ( _secs );
    }

    for ( auto it = subscribed_robots_.begin(); it != subscribed_robots_.end(); it++ ) {
        ( *it )->updateStatus ( _secs );
    }
}
void Router_Node::parametersCallback ( tuw_multi_robot_router::routerConfig &config, uint32_t level ) {
    //Important set router before settings
    uint32_t threads = config.nr_threads;
    if ( config.router_type == 1 )
        setPlannerType ( routerType::multiThreadSrr, threads );
    else
        setPlannerType ( routerType::singleThread, 1 );

    if ( config.collision_resolver == 0 )
        setCollisionResolutionType ( SegmentExpander::CollisionResolverType::none );
    else if ( config.collision_resolver == 1 )
        setCollisionResolutionType ( SegmentExpander::CollisionResolverType::backtracking );
    else
        setCollisionResolutionType ( SegmentExpander::CollisionResolverType::avoidance );

    if ( config.voronoi_graph )
        graphMode_ = graphType::voronoi;
    else
        graphMode_ = graphType::random;

    if ( config.goal_mode == 0 )
        goalMode_ = goalMode::use_map_goal;
    else if ( config.goal_mode == 1 )
        goalMode_ = goalMode::use_voronoi_goal;
    else
        goalMode_ = goalMode::use_segment_goal;

    routerTimeLimit_s_ = config.router_time_limit_s;
    topic_timeout_s_ = config.topic_timeout_s;

    priorityRescheduling_ = config.priority_rescheduling;
    speedRescheduling_ = config.speed_rescheduling;
    segmentOptimizations_ = config.path_endpoint_optimization;
}

void Router_Node::mapCallback ( const nav_msgs::OccupancyGrid &_map ) {
    std::vector<signed char> map = _map.data;

    Eigen::Vector2d origin;
    origin[0] = _map.info.origin.position.x;
    origin[1] = _map.info.origin.position.y;

    size_t new_hash = getHash ( map, origin, _map.info.resolution );

    ROS_INFO ( "map %f %f %f", origin[0], origin[1], _map.info.resolution );

    if ( new_hash != current_map_hash_ ) {
        mapOrigin_[0] = _map.info.origin.position.x;
        mapOrigin_[1] = _map.info.origin.position.y;
        mapResolution_ = _map.info.resolution;

        cv::Mat m ( _map.info.height, _map.info.width, CV_8SC1, map.data() );

        m.convertTo ( m, CV_8UC1 );
        cv::bitwise_not ( m, m );

        cv::threshold ( m, m, 40, 255, CV_THRESH_BINARY | CV_THRESH_OTSU );
        cv::distanceTransform ( m, distMap_, CV_DIST_L1, 3 );

        current_map_hash_ = new_hash;
        got_map_ = true;

        ROS_INFO ( "Multi Robot Router: New Map %i %i %lu", _map.info.width, _map.info.height, current_map_hash_ );
    }
}


float Router_Node::calcRadius ( const int shape, const std::vector<float> &shape_variables ) const {
    tuw_multi_robot_msgs::RobotInfo ri;
    if ( shape == ri.SHAPE_CIRCLE ) {
        return shape_variables[0];
    }

    return -1;
}

void Router_Node::robotInfoCallback ( const tuw_multi_robot_msgs::RobotInfo &_robotInfo ) {
    TopicStatus s ( TopicStatus::status::active, topic_timeout_s_ );

    auto robot = RobotInfo::findObj ( subscribed_robots_, _robotInfo.robot_name );
    if ( robot == subscribed_robots_.end() ) {
        // create new entry
        RobotInfoPtr robot_new = std::make_shared<RobotInfo> ( _robotInfo );
        robot_new->initTopics ( n_ );
        subscribed_robots_.push_back ( robot_new );
    } else {
        ( *robot )->updateInfo ( _robotInfo );
    }

    robot_radius_max_ = 0;
    for ( RobotInfoPtr &r: subscribed_robots_ ) {
        if ( r->radius() > robot_radius_max_ )
            robot_radius_max_ = r->radius();
    }
}

void Router_Node::graphCallback ( const tuw_multi_robot_msgs::Graph &msg ) {
    std::vector<Segment> graph;

    for ( const tuw_multi_robot_msgs::Vertex &segment : msg.vertices ) {
        std::vector<Eigen::Vector2d> points;

        for ( const geometry_msgs::Point &point : segment.path ) {
            points.emplace_back ( point.x, point.y );
        }

        std::vector<uint32_t> successors;

        for ( const auto &succ : segment.successors ) {
            successors.emplace_back ( succ );
        }

        std::vector<uint32_t> predecessors;

        for ( const auto &pred : segment.predecessors ) {
            predecessors.emplace_back ( pred );
        }

        if ( segment.valid ) {
            graph.emplace_back ( segment.id, points, successors, predecessors,  2 * robot_radius_max_ / mapResolution_ ); //segment.width);
        } else {
            graph.emplace_back ( segment.id, points, successors, predecessors, 0 );
        }
    }

    std::sort ( graph.begin(), graph.end(), sortSegments );

    size_t hash = getHash ( graph );

    if ( current_graph_hash_ != hash ) {
        current_graph_hash_ = hash;
        graph_ = graph;
        ROS_INFO ( "Multi Robot Router: Graph %lu", hash );
    }
    got_graph_ = true;
}

bool Router_Node::preparePlanning ( std::vector<float> &_radius, std::vector<Eigen::Vector3d> &_starts, std::vector<Eigen::Vector3d> &_goals, const tuw_multi_robot_msgs::RobotGoalsArray &goal_msg, std::vector<std::string> &robot_names) {
    bool retval = true;
    active_robots_.clear();
    _starts.clear();
    _goals.clear();
    _radius.clear();

    for ( int k = 0; k < goal_msg.robots.size(); k++ ) {
        const tuw_multi_robot_msgs::RobotGoals &route_request = goal_msg.robots[k];
        RobotInfoPtrIterator active_robot = RobotInfo::findObj ( subscribed_robots_, route_request.robot_name );
        if ( active_robot == subscribed_robots_.end() ) {
            ROS_INFO ( "No robot subsribed with the name: %s", route_request.robot_name.c_str() );
        } else {
            if ( route_request.destinations.empty() ) {
                ROS_INFO ( "No robot: %s has not goal defined", route_request.robot_name.c_str() );
                continue;
            } else {

                active_robots_.push_back ( *active_robot );

                _radius.push_back ( ( *active_robot )->radius() );
                if ( route_request.destinations.size() > 1 ) {
                    geometry_msgs::Pose p = route_request.destinations[0];
                    _starts.push_back(Eigen::Vector3d ( p.position.x, p.position.y, getYaw ( p.orientation ) ));
                } else {
                    _starts.push_back(( *active_robot )->getPose());
                }

                geometry_msgs::Pose p = route_request.destinations.back();
                _goals.push_back(Eigen::Vector3d ( p.position.x, p.position.y, getYaw ( p.orientation ) ));
            }

        }
    }
    robot_names.resize(active_robots_.size());
    for(size_t i=0; i < active_robots_.size(); i++){
        robot_names[i] = active_robots_[i]->robot_name;
    }
    return retval;
}

void Router_Node::goalsCallback ( const tuw_multi_robot_msgs::RobotGoalsArray &_goals ) {
    //Get robots
    std::vector<Eigen::Vector3d> starts;
    std::vector<Eigen::Vector3d> goals;
    std::vector<float> radius;
    std::vector<std::string> robot_names;

    
    
    bool preparationSuccessful = preparePlanning ( radius, starts, goals, _goals, robot_names );

    if ( preparationSuccessful && got_map_ && got_graph_ ) {
        auto t1 = std::chrono::high_resolution_clock::now();
        preparationSuccessful &= makePlan ( starts, goals, radius, distMap_, mapResolution_, mapOrigin_, graph_, robot_names );
        
        if ( preparationSuccessful ) {
            int nx = distMap_.cols;
            int ny = distMap_.rows;

            double res = mapResolution_;
            int cx = mapOrigin_[0];
            int cy = mapOrigin_[1];

            publish();
            ROS_INFO ( "Multi Robot Router: Publishing Plan" );
            freshPlan_ = false;
        } else {
            ROS_INFO ( "Multi Robot Router: No Plan found" );

            publishEmpty();
        }

        auto t2 = std::chrono::high_resolution_clock::now();
        int duration = std::chrono::duration_cast<std::chrono::milliseconds> ( t2 - t1 ).count();
        ROS_INFO ( "Multi Robot Router: OverallTime %i ms", duration );

        id_++;
    } else if ( !got_map_ || !got_graph_ ) {
        publishEmpty();
        ROS_INFO ( "Multi Robot Router: No Map or Graph received" );
    } else {
        publishEmpty();
    }
}

float Router_Node::getYaw ( const geometry_msgs::Quaternion &_rot ) {
    double roll, pitch, yaw;

    tf::Quaternion q ( _rot.x, _rot.y, _rot.z, _rot.w );
    tf::Matrix3x3 ( q ).getRPY ( roll, pitch, yaw );
    return yaw;
}

void Router_Node::publishEmpty() {
    nav_msgs::Path msg_path;
    tuw_multi_robot_msgs::Route msg_route;
    msg_path.header.seq = 0;
    msg_path.header.stamp = ros::Time::now();
    msg_path.header.frame_id = "map";
    msg_route.header = msg_path.header;
    
    for ( RobotInfoPtr &robot: subscribed_robots_) {
        robot->pubPaths_.publish(msg_path);
        robot->pubRoute_.publish(msg_route);
    }

    mrrp_status_.id = id_;
    mrrp_status_.success = 0;
    mrrp_status_.duration = getDuration_ms();
    pubPlannerStatus_.publish ( mrrp_status_ );
}

void Router_Node::publish() {
    nav_msgs::Path msg_path;
    tuw_multi_robot_msgs::Route msg_route;
    msg_path.header.seq = 0;
    msg_path.header.stamp = ros::Time::now();
    msg_path.header.frame_id = "map";
    msg_route.header = msg_path.header;
    
    for ( int i = 0; i < active_robots_.size(); i++ ) {
        RobotInfoPtr robot = active_robots_[i];
        const std::vector<Checkpoint> &route = getRoute ( i );
        msg_path.poses.clear();

        //Add first point
        geometry_msgs::PoseStamped pose_1;
        pose_1.header.seq = 0;
        pose_1.header.stamp = ros::Time::now();
        pose_1.header.frame_id = "map";

        Eigen::Vector2d pos ( route[0].start[0] * mapResolution_, route[0].start[1] * mapResolution_ );
        pose_1.pose.position.x = pos[0] + mapOrigin_[0];
        pose_1.pose.position.y = pos[1] + mapOrigin_[1];

        pose_1.pose.orientation.w = 1;
        msg_path.poses.push_back ( pose_1 );

        //Add other points
        for ( const Checkpoint &c : route ) {
            geometry_msgs::PoseStamped pose;
            pose.header.seq = 0;
            pose.header.stamp = ros::Time::now();
            pose.header.frame_id = "map";

            Eigen::Vector2d pos ( c.end[0] * mapResolution_, c.end[1] * mapResolution_ );
            pose.pose.position.x = pos[0] + mapOrigin_[0];
            pose.pose.position.y = pos[1] + mapOrigin_[1];

            tf::Quaternion q;
            q.setEuler ( 0, 0, c.end[2] );

            pose.pose.orientation.w = q.w();
            pose.pose.orientation.x = q.x();
            pose.pose.orientation.y = q.y();
            pose.pose.orientation.z = q.z();
            msg_path.poses.push_back ( pose );
        }
        
        robot->pubPaths_.publish ( msg_path );
        
        
        msg_route.segments.clear();
        
        for ( const Checkpoint &cp : route ) {
            tuw_multi_robot_msgs::RouteSegment seg;

            Eigen::Vector2d posStart ( cp.start[0] * mapResolution_, cp.start[1] * mapResolution_ );
            tf::Quaternion qStart;
            qStart.setEuler ( 0, 0, cp.start[2] );

            seg.start.position.x = posStart[0] + mapOrigin_[0];
            seg.start.position.y = posStart[1] + mapOrigin_[1];
            seg.start.orientation.w = qStart.w();
            seg.start.orientation.x = qStart.x();
            seg.start.orientation.y = qStart.y();
            seg.start.orientation.z = qStart.z();

            Eigen::Vector2d posEnd ( cp.end[0] * mapResolution_, cp.end[1] * mapResolution_ );
            tf::Quaternion qEnd;
            qEnd.setEuler ( 0, 0, cp.end[2] );

            seg.end.position.x = posEnd[0] + mapOrigin_[0];
            seg.end.position.y = posEnd[1] + mapOrigin_[1];
            seg.end.orientation.w = qEnd.w();
            seg.end.orientation.x = qEnd.x();
            seg.end.orientation.y = qEnd.y();
            seg.end.orientation.z = qEnd.z();

            seg.segment_id = cp.segId;
            seg.width = graph_[cp.segId].width() * mapResolution_;

            for ( int j = 0; j < cp.preconditions.size(); j++ ) {
                tuw_multi_robot_msgs::RoutePrecondition pc;
                pc.robot_id = active_robots_[cp.preconditions[j].robotId]->robot_name;
                pc.current_route_segment = cp.preconditions[j].stepCondition;
                seg.preconditions.push_back ( pc );
            }

            msg_route.segments.push_back ( seg );
        }

        robot->pubRoute_.publish ( msg_route );
    }

    tuw_multi_robot_msgs::RouterStatus ps;
    ps.id = id_;
    ps.success = 1;
    ps.overall_path_length = ( int32_t ) getOverallPathLength();
    ps.longest_path_length = ( int32_t ) getLongestPathLength();
    ps.priority_scheduling_attemps = ( int32_t ) getPriorityScheduleAttemps();
    ps.speed_scheduling_attemps = ( int32_t ) getSpeedScheduleAttemps();
    ps.duration = ( int32_t ) getDuration_ms();

    pubPlannerStatus_.publish ( ps );
}

size_t Router_Node::getHash ( const std::vector<signed char> &_map, const Eigen::Vector2d &_origin, const float &_resolution ) {
    std::size_t seed = 0;

    boost::hash_combine ( seed, _origin[0] );
    boost::hash_combine ( seed, _origin[1] );
    boost::hash_combine ( seed, _resolution );

    for ( const signed char &val : _map ) {
        boost::hash_combine ( seed, val );
    }

    return seed;
}

std::size_t Router_Node::getHash ( const std::vector<Segment> &_graph ) {
    std::size_t seed = 0;

    for ( const Segment &seg : _graph ) {
        boost::hash_combine ( seed, seg.width() );
        boost::hash_combine ( seed, seg.length() );
        boost::hash_combine ( seed, seg.getSegmentId() );

        for ( const int &p : seg.getPredecessors() ) {
            boost::hash_combine ( seed, p );
        }

        for ( const int &s : seg.getSuccessors() ) {
            boost::hash_combine ( seed, s );
        }

        for ( const Eigen::Vector2d &vec : seg.getPoints() ) {
            boost::hash_combine ( seed, vec[0] );
            boost::hash_combine ( seed, vec[1] );
        }
    }

    return seed;
}




Router_Node::TopicStatus::TopicStatus ( const status _status, const float _activeTime ) {
    setStatus ( _status, _activeTime );
}

Router_Node::TopicStatus::TopicStatus() : TopicStatus ( status::inactive ) {
}

Router_Node::TopicStatus::status Router_Node::TopicStatus::getStatus() const {
    return status_;
}

void Router_Node::TopicStatus::updateStatus ( const float _updateTime ) {
    if ( activeTime_ > 0 )
        activeTime_ -= _updateTime;

    if ( activeTime_ < 0 )
        activeTime_ = 0;

    if ( activeTime_ == 0 && status_ != status::fixed )
        status_ = status::inactive;
}

void Router_Node::TopicStatus::setStatus ( const status _status, const float _activeTime ) {
    if ( _status != status::active )
        activeTime_ = 0;
    else
        activeTime_ = _activeTime;

    status_ = _status;
}

} // namespace multi_robot_router
