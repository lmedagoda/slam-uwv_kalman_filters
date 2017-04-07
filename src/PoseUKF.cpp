#include "PoseUKF.hpp"
#include <base/Float.hpp>
#include <base-logging/Logging.hpp>
#include <uwv_dynamic_model/DynamicModel.hpp>
#include <pose_estimation/GravitationalModel.hpp>
#include <pose_estimation/GeographicProjection.hpp>

using namespace uwv_kalman_filters;

// process model
template <typename FilterState>
FilterState
processModel(const FilterState &state, const Eigen::Vector3d& rotation_rate,
             boost::shared_ptr<pose_estimation::GeographicProjection> projection,
             double gyro_bias_tau, double acc_bias_tau, double delta_time)
{
    FilterState new_state(state);

    // apply velocity
    new_state.position.boxplus(state.velocity, delta_time);

    // apply angular velocity
    double latitude, longitude;
    projection->navToWorld(state.position.x(), state.position.y(), latitude, longitude);
    Eigen::Vector3d earth_rotation = Eigen::Vector3d(pose_estimation::EARTHW * cos(latitude), 0., pose_estimation::EARTHW * sin(latitude));
    Eigen::Vector3d angular_velocity = state.orientation * (rotation_rate - state.bias_gyro) - earth_rotation;
    new_state.orientation.boxplus(angular_velocity, delta_time);

    // apply acceleration
    Eigen::Vector3d acceleration = state.orientation * state.acceleration;
    new_state.velocity.boxplus(acceleration, delta_time);

    Eigen::Vector3d gyro_bias_delta = (-1.0/gyro_bias_tau) * state.bias_gyro;
    new_state.bias_gyro.boxplus(gyro_bias_delta, delta_time);

    Eigen::Vector3d acc_bias_delta = (-1.0/acc_bias_tau) * state.bias_acc;
    new_state.bias_acc.boxplus(acc_bias_delta, delta_time);

    return new_state;
}

// measurement models
template <typename FilterState>
Eigen::Matrix<TranslationType::scalar, 2, 1>
measurementXYPosition(const FilterState &state)
{
    return state.position.block(0,0,2,1);
}

template <typename FilterState>
Eigen::Matrix<TranslationType::scalar, 1, 1>
measurementZPosition(const FilterState &state)
{
    return state.position.block(2,0,1,1);
}

template <typename FilterState>
VelocityType
measurementVelocity(const FilterState &state, const Eigen::Quaterniond& orientation)
{
    // return expected velocities in the IMU frame
    return VelocityType(orientation.inverse() * state.velocity);
}

template <typename FilterState>
AccelerationType
measurementAcceleration(const FilterState &state)
{
    // returns expected accelerations in the IMU frame
    base::Quaterniond orientation = base::removeYaw(state.orientation);
    return AccelerationType(state.acceleration + state.bias_acc + orientation.inverse() * Eigen::Vector3d(0., 0., state.gravity(0)));
}

template <typename FilterState>
Eigen::Matrix<TranslationType::scalar, 3, 1>
measurementEfforts(const FilterState &state, boost::shared_ptr<uwv_dynamic_model::DynamicModel> dynamic_model,
                   const Eigen::Vector3d& imu_in_body, const Eigen::Vector3d& rotation_rate,
                   boost::shared_ptr<pose_estimation::GeographicProjection> projection)
{
    RotationType::base orientation_inv = state.orientation.inverse();

    double latitude, longitude;
    projection->navToWorld(state.position.x(), state.position.y(), latitude, longitude);
    Eigen::Vector3d earth_rotation = Eigen::Vector3d(pose_estimation::EARTHW * cos(latitude), 0., pose_estimation::EARTHW * sin(latitude));

    // for the rotation rate IMU and body frame a the same, since they are not rotated to each other
    Eigen::Vector3d rotation_rate_body = rotation_rate - state.bias_gyro - orientation_inv * earth_rotation;
    // assume center of rotation to be the body frame
    Eigen::Vector3d velocity_body = orientation_inv * state.velocity - rotation_rate_body.cross(imu_in_body);
    base::Vector6d velocity_6d;
    velocity_6d << velocity_body, rotation_rate_body;

    // assume center of rotation to be the body frame
    Eigen::Vector3d acceleration_body = Eigen::Vector3d(state.acceleration) - rotation_rate_body.cross(rotation_rate_body.cross(imu_in_body));
    base::Vector6d acceleration_6d;
    // assume the angular acceleration to be zero
    acceleration_6d << acceleration_body, base::Vector3d::Zero();

    base::Vector6d efforts = dynamic_model->calcEfforts(acceleration_6d, velocity_6d, state.orientation);

    // returns the expected linear body efforts given the current state
    return efforts.block(0,0,3,1);
}

