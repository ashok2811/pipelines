#pragma once

#include "contour_tree.h"
#include "fps_counter.h"
#include "node.h"
#include "types.h"
#include <chrono>
#include <eigen3/Eigen/Dense>
#include <opencv2/calib3d.hpp>
#include <opencv2/core/persistence.hpp>
#include <thread>
namespace frame_syncer {
using input_type = type_list_t<TimedMatWithCTree, TimedMatWithCTree>;
using output_type = type_list_t<CameraPairData>;
}; // namespace frame_syncer

using namespace std;

class FrameSyncer
    : public Node<frame_syncer::input_type, frame_syncer::output_type> {
public:
  FrameSyncer() {
    cv::FileStorage fs("../scripts/stereoParams.xml", cv::FileStorage::READ);
    fs["K1"] >> K1;
    fs["K2"] >> K2;
    fs["D1"] >> D1;
    fs["D2"] >> D2;
    fs["R1"] >> R1;
    fs["R2"] >> R2;
    fs["P1"] >> P1;
    fs["P2"] >> P2;
  }
  void process() {
    FpsCounter fc(240, "FS");
    // std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    while (*running) {
      auto dt1 = readData<0, TimedMatWithCTree>();
      auto dt2 = readData<1, TimedMatWithCTree>();

      // std::this_thread::sleep_for(std::chrono::microseconds(16500));
      auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
          dt1.timestamp - dt2.timestamp);

      int iters = (abs(delay.count()) / 10) - 1;
      if (abs(delay.count()) > 4) {
        std::cout << "delay happen, what do: " << delay.count() << std::endl;
      }

      vector<CombinedTrajectory> alignedTrajectories;
      alignGroups(dt1.contourGroupList, dt2.contourGroupList,
                  alignedTrajectories);

      /*for (auto &traj : alignedTrajectories) {
        auto obvSize = traj.atc.size();
        Eigen::MatrixXf A(obvSize, 3);
        Eigen::MatrixXf b(obvSize, 1);
        auto t0 = traj.atc[0].t_avg;
        for (int i = 0; i < traj.atc.size(); i++) {
          auto &cm = traj.atc[i];
          auto time_elapsed =
              std::chrono::duration_cast<std::chrono::microseconds>(cm.t_avg -
                                                                    t0)
                  .count() /
              (1000.0 * 1000.0);
          A(i, 0) = 1;
          A(i, 1) = time_elapsed;
          A(i, 2) = (time_elapsed * time_elapsed) / 2;
          b(i) = cm.y;
          cout << "(" << abs(cm.leftCenter.x - cm.rightCenter.x) << " , "
               << abs(cm.leftCenter.y - cm.rightCenter.y) << " , "
               << time_elapsed << ") , ";
        }
        Eigen::MatrixXf soln =
            (A.transpose() * A).ldlt().solve(A.transpose() * b);
        cout << "G: " << (A.transpose() * A).ldlt().solve(A.transpose() * b)
             << endl;
        cout << "left: " << traj.lt.id << " right: " << traj.rt.id
             << " L: " << traj.atc.size() << " G: " << soln(2) << endl;
      }*/

      auto vecPtr = new vector(alignedTrajectories);

      CameraPairData dt = {
          .leftTMCT = dt1, .rightTMCT = dt2, .trajectories = vecPtr};
      writeData<0>(dt);
    }
    std::cout << "Closing heavy frame syncer" << std::endl;
  }

  void solveAssumingGravity(vector<AlignedTimedContour> &atc, double &x0,
                            double &y0, double &z0, double &vx, double &vy,
                            double &vz) {
    auto obvSize = atc.size();
    Eigen::MatrixXf A(obvSize, 2);
    Eigen::MatrixXf b(obvSize, 1);
    auto t0 = atc[0].t_avg;
    for (int i = 0; i < atc.size(); i++) {
      auto &cm = atc[i];
      auto time_elapsed =
          std::chrono::duration_cast<std::chrono::microseconds>(cm.t_avg - t0)
              .count() /
          (1000.0 * 1000.0);
      A(i, 0) = 1;
      A(i, 1) = time_elapsed;
    }
    // Solve for X params
    for (int i = 0; i < atc.size(); i++) {
      auto &cm = atc[i];
      b(i) = cm.x;
    }
    Eigen::MatrixXf solnX = (A.transpose() * A).ldlt().solve(A.transpose() * b);
    x0 = solnX(0);
    vx = solnX(1);

    // Solve for X params
    for (int i = 0; i < atc.size(); i++) {
      auto &cm = atc[i];
      b(i) = cm.y - 9.8 * A(i, 1) * A(i, 1) / 2.0;
    }
    Eigen::MatrixXf solnY = (A.transpose() * A).ldlt().solve(A.transpose() * b);
    y0 = solnY(0);
    vy = solnY(1);

    // Solve for Z params
    for (int i = 0; i < atc.size(); i++) {
      auto &cm = atc[i];
      b(i) = cm.z;
    }
    Eigen::MatrixXf solnZ = (A.transpose() * A).ldlt().solve(A.transpose() * b);
    z0 = solnZ(0);
    vz = solnZ(1);
  }

  void alignGroups(vector<SingleTrajectory> *leftGroups,
                   vector<SingleTrajectory> *rightGroups,
                   vector<CombinedTrajectory> &alignedTrajectories) {
    auto wLeftGroups = *leftGroups;
    auto wRightGroups = *rightGroups;
    for (int i = 0; i < wLeftGroups.size(); i++) {
      if (wLeftGroups[i].tc.size() < 4) {
        continue;
      }
      for (int j = 0; j < wRightGroups.size(); j++) {
        if (wRightGroups[j].tc.size() < 4) {
          continue;
        }
        SingleTrajectory &leftGroup = wLeftGroups[i];
        SingleTrajectory &rightGroup = wRightGroups[j];

        // Align a pair of trajectories.
        alignTrajectory(leftGroup, rightGroup, alignedTrajectories);
      }
    }
  }

  void alignTrajectory(SingleTrajectory &leftTrajectory,
                       SingleTrajectory &rightTrajectory,
                       vector<CombinedTrajectory> &alignedTrajectories) {
    int leftIdx = leftTrajectory.tc.size() - 1;
    int rightIdx = rightTrajectory.tc.size() - 1;
    vector<pair<int, int>> alignedIndices;
    while (leftIdx >= 0 && rightIdx >= 0) {
      TimedContour &left = leftTrajectory.tc[leftIdx];
      TimedContour &right = rightTrajectory.tc[rightIdx];
      bool alignedInTime = areAlignedInTime(left, right);
      // bool alignedInTime = false;
      bool alignedInY = areAlignedInY(left, right);
      auto offset_in_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              left.timestamp - right.timestamp)
                              .count();

      if (!alignedInTime || !alignedInY) {
        // The latest contours are not aligned, we will not consider this
        // trajectory further;

        if (alignedInTime) {
          // They belong to the same frame but not together. Sad! Ignore both
          leftIdx--;
          rightIdx--;
        } else {
          auto offset_in_ms =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  left.timestamp - right.timestamp)
                  .count();
          // offset more than zero means that left is on a newer node than
          // right. Make left go back because we can't make right go forward.
          // Because reverse iteration and vice versa.
          if (offset_in_ms > 0) {
            leftIdx--;
          } else {
            rightIdx--;
          }
        }
      } else if (alignedInTime && alignedInY) {
        // They are aligned in both time and space. True love
        alignedIndices.push_back({leftIdx, rightIdx});
        leftIdx--;
        rightIdx--;
      }
    }

    if (alignedIndices.size() >= 4) {
      vector<AlignedTimedContour> alignedTrajectory;
      for (int i = alignedIndices.size() - 1; i >= 0; i--) {
        int li = alignedIndices[i].first;
        int ri = alignedIndices[i].second;

        auto &ltc = leftTrajectory.tc[li];
        auto &rtc = rightTrajectory.tc[ri];
        ImVec2 leftctr, rightctr;
        alignedCenter(ltc, rtc, leftctr, rightctr);
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
            ltc.timestamp - rtc.timestamp);
        auto diff_2 = diff / 2;
        auto t_avg = rtc.timestamp + diff_2;

        auto disparity = (rightctr.x - leftctr.x + 0.001);
        auto depth = (1084.5238 * 0.823378) / disparity;
        auto height = ((leftctr.y - 364.5288) * depth) / 1093.4296;
        auto x =
            (((leftctr.x + rightctr.x - 661.7751 - 621.3420) / 2.0) * depth) /
            1084.5238;
        AlignedTimedContour alignedTimedContour = {
            .lt = ltc,
            .rt = rtc,
            .leftCenter = leftctr,
            .rightCenter = rightctr,
            .x = x,
            .y = height,
            .z = depth,
            .t_avg = ltc.timestamp,
        };
        alignedTrajectory.push_back(alignedTimedContour);
        // cout << x << " " << height << " " << depth << endl;
      }
      double x0, y0, z0, vx, vy, vz;
      solveAssumingGravity(alignedTrajectory, x0, y0, z0, vx, vy, vz);
      alignedTrajectories.push_back({
          .lt = leftTrajectory,
          .rt = rightTrajectory,
          .atc = alignedTrajectory,
          .x0 = x0,
          .y0 = y0,
          .z0 = z0,
          .vx = vx,
          .vy = vy,
          .vz = vz,
      });
    }
  }

  bool areAlignedInTime(TimedContour &left, TimedContour &right) {
    auto offset_in_ms =
        abs(std::chrono::duration_cast<std::chrono::milliseconds>(
                left.timestamp - right.timestamp)
                .count());
    return offset_in_ms <= ConfigStore::maxFrameOffset;
  }

  bool areAlignedInY(TimedContour &left, TimedContour &right) {
    ImVec2 leftctr, rightctr;
    alignedCenter(left, right, leftctr, rightctr);

    return abs(leftctr.y - rightctr.y) < 20;
  }

  void alignedCenter(TimedContour &left, TimedContour &right, ImVec2 &leftctr,
                     ImVec2 &rightctr) {
    auto leftCenter = contourCenterPoint(left.contour);
    auto rightCenter = contourCenterPoint(right.contour);
    vector<cv::Point2f> lefts = {leftCenter};
    vector<cv::Point2f> unLC;
    vector<cv::Point2f> rights = {rightCenter};
    vector<cv::Point2f> unRC;
    cv::undistortPoints(lefts, unLC, K1, D1, R1, P1);
    cv::undistortPoints(rights, unRC, K2, D2, R2, P2);
    leftctr.x = unLC[0].x;
    leftctr.y = unLC[0].y;
    rightctr.x = unRC[0].x;
    rightctr.y = unRC[0].y;
  }

  cv::Point contourCenterPoint(std::vector<cv::Point> &contour) {

    cv::Moments M = cv::moments(contour);
    int X = M.m10 / M.m00;
    int Y = M.m01 / M.m00;
    return cv::Point(X, Y);
  }

  ImVec2 contourCenterVec2(vector<cv::Point> &contour) {
    cv::Moments M = cv::moments(contour);
    int X = M.m10 / M.m00;
    int Y = M.m01 / M.m00;
    return ImVec2(X, Y);
  }

  cv::Mat K1, K2, D1, D2, R1, R2, P1, P2;
};
