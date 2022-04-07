#include "utility.h"
 
#include <gtsam/geometry/Rot3.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/navigation/GPSFactor.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/navigation/CombinedImuFactor.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/inference/Symbol.h>
 
#include <gtsam/nonlinear/ISAM2.h>
#include <gtsam_unstable/nonlinear/IncrementalFixedLagSmoother.h>
 
using gtsam::symbol_shorthand::X; // Pose3 (x,y,z,r,p,y)
using gtsam::symbol_shorthand::V; // Vel   (xdot,ydot,zdot)
using gtsam::symbol_shorthand::B; // Bias  (ax,ay,az,gx,gy,gz)
 
 
// ���ļ�����̼ƣ�����MapOptimization����IMU��̼ƣ�
//����ǰһʱ�̼�����̼ƣ��͸�ʱ�̵���ǰʱ�̵�IMU��̼Ʊ任������
//���㵱ǰʱ��IMU��̼ƣ�
//rvizչʾIMU��̼ƹ켣���ֲ�����
class TransformFusion : public ParamServer
{
public:
    std::mutex mtx;
 
    ros::Subscriber subImuOdometry;
    ros::Subscriber subLaserOdometry;
 
    ros::Publisher pubImuOdometry;
    ros::Publisher pubImuPath;
 
    Eigen::Affine3f lidarOdomAffine;
    Eigen::Affine3f imuOdomAffineFront;
    Eigen::Affine3f imuOdomAffineBack;
 
    tf::TransformListener tfListener;
    tf::StampedTransform lidar2Baselink;
 
    double lidarOdomTime = -1;
    deque<nav_msgs::Odometry> imuOdomQueue;
 
    TransformFusion()
    {
        // ���lidarϵ��baselinkϵ��ͬ������ϵ������ϵ������Ҫ�ⲿ�ṩ����֮��ı任��ϵ
        if(lidarFrame != baselinkFrame)
        {
            try
            {
                // �ȴ�3s
                tfListener.waitForTransform(lidarFrame, baselinkFrame, ros::Time(0), ros::Duration(3.0));
                // lidarϵ��baselinkϵ�ı任
                tfListener.lookupTransform(lidarFrame, baselinkFrame, ros::Time(0), lidar2Baselink);
            }
            catch (tf::TransformException ex)
            {
                ROS_ERROR("%s",ex.what());
            }
        }
        // ���ļ�����̼ƣ�����mapOptimization
        subLaserOdometry = nh.subscribe<nav_msgs::Odometry>("lio_sam/mapping/odometry", 5, &TransformFusion::lidarOdometryHandler, this, ros::TransportHints().tcpNoDelay());
        // ����imu��̼ƣ�����IMUPreintegration(IMUPreintegration.cpp�е���IMUPreintegration)
        //topic name: odometry/imu_incremental
        //ע������lio_sam/mapping/odometry_incremental
        //Ŀǰ������ȷ��һ�㣬odometry/imu_incremental���������ݣ�����֡������̼�֮���Ԥ��������,�����Ͽ�ʼ�ļ�����̼Ʊ����е�λ�ˣ�
        //imuIntegratorImu_�����Ǹ���������ֻ����֮֡���Ԥ���֣����Ƿ�����ʱ�򷢲���ʵ���ǽ����ǰ����̼Ʊ����е�λ��
        //�����predict���prevStateOdom:
        //currentState = imuIntegratorImu_->predict(prevStateOdom, prevBiasOdom);
        subImuOdometry   = nh.subscribe<nav_msgs::Odometry>(odomTopic+"_incremental",   2000, &TransformFusion::imuOdometryHandler,   this, ros::TransportHints().tcpNoDelay());
        // ����imu��̼ƣ�����rvizչʾ
        pubImuOdometry   = nh.advertise<nav_msgs::Odometry>(odomTopic, 2000);
        // ����imu��̼ƹ켣
        pubImuPath       = nh.advertise<nav_msgs::Path>    ("lio_sam/imu/path", 1);
    }
 
 
    /**
     * ��̼ƶ�Ӧ�任����
    */
    Eigen::Affine3f odom2affine(nav_msgs::Odometry odom)
    {
        double x, y, z, roll, pitch, yaw;
        x = odom.pose.pose.position.x;
        y = odom.pose.pose.position.y;
        z = odom.pose.pose.position.z;
        tf::Quaternion orientation;
        tf::quaternionMsgToTF(odom.pose.pose.orientation, orientation);
        tf::Matrix3x3(orientation).getRPY(roll, pitch, yaw);
        return pcl::getTransformation(x, y, z, roll, pitch, yaw);
    }
 
    /**
     * ���ļ�����̼ƵĻص�����������mapOptimization
    */
    void lidarOdometryHandler(const nav_msgs::Odometry::ConstPtr& odomMsg)
    {
        std::lock_guard<std::mutex> lock(mtx);
        // ������̼ƶ�Ӧ�任����
        lidarOdomAffine = odom2affine(*odomMsg);
        // ������̼�ʱ���
        lidarOdomTime = odomMsg->header.stamp.toSec();
        //��������汣��Ķ��������һ���״Ｄ����̼Ƶı任��ʱ�������������һ��vector֮��Ķ�������������
    }
 
