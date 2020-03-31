#pragma once

// EIGEN
#include <Eigen/Dense>
#include <Eigen/Geometry>

// CPLEX
#include <ilcplex/ilocplex.h>

// Submodules
#include <rbp_corridor.hpp>
#include <init_traj_planner.hpp>
#include <mission.hpp>
#include <param.hpp>
#include <plan_result.hpp>

ILOSTLBEGIN

namespace SwarmPlanning {
    class RBPPlanner {
    public:
        RBPPlanner(Mission _mission,
                   Param _param)
                : mission(std::move(_mission)),
                  param(std::move(_param)) {
            n = param.n; // degree of polynomial
            phi = param.phi; // desired derivatives
            N = mission.qn; // the number of agents
            outdim = 3; // the number of outputs (x,y,z)

            setBatch(0);
        }

        bool update(bool log, SwarmPlanning::PlanResult* _planResult_ptr) {
            planResult_ptr = _planResult_ptr;
            offset_seg = n + 1;

            M.resize(N);
            planResult_ptr->coef.resize(N);
            for (int qi = 0; qi < N; qi++) {
                M[qi] = planResult_ptr->T[qi].size() - 1; // the number of segments
                planResult_ptr->coef[qi] = Eigen::MatrixXd::Zero(M[qi] * offset_seg, outdim);
            }

            IloEnv env;
            Timer timer;
            try {
                // Construct constraint matrix
                timer.reset();
                buildConstMtx();
                timer.stop();
                ROS_INFO_STREAM("RBPPlanner: Constraint Matrix runtime=" << timer.elapsedSeconds());

                // Solve QP
                timer.reset();
                solveQP(env, log);
                timer.stop();
                ROS_INFO_STREAM("RBPPlanner: QP runtime=" << timer.elapsedSeconds());
                ROS_INFO_STREAM("RBPPlanner: x size=" << count_x);
                ROS_INFO_STREAM("RBPPlanner: eq const size=" << count_eq);
                ROS_INFO_STREAM("RBPPlanner: ineq const size=" << count_lq);
            }
            catch (IloException &e) {
                ROS_ERROR_STREAM("RBPPlanner: CPLEX Concert exception caught: " << e);
                return false;
            }
            catch (...) {
                ROS_ERROR("RBPPlanner: CPLEX Unknown exception caught");
                return false;
            }
            env.end();

            if(param.time_scale) {
                timer.reset();
                timeScale();
                timer.stop();
                ROS_INFO_STREAM("RBPPlanner: timeScale runtime=" << timer.elapsedSeconds());
            }

            if(param.log){
                generateCoefCSV();
            }

            planResult_ptr->state = OPTIMIZATION;
            return true;
        }

    private:
        Mission mission;
        Param param;

        SwarmPlanning::PlanResult* planResult_ptr;
        std::vector<std::vector<int>> batches;
        std::vector<int> M;
        int n, phi, N, outdim, offset_seg;
        IloNum count_x, count_eq, count_lq;

        // std::shared_ptr<Eigen::MatrixXd> Q_obj, Aeq_obj, Alq_obj, deq_obj, dlq_obj;
        Eigen::MatrixXd Q_base, basis;
        std::vector<Eigen::MatrixXd> Aeq_base, deq, dlq_box, dummy;

        void buildConstMtx() {
            build_Q_base();
            build_Aeq_base();
            build_deq();
            build_dlq_box();

            if (param.sequential) {
                build_dummy();
            }
        }

