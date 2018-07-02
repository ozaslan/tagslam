/* -*-c++-*--------------------------------------------------------------------
 * 2018 Bernd Pfrommer bernd.pfrommer@gmail.com
 */

#include "tagslam/tag_graph.h"
#include "tagslam/point_distance_factor.h"
#include "tagslam/cal3ds2u.h"
#include <boost/range/irange.hpp>
#include <gtsam/slam/expressions.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/slam/ProjectionFactor.h>
#include <gtsam/slam/ReferenceFrameFactor.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>

namespace tagslam {
  // you can probably bump MAX_TAG_ID without too much trouble,
  // but watch out for integer overflow on the 'w' symbol
  // when using a large number of frames.
  // TODO: safeguard against overflow!
  static const unsigned int MAX_TAG_ID = 255;
  static const unsigned int MAX_CAM_ID = 8;
  static const unsigned int MAX_BODY_ID = 'Z' - 'A' - 1;

  typedef gtsam::GenericProjectionFactor<gtsam::Pose3,
                                         gtsam::Point3,
                                         gtsam::Cal3DS2> ProjectionFactor;
  using boost::irange;

  // transform from object (tag) coordinate space to static object
  static const gtsam::Symbol sym_T_b_o(int tagId) {
    return (gtsam::Symbol('t', tagId));
  }
  // tag corners in world coordinates
  static const gtsam::Symbol sym_X_w_i(int tagId, int corner,
                                       unsigned int frame) {
    if (tagId > MAX_TAG_ID) {
      throw std::runtime_error("tag id exceeds MAX_TAG_ID: " + std::to_string(tagId));
    }
    return (gtsam::Symbol('w', frame * MAX_TAG_ID * 4 + tagId * 4 + corner));
  }
  // T_w_c(t) camera-to-world transform for given frame  
  static const gtsam::Symbol sym_T_c_t(int camId, unsigned int frame_num) {
    if (camId >= MAX_CAM_ID) {
      throw std::runtime_error("cam id exceeds MAX_CAM_ID: " + std::to_string(camId));
    }
    return (gtsam::Symbol('a' + camId, frame_num));
  }
  // T_w_b(t) dynamic_body-to-world transform for given frame  
  static const gtsam::Symbol sym_T_w_b(int bodyIdx, unsigned int frame_num) {
    if (bodyIdx >= MAX_BODY_ID) {
      throw std::runtime_error("body idx exceeds MAX_BODY_ID: " + std::to_string(bodyIdx));
    }
    return (gtsam::Symbol('A' + bodyIdx, frame_num));
  }

  static unsigned int X_w_i_sym_to_index(gtsam::Key k, int *tagId, int *corner) {
    gtsam::Symbol sym(k);
    int idx = sym.index();
    unsigned int frame_num = idx / (4 * MAX_TAG_ID);
    *tagId  = (idx - frame_num * 4 * MAX_TAG_ID) / 4;
    *corner = (idx - frame_num * 4 * MAX_TAG_ID) % 4;
    return (frame_num);
  }

  TagGraph::TagGraph() {
    pixelNoise_= gtsam::noiseModel::Isotropic::Sigma(2, 1.0);
  }

  void TagGraph::setPixelNoise(double numPix) {
    pixelNoise_ = gtsam::noiseModel::Isotropic::Sigma(2, numPix);
  }

  int TagGraph::getMaxNumBodies() const {
    return (MAX_BODY_ID);
  }

  void TagGraph::addTags(const RigidBodyPtr &rb, const TagVec &tags) {
    for (const auto &tag: tags) {
      //std::cout << "TagGraph: adding tag: " << tag->id << std::endl;
      gtsam::Pose3   tagPose  = tag->poseEstimate;
      PoseNoise tagNoise = tag->poseEstimate.getNoise();
      // ----- insert transform T_b_o and pin it down with prior factor if known
      gtsam::Symbol T_b_o_sym = sym_T_b_o(tag->id);
      if (values_.find(T_b_o_sym) != values_.end()) {
        std::cout << "TagGraph ERROR: duplicate tag id inserted: " << tag->id << std::endl;
        return;
      }
      values_.insert(T_b_o_sym, tagPose);
      if (tag->hasKnownPose) {
        graph_.push_back(gtsam::PriorFactor<gtsam::Pose3>(T_b_o_sym, tagPose, tagNoise));
      }
    }
  }