    /**
     * ����imu��̼ƣ�����IMUPreintegration
     * 1�������һ֡������̼�λ��Ϊ�����������ʱ���뵱ǰʱ�̼�imu��̼�����λ�˱任����˵õ���ǰʱ��imu��̼�λ��
     * 2��������ǰʱ����̼�λ�ˣ�����rvizչʾ������imu��̼�·����ע��ֻ�����һ֡������̼�ʱ���뵱ǰʱ��֮���һ��
    */
    void imuOdometryHandler(const nav_msgs::Odometry::ConstPtr& odomMsg)
    {
        // ����tf��map��odomϵ��Ϊͬһ��ϵ
        static tf::TransformBroadcaster tfMap2Odom;
        static tf::Transform map_to_odom = tf::Transform(tf::createQuaternionFromRPY(0, 0, 0), tf::Vector3(0, 0, 0));
        tfMap2Odom.sendTransform(tf::StampedTransform(map_to_odom, odomMsg->header.stamp, mapFrame, odometryFrame));
 
        std::lock_guard<std::mutex> lock(mtx);
        // ���imu��̼Ƶ����У�ע��imu��̼��ɱ�cpp�е���һ����imuPreintegration������
        imuOdomQueue.push_back(*odomMsg);
 
        // get latest odometry (at current IMU stamp)
        // ��imu��̼ƶ�����ɾ����ǰ�������һ֡��������̼�ʱ��֮ǰ������
        // lidarOdomTime��ʼ��Ϊ-1�����յ�lidar��̼����ݺ��ڻص�����lidarOdometryHandler�б���ֵʱ���
        if (lidarOdomTime == -1)
            return;
        while (!imuOdomQueue.empty())
        {
            if (imuOdomQueue.front().header.stamp.toSec() <= lidarOdomTime)
                imuOdomQueue.pop_front();
            else
                break;
        }
        // �����һ֡������̼�ʱ�̶�Ӧimu��̼�λ��
        Eigen::Affine3f imuOdomAffineFront = odom2affine(imuOdomQueue.front());
        // ��ǰʱ��imu��̼�λ��
        Eigen::Affine3f imuOdomAffineBack = odom2affine(imuOdomQueue.back());
        // imu��̼�����λ�˱任
        Eigen::Affine3f imuOdomAffineIncre = imuOdomAffineFront.inverse() * imuOdomAffineBack;
        //  ��ǰʱ��imu��̼�λ��=�����һ֡������̼�λ�� * imu��̼�����λ�˱任 
        //lidarOdomAffine�ڱ����lidarOdometryHandler�ص������б���ֵ����Ϣ��Դ��mapOptimization.cpp����,�Ǽ�����̼�
        Eigen::Affine3f imuOdomAffineLast = lidarOdomAffine * imuOdomAffineIncre;
        float x, y, z, roll, pitch, yaw;
        pcl::getTranslationAndEulerAngles(imuOdomAffineLast, x, y, z, roll, pitch, yaw);
        
        // publish latest odometry
        //������Ϊ"odometry/imu"�Ļ���
        //Ҳ����˵����������ȼ�������IMUPreintegration�з�����odometry/imu_incremental��
        //Ȼ�����imu����֮���������Ȼ���ڼ���Ļ����ϼ������������·���
        //���Կ��������Ĳ���odometry/imu_incremental��һ���������ݣ����ǰ�x��y��z��roll��pitch��yaw�������˼�����̼Ƶ��������ŷ���
        //���Կ�����η��������ݣ��ǵ�ǰʱ����̼�λ��.��������Ϊ"odometry/imu"
        nav_msgs::Odometry laserOdometry = imuOdomQueue.back(); 
        laserOdometry.pose.pose.position.x = x;  //�����µ�ֵ
        laserOdometry.pose.pose.position.y = y;
        laserOdometry.pose.pose.position.z = z;
        laserOdometry.pose.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(roll, pitch, yaw);
        pubImuOdometry.publish(laserOdometry);
 
        // ����tf����ǰʱ��odom��baselinkϵ�任��ϵ
        //����֮ǰ��map��odom����ϵ�̶��ˣ������������Ϊ�����ľ�������������λ�˹�ϵ
        //map�Ż��ṩ���⣬Ԥ�����ṩimu��imu֮��任�ٳ��Լ�����̼Ƶõ�����ʱ�̾�ȷλ��
        static tf::TransformBroadcaster tfOdom2BaseLink;
        tf::Transform tCur;
        tf::poseMsgToTF(laserOdometry.pose.pose, tCur);
        if(lidarFrame != baselinkFrame)
            tCur = tCur * lidar2Baselink;
        tf::StampedTransform odom_2_baselink = tf::StampedTransform(tCur, odomMsg->header.stamp, odometryFrame, baselinkFrame);
        tfOdom2BaseLink.sendTransform(odom_2_baselink);
 
        // publish IMU path
        // ����imu��̼�·����ע��ֻ�����һ֡������̼�ʱ���뵱ǰʱ��֮���һ��
        static nav_msgs::Path imuPath;
        static double last_path_time = -1;
        double imuTime = imuOdomQueue.back().header.stamp.toSec();
        // ÿ��0.1s���һ��
        if (imuTime - last_path_time > 0.1)
        {
            last_path_time = imuTime;
            geometry_msgs::PoseStamped pose_stamped;
            pose_stamped.header.stamp = imuOdomQueue.back().header.stamp;
            pose_stamped.header.frame_id = odometryFrame; //�ͽ�"odom"
            pose_stamped.pose = laserOdometry.pose.pose;
            imuPath.poses.push_back(pose_stamped);
            // ɾ�����һ֡������̼�ʱ��֮ǰ��imu��̼�
            while(!imuPath.poses.empty() && imuPath.poses.front().header.stamp.toSec() < lidarOdomTime - 1.0)
                imuPath.poses.erase(imuPath.poses.begin());
            if (pubImuPath.getNumSubscribers() != 0)
            {
                imuPath.header.stamp = imuOdomQueue.back().header.stamp;
                imuPath.header.frame_id = odometryFrame;
                pubImuPath.publish(imuPath);
            }
        }
    }
};
 
class IMUPreintegration : public ParamServer
{
public:
 
    std::mutex mtx;
 
    ros::Subscriber subImu;
    ros::Subscriber subOdometry;
    ros::Publisher pubImuOdometry;
 
    bool systemInitialized = false;
    // ����Э����
    gtsam::noiseModel::Diagonal::shared_ptr priorPoseNoise;
    gtsam::noiseModel::Diagonal::shared_ptr priorVelNoise;
    gtsam::noiseModel::Diagonal::shared_ptr priorBiasNoise;
    gtsam::noiseModel::Diagonal::shared_ptr correctionNoise;
    gtsam::noiseModel::Diagonal::shared_ptr correctionNoise2;
    gtsam::Vector noiseModelBetweenBias;
 