        void solveQP(const IloEnv &env, bool log) {
            Timer timer;
            IloNum total_cost = 0;

            IloCplex cplex(env);
//            cplex.setParam(IloCplex::Param::TimeLimit, 0.04);

            // publish dummy agents' trajectory
            if(param.sequential && param.batch_iter == 0){
                // Translate Bernstein basis to Polynomial coefficients
                for (int k = 0; k < outdim; k++) {
                    for (int qi = 0; qi < N; qi++) {
                        for (int m = 0; m < M[qi]; m++) {
                            Eigen::MatrixXd c = Eigen::MatrixXd::Zero(1, n + 1);
                            Eigen::MatrixXd tm;
                            timeMatrix(1.0 / (planResult_ptr->T[qi][m+1] - planResult_ptr->T[qi][m]), &tm);
                            tm = basis * tm;

                            for (int i = 0; i < n + 1; i++) {
                                c = c + dummy[qi](m * offset_seg + i, k) * tm.row(i);
                            }
                            planResult_ptr->coef[qi].block(m * offset_seg, k, n + 1, 1) = c.transpose();
                        }
                    }
                    timer.stop();
                }
                return;
            }

            for (int iter = 0; iter < param.iteration; iter++) {
                total_cost = 0;
                for (int l = 0; l < param.batch_iter; l++) {
                    timer.reset();
                    IloModel model(env);
                    IloNumVarArray var(env);
                    IloRangeArray con(env);

                    populatebyrow(model, var, con, l);
                    cplex.extract(model);
                    if (log) {
                        std::string QPmodel_path = param.package_path + "/log/QPmodel.lp";
                        cplex.exportModel(QPmodel_path.c_str());
                    } else {
                        cplex.setOut(env.getNullStream());
                    }

                    // Optimize the problem and obtain solution.
                    if (!cplex.solve()) {
                        ROS_ERROR("RBPPlanner: Failed to optimize QP");
                        throw (-1);
                    }

                    IloNumArray vals(env);
                    total_cost += cplex.getObjValue();
                    cplex.getValues(vals, var);

                    // Translate Bernstein basis to Polynomial coefficients
                    int offset_dim = getOffset_dim(l);
                    int batch_max_iter = ceil((double)N / (double)param.batch_size);
                    for (int k = 0; k < outdim; k++) {
                        for (int qi = 0; qi < N; qi++) {
                            for (int m = 0; m < M[qi]; m++) {
                                Eigen::MatrixXd c = Eigen::MatrixXd::Zero(1, n + 1);
                                Eigen::MatrixXd tm;
                                timeMatrix(1.0 / (planResult_ptr->T[qi][m+1] - planResult_ptr->T[qi][m]), &tm);
                                tm = basis * tm;

                                int bi = isQuadInBatch(qi, l);
                                int offset_quad = getOffset_quad(l, bi);
                                if (bi >= 0) {
                                    for (int i = 0; i < n + 1; i++) {
                                        c = c + vals[k * offset_dim + offset_quad + m * offset_seg + i] * tm.row(i);
                                        if (param.sequential) {
                                            dummy[qi](m * offset_seg + i, k) = vals[k * offset_dim + offset_quad + m * offset_seg + i];
                                        }
                                    }
                                    planResult_ptr->coef[qi].block(m * offset_seg, k, n + 1, 1) = c.transpose();
                                } else if (param.sequential && param.batch_iter < batch_max_iter) {
                                    for (int i = 0; i < n + 1; i++) {
                                        c = c + dummy[qi](m * offset_seg + i, k) * tm.row(i);
                                    }
                                    planResult_ptr->coef[qi].block(m * offset_seg, k, n + 1, 1) = c.transpose();
                                }
                            }
                        }
                        timer.stop();
                    }
                    if (param.sequential) {
                        ROS_INFO_STREAM("RBPPlanner: QP runtime of batch " << l << "=" << timer.elapsedSeconds());
                        ROS_INFO_STREAM("RBPPlanner: QP cost of batch " << l << "=" << cplex.getObjValue());
                    }
                }
                if (param.iteration > 1)
                    ROS_INFO_STREAM("RBPPlanner: QP iteration " << iter << " total_cost=" << total_cost);
            }
            ROS_INFO_STREAM("RBPPlanner: QP total cost=" << total_cost);
        }

        // For all segment of trajectory, check maximum velocity and accelation, and scale the segment time
        void timeScale() {
            if (phi != 3 || n != 5) {
                return;
            }

            Eigen::MatrixXd coef_der;
            double time_scale, time_scale_tmp, acc_max;
            time_scale = 1;
            for (int qi = 0; qi < N; qi++) {
                for (int k = 0; k < outdim; k++) {
                    for (int m = 0; m < M[qi]; m++) {
                        derivative_segment(qi, k, m, &coef_der);

                        time_scale_tmp = scale_to_max_vel(qi, k, m, coef_der);
                        if (time_scale < time_scale_tmp) {
                            time_scale = time_scale_tmp;
                        }

                        time_scale_tmp = scale_to_max_acc(qi, k, m, coef_der);
                        if (time_scale < time_scale_tmp) {
                            time_scale = time_scale_tmp;
                        }
                    }
                }
            }

            ROS_INFO_STREAM("RBPPlanner: Time scale=" << time_scale);
            if (time_scale != 1) {
                for (int qi = 0; qi < N; qi++) {
                    // trajectory
                    for (int k = 0; k < outdim; k++) {
                        for (int m = 0; m < M[qi]; m++) {
                            Eigen::MatrixXd tm;
                            timeMatrix(1.0 / time_scale, &tm);

                            planResult_ptr->coef[qi].block(m * offset_seg, k, n + 1, 1) =
                                    tm * planResult_ptr->coef[qi].block(m * offset_seg, k, n + 1, 1);
                        }
                    }

                    // SFC
                    for (int bi = 0; bi < planResult_ptr->SFC[qi].size(); bi++){
                        planResult_ptr->SFC[qi][bi].start_time *= time_scale;
                        planResult_ptr->SFC[qi][bi].end_time *= time_scale;
                    }

                    // RSFC
                    for (int qj = qi + 1; qj < N; qj++){
                        for (int ri = 0; ri < planResult_ptr->RSFC[qi][qj].size(); ri++){
                            planResult_ptr->RSFC[qi][qj][ri].start_time *= time_scale;
                            planResult_ptr->RSFC[qi][qj][ri].end_time *= time_scale;
                        }
                    }

                    // segment time
                    for (int m = 0; m < M[qi] + 1; m++) {
                        planResult_ptr->T[qi][m] *= time_scale;
                    }
                }
            }
        }