  double distance(const gtsam::Point3 &p1, const gtsam::Point3 &p2, gtsam::OptionalJacobian<1, 3> H1 = boost::none,
                  gtsam::OptionalJacobian<1, 3> H2 = boost::none) {
    const gtsam::Point3 d = p1-p2;
    double r = sqrt(d.x() * d.x() + d.y() * d.y() + d.z() * d.z());
    if (H1) *H1 << d.x() / r, d.y() / r, d.z() / r;
    if (H2) *H2 << -d.x() / r, -d.y() / r, -d.z() / r;
    return r;
  }

  bool
  TagGraph::addDistanceMeasurement(const RigidBodyPtr &rb1,
                                   const RigidBodyPtr &rb2,
                                   const TagConstPtr &tag1,
                                   const TagConstPtr &tag2,
                                   const DistanceMeasurement &dm) {
    if (!rb1->isStatic || !rb2->isStatic) {
      std::cout << "TagGraph ERROR: non-rigid body has tag measurement!" << std::endl;
      return (false);
    }
    const auto T_w_b1_sym = sym_T_w_b(rb1->index, 0);
    const auto T_w_b2_sym = sym_T_w_b(rb2->index, 0);
    const auto T_b1_o_sym = sym_T_b_o(tag1->id);
    const auto T_b2_o_sym = sym_T_b_o(tag2->id);

    if (!values_.exists(T_w_b1_sym) || !values_.exists(T_w_b2_sym) ||
        !values_.exists(T_b1_o_sym) || !values_.exists(T_b2_o_sym)) {
      //std::cout << "TagGraph: NOT adding rb measurement: " << dm.tag1 << " to " << dm.tag2 << std::endl;
      return (false);
    } else {
      std::cout << "TagGraph adding rb measurement: " << dm.tag1 << " to " << dm.tag2 << std::endl;
    }
    gtsam::Expression<gtsam::Pose3>  T_w_b_1(T_w_b1_sym);
    gtsam::Expression<gtsam::Pose3>  T_b_o_1(T_b1_o_sym);
    gtsam::Expression<gtsam::Point3> X_o_1(tag1->getObjectCorner(dm.corner1));
    gtsam::Expression<gtsam::Point3> X_w_1 = gtsam::transform_from(T_w_b_1, gtsam::transform_from(T_b_o_1, X_o_1));
    
    gtsam::Expression<gtsam::Pose3>  T_w_b_2(T_w_b2_sym);
    gtsam::Expression<gtsam::Pose3>  T_b_o_2(T_b2_o_sym);
    gtsam::Expression<gtsam::Point3> X_o_2(tag2->getObjectCorner(dm.corner2));
    gtsam::Expression<gtsam::Point3> X_w_2 = gtsam::transform_from(T_w_b_2, gtsam::transform_from(T_b_o_2, X_o_2));

    gtsam::Expression<double> dist = gtsam::Expression<double>(&distance, X_w_1, X_w_2);
    graph_.addExpressionFactor(dist, dm.distance, gtsam::noiseModel::Isotropic::Sigma(1, dm.noise));
    return (true);
  }

  double proj(const gtsam::Point3 &p, const gtsam::Point3 &n, gtsam::OptionalJacobian<1, 3> Hp = boost::none,
              gtsam::OptionalJacobian<1,3> Hn = boost::none) {
    double r = p.x() * n.x() + p.y() * n.y() + p.z() * n.z();
    if (Hp) *Hp << n.x(), n.y(), n.z();
    if (Hn) *Hn << p.x(), p.y(), p.z();
    return r;
  }