    // imuԤ������
 
    //imuIntegratorOpt_����Ԥ��������������̼�֮���imu���ݣ���ΪԼ����������ͼ�������Ż���bias
    gtsam::PreintegratedImuMeasurements *imuIntegratorOpt_;
    //imuIntegratorImu_���������µļ�����̼Ƶ�����Ѿ��Ż��õ�bias��Ԥ��ӵ�ǰ֡��ʼ����һ֡������̼Ƶ���֮ǰ��imu��̼�����
    gtsam::PreintegratedImuMeasurements *imuIntegratorImu_;
 
    // imu���ݶ���
    //imuQueOpt������imuIntegratorOpt_�ṩ������Դ����Ҫ�ľ͵���(�Ӷ�ͷ��ʼ�������ȵ�ǰ������̼��������imuͨͨ���֣���һ����һ��)��
    std::deque<sensor_msgs::Imu> imuQueOpt;
    //imuQueImu������imuIntegratorImu_�ṩ������Դ����Ҫ�ľ͵���(������ǰ������̼�֮ǰ��imu����,Ԥ��������һ����һ��)��
    std::deque<sensor_msgs::Imu> imuQueImu;
 
    // imu����ͼ�Ż������е�״̬����
    gtsam::Pose3 prevPose_;
    gtsam::Vector3 prevVel_;
    gtsam::NavState prevState_;
    gtsam::imuBias::ConstantBias prevBias_;
 
    // imu״̬
    gtsam::NavState prevStateOdom;
    gtsam::imuBias::ConstantBias prevBiasOdom;
 
    bool doneFirstOpt = false;
    double lastImuT_imu = -1;
    double lastImuT_opt = -1;
 
    // ISAM2�Ż���
    gtsam::ISAM2 optimizer;
    gtsam::NonlinearFactorGraph graphFactors;  //�ܵ�����ͼģ��
    gtsam::Values graphValues;  //����ͼģ���е�ֵ
 
    const double delta_t = 0;
 
    int key = 1;
 
    // imu-lidarλ�˱任
    //���Ҫע�⣬tixiaoshan���������ĺ���������ֻ��һ��ƽ�Ʊ任��
    //ͬ��ͷ�ļ���imuConverter�У�Ҳֻ��һ����ת�任��������Բ��������Ϊ��imu����ת��lidar�µı任����
    //��ʵ�ϣ����ߺ����ǰ�imu��������imuConverter��ת���״�ϵ�£�����ʵ�����˸�ƽ�ƣ���
    //���������ǰ��״������ָ���lidar2Imu����ƽ����һ�£���ת���Ժ���˸�ƽ�Ƶ�imu�����ڡ��м�ϵ�����룬
    //֮�������ִ��м�ϵͨ��imu2LidarŲ�����״�ϵ����publish��
    gtsam::Pose3 imu2Lidar = gtsam::Pose3(gtsam::Rot3(1, 0, 0, 0), gtsam::Point3(-extTrans.x(), -extTrans.y(), -extTrans.z()));
    gtsam::Pose3 lidar2Imu = gtsam::Pose3(gtsam::Rot3(1, 0, 0, 0), gtsam::Point3(extTrans.x(), extTrans.y(), extTrans.z()));
 
    IMUPreintegration()
    {
        //����imuԭʼ���ݣ�����������ͼ�Ż��Ľ����ʩ����֮֡���imuԤ�Ʒ�����Ԥ��ÿһʱ�̣�imuƵ�ʣ���imu��̼�
        //imuTopic name: "imu_correct"
        subImu      = nh.subscribe<sensor_msgs::Imu>  (imuTopic,                   2000, &IMUPreintegration::imuHandler,      this, ros::TransportHints().tcpNoDelay());
        // ���ļ�����̼ƣ�����mapOptimization������֮֡���imuԤ�Ʒ�����������ͼ��
        // �Ż���ǰ֡λ�ˣ����λ�˽����ڸ���ÿʱ�̵�imu��̼ƣ��Լ���һ������ͼ�Ż���
        subOdometry = nh.subscribe<nav_msgs::Odometry>("lio_sam/mapping/odometry_incremental", 5,    &IMUPreintegration::odometryHandler, this, ros::TransportHints().tcpNoDelay());
        
        //����imu��̼�: odometry/imu_incremental
        pubImuOdometry = nh.advertise<nav_msgs::Odometry> (odomTopic+"_incremental", 2000);
 
        // imuԤ���ֵ�����Э����
        boost::shared_ptr<gtsam::PreintegrationParams> p = gtsam::PreintegrationParams::MakeSharedU(imuGravity);
 
        //imuAccNoise��imuGyrNoise���Ƕ�����ͷ�ļ��еĸ�˹���������������ļ���д��
        p->accelerometerCovariance  = gtsam::Matrix33::Identity(3,3) * pow(imuAccNoise, 2); // acc white noise in continuous
        p->gyroscopeCovariance      = gtsam::Matrix33::Identity(3,3) * pow(imuGyrNoise, 2); // gyro white noise in continuous
        //�����ٶȵĻ����������ʱ��̫���
        p->integrationCovariance    = gtsam::Matrix33::Identity(3,3) * pow(1e-4, 2); // error committed in integrating position from velocities
        //����û�г�ʼ��bias
        gtsam::imuBias::ConstantBias prior_imu_bias((gtsam::Vector(6) << 0, 0, 0, 0, 0, 0).finished());; // assume zero initial bias
        
        // ��������
        //Diagonal�Խ��߾���
        //����diagonal��һ�����.finished(),ע����˵finished()��Ϊ����������ϵ���󷵻ع����ľ���
        priorPoseNoise  = gtsam::noiseModel::Diagonal::Sigmas((gtsam::Vector(6) << 1e-2, 1e-2, 1e-2, 1e-2, 1e-2, 1e-2).finished()); // rad,rad,rad,m, m, m
        priorVelNoise   = gtsam::noiseModel::Isotropic::Sigma(3, 1e4); // m/s
        priorBiasNoise  = gtsam::noiseModel::Isotropic::Sigma(6, 1e-3); // 1e-2 ~ 1e-3 seems to be good
        // ������̼�scan-to-map�Ż������з����˻�����ѡ��һ���ϴ��Э����
        correctionNoise = gtsam::noiseModel::Diagonal::Sigmas((gtsam::Vector(6) << 0.05, 0.05, 0.05, 0.1, 0.1, 0.1).finished()); // rad,rad,rad,m, m, m
        correctionNoise2 = gtsam::noiseModel::Diagonal::Sigmas((gtsam::Vector(6) << 1, 1, 1, 1, 1, 1).finished()); // rad,rad,rad,m, m, m
        noiseModelBetweenBias = (gtsam::Vector(6) << imuAccBiasN, imuAccBiasN, imuAccBiasN, imuGyrBiasN, imuGyrBiasN, imuGyrBiasN).finished();
        
        //imuԤ������������Ԥ��ÿһʱ�̣�imuƵ�ʣ���imu��̼ƣ�ת��lidarϵ�ˣ��뼤����̼�ͬһ��ϵ��
        imuIntegratorImu_ = new gtsam::PreintegratedImuMeasurements(p, prior_imu_bias); // setting up the IMU integration for IMU message thread
        //imuԤ����������������ͼ�Ż�
        imuIntegratorOpt_ = new gtsam::PreintegratedImuMeasurements(p, prior_imu_bias); // setting up the IMU integration for optimization        
    }
 