        // translate coef to crazyswarm trajectory csv file
        // n should be smaller than 7
        void generateCoefCSV(){
            if(n > 7){
                ROS_WARN("RBPPlanner: n>7, do not make CSV file");
                return;
            }
            for(int qi = 0; qi < N; qi++) {
                std::ofstream coefCSV;
                coefCSV.open(param.package_path + "/log/coef" + std::to_string(qi + 1) + ".csv");
                coefCSV << "duration,x^0,x^1,x^2,x^3,x^4,x^5,x^6,x^7,y^0,y^1,y^2,y^3,y^4,y^5,y^6,y^7,z^0,z^1,z^2,z^3,z^4,z^5,z^6,z^7,yaw^0,yaw^1,yaw^2,yaw^3,yaw^4,yaw^5,yaw^6,yaw^7\n";
                for(int m = 0; m < M[qi]; m++) {
                    coefCSV << planResult_ptr->T[qi][m + 1] - planResult_ptr->T[qi][m] << ",";
                    // x,y,z
                    for (int k = 0; k < outdim; k++) {
                        for (int i = n; i >= 0; i--) {
                            coefCSV << planResult_ptr->coef[qi](m * offset_seg + i, k) << ",";
                        }
                        // to match
                        for(int i = 0; i < 7 - n; i++){
                            coefCSV << "0,";
                        }
                    }
                    // yaw
                    for(int i = 0; i < 8; i++){
                        coefCSV << "0,";
                    }
                    coefCSV << "\n";
                }
                coefCSV.close();
            }
        }

        // Cost matrix Q
        void build_Q_base() {
            if (phi == 3 && n == 5) {
                Q_base = Eigen::MatrixXd::Zero(n + 1, n + 1);
                Q_base << 720, -1800, 1200, 0, 0, -120,
                        -1800, 4800, -3600, 0, 600, 0,
                        1200, -3600, 3600, -1200, 0, 0,
                        0, 0, -1200, 3600, -3600, 1200,
                        0, 600, 0, -3600, 4800, -1800,
                        -120, 0, 0, 1200, -1800, 720;

                basis = Eigen::MatrixXd::Zero(n + 1, n + 1);
                basis << -1, 5, -10, 10, -5, 1,
                        5, -20, 30, -20, 5, 0,
                        -10, 30, -30, 10, 0, 0,
                        10, -20, 10, 0, 0, 0,
                        -5, 5, 0, 0, 0, 0,
                        1, 0, 0, 0, 0, 0;
            } else {
                ROS_ERROR("RBPPlanner: n should be 5"); //TODO: debug when n is not 5
            }
        }

        Eigen::MatrixXd build_Q_p(int qi, int m) {
            return Q_base * pow(planResult_ptr->T[qi][m + 1] - planResult_ptr->T[qi][m], -2 * phi + 1);
        }