  std::pair<gtsam::Point3, bool>
  TagGraph::getDifference(const RigidBodyPtr &rb1, const RigidBodyPtr &rb2,
                          const TagConstPtr &tag1, int corner1,
                          const TagConstPtr &tag2, int corner2) const {
    const auto T_w_b1_sym = sym_T_w_b(rb1->index, 0);
    const auto T_w_b2_sym = sym_T_w_b(rb2->index, 0);
    const auto T_b1_o_sym = sym_T_b_o(tag1->id);
    const auto T_b2_o_sym = sym_T_b_o(tag2->id);

    if (!values_.exists(T_w_b1_sym) || !values_.exists(T_w_b2_sym) ||
        !values_.exists(T_b1_o_sym) || !values_.exists(T_b2_o_sym)) {
      return (std::pair<gtsam::Point3,bool>(gtsam::Point3(), false));
    }
    const gtsam::Point3 X_w_1 = values_.at<gtsam::Pose3>(T_w_b1_sym) *
      values_.at<gtsam::Pose3>(T_b1_o_sym) * tag1->getObjectCorner(corner1);
    const gtsam::Point3 X_w_2 = values_.at<gtsam::Pose3>(T_w_b2_sym) *
      values_.at<gtsam::Pose3>(T_b2_o_sym) * tag2->getObjectCorner(corner2);
    return (std::pair<gtsam::Point3,bool>(X_w_1 - X_w_2, true));
  }


  bool
  TagGraph::addPositionMeasurement(const RigidBodyPtr &rb,
                                   const TagConstPtr &tag,
                                   const PositionMeasurement &m) {
    if (!rb->isStatic) {
      std::cout << "TagGraph ERROR: non-rigid body has position measurement!" << std::endl;
      return (false);
    }
    const auto T_w_b_sym = sym_T_w_b(rb->index, 0);
    const auto T_b_o_sym = sym_T_b_o(tag->id);
    if (!values_.exists(T_w_b_sym) || !values_.exists(T_b_o_sym)) {
      return (false);
    }
    std::cout << "TagGraph adding position measurement: " << m.tag << std::endl;
    gtsam::Expression<gtsam::Pose3>  T_w_b(T_w_b_sym);
    gtsam::Expression<gtsam::Pose3>  T_b_o(T_b_o_sym);
    gtsam::Expression<gtsam::Point3> X_o(tag->getObjectCorner(m.corner));
    gtsam::Expression<gtsam::Point3> X_w = gtsam::transform_from(T_w_b, gtsam::transform_from(T_b_o, X_o));
    gtsam::Expression<gtsam::Point3> n(m.dir);
    gtsam::Expression<double> len = gtsam::Expression<double>(&proj, X_w, n);
    graph_.addExpressionFactor(len, m.length, gtsam::noiseModel::Isotropic::Sigma(1, m.noise));
    return (true);
  }

  std::pair<gtsam::Point3, bool>
  TagGraph::getPosition(const RigidBodyPtr &rb, const TagConstPtr &tag,
                        int corner) const {
    const auto T_w_b_sym = sym_T_w_b(rb->index, 0);
    const auto T_b_o_sym = sym_T_b_o(tag->id);

    if (!values_.exists(T_w_b_sym) || !values_.exists(T_b_o_sym)) {
      return (std::pair<gtsam::Point3,bool>(gtsam::Point3(), false));
    }
    const gtsam::Point3 X_w = values_.at<gtsam::Pose3>(T_w_b_sym) *
      values_.at<gtsam::Pose3>(T_b_o_sym) * tag->getObjectCorner(corner);
    return (std::pair<gtsam::Point3,bool>(X_w, true));
  }

  PoseEstimate
  TagGraph::getCameraPose(const CameraPtr &cam,
                          unsigned int frame_num) const {
    PoseEstimate pe;
    gtsam::Symbol T_w_c_sym = sym_T_c_t(cam->index, cam->isStatic ? 0 : frame_num);
    if (values_.find(T_w_c_sym) != values_.end()) {
      pe = PoseEstimate(values_.at<gtsam::Pose3>(T_w_c_sym), 0.0, 0);
    }
    return (pe);
  }