    void resetOptimization()
    {
        gtsam::ISAM2Params optParameters;
        optParameters.relinearizeThreshold = 0.1;
        optParameters.relinearizeSkip = 1;
        optimizer = gtsam::ISAM2(optParameters);
 
        gtsam::NonlinearFactorGraph newGraphFactors;
        graphFactors = newGraphFactors;
 
        gtsam::Values NewGraphValues;
        graphValues = NewGraphValues;
    }
 
    void resetParams()
    {
        lastImuT_imu = -1;
        doneFirstOpt = false;
        systemInitialized = false;
    }
    // ���ĵ��Ǽ�����̼�,"lio_sam/mapping/odometry_incremental"
    void odometryHandler(const nav_msgs::Odometry::ConstPtr& odomMsg)
    {
        std::lock_guard<std::mutex> lock(mtx);
        // ��ǰ֡������̼�ʱ���
        double currentCorrectionTime = ROS_TIME(odomMsg);
 
        // ȷ��imu�Ż���������imu���ݽ���Ԥ����
        if (imuQueOpt.empty())
            return;
 
        // ��ǰ֡����λ�ˣ�����scan-to-mapƥ�䡢����ͼ�Ż����λ��
        float p_x = odomMsg->pose.pose.position.x;
        float p_y = odomMsg->pose.pose.position.y;
        float p_z = odomMsg->pose.pose.position.z;
        float r_x = odomMsg->pose.pose.orientation.x;
        float r_y = odomMsg->pose.pose.orientation.y;
        float r_z = odomMsg->pose.pose.orientation.z;
        float r_w = odomMsg->pose.pose.orientation.w;
        bool degenerate = (int)odomMsg->pose.covariance[0] == 1 ? true : false;
        gtsam::Pose3 lidarPose = gtsam::Pose3(gtsam::Rot3::Quaternion(r_w, r_x, r_y, r_z), gtsam::Point3(p_x, p_y, p_z));
 
 
        // 0. initialize system
        // 0. ϵͳ��ʼ������һ֡
        if (systemInitialized == false)
        {
            // ����ISAM2�Ż���
            resetOptimization();
 
            // pop old IMU message
            // ��imu�Ż�������ɾ����ǰ֡������̼�ʱ��֮ǰ��imu����,delta_t=0
            while (!imuQueOpt.empty())
            {
                if (ROS_TIME(&imuQueOpt.front()) < currentCorrectionTime - delta_t)
                {
                    lastImuT_opt = ROS_TIME(&imuQueOpt.front());
                    imuQueOpt.pop_front();
                }
                else
                    break;
            }
            // initial pose
            // �����̼�λ����������
            //lidarPose Ϊ���ص������յ��ļ�����̼����ݣ������gtsam��pose��ʽ
            //��ת��imu����ϵ��,�Ҳ²�compose�������������֮��ĺ����
            prevPose_ = lidarPose.compose(lidar2Imu);
            //X�����ǹ̶����䣨��ʹ��Poseʱ����������ٶ�����V��bias����B
            gtsam::PriorFactor<gtsam::Pose3> priorPose(X(0), prevPose_, priorPoseNoise);
            //ͨ�������ܵ�����ͼģ�͵�add��ʽ����ӵ�һ������
            //PriorFactor ����ɿ�gtsam  ������λ�� �ٶ�  bias 
            //����PriorFactor��ͼ�Ż��л������Ǳ����ǰ��
            //����noise�������ڹ��캯������
            graphFactors.add(priorPose);
            // initial velocity
            prevVel_ = gtsam::Vector3(0, 0, 0);
            gtsam::PriorFactor<gtsam::Vector3> priorVel(V(0), prevVel_, priorVelNoise);
            graphFactors.add(priorVel);
            // initial bias
            prevBias_ = gtsam::imuBias::ConstantBias();
            gtsam::PriorFactor<gtsam::imuBias::ConstantBias> priorBias(B(0), prevBias_, priorBiasNoise);
            graphFactors.add(priorBias);
            // add values
            // �����ڵ㸳��ֵ
            graphValues.insert(X(0), prevPose_);
            graphValues.insert(V(0), prevVel_);
            graphValues.insert(B(0), prevBias_);
            // optimize once
            // �Ż�һ��
            optimizer.update(graphFactors, graphValues);
            //ͼ�ͽڵ������  ΪʲôҪ���㲻�ܼ�������?
            //����Ϊ�ڵ���Ϣ������gtsam::ISAM2 optimizer������Ҫ�������ܼ���ʹ��
            graphFactors.resize(0);
            graphValues.clear();
 
            //����������,�����Ż�֮���ƫ��
            imuIntegratorImu_->resetIntegrationAndSetBias(prevBias_);
            imuIntegratorOpt_->resetIntegrationAndSetBias(prevBias_);
            
            key = 1;
            systemInitialized = true;
            return;
        }
 
 
        // reset graph for speed
        // ÿ��100֡������̼ƣ�����ISAM2�Ż�������֤�Ż�Ч��
        if (key == 100)
        {
            // get updated noise before reset
            // ǰһ֡��λ�ˡ��ٶȡ�ƫ������ģ��
            //������������ֵ
            gtsam::noiseModel::Gaussian::shared_ptr updatedPoseNoise = gtsam::noiseModel::Gaussian::Covariance(optimizer.marginalCovariance(X(key-1)));
            gtsam::noiseModel::Gaussian::shared_ptr updatedVelNoise  = gtsam::noiseModel::Gaussian::Covariance(optimizer.marginalCovariance(V(key-1)));
            gtsam::noiseModel::Gaussian::shared_ptr updatedBiasNoise = gtsam::noiseModel::Gaussian::Covariance(optimizer.marginalCovariance(B(key-1)));
            // reset graph
            // ����ISAM2�Ż���
            resetOptimization();
            // add pose
            // ���λ���������ӣ���ǰһ֡��ֵ��ʼ��
            //����֮�����������ʼ���Ĺ��� ������������ֵ��ͬ
            //prevPose_�����Ҳ����һʱ�̵õ��ģ�
            //����ʼʱ����lidar��̼Ƶ�poseֱ����lidar2IMU����ת��imu����ϵ�£����˴�����ͨ����һʱ�̣����������ĺ����Ż��еõ���
            gtsam::PriorFactor<gtsam::Pose3> priorPose(X(0), prevPose_, updatedPoseNoise);
            graphFactors.add(priorPose);
            // add velocity
            gtsam::PriorFactor<gtsam::Vector3> priorVel(V(0), prevVel_, updatedVelNoise);
            graphFactors.add(priorVel);
            // add bias
            gtsam::PriorFactor<gtsam::imuBias::ConstantBias> priorBias(B(0), prevBias_, updatedBiasNoise);
            graphFactors.add(priorBias);
            // add values
            graphValues.insert(X(0), prevPose_);
            graphValues.insert(V(0), prevVel_);
            graphValues.insert(B(0), prevBias_);
            // optimize once
            optimizer.update(graphFactors, graphValues);
            graphFactors.resize(0);
            graphValues.clear();
 
            key = 1;
        }
 
 
        // 1. integrate imu data and optimize
        // 1. ����ǰһ֡�뵱ǰ֮֡���imuԤ����������ǰһ֡״̬ʩ��Ԥ�������õ���ǰ֡��ʼ״̬���ƣ�
        //  �������mapOptimization�ĵ�ǰ֡λ�ˣ���������ͼ�Ż������µ�ǰ֡״̬
        while (!imuQueOpt.empty())
        {
            // pop and integrate imu data that is between two optimizations
            // ��ȡǰһ֡�뵱ǰ֮֡���imu���ݣ�����Ԥ����
            sensor_msgs::Imu *thisImu = &imuQueOpt.front();
            double imuTime = ROS_TIME(thisImu);
            //currentCorrectionTime�ǵ�ǰ�ص������յ��ļ�����̼����ݵ�ʱ��
            if (imuTime < currentCorrectionTime - delta_t)
            {
                double dt = (lastImuT_opt < 0) ? (1.0 / 500.0) : (imuTime - lastImuT_opt);
                // imuԤ�����������룺���ٶȡ����ٶȡ�dt
                // ������������������ͼ�Ż���Ԥ������imuIntegratorOpt_,ע���������һ�������dt
                //����Ҫ���9��imu������ŷ�����ڱ������ļ���û���κ��õ�,ȫ�ڵ�ͼ�Ż����õ���
                imuIntegratorOpt_->integrateMeasurement(
                        gtsam::Vector3(thisImu->linear_acceleration.x, thisImu->linear_acceleration.y, thisImu->linear_acceleration.z),
                        gtsam::Vector3(thisImu->angular_velocity.x,    thisImu->angular_velocity.y,    thisImu->angular_velocity.z), dt);
                //���Ƴ�һ������ǰ������һ�����ݵ�ʱ���
 
                lastImuT_opt = imuTime;
                // �Ӷ�����ɾ���Ѿ������imu����
                imuQueOpt.pop_front();
            }
            else
                break;
        }
        // add imu factor to graph
        //������֮֡���IMU���������Ԥ���ֺ�����imu���ӵ�����ͼ��,
        //ע��������ױ��ڵ���imuIntegratorOpt_��ֵ������ʽת��������preint_imu��
        //��˿����Ʋ�imuIntegratorOpt_�е�integrateMeasurement����Ӧ�þ���һ���򵥵Ļ������ӣ�
        //�������ݺ�dt���õ�һ��������,���ݻᱻ�����imuIntegratorOpt_��
        const gtsam::PreintegratedImuMeasurements& preint_imu = dynamic_cast<const gtsam::PreintegratedImuMeasurements&>(*imuIntegratorOpt_);
        // ������ǰһ֡λ�ˣ�ǰһ֡�ٶȣ���ǰ֡λ�ˣ���ǰ֡�ٶȣ�ǰһ֡ƫ�ã�Ԥ�Ʒ���
        gtsam::ImuFactor imu_factor(X(key - 1), V(key - 1), X(key), V(key), B(key - 1), preint_imu);
        graphFactors.add(imu_factor);
        // add imu bias between factor
        // ���imuƫ�����ӣ�ǰһ֡ƫ��B(key - 1)����ǰ֡ƫ��B(key)���۲�ֵ������Э���deltaTij()�ǻ��ֶε�ʱ��
        graphFactors.add(gtsam::BetweenFactor<gtsam::imuBias::ConstantBias>(B(key - 1), B(key), gtsam::imuBias::ConstantBias(),
                         gtsam::noiseModel::Diagonal::Sigmas(sqrt(imuIntegratorOpt_->deltaTij()) * noiseModelBetweenBias)));
        // add pose factor
        // ���λ������
        gtsam::Pose3 curPose = lidarPose.compose(lidar2Imu);
        gtsam::PriorFactor<gtsam::Pose3> pose_factor(X(key), curPose, degenerate ? correctionNoise2 : correctionNoise);
        graphFactors.add(pose_factor);
        // insert predicted values
        // ��ǰһ֡��״̬��ƫ�ã�ʩ��imuԤ�Ʒ������õ���ǰ֡��״̬
        gtsam::NavState propState_ = imuIntegratorOpt_->predict(prevState_, prevBias_);
        // �����ڵ㸳��ֵ
        graphValues.insert(X(key), propState_.pose());
        graphValues.insert(V(key), propState_.v());
        graphValues.insert(B(key), prevBias_);
        // optimize
        optimizer.update(graphFactors, graphValues);
        optimizer.update();
        graphFactors.resize(0);
        graphValues.clear();
        // Overwrite the beginning of the preintegration for the next step.
         // �Ż����
        gtsam::Values result = optimizer.calculateEstimate();
        // ���µ�ǰ֡λ�ˡ��ٶ�
        prevPose_  = result.at<gtsam::Pose3>(X(key));
        prevVel_   = result.at<gtsam::Vector3>(V(key));
        // ���µ�ǰ֡״̬
        prevState_ = gtsam::NavState(prevPose_, prevVel_);
        // ���µ�ǰ֡imuƫ��
        prevBias_  = result.at<gtsam::imuBias::ConstantBias>(B(key));
        // Reset the optimization preintegration object.
        //����Ԥ�������������µ�ƫ�ã�������һ֡������̼ƽ�����ʱ��Ԥ������������֮֡�������
        imuIntegratorOpt_->resetIntegrationAndSetBias(prevBias_);
        // check optimization
        // imu����ͼ�Ż�������ٶȻ���ƫ�ù�����Ϊʧ��
        if (failureDetection(prevVel_, prevBias_))
        {
            resetParams();
            return;
        }
 
 
        // 2. after optiization, re-propagate imu odometry preintegration
        // 2. �Ż�֮��ִ���ش������Ż�������imu��ƫ�ã�
        //�����µ�ƫ�����¼��㵱ǰ������̼�ʱ��֮���imuԤ���֣����Ԥ�������ڼ���ÿʱ��λ��
        prevStateOdom = prevState_;
        prevBiasOdom  = prevBias_;
        // first pop imu message older than current correction data
        // ��imu������ɾ����ǰ������̼�ʱ��֮ǰ��imu����
        double lastImuQT = -1;
        //ע�⣬������Ҫ��ɾ������ǰ֡��֮ǰ����imu���ݣ�������ݵ�ǰ֡��֮�󡱵��ۻ����ơ�
        //��ǰ��imuIntegratorOpt_���������ǣ�����ȡ����ǰ֡��֮ǰ����imu���ݣ�����֮֡���imu���ݽ��л��֡�������ľ͵�������
        //��ˣ��µ�һ֡����֡��̼�����ʱ��imuQueOpt���б仯���£�
        //��ǰ֮֡ǰ�����ݱ�����������֣���һ��ɾһ����������һ֡����󣬶����оͲ�����������֮֡ǰ�������ˣ�
        //��ô�ڸ������Ժ�imuQueOpt���в��ٱ仯��ʣ�µ�ԭʼimu����������һ���Ż�ʱ�����ݡ�
        //��imuQueImu�������ǰѵ�ǰ֮֡ǰ��imu���ݶ���ֱ���޳�������������ǰ֮֡���imu���ݣ�
        //������֡lidar��̼Ƶ���ʱ��֮�䷢����imu����ʽ��̼Ƶ�Ԥ�⡣
        //imuQueImu��imuQueOpt������Ҫ��ȷ,imuIntegratorImu_��imuIntegratorOpt_������ҲҪ��ȷ,��imuhandler�е�ע��
 
        while (!imuQueImu.empty() && ROS_TIME(&imuQueImu.front()) < currentCorrectionTime - delta_t)
        {
            lastImuQT = ROS_TIME(&imuQueImu.front());
            imuQueImu.pop_front();
        }
        // repropogate
        // ��ʣ���imu���ݼ���Ԥ����
        if (!imuQueImu.empty())
        {
            // reset bias use the newly optimized bias
            // ����״̬,����Ԥ�����������µ�ƫ��
            imuIntegratorImu_->resetIntegrationAndSetBias(prevBiasOdom);
            // integrate imu message from the beginning of this optimization
            // ����Ԥ����
            //����imuQueImu�е����ݽ���Ԥ���� ��Ҫ�����������һ�еĸ�����bias
            for (int i = 0; i < (int)imuQueImu.size(); ++i)
            {
                sensor_msgs::Imu *thisImu = &imuQueImu[i];
                double imuTime = ROS_TIME(thisImu);
                double dt = (lastImuQT < 0) ? (1.0 / 500.0) :(imuTime - lastImuQT);
                // ע��:�������������ڴ����ĵ�Ԥ������imuIntegratorImu_,(֮ǰ�����������imuIntegratorOpt_,��
                //ע���������һ�������dt
                //����������imuIntegratorImu_��
                imuIntegratorImu_->integrateMeasurement(gtsam::Vector3(thisImu->linear_acceleration.x, thisImu->linear_acceleration.y, thisImu->linear_acceleration.z),
                                                        gtsam::Vector3(thisImu->angular_velocity.x,    thisImu->angular_velocity.y,    thisImu->angular_velocity.z), dt);
                lastImuQT = imuTime;
            }
        }
 
        ++key;
        //���ó�True������֪ͨ��һ�����𷢲�imu��̼ƵĻص�����imuHandler�����Է����ˡ�
        doneFirstOpt = true;
    }
 