        void build_Aeq_base() {
            // Build A_0, A_T
            Eigen::MatrixXd A_0 = Eigen::MatrixXd::Zero(n + 1, n + 1);
            Eigen::MatrixXd A_T = Eigen::MatrixXd::Zero(n + 1, n + 1);
            if (phi == 3 && n == 5) {
                A_0 << 1, 0, 0, 0, 0, 0,
                        -1, 1, 0, 0, 0, 0,
                        1, -2, 1, 0, 0, 0,
                        -1, 3, -3, 1, 0, 0,
                        1, -4, 6, -4, 1, 0,
                        -1, 5, -10, 10, -5, 1;

                A_T << 0, 0, 0, 0, 0, 1,
                        0, 0, 0, 0, -1, 1,
                        0, 0, 0, 1, -2, 1,
                        0, 0, -1, 3, -3, 1,
                        0, 1, -4, 6, -4, 1,
                        -1, 5, -10, 10, -5, 1;
            } else {
                ROS_ERROR("RBPPlanner: n should be 5"); //TODO: Compute A_0, A_T when n is not 5
            }

            Aeq_base.resize(N);
            for(int qi = 0; qi < N; qi++) {
                Aeq_base[qi] = Eigen::MatrixXd::Zero((2 * phi + (M[qi] - 1) * phi), M[qi] * (n + 1));
                Eigen::MatrixXd A_waypoints = Eigen::MatrixXd::Zero(2 * phi, M[qi] * (n + 1));
                Eigen::MatrixXd A_cont = Eigen::MatrixXd::Zero((M[qi] - 1) * phi, M[qi] * (n + 1));

                // Build A_waypoints
                int nn = 1;
                for (int i = 0; i < phi; i++) {
                    A_waypoints.block(i, 0, 1, n + 1) =
                            pow(planResult_ptr->T[qi][1] - planResult_ptr->T[qi][0], -i) * nn * A_0.row(i);
                    A_waypoints.block(phi + i, (n + 1) * (M[qi] - 1), 1, n + 1) =
                            pow(planResult_ptr->T[qi][planResult_ptr->T[qi].size() - 1] -
                                planResult_ptr->T[qi][planResult_ptr->T[qi].size() - 2], -i) * nn * A_T.row(i);
                    nn = nn * (n - i);
                }

                // Build A_cont
                for (int m = 1; m < M[qi]; m++) {
                    nn = 1;
                    for (int j = 0; j < phi; j++) {
                        A_cont.block(phi * (m - 1) + j, (n + 1) * (m - 1), 1, n + 1) =
                                pow(planResult_ptr->T[qi][m] - planResult_ptr->T[qi][m - 1], -j) * nn * A_T.row(j);
                        A_cont.block(phi * (m - 1) + j, (n + 1) * m, 1, n + 1) =
                                -pow(planResult_ptr->T[qi][m + 1] - planResult_ptr->T[qi][m], -j) * nn * A_0.row(j);
                        nn = nn * (n - j);
                    }
                }

                // Build Aeq_base
                Aeq_base[qi] << A_waypoints,
                                A_cont;
            }
        }

        // Equality constraints condition vector deq
        void build_deq() {
            deq.resize(N);
            for (int qi = 0; qi < N; qi++) {
                deq[qi] = Eigen::MatrixXd::Zero((2 * phi + (M[qi] - 1) * phi), outdim);
                Eigen::MatrixXd d_waypoints = Eigen::MatrixXd::Zero(2 * phi, outdim);
                Eigen::MatrixXd d_cont = Eigen::MatrixXd::Zero((M[qi] - 1) * phi, outdim);
                for (int k = 0; k < outdim; k++) {
                    d_waypoints(0, k) = mission.startState[qi][k];
                    d_waypoints(1, k) = mission.startState[qi][k + 3];
                    d_waypoints(2, k) = mission.startState[qi][k + 6];
                    d_waypoints(phi, k) = mission.goalState[qi][k];
                    d_waypoints(phi + 1, k) = mission.goalState[qi][k + 3];
                    d_waypoints(phi + 2, k) = mission.goalState[qi][k + 6];
                }

                // Build deq
                deq[qi] << d_waypoints,
                           d_cont;
            }
        }

        // Inequality constraints condition vector dlq
        void build_dlq_box() {
            // Build dlq_box
            dlq_box.resize(N);
            for (int qi = 0; qi < N; qi++) {
                dlq_box[qi] = Eigen::MatrixXd::Zero(2 * M[qi] * offset_seg, outdim);
                Eigen::MatrixXd d_upper = Eigen::MatrixXd::Zero(M[qi] * offset_seg, outdim);
                Eigen::MatrixXd d_lower = Eigen::MatrixXd::Zero(M[qi] * offset_seg, outdim);

                int bi = 0;
                for (int m = 0; m < M[qi]; m++) {
                    // find box number
                    while (bi < planResult_ptr->SFC[qi].size() &&
                           planResult_ptr->SFC[qi][bi].end_time < planResult_ptr->T[qi][m + 1]) {
                        bi++;
                    }

                    d_upper.block(m * offset_seg, 0, n + 1, 1) =
                            Eigen::MatrixXd::Constant(n + 1, 1, planResult_ptr->SFC[qi][bi].box[3]);
                    d_lower.block(m * offset_seg, 0, n + 1, 1) =
                            Eigen::MatrixXd::Constant(n + 1, 1, -planResult_ptr->SFC[qi][bi].box[0]);

                    d_upper.block(m * offset_seg, 1, n + 1, 1) =
                            Eigen::MatrixXd::Constant(n + 1, 1, planResult_ptr->SFC[qi][bi].box[4]);
                    d_lower.block(m * offset_seg, 1, n + 1, 1) =
                            Eigen::MatrixXd::Constant(n + 1, 1, -planResult_ptr->SFC[qi][bi].box[1]);

                    d_upper.block(m * offset_seg, 2, n + 1, 1) =
                            Eigen::MatrixXd::Constant(n + 1, 1, planResult_ptr->SFC[qi][bi].box[5]);
                    d_lower.block(m * offset_seg, 2, n + 1, 1) =
                            Eigen::MatrixXd::Constant(n + 1, 1, -planResult_ptr->SFC[qi][bi].box[2]);
                }
                dlq_box[qi] << d_upper,
                               d_lower;
            }
        }