  void TagGraph::observedTags(const CameraPtr &cam, const RigidBodyPtr &rb,
                              const TagVec &tags,
                              unsigned int frame_num) {
    //std::cout << "---------- points for cam " << cam->name << " body: " << rb->name << std::endl;
    if (tags.empty()) {
      std::cout << "TagGraph WARN: no tags for " << cam->name << " in frame "
                << frame_num << std::endl;
      return;
    }
    if (!cam->poseEstimate.isValid()) {
      std::cout << "TagGraph WARN: no pose estimate for cam " << cam->name
                <<  " in frame " << frame_num << std::endl;
      return;
    }
    if (!rb->poseEstimate.isValid()) {
      return;
    }
    // new camera location
    gtsam::Symbol T_w_c_sym = sym_T_c_t(cam->index, cam->isStatic ? 0 : frame_num);
    if (!values_.exists(T_w_c_sym)) {
      values_.insert(T_w_c_sym, cam->poseEstimate.getPose());
    }

    gtsam::Symbol T_w_b_sym = sym_T_w_b(rb->index, rb->isStatic ? 0 : frame_num);
    if (!values_.exists(T_w_b_sym)) {
      const auto &pe = rb->poseEstimate;
      values_.insert(T_w_b_sym, pe.getPose());
      if (rb->isStatic && rb->hasPosePrior) {
        std::cout << "TagGraph: adding prior for body: " << rb->name << std::endl;
        graph_.push_back(gtsam::PriorFactor<gtsam::Pose3>(T_w_b_sym,
                                                          pe.getPose(), pe.getNoise()));
      }
    }
    for (const auto &tag: tags) {
      if (!tag->poseEstimate.isValid()) {
        std::cout << "TagGraph WARN: tag " << tag->id << " has invalid pose!" << std::endl;
        continue;
      }
      const auto &measured = tag->getImageCorners();
      gtsam::Expression<gtsam::Pose3>  T_b_o(sym_T_b_o(tag->id));
      gtsam::Expression<gtsam::Pose3>  T_w_b(T_w_b_sym);
      gtsam::Expression<gtsam::Pose3>  T_w_c(T_w_c_sym);
      for (const auto i: irange(0, 4)) {
        gtsam::Expression<gtsam::Point3> X_o(tag->getObjectCorner(i));
        // transform_from does X_A = T_AB * X_B
        gtsam::Expression<gtsam::Point3> X_w = gtsam::transform_from(T_w_b, gtsam::transform_from(T_b_o, X_o));
        gtsam::Expression<gtsam::Point2> xp  = gtsam::project(gtsam::transform_to(T_w_c, X_w));
        if (cam->radtanModel) {
          gtsam::Expression<Cal3DS2U> cK(*cam->radtanModel);
          gtsam::Expression<gtsam::Point2> predict(cK, &Cal3DS2U::uncalibrate, xp);
          graph_.addExpressionFactor(predict, measured[i], pixelNoise_);
        } else if (cam->equidistantModel) {
          gtsam::Expression<Cal3FS2> cK(*cam->equidistantModel);
          gtsam::Expression<gtsam::Point2> predict(cK, &Cal3FS2::uncalibrate, xp);
          graph_.addExpressionFactor(predict, measured[i], pixelNoise_);
        }
      }
    }
  }

  bool TagGraph::getBodyPose(const RigidBodyConstPtr &rb, PoseEstimate *pe,
                             unsigned int frame) const {
    const auto T_w_b_sym = sym_T_w_b(rb->index, rb->isStatic? 0 : frame);
    if (values_.find(T_w_b_sym) != values_.end()) {
      gtsam::Pose3 pose = values_.at<gtsam::Pose3>(T_w_b_sym);
      if (marginals_) {
        const auto &noiseMat = marginals_->marginalCovariance(T_w_b_sym);
        *pe = PoseEstimate(pose, 0.0, 0, gtsam::noiseModel::Gaussian::Covariance(noiseMat));
      } else {
        *pe = PoseEstimate(pose, 0.0, 0);
      }
      return (true);
    }
    *pe = PoseEstimate();
    return (false);;
  }

  void TagGraph::computeMarginals() {
    marginals_.reset(new gtsam::Marginals(graph_, optimizedValues_));
  }