    /**
     * imu����ͼ�Ż�������ٶȻ���ƫ�ù�����Ϊʧ��
    */
    bool failureDetection(const gtsam::Vector3& velCur, const gtsam::imuBias::ConstantBias& biasCur)
    {
        Eigen::Vector3f vel(velCur.x(), velCur.y(), velCur.z());
        if (vel.norm() > 30)
        {
            ROS_WARN("Large velocity, reset IMU-preintegration!");
            return true;
        }
 
        Eigen::Vector3f ba(biasCur.accelerometer().x(), biasCur.accelerometer().y(), biasCur.accelerometer().z());
        Eigen::Vector3f bg(biasCur.gyroscope().x(), biasCur.gyroscope().y(), biasCur.gyroscope().z());
        if (ba.norm() > 1.0 || bg.norm() > 1.0)
        {
            ROS_WARN("Large bias, reset IMU-preintegration!");
            return true;
        }
 
        return false;
    }
 
    /**
     * ����imuԭʼ����
     * 1������һ֡������̼�ʱ�̶�Ӧ��״̬��ƫ�ã�
     * ʩ�ӴӸ�ʱ�̿�ʼ����ǰʱ�̵�imuԤ�Ʒ������õ���ǰʱ�̵�״̬��Ҳ����imu��̼�
     * 2��imu��̼�λ��ת��lidarϵ��������̼�
    */
 