        Eigen::MatrixXd build_G(int qi, double start_time, double end_time) {
            Eigen::MatrixXd G = Eigen::MatrixXd::Zero(n + 1, n + 1);

            // build B, B_inv, C
            Eigen::MatrixXd B = Eigen::MatrixXd::Zero(n + 1, n + 1);
            Eigen::MatrixXd B_inv = Eigen::MatrixXd::Zero(n + 1, n + 1);
            Eigen::MatrixXd C = Eigen::MatrixXd::Zero(n + 1, n + 1);
            if (n == 5) {
                B <<  1,   0,   0,  0,    0,   0,
                     -5,   5,   0,  0,    0,   0,
                     10, -20,  10,  0,    0,   0,
                    -10,  30, -30,  10,   0,   0,
                      5, -20,  30, -20,   5,   0,
                     -1,   5, -10,  10,  -5,   1;

                B_inv << 1,   0,   0,   0,   0,   0,
                         1, 0.2,   0,   0,   0,   0,
                         1, 0.4, 0.1,   0,   0,   0,
                         1, 0.6, 0.3, 0.1,   0,   0,
                         1, 0.8, 0.6, 0.4, 0.2,   0,
                         1,   1,   1,   1,   1,   1;

                C << 1,  1,  1,  1,  1,  1,
                     0,  1,  2,  3,  4,  5,
                     0,  0,  1,  3,  6, 10,
                     0,  0,  0,  1,  4, 10,
                     0,  0,  0,  0,  1,  5,
                     0,  0,  0,  0,  0,  1;
            } else {
                ROS_ERROR("RBPPlanner: n should be 5"); //TODO: Compute B, B_inv when n is not 5
            }

            // build F
            Eigen::MatrixXd F = Eigen::MatrixXd::Zero(n + 1, n + 1);
            int m = planResult_ptr->findSegmentIdx(qi, start_time, end_time);
            double alpha, beta, tau_i, tau_f;
            tau_i = (start_time - planResult_ptr->T[qi][m])/(planResult_ptr->T[qi][m + 1] - planResult_ptr->T[qi][m]);
            tau_f = (end_time - planResult_ptr->T[qi][m])/(planResult_ptr->T[qi][m + 1] - planResult_ptr->T[qi][m]);
            alpha = tau_f - tau_i;
            beta = tau_i;

            for(int i = 0; i < n + 1; i++){
                for(int j = i; j < n + 1; j++){
                    F(i,j) = C(i,j) * pow(alpha, i) * pow(beta, j-i);
                }
            }

            G = B_inv * F * B;
            return G;
        }

        void build_dummy() {
            dummy.resize(N);
            for (int qi = 0; qi < N; qi++) {
                dummy[qi] = Eigen::MatrixXd::Zero(M[qi] * offset_seg, outdim);
                for (int m = 0; m < M[qi]; m++) {
                    for (int j = 0; j < n + 1; j++) {
                        int a = 1;
                        if (j < (n + 1) / 2) {
                            a = 0;
                        }
                        dummy[qi](m * offset_seg + j, 0) = (1 - a) * planResult_ptr->initTraj[qi][m].x()
                                                               + a * planResult_ptr->initTraj[qi][m + 1].x();
                        dummy[qi](m * offset_seg + j, 1) = (1 - a) * planResult_ptr->initTraj[qi][m].y()
                                                               + a * planResult_ptr->initTraj[qi][m + 1].y();
                        dummy[qi](m * offset_seg + j, 2) = (1 - a) * planResult_ptr->initTraj[qi][m].z()
                                                               + a * planResult_ptr->initTraj[qi][m + 1].z();
                    }
                }
            }
        }