  double TagGraph::tryOptimization(gtsam::Values *result,
                                   const gtsam::NonlinearFactorGraph &graph,
                                   const gtsam::Values &values,
                                   const std::string &verbosity, int maxIter) {
      gtsam::LevenbergMarquardtParams lmp;
      //lmp.setVerbosity(verbosity);
      lmp.setVerbosity("TERMINATION");
      lmp.setMaxIterations(maxIter);
      lmp.setAbsoluteErrorTol(1e-10);
      lmp.setRelativeErrorTol(0);
      //lmp.setlambdaUpperBound(1e20);
      gtsam::LevenbergMarquardtOptimizer lmo(graph, values, lmp);
      *result = lmo.optimize();
      double ni = numProjectionFactors_ > 0 ? 1.0/numProjectionFactors_ : 1.0;
      optimizerError_ = lmo.error() * ni;
      optimizerIterations_ = lmo.iterations();
      return (optimizerError_);
  }


  PoseEstimate TagGraph::getTagWorldPose(const RigidBodyConstPtr &rb,
                                         int tagId, unsigned int frame_num) const {
    PoseEstimate pe;   // defaults to invalid
    const auto T_b_o_sym = sym_T_b_o(tagId);
    if (values_.find(T_b_o_sym) != values_.end()) {
      const auto T_w_b_sym = sym_T_w_b(rb->index, rb->isStatic ? 0:frame_num);
      if (values_.find(T_w_b_sym) != values_.end()) {
        // T_w_o = T_w_b * T_b_o
        // cov(T_w_o, T_w_o) = sum (R_w_b*x) (R_w_b*x)T
        // = R_w_b * cov(T_b_o) * R_w_b^T
        gtsam::Pose3 T_w_o = values_.at<gtsam::Pose3>(T_w_b_sym) *
          values_.at<gtsam::Pose3>(T_b_o_sym);
        pe = PoseEstimate(T_w_o, 0.0, 0);
      }
    }
    return (pe);
  }
  
  bool
  TagGraph::getTagRelPose(const RigidBodyPtr &rb, int tagId,
                          gtsam::Pose3 *pose) const {
    const auto T_b_o_sym = sym_T_b_o(tagId);
    if (values_.find(T_b_o_sym) != values_.end()) {
      *pose = values_.at<gtsam::Pose3>(T_b_o_sym);
      return (true);
    }
    return (false);
  }

  void
  TagGraph::printDistances() const {
    const auto symMin = sym_X_w_i(0, 0, 0);
    const auto symMax = sym_X_w_i(MAX_TAG_ID, 3, 0);
    gtsam::Values::const_iterator it_start = values_.lower_bound(symMin);
    gtsam::Values::const_iterator it_stop  = values_.upper_bound(symMax);
    for (gtsam::Values::const_iterator it1 = it_start; it1 != it_stop; ++it1) {
      gtsam::Point3 p1 = values_.at<gtsam::Point3>(it1->key);
      int tagid, corn;
      X_w_i_sym_to_index(it1->key, &tagid, &corn);
      printf("tag %3d corner %d:", tagid, corn);
      for (gtsam::Values::const_iterator it2 = it_start; it2 != it_stop; ++it2) {
        gtsam::Point3 p2 = values_.at<gtsam::Point3>(it2->key);
        printf(" %7.4f", p1.distance(p2));
      }
      printf("\n");
    }
  }

/*  
  void
  TagGraph::getTagWorldPoses(std::vector<std::pair<int, gtsam::Pose3>> *poses) const {
    gtsam::Values::const_iterator it_start = values_.lower_bound(gtsam::Symbol('s', 0));
    gtsam::Values::const_iterator it_stop  = values_.upper_bound(gtsam::Symbol('s', 1024*1024));
    for (gtsam::Values::const_iterator it = it_start; it != it_stop; ++it) {
      gtsam::Symbol sym(it->key);
      poses->push_back(std::pair<int, gtsam::Pose3>(sym.index(),
                                                    values_.at<gtsam::Pose3>(it->key)));
    }
  }
*/

  void TagGraph::optimize() {
    //graph_.print();
    //values_.print();
    //double err = tryOptimization(&optimizedValues_, graph_, values_, "TERMINATION", 100);
    double err = tryOptimization(&optimizedValues_, graph_, values_, "TERMINATION", 100);
    values_ = optimizedValues_;
    //optimizedValues_.print();
  }

}  // namespace