    void imuHandler(const sensor_msgs::Imu::ConstPtr& imu_raw)
    {
        std::lock_guard<std::mutex> lock(mtx);
        // imuԭʼ��������ת����lidarϵ�����ٶȡ����ٶȡ�RPY
        sensor_msgs::Imu thisImu = imuConverter(*imu_raw);
 
        // ��ӵ�ǰ֡imu���ݵ�����
        // ����˫�˶��зֱ�װ���Ż�ǰ���imu����
        imuQueOpt.push_back(thisImu);
        imuQueImu.push_back(thisImu);
 
        // Ҫ����һ��imu����ͼ�Ż�ִ�гɹ���ȷ����������һ֡��������̼�֡����״̬��ƫ�ã�Ԥ�����Ѿ������¼���
        // ������Ҫ����odomhandler���Ż�һ�κ��ٽ��иú��������Ĺ���
        if (doneFirstOpt == false)
            return;
 
        double imuTime = ROS_TIME(&thisImu);
        //lastImuT_imu������ʼ����ֵΪ-1
        // ���ʱ����, ��һ��Ϊ1/500,֮��������imuTime��Ĳ�
        double dt = (lastImuT_imu < 0) ? (1.0 / 500.0) : (imuTime - lastImuT_imu);
        lastImuT_imu = imuTime;
 
        // integrate this single imu message
        // imuԤ���������һ֡imu���ݣ�ע�����Ԥ����������ʼʱ������һ֡������̼�ʱ��
        imuIntegratorImu_->integrateMeasurement(gtsam::Vector3(thisImu.linear_acceleration.x, thisImu.linear_acceleration.y, thisImu.linear_acceleration.z),
                                                gtsam::Vector3(thisImu.angular_velocity.x,    thisImu.angular_velocity.y,    thisImu.angular_velocity.z), dt);
 
        // predict odometry
        // ����һ֡������̼�ʱ�̶�Ӧ��״̬��ƫ�ã�ʩ�ӴӸ�ʱ�̿�ʼ����ǰʱ�̵�imuԤ�Ʒ������õ���ǰʱ�̵�״̬
        gtsam::NavState currentState = imuIntegratorImu_->predict(prevStateOdom, prevBiasOdom);
 
        // publish odometry
        // ����imu��̼ƣ�ת��lidarϵ���뼤����̼�ͬһ��ϵ��
        nav_msgs::Odometry odometry;
        odometry.header.stamp = thisImu.header.stamp;
        odometry.header.frame_id = odometryFrame; //"odom"
        odometry.child_frame_id = "odom_imu";
 
        // transform imu pose to ldiar
        //Ԥ��ֵcurrentState���imuλ��, ����imu���״�任, ����״�λ��
        gtsam::Pose3 imuPose = gtsam::Pose3(currentState.quaternion(), currentState.position());
        gtsam::Pose3 lidarPose = imuPose.compose(imu2Lidar);
        // ���������ʣ���cpp����󲹳䣺
        // ΪʲôcurrentState��õ���imu��λ�ˣ�ԭʼimu�����ѵ�������ת�����״�����ϵ�µ����ݣ�this imu�������͵�imuԤ����������
        //�� ��֮ǰ���Ż�����odometryHandler�У�thisIMU��ֱ�Ӵ�imuQueOpt��ȡֵ.
        //��imuQueOpt�е����ݣ����Ѿ���imuԭʼ��������ת������lidar"�м�ϵ"ϵ�£��ڱ������ڶ��У������������״�ϵ������һ��ƽ��
        //odometryHandler�����и���prevPose_ = lidarPose.compose(lidar2Imu)�õ�����֡����λ��(lidarPose)ת����imuϵ��,���൱�ڴ��������״�ϵŤ��������м�ϵ�У�
        //��Ϊ��ֵ���������ӽ����Ż���
        //����imuIntegratorOpt_->integrateMeasurement�еõ���Ӧ����dt֮���Ԥ��������
        //�����䴦��ѭ���У�������ڵ����ۻ�������֮֡���Ԥ��������
        //���൱����ÿ��һ֡���ͰѶ���֮���Ԥ����������һ�飬�����Ż�һ�飬�洢��imuIntegratorOpt_���С�
 
        //��Ϊ������Ϊimu�ص�������ÿ�յ�һ��imu���ݣ���֮ǰ����ͼ���������£�
        //��imuIntegratorImu_�Ļ����ϼ����������յ���imu���ݣ����ҽ���Ԥ�⡣
        //����imu��ת��lidarϵ�½��з�����
        //ע�⣺���﷢��������֮֡��ġ�������imu��̼���Ϣ��
        //imuIntegratorImu_�����Ǹ���������ֻ����֮֡���Ԥ���֣����Ƿ�����ʱ�򷢲���ʵ���ǽ����ǰ����̼Ʊ����е�λ��
        //�����predict���prevStateOdom:
        //currentState = imuIntegratorImu_->predict(prevStateOdom, prevBiasOdom);
 
        //����imuIntegratorImu_�������ص������ж�����integrateMeasurement������֮���Ƿ���г�ͻ�أ�
        //�Ҿ��ùؼ�����odometryHandler����һ�䣺imuIntegratorImu_->resetIntegrationAndSetBias(prevBiasOdom)��
        //��imuIntegratorOpt_�Ż�����֡imu����֮��imuIntegratorImu_ֱ�Ӱѻ��ֺ�bias��reset����
        //Ȼ��ʼ����imuIntegratorOpt_�Ż�����bias������imuIntegratorImu_��
 
        //imuIntegratorImu_��imuIntegratorOpt_���������ڣ�opt�洢�����µ�һ֡������һ֮֡���Ԥ����������ΪԼ����ִ���Ż���
        //�Ż���imuIntegratorImu_�����µ�bias�����µ�����һ֡�Ļ����ϣ����ơ�֮�󡱵�Ԥ��������
        //�����Բ��ܰ�imuIntegratorOpt_��imuIntegratorImu_���Ϊͬһ��imu�������Ż�ǰ��Ĳ�ֵͬ��
 
 
 
        //�ڸ��µĹ����в��õ��ĻᱻimuHandler�е�imuIntegratorImu_->integrateMeasurement��������
        //������ΪimuHandlerҪ����doneFirstOpt�����odometryHandler�ǲ����Ѿ�������bias�ˡ�
        //��Ϊ���²�����ʵʱ�ģ�����һ֡�������ݵ��˲Ÿ��¡�
        //������һ֡���Ⲣû�е��������ڴ��ڼ�imu����ʽ��̼ƻ��ǵ��ճ�������
        //�����ǰ֡�Ѿ�������bias�ˣ�Ȼ��Ϳ���ֱ���������bias��������µ���ImuIntegratorImu_��
 
        odometry.pose.pose.position.x = lidarPose.translation().x();
        odometry.pose.pose.position.y = lidarPose.translation().y();
        odometry.pose.pose.position.z = lidarPose.translation().z();
        odometry.pose.pose.orientation.x = lidarPose.rotation().toQuaternion().x();
        odometry.pose.pose.orientation.y = lidarPose.rotation().toQuaternion().y();
        odometry.pose.pose.orientation.z = lidarPose.rotation().toQuaternion().z();
        odometry.pose.pose.orientation.w = lidarPose.rotation().toQuaternion().w();
        
        odometry.twist.twist.linear.x = currentState.velocity().x();
        odometry.twist.twist.linear.y = currentState.velocity().y();
        odometry.twist.twist.linear.z = currentState.velocity().z();
        odometry.twist.twist.angular.x = thisImu.angular_velocity.x + prevBiasOdom.gyroscope().x();
        odometry.twist.twist.angular.y = thisImu.angular_velocity.y + prevBiasOdom.gyroscope().y();
        odometry.twist.twist.angular.z = thisImu.angular_velocity.z + prevBiasOdom.gyroscope().z();
        pubImuOdometry.publish(odometry);
    }
};
//�����������⣺
//1.��һ���ǣ�Ϊʲôimuԭʼ������Ҫ����imuConverter�䵽lidarϵ��
//��ô֮��imuintegrator->integrateMeasurement�㵽��Ԥ�������ݲ�����lidarϵ�µ���
//�ڴ����ʱ�����ǰ�lidar��̼Ƶ�����ϵ����compose�����䵽imuϵ�����ѵ����ǲ���Ӧ����:
 