        void populatebyrow(IloModel model, IloNumVarArray x, IloRangeArray c, int l) {
            int offset_dim = getOffset_dim(l);
            IloEnv env = model.getEnv();
            for (int k = 0; k < outdim; k++) {
                for (int bi = 0; bi < batches[l].size(); bi++) {
                    int qi = batches[l][bi];
                    int offset_quad = getOffset_quad(l, bi);
                    for (int m = 0; m < M[qi]; m++) {
                        for (int i = 0; i < n + 1; i++) {
                            x.add(IloNumVar(env, -IloInfinity, IloInfinity));
                            int row = k * offset_dim + offset_quad + m * offset_seg + i;
                            std::string name;
                            if (k == 0) {
                                name = "x_" + std::to_string(qi) + "_" + std::to_string(m) + "_" + std::to_string(i);
                            } else if (k == 1) {
                                name = "y_" + std::to_string(qi) + "_" + std::to_string(m) + "_" + std::to_string(i);
                            } else if (k == 2) {
                                name = "z_" + std::to_string(qi) + "_" + std::to_string(m) + "_" + std::to_string(i);
                            } else {
                                ROS_ERROR("RBPPlanner: Invalid outdim");
                            }

                            x[row].setName(name.c_str());
                        }
                    }
                }
            }
            count_x = x.getSize();

            // Cost function
            IloNumExpr cost(env);
            for (int k = 0; k < outdim; k++) {
                for (int bi = 0; bi < batches[l].size(); bi++) {
                    int qi = batches[l][bi];
                    int offset_quad = getOffset_quad(l, bi);
                    for (int m = 0; m < M[qi]; m++) {
                        Eigen::MatrixXd Q_p = build_Q_p(qi, m);

                        for (int i = 0; i < n + 1; i++) {
                            int row = k * offset_dim + offset_quad + m * offset_seg + i;
                            for (int j = 0; j < n + 1; j++) {
                                int col = k * offset_dim + offset_quad + m * offset_seg + j;
                                if (Q_p(i, j) != 0) {
                                    cost += Q_p(i, j) * x[row] * x[col];
                                }
                            }
                        }
                    }
                }
            }
            model.add(IloMinimize(env, cost));

            // Equality Constraints
            for (int k = 0; k < outdim; k++) {
                for (int bi = 0; bi < batches[l].size(); bi++) {
                    int qi = batches[l][bi];
                    int offset_quad = getOffset_quad(l,bi);
                    for (int i = 0; i < 2 * phi + (M[qi] - 1) * phi; i++) {
                        IloNumExpr expr(env);
                        for (int j = 0; j < M[qi] * (n + 1); j++) {
                            if (Aeq_base[qi](i, j) != 0) {
                                expr += Aeq_base[qi](i, j) * x[k * offset_dim + offset_quad + j];
                            }
                        }
                        c.add(expr == deq[qi](i, k));
                        expr.end();
                    }
                }
            }
            count_eq = c.getSize();

            // Inequality Constraints
            for (int k = 0; k < outdim; k++) {
                for (int bi = 0; bi < batches[l].size(); bi++) {
                    int qi = batches[l][bi];
                    int offset_quad = getOffset_quad(l,bi);

                    for(int m = 0; m < M[qi]; m++) {
                        for (int j = 0; j < offset_seg; j++) {
                            int idx = k * offset_dim + offset_quad + m * offset_seg + j;
                            c.add(x[idx] <= dlq_box[qi](m * offset_seg + j, k));
                            c.add(-x[idx] <= dlq_box[qi](M[qi] * offset_seg + m * offset_seg + j, k));
                        }
                    }
                }
            }
            for (int qi = 0; qi < N; qi++) {
                for (int qj = qi + 1; qj < N; qj++) {
                    int bi = isQuadInBatch(qi, l);
                    int bj = isQuadInBatch(qj, l);
                    std::vector<double> T_ij = planResult_ptr->combineSegmentTimes(qi, qj);
                    for(int m = 0; m < T_ij.size() - 1; m++) {
                        Eigen::MatrixXd G_i = build_G(qi, T_ij[m], T_ij[m + 1]);
                        Eigen::MatrixXd G_j = build_G(qj, T_ij[m], T_ij[m + 1]);
                        double n_x,n_y,n_z;
                        n_x = planResult_ptr->RSFC[qi][qj][m].normal_vector.x();
                        n_y = planResult_ptr->RSFC[qi][qj][m].normal_vector.y();
                        n_z = planResult_ptr->RSFC[qi][qj][m].normal_vector.z();
                        int m_i, m_j, offset_quad_i, offset_quad_j;
                        m_i = planResult_ptr->findSegmentIdx(qi, T_ij[m], T_ij[m + 1]);
                        m_j = planResult_ptr->findSegmentIdx(qj, T_ij[m], T_ij[m + 1]);

                        if (bi < 0 && bj < 0) {

                        } else if (bi >= 0 && bj < 0) {
                            for (int i = 0; i < n + 1; i++) {
                                IloNumExpr expr(env);
                                for(int j = 0; j < n + 1; j++) {
                                    expr += G_j(i, j) * (n_x * dummy[qj](m_j * offset_seg + j, 0)
                                                         + n_y * dummy[qj](m_j * offset_seg + j, 1)
                                                         + n_z * dummy[qj](m_j * offset_seg + j, 2));
                                    expr -= G_i(i, j) * (n_x * x[getIdx(l, bi, 0, m_i, j)]
                                                         + n_y * x[getIdx(l, bi, 1, m_i, j)]
                                                         + n_z * x[getIdx(l, bi, 2, m_i, j)]);

                                }
                                c.add(expr >= mission.quad_size[qi] + mission.quad_size[qj]);
                                expr.end();
                            }
                        } else if (bi < 0 && bj >= 0) {
                            for (int i = 0; i < n + 1; i++) {
                                IloNumExpr expr(env);
                                for(int j = 0; j < n + 1; j++) {
                                    expr += G_j(i, j) * (n_x * x[getIdx(l, bj, 0, m_j, j)]
                                                         + n_y * x[getIdx(l, bj, 1, m_j, j)]
                                                         + n_z * x[getIdx(l, bj, 2, m_j, j)]);
                                    expr -= G_i(i, j) * (n_x * dummy[qi](m_i * offset_seg + j, 0)
                                                         + n_y * dummy[qi](m_i * offset_seg + j, 1)
                                                         + n_z * dummy[qi](m_i * offset_seg + j, 2));

                                }
                                c.add(expr >= mission.quad_size[qi] + mission.quad_size[qj]);
                                expr.end();
                            }
                        } else {
                            for (int i = 0; i < n + 1; i++) {
                                IloNumExpr expr(env);
                                for(int j = 0; j < n + 1; j++) {
                                    expr += G_j(i, j) * (n_x * x[getIdx(l, bj, 0, m_j, j)]
                                                         + n_y * x[getIdx(l, bj, 1, m_j, j)]
                                                         + n_z * x[getIdx(l, bj, 2, m_j, j)]);
                                    expr -= G_i(i, j) * (n_x * x[getIdx(l, bi, 0, m_i, j)]
                                                         + n_y * x[getIdx(l, bi, 1, m_i, j)]
                                                         + n_z * x[getIdx(l, bi, 2, m_i, j)]);

                                }
                                c.add(expr >= mission.quad_size[qi] + mission.quad_size[qj]);
                                expr.end();
                            }
                        }
                    }
                }
            }

            model.add(c);
            count_lq = c.getSize() - count_eq;
        }