PoseUKF::PoseUKF(const State& initial_state, const Covariance& state_cov,
                const LocationConfiguration& location, const uwv_dynamic_model::UWVParameters& model_parameters,
                const Eigen::Vector3d& imu_in_body, double gyro_bias_tau, double acc_bias_tau) :
                    imu_in_body(imu_in_body), gyro_bias_tau(gyro_bias_tau), acc_bias_tau(acc_bias_tau)
{
    initializeFilter(initial_state, state_cov);

    rotation_rate = RotationRate::Mu::Zero();

    dynamic_model.reset(new uwv_dynamic_model::DynamicModel());
    dynamic_model->setUWVParameters(model_parameters);

    projection.reset(new pose_estimation::GeographicProjection(location.latitude, location.longitude));
}


void PoseUKF::predictionStepImpl(double delta_t)
{
    Eigen::Matrix3d rot = ukf->mu().orientation.matrix();
    Covariance process_noise = process_noise_cov;
    // uncertainty matrix calculations
    process_noise.block(3,3,3,3) = rot * process_noise_cov.block(3,3,3,3) * rot.transpose();
    process_noise.block(6,6,3,3) = rot * process_noise_cov.block(6,6,3,3) * rot.transpose();
    process_noise = pow(delta_t, 2.) * process_noise;

    ukf->predict(boost::bind(processModel<WState>, _1, rotation_rate, projection, gyro_bias_tau, acc_bias_tau, delta_t),
                 MTK_UKF::cov(process_noise));
}

void PoseUKF::integrateMeasurement(const Velocity& velocity)
{
    checkMeasurment(velocity.mu, velocity.cov);
    ukf->update(velocity.mu, boost::bind(measurementVelocity<State>, _1, ukf->mu().orientation),
                boost::bind(ukfom::id< Velocity::Cov >, velocity.cov),
                ukfom::accept_any_mahalanobis_distance<State::scalar>);
}

void PoseUKF::integrateMeasurement(const Acceleration& acceleration)
{
    checkMeasurment(acceleration.mu, acceleration.cov);
    ukf->update(acceleration.mu, boost::bind(measurementAcceleration<State>, _1),
                boost::bind(ukfom::id< Acceleration::Cov >, acceleration.cov),
                ukfom::accept_any_mahalanobis_distance<State::scalar>);
}

void PoseUKF::integrateMeasurement(const RotationRate& rotation_rate)
{
    checkMeasurment(rotation_rate.mu, rotation_rate.cov);
    this->rotation_rate = rotation_rate.mu;
}

void PoseUKF::integrateMeasurement(const Z_Position& z_position)
{
    checkMeasurment(z_position.mu, z_position.cov);
    ukf->update(z_position.mu, boost::bind(measurementZPosition<State>, _1),
                boost::bind(ukfom::id< Z_Position::Cov >, z_position.cov),
                ukfom::accept_any_mahalanobis_distance<State::scalar>);
}

void PoseUKF::integrateMeasurement(const XY_Position& xy_position)
{
    checkMeasurment(xy_position.mu, xy_position.cov);
    ukf->update(xy_position.mu, boost::bind(measurementXYPosition<State>, _1),
                boost::bind(ukfom::id< XY_Position::Cov >, xy_position.cov),
                ukfom::accept_any_mahalanobis_distance<State::scalar>);
}

void PoseUKF::integrateMeasurement(const GeographicPosition& geo_position, const Eigen::Vector3d& gps_in_body)
{
    checkMeasurment(geo_position.mu, geo_position.cov);

    // project geographic position to local NWU plane
    Eigen::Matrix<TranslationType::scalar, 2, 1> projected_position;
    projection->worldToNav(geo_position.mu.x(), geo_position.mu.y(), projected_position.x(), projected_position.y());
    projected_position = projected_position - (ukf->mu().orientation * gps_in_body).head<2>();

    ukf->update(projected_position, boost::bind(measurementXYPosition<State>, _1),
                boost::bind(ukfom::id< XY_Position::Cov >, geo_position.cov),
                ukfom::accept_any_mahalanobis_distance<State::scalar>);
}

void PoseUKF::integrateMeasurement(const BodyEffortsMeasurement& body_efforts)
{
    checkMeasurment(body_efforts.mu, body_efforts.cov);
    ukf->update(body_efforts.mu, boost::bind(measurementEfforts<State>, _1, dynamic_model, imu_in_body, rotation_rate, projection),
                boost::bind(ukfom::id< BodyEffortsMeasurement::Cov >, body_efforts.cov),
                ukfom::accept_any_mahalanobis_distance<State::scalar>);
}

PoseUKF::RotationRate::Mu PoseUKF::getRotationRate()
{
    double latitude, longitude;
    projection->navToWorld(ukf->mu().position.x(), ukf->mu().position.y(), latitude, longitude);
    Eigen::Vector3d earth_rotation = Eigen::Vector3d(pose_estimation::EARTHW * cos(latitude), 0., pose_estimation::EARTHW * sin(latitude));
    return rotation_rate - ukf->mu().bias_gyro - ukf->mu().orientation.inverse() * earth_rotation;
}