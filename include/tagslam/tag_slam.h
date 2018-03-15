/* -*-c++-*--------------------------------------------------------------------
 * 2018 Bernd Pfrommer bernd.pfrommer@gmail.com
 */

#ifndef TAGSLAM_TAGSLAM_H
#define TAGSLAM_TAGSLAM_H

#include "tagslam/camera.h"
#include "tagslam/tag_graph.h"
#include <ros/ros.h>
#include <tf/transform_broadcaster.h>
#include <apriltag_msgs/ApriltagArrayStamped.h>
#include <vector>
#include <message_filters/time_synchronizer.h>
#include <message_filters/subscriber.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <memory>
#include <map>
#include <string>
#include <unordered_map>

namespace tagslam {
  using TagArray = apriltag_msgs::ApriltagArrayStamped;
  using TagArrayConstPtr = TagArray::ConstPtr;
  typedef message_filters::sync_policies::ApproximateTime<TagArray, TagArray> SyncPolicy2;
  typedef message_filters::sync_policies::ApproximateTime<TagArray, TagArray, TagArray> SyncPolicy3;
  typedef message_filters::Synchronizer<SyncPolicy2> TimeSync2;
  typedef message_filters::Synchronizer<SyncPolicy3> TimeSync3;
  class TagSlam {
  public:
    TagSlam(const ros::NodeHandle &pnh);
    ~TagSlam();

    TagSlam(const TagSlam&) = delete;
    TagSlam& operator=(const TagSlam&) = delete;

    bool initialize();

    void callback1(TagArrayConstPtr const &tag0);
    void callback2(TagArrayConstPtr const &tag0, TagArrayConstPtr const &tag1);
    void callback3(TagArrayConstPtr const &tag0, TagArrayConstPtr const &tag1, TagArrayConstPtr const &tag2);
  private:
    struct PoseInfo {
      PoseInfo(const gtsam::Pose3 &p = gtsam::Pose3(), const ros::Time &t = ros::Time(0),
               const std::string &frid = "") : pose(p), time(t), frame_id(frid) {}
      gtsam::Pose3 pose;
      ros::Time    time;
      std::string  frame_id;
    };
    struct StaticObject {
      StaticObject(const std::string &n  = std::string(""),
                   const gtsam::Pose3 &p = gtsam::Pose3(),
                   const Tag::PoseNoise &pn = Tag::PoseNoise()) :
        name(n), pose(p), noise(pn) {};
      std::string    name;
      gtsam::Pose3   pose;
      Tag::PoseNoise noise;
    };
    void parseStaticObject(const std::string &name,
                           XmlRpc::XmlRpcValue &staticObject);
    void process(const std::vector<TagArrayConstPtr> &msgvec);
    bool subscribe();
    void broadcastTransforms(const std::vector<PoseInfo> &poses);
    void broadcastCameraPoses(const ros::Time &t);
    void broadcastTagPoses(const ros::Time &t);
    bool estimateInitialTagPose(int cam_idx, const gtsam::Pose3 &T_w_c,
                                const gtsam::Point2 *corners,
                                gtsam::Pose3 *pose) const;

    bool estimateInitialTagPose(int cam_idx, const Tag &tag, gtsam::Pose3 *pose) const;
    bool estimateCameraPose(int cam_idx, const std::vector<Tag> &tags,
                            gtsam::Pose3 *pose, double *err);
    void updateTagPosesFromGraph(const std::vector<Tag> &tags);
    Tag  makeTag(int id, double size, const gtsam::Pose3 &pose,
                 const Tag::PoseNoise &noise,
                 const geometry_msgs::Point *corners,
                 int parentIdx);
    int  findTagType(double size);
    void findKnownTags(const TagArrayConstPtr &observedTags,
                       std::vector<Tag> *knownTags,
                       std::vector<Tag> *unknownTags);
    void findTagInitialPoses(std::vector<Tag> *tagsWithPoses,
                             const std::vector<Tag> &newTags, int cam_idx,
                             const gtsam::Pose3 &T_w_c);


    // ----------------------------------------------------------
    typedef message_filters::Subscriber<TagArray> TagSubscriber;
    typedef std::unordered_map<int, Tag>          IdToTagMap;
    ros::Subscriber                               singleCamSub_;
    std::vector<std::shared_ptr<TagSubscriber>>   sub_;
    std::unique_ptr<TimeSync2>                    approxSync2_;
    std::unique_ptr<TimeSync3>                    approxSync3_;
    ros::NodeHandle                               nh_;
    CameraVec                                     cameras_;
    TagGraph                                      tagGraph_;
    std::map<double, int>                         tagTypeMap_;
    IdToTagMap                                    idToTag_;
    std::vector<StaticObject>                     staticObjects_;
    double                                        defaultTagSize_{0.5};
    unsigned int                                  frameNum_{0};
    tf::TransformBroadcaster                      tfBroadcaster_;
  };

}

#endif