        // timeMatrix is mapping matrix (n + 1) x (n + 1)
        // e.g. [1   0   0   ...]
        //      [0   t   0   ...]
        //      [0   0   t^2 ...]
        //      [... ... ... ...]
        void timeMatrix(double t, Eigen::MatrixXd* tm_ptr) {
            *tm_ptr = Eigen::MatrixXd::Zero(n + 1, n + 1);
            for (int i = 0; i < n + 1; i++) {
                (*tm_ptr)(i, i) = pow(t, n - i);
            }
        }

        // Get derivative of m^th segment(polynomial) of qi^th agent
        // e.g. if phi = 3, n = 5, polynomial [1 1 1 1 1 1] then
        //      coef_der = [1  1  1  1  1  1]
        //                 [5  4  3  2  1  0]
        //                 [20 12 6  2  0  0]
        //                 [60 24 6  0  0  0]
        void derivative_segment(int qi, int k, int m, Eigen::MatrixXd* coef_der_ptr) {
            *coef_der_ptr = Eigen::MatrixXd::Zero(phi + 1, n + 1);
            for (int i = 0; i < phi + 1; i++) {
                for (int j = 0; j < n + 1; j++) {
                    if (i <= j)
                        (*coef_der_ptr)(i, n - j) = coef_derivative(i, j) *
                                                    planResult_ptr->coef[qi](m * offset_seg + n - j, k);
                    else
                        (*coef_der_ptr)(i, n - j) = 0;
                }
            }
        }

        // Get j^th coefficient of i^th derivative of polynomial [1 1 1 1 ... 1]
        int coef_derivative(int i, int j) {
            return (i == 0) ? 1 : coef_derivative(i - 1, j - 1) * j;
        }

        // Get roots of i^th derivative of polynomial with coefficient coef
        // The roots of the polynomial are calculated by computing the eigenvalues of the companion matrix, A
        std::vector<double> roots_derivative(int i, const Eigen::MatrixXd& coef_der) {
            std::vector<double> roots_der;
            int n_der = n - i;
            int iter = 0;
            while(n_der > 0 && coef_der(i, n - i - n_der) == 0 ){
                n_der--;
            }
            if(n_der == 0){
                return roots_der; // return empty vector
            }

            Eigen::MatrixXd A = Eigen::MatrixXd::Zero(n_der, n_der);
            for (int j = 0; j < n_der; j++) {
                if (j < n_der - 1) {
                    A(j + 1, j) = 1;
                }
                A(0, j) = -coef_der(i, n - i - n_der + j + 1) / coef_der(i, n - i - n_der);
            }

            Eigen::EigenSolver<Eigen::MatrixXd> es(A);
            for (int j = 0; j < i; j++) {
                complex<double> lambda = es.eigenvalues()[j];
                if (lambda.imag() == 0) {
                    roots_der.emplace_back(lambda.real());
                }
            }
            return roots_der;
        }

