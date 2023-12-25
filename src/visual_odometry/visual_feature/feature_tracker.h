/*
    @brief 视觉里程计特征提取类 和 利用激光雷达测距值的深度值注册类
*/
#pragma once

#include <cstdio>
#include <iostream>
#include <queue>
#include <execinfo.h>
#include <csignal>

#include <opencv2/opencv.hpp>
#include <eigen3/Eigen/Dense>

#include "camera_models/CameraFactory.h"
#include "camera_models/CataCamera.h"
#include "camera_models/PinholeCamera.h"

#include "parameters.h"
#include "tic_toc.h"

using namespace std;
using namespace camodocal;
using namespace Eigen;

bool inBorder(const cv::Point2f &pt);

void reduceVector(vector<cv::Point2f> &v, vector<uchar> status);
void reduceVector(vector<int> &v, vector<uchar> status);

class FeatureTracker
{
  public:
    FeatureTracker();
    //图片输入函数：图片；对应时间戳
    void readImage(const cv::Mat &_img,double _cur_time);
    //
    void setMask();

    void addPoints();

    bool updateID(unsigned int i);

    void readIntrinsicParameter(const string &calib_file);

    void showUndistortion(const string &name);

    void rejectWithF();

    void undistortedPoints();

    cv::Mat mask;
    cv::Mat fisheye_mask;
    cv::Mat prev_img, cur_img, forw_img; //上上帧, 上一帧, 当前帧 (对上一帧的特征点进行光流跟踪)
    vector<cv::Point2f> n_pts; // 从当前帧中新提取出来的特征点 (待发布的frame至少需要150个特征点)
    vector<cv::Point2f> prev_pts, cur_pts, forw_pts;
    vector<cv::Point2f> prev_un_pts, cur_un_pts; // 去畸变后的归一化坐标
    vector<cv::Point2f> pts_velocity;
    vector<int> ids; // forw_img特征点的index
    vector<int> track_cnt; // 特征点被成功跟踪的次数，(新提取的特征点，跟踪次数为1)
    map<int, cv::Point2f> cur_un_pts_map;// 去畸变后的归一化坐标<index,坐标>
    map<int, cv::Point2f> prev_un_pts_map;
    camodocal::CameraPtr m_camera;
    double cur_time;
    double prev_time;

    static int n_id;
};

// 深度值注册类
class DepthRegister
{
public:

    ros::NodeHandle n;
    // publisher for visualization
    ros::Publisher pub_depth_feature;
    ros::Publisher pub_depth_image;
    ros::Publisher pub_depth_cloud;

    tf::TransformListener listener;
    tf::StampedTransform transform;

    const int num_bins = 360;
    vector<vector<PointType>> pointsArray;

