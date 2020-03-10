//
// Created by ljn on 20-3-10.
//

#include "path_optimizer/solver/solver_kp_as_input.hpp"
#include "path_optimizer/data_struct/data_struct.hpp"
#include "path_optimizer/config/config.hpp"
#include "path_optimizer/tools/tools.hpp"

namespace PathOptimizationNS {
SolverKpAsInput::SolverKpAsInput(const Config &config,
                                 const ReferencePath &reference_path,
                                 const VehicleState &vehicle_state,
                                 const size_t &horizon) :
    OsqpSolver(config, reference_path, vehicle_state, horizon),
    keep_control_steps_(4), // TODO: adjust this.
    control_horizon_((horizon_ + keep_control_steps_ - 2) / keep_control_steps_),
    state_size_(3 * horizon_),
    control_size_(control_horizon_),
    slack_size_(horizon_) {
    std::cout << "optimization horizon: " << horizon_ << std::endl;
    std::cout << "control horizon: " << control_horizon_ << std::endl;
}

void SolverKpAsInput::setHessianMatrix(Eigen::SparseMatrix<double> *matrix_h) const {


    const size_t matrix_size = state_size_ + control_size_ + slack_size_;
    Eigen::MatrixXd hessian{Eigen::MatrixXd::Constant(matrix_size, matrix_size, 0)};
    double w_c = config_.opt_curvature_w_;
    double w_cr = config_.opt_curvature_rate_w_;
    double w_pq = config_.opt_deviation_w_;
    double w_e = config_.opt_slack_w_;
    for (size_t i = 0; i != horizon_; ++i) {
        hessian(3 * i, 3 * i) += w_pq;
        hessian(3 * i + 2, 3 * i + 2) += w_c;
        hessian(state_size_ + control_size_ + i, state_size_ + control_size_ + i) += w_e;
    }
    for (size_t i = 0; i != control_horizon_; ++i) {
        hessian(state_size_ + i, state_size_ + i) += keep_control_steps_ * w_cr;
    }
    *matrix_h = hessian.sparseView();
}

void SolverKpAsInput::setConstraintMatrix(Eigen::SparseMatrix<double> *matrix_constraints,
                                          Eigen::VectorXd *lower_bound,
                                          Eigen::VectorXd *upper_bound) const {
    const auto &seg_s_list = reference_path_.seg_s_list_;
    const auto &seg_k_list = reference_path_.seg_k_list_;
    const auto &seg_angle_list = reference_path_.seg_angle_list_;
    const auto &seg_clearance_list = reference_path_.seg_clearance_list_;
    const size_t trans_range_begin{0};
    const size_t vars_range_begin{trans_range_begin + 3 * horizon_};
    const size_t collision_range_begin{vars_range_begin + 2 * horizon_ + control_horizon_};
    const size_t end_state_range_begin{collision_range_begin + 5 * horizon_};
    Eigen::MatrixXd cons = Eigen::MatrixXd::Zero(10 * horizon_ + control_horizon_ + 2, 4 * horizon_ + control_horizon_);
    // Set transition part.
    for (size_t i = 0; i != state_size_; ++i) {
        cons(i, i) = -1;
    }
    Eigen::Matrix3d a(Eigen::Matrix3d::Zero());
    a(0, 1) = 1;
    a(1, 2) = 1;
    Eigen::Matrix<double, 3, 1> b(Eigen::MatrixXd::Constant(3, 1, 0));
    b(2, 0) = 1;
    std::vector<Eigen::MatrixXd> c_list;
    for (size_t i = 0; i != horizon_ - 1; ++i) {
        const auto ref_k{seg_k_list[i]};
        const auto ds{seg_s_list[i + 1] - seg_s_list[i]};
        const auto ref_kp{(seg_k_list[i + 1] - ref_k) / ds};
        a(1, 0) = -pow(ref_k, 2);
        auto A{a * ds + Eigen::Matrix3d::Identity()};
        auto B{b * ds};
        cons.block(3 * (i + 1), 3 * i, 3, 3) = A;
        size_t control_index{i / keep_control_steps_};
        cons.block(3 * (i + 1), state_size_ + control_index, 3, 1) = B;
        Eigen::Matrix<double, 3, 1> c, ref_state;
        c << 0, 0, ref_kp;
        ref_state << 0, 0, ref_k;
        c_list.emplace_back(ds * (c - a * ref_state - b * ref_kp));
    }

    // Set vars part.
    for (size_t i = 0; i != horizon_; ++i) {
        cons(vars_range_begin + i, 3 * i + 2) = 1;
        cons(vars_range_begin + horizon_ + control_horizon_ + i, state_size_ + control_size_ + i) = 1;
    }
    for (size_t i = 0; i != control_size_; ++i) {
        cons(vars_range_begin + horizon_ + i, state_size_ + i) = 1;
    }

    // Set collision part.
    Eigen::Matrix<double, 3, 2> collision;
    collision << 1, config_.d1_ + config_.rear_axle_to_center_distance_,
//        config_.d2_ + config_.rear_axle_to_center_distance_, 1,
        1, config_.d3_ + config_.rear_axle_to_center_distance_,
        1, config_.d4_ + config_.rear_axle_to_center_distance_;
    for (size_t i = 0; i != horizon_; ++i) {
        cons.block(collision_range_begin + 3 * i, 3 * i, 3, 2) = collision;
    }
    Eigen::Matrix<double, 1, 2> collision1;
    collision1 << 1, config_.d2_ + config_.rear_axle_to_center_distance_;
    for (size_t i = 0; i != horizon_; ++i) {
        cons.block(collision_range_begin + 3 * horizon_ + i, 3 * i, 1, 2) = collision1;
        cons(collision_range_begin + 3 * horizon_ + i, state_size_ + control_size_ + i) = -1;
        cons.block(collision_range_begin + 4 * horizon_ + i, 3 * i, 1, 2) = collision1;
        cons(collision_range_begin + 4 * horizon_ + i, state_size_ + control_size_ + i) = 1;
    }

    // End state.
    cons(end_state_range_begin, state_size_ - 3) = 1; // end ey
    cons(end_state_range_begin + 1, state_size_ - 2) = 1; // end ephi
    *matrix_constraints = cons.sparseView();

    // Set bounds.
    *lower_bound = Eigen::MatrixXd::Zero(10 * horizon_ + control_horizon_ + 2, 1);
    *upper_bound = Eigen::MatrixXd::Zero(10 * horizon_ + control_horizon_ + 2, 1);
    Eigen::Matrix<double, 3, 1> x0;
    x0 << vehicle_state_.initial_offset_, vehicle_state_.initial_heading_error_, vehicle_state_.start_state_.k;
    lower_bound->block(0, 0, 3, 1) = -x0;
    upper_bound->block(0, 0, 3, 1) = -x0;
    for (size_t i = 0; i != horizon_ - 1; ++i) {
        lower_bound->block(3 * (i + 1), 0, 3, 1) = -c_list[i];
        upper_bound->block(3 * (i + 1), 0, 3, 1) = -c_list[i];
    }

    // Vars bound.
    for (size_t i = 0; i != horizon_; ++i) {
        (*lower_bound)(vars_range_begin + i) = -tan(config_.max_steer_angle_) / config_.wheel_base_;
        (*upper_bound)(vars_range_begin + i) = tan(config_.max_steer_angle_) / config_.wheel_base_;
        (*lower_bound)(vars_range_begin + horizon_ + control_horizon_ + i) = 0;
        (*upper_bound)(vars_range_begin + horizon_ + control_horizon_ + i) = config_.expected_safety_margin_;
    }
    for (size_t i = 0; i != control_horizon_; ++i) {
        (*lower_bound)(vars_range_begin + horizon_ + i) = -OsqpEigen::INFTY;
        (*upper_bound)(vars_range_begin + horizon_ + i) = OsqpEigen::INFTY;
    }

    // Collision bound.
    for (size_t i = 0; i != horizon_; ++i) {
        Eigen::Vector3d ld, ud;
        ud
            << seg_clearance_list[i][0], seg_clearance_list[i][4], seg_clearance_list[i][6];
        ld
            << seg_clearance_list[i][1], seg_clearance_list[i][5], seg_clearance_list[i][7];
        lower_bound->block(collision_range_begin + 3 * i, 0, 3, 1) = ld;
        upper_bound->block(collision_range_begin + 3 * i, 0, 3, 1) = ud;
        double uds = seg_clearance_list[i][2] - config_.expected_safety_margin_;
        double lds = seg_clearance_list[i][3] + config_.expected_safety_margin_;
        (*upper_bound)(collision_range_begin + 3 * horizon_ + i, 0) = uds;
        (*lower_bound)(collision_range_begin + 3 * horizon_ + i, 0) = -OsqpEigen::INFTY;
        (*lower_bound)(collision_range_begin + 4 * horizon_ + i, 0) = lds;
        (*upper_bound)(collision_range_begin + 4 * horizon_ + i, 0) = OsqpEigen::INFTY;
    }

    // End state.
    // End ey is not constrained.
    (*lower_bound)(end_state_range_begin) = -OsqpEigen::INFTY;
    (*upper_bound)(end_state_range_begin) = OsqpEigen::INFTY;
    (*lower_bound)(end_state_range_begin + 1) = -OsqpEigen::INFTY;
    (*upper_bound)(end_state_range_begin + 1) = OsqpEigen::INFTY;
    if (config_.constraint_end_heading_) {
        double end_psi = constraintAngle(vehicle_state_.end_state_.z - seg_angle_list.back());
        if (end_psi < 70 * M_PI / 180) {
            (*lower_bound)(end_state_range_begin + 1) = end_psi - 5 * M_PI / 180;
            (*upper_bound)(end_state_range_begin + 1) = end_psi + 5 * M_PI / 180;
        }
    }

}

bool SolverKpAsInput::solve(std::vector<PathOptimizationNS::State> *optimized_path) {
    solver_.settings()->setVerbosity(false);
    solver_.settings()->setWarmStart(true);
    solver_.data()->setNumberOfVariables(state_size_ + control_size_ + slack_size_);
    solver_.data()->setNumberOfConstraints(10 * horizon_ + control_horizon_ + 2);
    // Allocate QP problem matrices and vectors.
    Eigen::SparseMatrix<double> hessian;
    Eigen::VectorXd gradient = Eigen::VectorXd::Zero(state_size_ + control_size_ + slack_size_);
    Eigen::SparseMatrix<double> linearMatrix;
    Eigen::VectorXd lowerBound;
    Eigen::VectorXd upperBound;
    // Set Hessian matrix.
    setHessianMatrix(&hessian);
    // Set state transition matrix, constraint matrix and bound vector.
    setConstraintMatrix(
        &linearMatrix,
        &lowerBound,
        &upperBound);
    // Input to solver.
    if (!solver_.data()->setHessianMatrix(hessian)) return false;
    if (!solver_.data()->setGradient(gradient)) return false;
    if (!solver_.data()->setLinearConstraintsMatrix(linearMatrix)) return false;
    if (!solver_.data()->setLowerBound(lowerBound)) return false;
    if (!solver_.data()->setUpperBound(upperBound)) return false;
    // Solve.
    if (!solver_.initSolver()) return false;
    if (!solver_.solve()) return false;
    const auto &QPSolution = solver_.getSolution();
    optimized_path->clear();
    double tmp_s = 0;
    for (size_t i = 0; i != horizon_; ++i) {
        double length_on_ref_path = reference_path_.seg_s_list_[i];
        double angle = reference_path_.seg_angle_list_[i];
        double new_angle = constraintAngle(angle + M_PI_2);
        double tmp_x = reference_path_.x_s_(length_on_ref_path) + QPSolution(3 * i) * cos(new_angle);
        double tmp_y = reference_path_.y_s_(length_on_ref_path) + QPSolution(3 * i) * sin(new_angle);
        double k = QPSolution(3 * i + 2);
        if (i != 0) {
            tmp_s += sqrt(pow(tmp_x - optimized_path->back().x, 2) + pow(tmp_y - optimized_path->back().y, 2));
        }
        optimized_path->emplace_back(tmp_x, tmp_y, angle + QPSolution(3 * i + 1), k, tmp_s);
    }
    return true;
}

}