        double scale_to_max_vel(int qi, int k, int m, const Eigen::MatrixXd& coef_der) {
            assert(phi > 1);
            double scale_update_rate = 1.1; //TODO: parameterization?

            // Get maximum velocity
            double vel_max = 0, t_max = 0;
            std::vector<double> ts = roots_derivative(2, coef_der);
            ts.emplace_back(0);
            ts.emplace_back(planResult_ptr->T[qi][m + 1] - planResult_ptr->T[qi][m]);
            for (auto t : ts) {
                if (t < 0 || t > planResult_ptr->T[qi][m + 1] - planResult_ptr->T[qi][m]) {
                    continue;
                }

                double vel = 0;
                for (int i = 0; i < n; i++) {
                    vel += coef_der(1, i) * pow(t, n - 1 - i); //?
                }
                vel = abs(vel);
                if (vel_max < vel) {
                    vel_max = vel;
                    t_max = t;
                }
            }

            // time_scale update
            double time_scale = 1;
            while (vel_max > mission.max_vel[qi][k]) {
                time_scale *= scale_update_rate;

                double vel = 0;
                for (int i = 0; i < n; i++) {
                    vel += coef_der(1, i) * pow(1 / time_scale, n - i) * pow(t_max, n - 1 - i);
                }
                vel_max = abs(vel);
            }

            return time_scale;
        }


        double scale_to_max_acc(int qi, int k, int m, const Eigen::MatrixXd &coef_der) {
            assert(phi == 3 && n == 5);
            double scale_update_rate = 1.1; //TODO: parameterization?

            // Get maximum accelaration
            double a, b, c, D, acc_max, t_max = 0;
            a = coef_der(3, 0);
            b = coef_der(3, 1);
            c = coef_der(3, 2);
            D = b * b - 4 * a * c;
            acc_max = 0;

            std::vector<double> ts{0, planResult_ptr->T[qi][m + 1] - planResult_ptr->T[qi][m]};
            if (D >= 0 && a != 0) {
                ts.emplace_back((-b + sqrt(D)) / (2 * a));
                ts.emplace_back((-b - sqrt(D)) / (2 * a));
            }
            else if(a == 0 && b != 0){
                ts.emplace_back(-c/b);
            }

            for (auto t : ts) {
                if (t < 0 || t > planResult_ptr->T[qi][m + 1] - planResult_ptr->T[qi][m]) {
                    continue;
                }

                double acc = 0;
                for (int i = 0; i < 4; i++) {
                    acc += coef_der(2, i) * pow(t, 3 - i);
                }
                acc = abs(acc);
                if (acc_max < acc) {
                    acc_max = acc;
                    t_max = t;
                }
            }

            // time_scale update
            double time_scale = 1;
            while (acc_max > mission.max_acc[qi][k]) {
                time_scale *= scale_update_rate;

                double acc = 0;
                for (int i = 0; i < 4; i++) {
                    acc += coef_der(2, i) * pow(1 / time_scale, n - i) * pow(t_max, 3 - i);
                }
                acc_max = abs(acc);
            }

            return time_scale;
        }

        void setBatch(int alg){
            int batch_max_iter = ceil((double)N / (double)param.batch_size);
            if (param.sequential) {
                int batch_max_iter = ceil((double)N / (double)param.batch_size);
                if (param.batch_iter < 0 || param.batch_iter > batch_max_iter) {
                    param.batch_iter = batch_max_iter;
                }
            } else {
                param.batch_size = N;
                param.batch_iter = 1;
            }

            batches.resize(batch_max_iter);

            //default groups
            if(alg == 0) {
                for (int qi = 0; qi < N; qi++) {
                    batches[qi / param.batch_size].emplace_back(qi);
                }
            }
            else{
                ROS_ERROR("RBPPlaner: invalid batch algorithm");
            }
        }

        int isQuadInBatch(int qi, int l){
            for(int bi = 0; bi < batches[l].size(); bi++){
                if(qi == batches[l][bi]){
                    return bi;
                }
            }
            return -1;
        }

        int getOffset_dim(int l){
            int offset_dim = 0;
            for(int bi = 0; bi < batches[l].size(); bi++){
                int qi = batches[l][bi];
                offset_dim += M[qi] * offset_seg;
            }
            return offset_dim;
        }

        int getOffset_quad(int l, int bi){
            int offset_quad = 0;

            for(int i = 0; i < bi; i++){
                int qi = batches[l][i];
                offset_quad += M[qi] * offset_seg;
            }

            return offset_quad;
        }

        int getIdx(int l, int bi, int k, int m, int j){
            int offset_dim = getOffset_dim(l);
            int offset_quad = getOffset_quad(l, bi);
            return k * offset_dim + offset_quad + m * offset_seg + j;
        }
    };
}