    //设置默认构造函数
    DepthRegister(ros::NodeHandle n_in):
    n(n_in)
    {
        // messages for RVIZ visualization
        pub_depth_feature = n.advertise<sensor_msgs::PointCloud2>(PROJECT_NAME + "/vins/depth/depth_feature", 5);
        pub_depth_image =   n.advertise<sensor_msgs::Image>      (PROJECT_NAME + "/vins/depth/depth_image",   5);
        pub_depth_cloud =   n.advertise<sensor_msgs::PointCloud2>(PROJECT_NAME + "/vins/depth/depth_cloud",   5);
        // std::string name_ = PROJECT_NAME;
        // std::cout << name_ << std::endl;
        //设置pointsArray容量为360x360
        pointsArray.resize(num_bins);
        for (int i = 0; i < num_bins; ++i)
            pointsArray[i].resize(num_bins);
    }
    /*
        sensor_msgs::ChannelFloat32消息类包含{string name;float32[] values}
        @brief 获取深度值函数
        @input 当前时间戳，当前图片，当前点云，相机内参模型，2D特征点数组
        @output 输出带有正确深度值的特征点(vins_body_ros系下)
    */
    sensor_msgs::ChannelFloat32 get_depth(const ros::Time& stamp_cur, const cv::Mat& imageCur, 
                                          const pcl::PointCloud<PointType>::Ptr& depthCloud,
                                          const camodocal::CameraPtr& camera_model ,
                                          const vector<geometry_msgs::Point32>& features_2d)//去畸变后视觉特征点的归一化坐标
    {
        // 0.1 initialize depth for return
        sensor_msgs::ChannelFloat32 depth_of_point;
        depth_of_point.name = "depth";
        depth_of_point.values.resize(features_2d.size(), -1);

        // 0.2  check if depthCloud available
        if (depthCloud->size() == 0)
            return depth_of_point;

        // 0.3 look up transform at current image time
        try{
            listener.waitForTransform("vins_world", "vins_body_ros", stamp_cur, ros::Duration(0.01));
            listener.lookupTransform("vins_world", "vins_body_ros", stamp_cur, transform);// 找到当前vins_body_ros to vins_world 的tf
        } 
        catch (tf::TransformException ex){
            // ROS_ERROR("image no tf");
            return depth_of_point;
        }
        //提取位置坐标和姿态角
        double xCur, yCur, zCur, rollCur, pitchCur, yawCur;
        xCur = transform.getOrigin().x();
        yCur = transform.getOrigin().y();
        zCur = transform.getOrigin().z();
        tf::Matrix3x3 m(transform.getRotation());
        m.getRPY(rollCur, pitchCur, yawCur);
        // std::cout << "[" << xCur << "," << yCur << "," << zCur << "," << rollCur << "," << pitchCur << "," << yawCur << "]\n"; 
        Eigen::Affine3f transNow = pcl::getTransformation(xCur, yCur, zCur, rollCur, pitchCur, yawCur);

        // 0.4 transform cloud from global frame to camera frame 将点云转换到相机系
        pcl::PointCloud<PointType>::Ptr depth_cloud_local(new pcl::PointCloud<PointType>());
        pcl::transformPointCloud(*depthCloud, *depth_cloud_local, transNow.inverse());

        // std::cout << "[" << features_2d[0].x << "," << features_2d[0].y << "," << features_2d[0].z << "]\n"; 
        // 0.5 project undistorted normalized (z) 2d features onto a unit sphere 将视觉特征点的归一化坐标投影到单位球坐标系下
        pcl::PointCloud<PointType>::Ptr features_3d_sphere(new pcl::PointCloud<PointType>());
        for (int i = 0; i < (int)features_2d.size(); ++i)
        {
            // normalize 2d feature to a unit sphere
            Eigen::Vector3f feature_cur(features_2d[i].x, features_2d[i].y, features_2d[i].z); // z always equal to 1
            feature_cur.normalize(); 
            // convert to ROS standard
            PointType p;
            p.x =  feature_cur(2);
            p.y = -feature_cur(0);
            p.z = -feature_cur(1);
            p.intensity = -1; // intensity will be used to save depth 先默认设置为-1，深度关联后 用lidar测距值进行赋值
            features_3d_sphere->push_back(p);
        }

        // 3. project depth cloud on a range image, filter points satcked in the same region
        float bin_res = 180.0 / (float)num_bins; // currently only cover the space in front of lidar (-90 ~ 90)
        cv::Mat rangeImage = cv::Mat(num_bins, num_bins, CV_32F, cv::Scalar::all(FLT_MAX));//构建激光点云的深度直方图，进行采样

        for (int i = 0; i < (int)depth_cloud_local->size(); ++i)
        {
            PointType p = depth_cloud_local->points[i];
            // filter points not in camera view 剔除不在相机视角范围的点
            if (p.x < 0 || abs(p.y / p.x) > 10 || abs(p.z / p.x) > 10)
                continue;
            // find row id in range image 查询当前点的行号，
            float row_angle = atan2(p.z, sqrt(p.x * p.x + p.y * p.y)) * 180.0 / M_PI + 90.0; // degrees, bottom -> up, 0 -> 360
            int row_id = round(row_angle / bin_res);
            // find column id in range image 查询当前点的列号
            float col_angle = atan2(p.x, p.y) * 180.0 / M_PI; // degrees, left -> right, 0 -> 360
            int col_id = round(col_angle / bin_res);
            // id may be out of boundary 剔除超范围的ID
            if (row_id < 0 || row_id >= num_bins || col_id < 0 || col_id >= num_bins)
                continue;
            // only keep points that's closer
            float dist = pointDistance(p);
            if (dist < rangeImage.at<float>(row_id, col_id))
            {
                rangeImage.at<float>(row_id, col_id) = dist; //储存深度值
                pointsArray[row_id][col_id] = p;//储存对应点
            }
        }

        // 4. extract downsampled depth cloud from range image 在深度直方图中，每个深度值只保存一个点
        pcl::PointCloud<PointType>::Ptr depth_cloud_local_filter2(new pcl::PointCloud<PointType>());
        for (int i = 0; i < num_bins; ++i)
        {
            for (int j = 0; j < num_bins; ++j)
            {
                if (rangeImage.at<float>(i, j) != FLT_MAX)
                    depth_cloud_local_filter2->push_back(pointsArray[i][j]);
            }
        }
        *depth_cloud_local = *depth_cloud_local_filter2;
        // std::cout << ">>>" << depth_cloud_local->points.size() << std::endl;
        publishCloud(&pub_depth_cloud, depth_cloud_local, stamp_cur, "vins_body_ros");

        // 5. project depth cloud onto a unit sphere 将保留的激光点投影至单位球坐标系
        pcl::PointCloud<PointType>::Ptr depth_cloud_unit_sphere(new pcl::PointCloud<PointType>());
        for (int i = 0; i < (int)depth_cloud_local->size(); ++i)
        {
            PointType p = depth_cloud_local->points[i];
            float range = pointDistance(p);
            p.x /= range;
            p.y /= range;
            p.z /= range;
            p.intensity = range;
            depth_cloud_unit_sphere->push_back(p);
        }
        if (depth_cloud_unit_sphere->size() < 10) //数量少于10直接返回视觉特征
            return depth_of_point;

        // 6. create a kd-tree using the spherical depth cloud //使用kdtree搜索每个视觉特征点最近的三个激光点
        pcl::KdTreeFLANN<PointType>::Ptr kdtree(new pcl::KdTreeFLANN<PointType>());
        kdtree->setInputCloud(depth_cloud_unit_sphere);

        // 7. find the feature depth using kd-tree
        vector<int> pointSearchInd;
        vector<float> pointSearchSqDis;
        float dist_sq_threshold = pow(sin(bin_res / 180.0 * M_PI) * 5.0, 2);//搜索距离阈值的平方
        for (int i = 0; i < (int)features_3d_sphere->size(); ++i)
        {
            kdtree->nearestKSearch(features_3d_sphere->points[i], 3, pointSearchInd, pointSearchSqDis);//在depth_cloud_unit_sphere中搜索和目标点的最近三个点
            if (pointSearchInd.size() == 3 && pointSearchSqDis[2] < dist_sq_threshold)
            {
                float r1 = depth_cloud_unit_sphere->points[pointSearchInd[0]].intensity;
                Eigen::Vector3f A(depth_cloud_unit_sphere->points[pointSearchInd[0]].x * r1,
                                  depth_cloud_unit_sphere->points[pointSearchInd[0]].y * r1,
                                  depth_cloud_unit_sphere->points[pointSearchInd[0]].z * r1); //A点实际坐标

                float r2 = depth_cloud_unit_sphere->points[pointSearchInd[1]].intensity;
                Eigen::Vector3f B(depth_cloud_unit_sphere->points[pointSearchInd[1]].x * r2,
                                  depth_cloud_unit_sphere->points[pointSearchInd[1]].y * r2,
                                  depth_cloud_unit_sphere->points[pointSearchInd[1]].z * r2);

                float r3 = depth_cloud_unit_sphere->points[pointSearchInd[2]].intensity;
                Eigen::Vector3f C(depth_cloud_unit_sphere->points[pointSearchInd[2]].x * r3,
                                  depth_cloud_unit_sphere->points[pointSearchInd[2]].y * r3,
                                  depth_cloud_unit_sphere->points[pointSearchInd[2]].z * r3);

                // https://math.stackexchange.com/questions/100439/determine-where-a-vector-will-intersect-a-plane
                //提取单位球坐标下的视觉特征点的坐标
                Eigen::Vector3f V(features_3d_sphere->points[i].x,
                                  features_3d_sphere->points[i].y,
                                  features_3d_sphere->points[i].z);
                
                //估计视觉特征点的深度
                Eigen::Vector3f N = (A - B).cross(B - C); //向量BA和向量CB的叉乘
                float s = (N(0) * A(0) + N(1) * A(1) + N(2) * A(2)) 
                        / (N(0) * V(0) + N(1) * V(1) + N(2) * V(2));
                //最小深度和最大深度情况
                float min_depth = min(r1, min(r2, r3));
                float max_depth = max(r1, max(r2, r3));
                if (max_depth - min_depth > 2 || s <= 0.5)
                {
                    continue;
                } else if (s - max_depth > 0) {
                    s = max_depth;
                } else if (s - min_depth < 0) {
                    s = min_depth;
                }
                // convert feature into cartesian space if depth is available 如果深度值正确，将视觉特征点投影到笛卡尔坐标系
                features_3d_sphere->points[i].x *= s;
                features_3d_sphere->points[i].y *= s;
                features_3d_sphere->points[i].z *= s;
                // the obtained depth here is for unit sphere, VINS estimator needs depth for normalized feature (by value z), (lidar x = camera z)
                features_3d_sphere->points[i].intensity = features_3d_sphere->points[i].x;
            }
        }

        // visualize features in cartesian 3d space (including the feature without depth (default 1)) 发布视觉特征点(vins_body_ros系下)
        publishCloud(&pub_depth_feature, features_3d_sphere, stamp_cur, "vins_body_ros"); 
        
        // update depth value for return
        for (int i = 0; i < (int)features_3d_sphere->size(); ++i)
        {
            if (features_3d_sphere->points[i].intensity > 3.0) //只更新大于3.0m的点 少于3.0m的点全都默认是-1
                depth_of_point.values[i] = features_3d_sphere->points[i].intensity;
        }

        // visualization project points on image for visualization (it's slow!)
        if (pub_depth_image.getNumSubscribers() != 0)
        {
            vector<cv::Point2f> points_2d;
            vector<float> points_distance;

            for (int i = 0; i < (int)depth_cloud_local->size(); ++i)
            {
                // convert points from 3D to 2D //相机系
                Eigen::Vector3d p_3d(-depth_cloud_local->points[i].y,
                                     -depth_cloud_local->points[i].z,
                                      depth_cloud_local->points[i].x);
                Eigen::Vector2d p_2d;
                camera_model->spaceToPlane(p_3d, p_2d);
                
                points_2d.push_back(cv::Point2f(p_2d(0), p_2d(1)));
                points_distance.push_back(pointDistance(depth_cloud_local->points[i]));
            }

            cv::Mat showImage, circleImage;
            cv::cvtColor(imageCur, showImage, cv::COLOR_GRAY2RGB);
            circleImage = showImage.clone();
            for (int i = 0; i < (int)points_2d.size(); ++i)
            {
                float r, g, b;
                getColor(points_distance[i], 50.0, r, g, b); //根据深度值赋值rgb三通道值
                cv::circle(circleImage, points_2d[i], 0, cv::Scalar(r, g, b), 5);
            }
            cv::addWeighted(showImage, 1.0, circleImage, 0.7, 0, showImage); // blend camera image and circle image

            cv_bridge::CvImage bridge;
            bridge.image = showImage;
            bridge.encoding = "rgb8";
            sensor_msgs::Image::Ptr imageShowPointer = bridge.toImageMsg();
            imageShowPointer->header.stamp = stamp_cur;
            pub_depth_image.publish(imageShowPointer);
        }

        return depth_of_point;
    }

    void getColor(float p, float np, float&r, float&g, float&b) 
    {
        float inc = 6.0 / np;
        float x = p * inc;
        r = 0.0f; g = 0.0f; b = 0.0f;
        if ((0 <= x && x <= 1) || (5 <= x && x <= 6)) r = 1.0f;
        else if (4 <= x && x <= 5) r = x - 4;
        else if (1 <= x && x <= 2) r = 1.0f - (x - 1);

        if (1 <= x && x <= 3) g = 1.0f;
        else if (0 <= x && x <= 1) g = x - 0;
        else if (3 <= x && x <= 4) g = 1.0f - (x - 3);

        if (3 <= x && x <= 5) b = 1.0f;
        else if (2 <= x && x <= 3) b = x - 2;
        else if (5 <= x && x <= 6) b = 1.0f - (x - 5);
        r *= 255.0;
        g *= 255.0;
        b *= 255.0;
    }
};