//2.����gtsam::Pose3 imu2Lidar = gtsam::Pose3(gtsam::Rot3(1, 0, 0, 0),
//gtsam::Point3(-extTrans.x(), -extTrans.y(), -extTrans.z()));
//����Ϊʲô���������ļ��е�extRot��extQRPY֮��������أ���ֻ������extTrans���ݣ�
 
//������㣬����github���ҵ��˽���:https://github.com/TixiaoShan/LIO-SAM/issues/30,
//imuConverter() to align the axis of the two coordinates,��û���漰��ƽ��,
//lidar2Imu or imu2Lidar ȴֻ��ƽ�Ƶ�����
//����յ�imu������imuConverter()ת�����״�ϵ�£�������ʵ���״�֮����Ȼ����һ��ƽ�ƣ���
//����ְ��״��������ֻ����ƽ�Ƶ�lidar2Imu ��ԭ������һ��ƽ�Ƶ�imu������������
//���൱����imu��ת���״�ϵ���Ժ�ƽ�ƣ�Ȼ����״ﵹ��ƽ�ƹ���,��һ�����м�ϵ�����룩��
//�������Ժ󣬵ȷ�����ʱ������imu2Lidar�ֵ��ص��������˾����״�ϵ��
 
//��ôtixiaoshanΪʲô��Ĭ�����ƽ�Ʋ�������Ϊ0��0��0��
//����github�еĽ���Ϊ: ���ڲ�ͬ�����ݼ��иı��˼���IMU�İ�װλ�á�����λ�����ǿ��������״
//����ÿ�β��Բ�ͬ�����ݼ�ʱ���Ҷ����ط���ȥ�޸�����������ϸ��˵���ҵķ����������롣��Ҫ�ṩ�˲����Ի�ø��õ����ܡ�
int main(int argc, char** argv)
{
    ros::init(argc, argv, "roboat_loam");
    
    IMUPreintegration ImuP;
 
    TransformFusion TF;
 
    ROS_INFO("\033[1;32m----> IMU Preintegration Started.\033[0m");
    
    ros::MultiThreadedSpinner spinner(4);
    spinner.spin();
    
    return 0;
}
