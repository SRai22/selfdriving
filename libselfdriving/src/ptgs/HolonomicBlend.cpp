/* -------------------------------------------------------------------------
 *   SelfDriving C++ library based on PTGs and mrpt-nav
 * Copyright (C) 2019-2022 Jose Luis Blanco, University of Almeria
 * See LICENSE for license information.
 * ------------------------------------------------------------------------- */

/* +------------------------------------------------------------------------+
   |                     Mobile Robot Programming Toolkit (MRPT)            |
   |                          https://www.mrpt.org/                         |
   |                                                                        |
   | Copyright (c) 2005-2022, Individual contributors, see AUTHORS file     |
   | See: https://www.mrpt.org/Authors - All rights reserved.               |
   | Released under BSD License. See: https://www.mrpt.org/License          |
   +------------------------------------------------------------------------+ */

#include <mrpt/core/round.h>
#include <mrpt/kinematics/CVehicleVelCmd_Holo.h>
#include <mrpt/math/CVectorFixed.h>
#include <mrpt/math/poly_roots.h>
#include <mrpt/serialization/CArchive.h>
#include <mrpt/system/CTimeLogger.h>
#include <selfdriving/ptgs/HolonomicBlend.h>

#include <iostream>  // debug only, remove!

using namespace mrpt::nav;
using namespace selfdriving::ptg;
using namespace mrpt::system;

IMPLEMENTS_SERIALIZABLE(
    HolonomicBlend, mrpt::nav::CParameterizedTrajectoryGenerator,
    selfdriving::ptg)

/*
Closed-form PTG. Parameters:
- Initial velocity vector (xip, yip)
- Target velocity vector depends on \alpha: xfp = V_MAX*cos(alpha), yfp =
V_MAX*sin(alpha)
- T_ramp_max max time for velocity interpolation (xip,yip) -> (xfp, yfp)
- W_MAX: Rotational velocity for robot heading forwards.

Number of steps "d" for each PTG path "k":
- Step = time increment PATH_TIME_STEP

*/

// Uncomment only for benchmarking during development
//#define DO_PERFORMANCE_BENCHMARK

#ifdef DO_PERFORMANCE_BENCHMARK
mrpt::system::CTimeLogger tl_holo("HolonomicBlend");
#define PERFORMANCE_BENCHMARK \
    CTimeLoggerEntry tle(tl_holo, __CURRENT_FUNCTION_NAME__);
#else
#define PERFORMANCE_BENCHMARK
#endif

double HolonomicBlend::PATH_TIME_STEP = 10e-3;  // 10 ms
double HolonomicBlend::eps = 1e-4;  // epsilon for detecting 1/0 situation

#if 0
static double calc_trans_distance_t_below_Tramp_abc_analytic(double t, double a, double b, double c)
{
	PERFORMANCE_BENCHMARK;

	ASSERT_(t >= 0);
	if (t == 0.0) return .0;

	double dist;
	// Handle special case: degenerate sqrt(a*t^2+b*t+c) =  sqrt((t-r)^2) = |t-r|
	const double discr = b*b - 4 * a*c;
	if (std::abs(discr)<1e-6)
	{
		const double r = -b / (2 * a);
		// dist= definite integral [0,t] of: |t-r| dt
		dist = r*std::abs(r)*0.5 - (r - t)*std::abs(r - t)*0.5;
	}
	else
	{
		// General case:
		// Indefinite integral of sqrt(a*t^2+b*t+c):
		const double int_t = (t*(1.0 / 2.0) + (b*(1.0 / 4.0)) / a)*sqrt(c + b*t + a*(t*t)) + 1.0 / pow(a, 3.0 / 2.0)*log(1.0 / sqrt(a)*(b*(1.0 / 2.0) + a*t) + sqrt(c + b*t + a*(t*t)))*(a*c - (b*b)*(1.0 / 4.0))*(1.0 / 2.0);
		// Limit when t->0:
		const double int_t0 = (b*sqrt(c)*(1.0 / 4.0)) / a + 1.0 / pow(a, 3.0 / 2.0)*log(1.0 / sqrt(a)*(b + sqrt(a)*sqrt(c)*2.0)*(1.0 / 2.0))*(a*c - (b*b)*(1.0 / 4.0))*(1.0 / 2.0);
		dist = int_t - int_t0;// Definite integral [0,t]
	}
#ifdef _DEBUG
	using namespace mrpt;
	MRPT_CHECK_NORMAL_NUMBER(dist);
	ASSERT_(dist >= .0);
#endif
	return dist;
}
#endif

// Numeric integration of: sqrt(a*t^2+b*t+c) for t=[0,T]
static double calc_trans_distance_t_below_Tramp_abc_numeric(
    double T, double a, double b, double c)
{
    PERFORMANCE_BENCHMARK;

    double             d         = .0;
    const unsigned int NUM_STEPS = 20;

    ASSERT_(a >= .0);
    ASSERT_(c >= .0);
    double feval_t = std::sqrt(c);  // t (initial: t=0)
    double feval_tp1;  // t+1

    const double At = T / (NUM_STEPS);
    double       t  = .0;
    for (unsigned int i = 0; i < NUM_STEPS; i++)
    {
        // Eval function at t+1:
        t += At;
        double dd = a * t * t + b * t + c;

        // handle numerical innacuracies near t=T_ramp:
        ASSERT_(dd > -1e-5);
        if (dd < 0) dd = .0;

        feval_tp1 = sqrt(dd);

        // Trapezoidal rule:
        d += At * (feval_t + feval_tp1) * 0.5;

        // for next step:
        feval_t = feval_tp1;
    }

    return d;
}

// Axiliary function for calc_trans_distance_t_below_Tramp() and others:
double HolonomicBlend::calc_trans_distance_t_below_Tramp_abc(
    double t, double a, double b, double c)
{
// JLB (29 Jan 2017): it turns out that numeric integration is *faster* and more
// accurate (does not have "special cases")...
#if 0
	double ret = calc_trans_distance_t_below_Tramp_abc_analytic(t, a, b, c);
#else
    double ret = calc_trans_distance_t_below_Tramp_abc_numeric(t, a, b, c);
#endif

    return ret;
}

// Axiliary function for computing the line-integral distance along the
// trajectory, handling special cases of 1/0:
double HolonomicBlend::calc_trans_distance_t_below_Tramp(
    double k2, double k4, double vxi, double vyi, double t)
{
    /*
    dd = sqrt( (4*k2^2 + 4*k4^2)*t^2 + (4*k2*vxi + 4*k4*vyi)*t + vxi^2 + vyi^2 )
    dt
                a t^2 + b t + c
    */
    const double c = (vxi * vxi + vyi * vyi);
    if (std::abs(k2) > eps || std::abs(k4) > eps)
    {
        const double a = ((k2 * k2) * 4.0 + (k4 * k4) * 4.0);
        const double b = (k2 * vxi * 4.0 + k4 * vyi * 4.0);

        // Numerically-ill case: b=c=0 (initial vel=0)
        if (std::abs(b) < eps && std::abs(c) < eps)
        {
            // Indefinite integral of simplified case: sqrt(a)*t
            const double int_t = sqrt(a) * (t * t) * 0.5;
            return int_t;  // Definite integral [0,t]
        }
        else
        {
            return calc_trans_distance_t_below_Tramp_abc(t, a, b, c);
        }
    }
    else
    {
        return std::sqrt(c) * t;
    }
}

void HolonomicBlend::onNewNavDynamicState()
{
    m_pathStepCountCache.assign(m_alphaValuesCount, -1);  // mark as invalid

    // Are we approaching a target with a slow-down condition?
    m_expr_target_dir = .0;
    if (m_nav_dyn_state_target_k != INVALID_PTG_PATH_INDEX)
        m_expr_target_dir = index2alpha(m_nav_dyn_state_target_k);

    m_expr_target_dist = m_nav_dyn_state.relTarget.norm();
}

void HolonomicBlend::loadDefaultParams()
{
    CParameterizedTrajectoryGenerator::loadDefaultParams();
    CPTG_RobotShape_Circular::loadDefaultParams();

    m_alphaValuesCount = 31;
    T_ramp_max         = 0.9;
    V_MAX              = 1.0;
    W_MAX              = mrpt::DEG2RAD(40);
}

void HolonomicBlend::loadFromConfigFile(
    const mrpt::config::CConfigFileBase& cfg, const std::string& sSection)
{
    CParameterizedTrajectoryGenerator::loadFromConfigFile(cfg, sSection);
    CPTG_RobotShape_Circular::loadShapeFromConfigFile(cfg, sSection);

    MRPT_LOAD_HERE_CONFIG_VAR_NO_DEFAULT(
        T_ramp_max, double, T_ramp_max, cfg, sSection);
    MRPT_LOAD_HERE_CONFIG_VAR_NO_DEFAULT(
        v_max_mps, double, V_MAX, cfg, sSection);
    MRPT_LOAD_HERE_CONFIG_VAR_DEGREES_NO_DEFAULT(
        w_max_dps, double, W_MAX, cfg, sSection);
    MRPT_LOAD_CONFIG_VAR(turningRadiusReference, double, cfg, sSection);

    MRPT_LOAD_HERE_CONFIG_VAR(expr_V, string, expr_V, cfg, sSection);
    MRPT_LOAD_HERE_CONFIG_VAR(expr_W, string, expr_W, cfg, sSection);
    MRPT_LOAD_HERE_CONFIG_VAR(expr_T_ramp, string, expr_T_ramp, cfg, sSection);
}
void HolonomicBlend::saveToConfigFile(
    mrpt::config::CConfigFileBase& cfg, const std::string& sSection) const
{
    MRPT_START
    const int WN = 25, WV = 30;

    CParameterizedTrajectoryGenerator::saveToConfigFile(cfg, sSection);

    cfg.write(
        sSection, "T_ramp_max", T_ramp_max, WN, WV,
        "Max duration of the velocity interpolation since a vel_cmd is issued "
        "[s].");
    cfg.write(
        sSection, "v_max_mps", V_MAX, WN, WV,
        "Maximum linear velocity for trajectories [m/s].");
    cfg.write(
        sSection, "w_max_dps", mrpt::RAD2DEG(W_MAX), WN, WV,
        "Maximum angular velocity for trajectories [deg/s].");
    cfg.write(
        sSection, "turningRadiusReference", turningRadiusReference, WN, WV,
        "An approximate dimension of the robot (not a critical parameter) "
        "[m].");

    cfg.write(
        sSection, "expr_V", expr_V, WN, WV,
        "Math expr for |V| as a function of "
        "`dir`,`V_MAX`,`W_MAX`,`T_ramp_max`.");
    cfg.write(
        sSection, "expr_W", expr_W, WN, WV,
        "Math expr for |omega| (disregarding the sign, only the module) as a "
        "function of `dir`,`V_MAX`,`W_MAX`,`T_ramp_max`.");
    cfg.write(
        sSection, "expr_T_ramp", expr_T_ramp, WN, WV,
        "Math expr for `T_ramp` as a function of "
        "`dir`,`V_MAX`,`W_MAX`,`T_ramp_max`.");

    CPTG_RobotShape_Circular::saveToConfigFile(cfg, sSection);

    MRPT_END
}

std::string HolonomicBlend::getDescription() const
{
    return mrpt::format(
        "selfdriving_HolonomicBlend=%.03f_Vmax=%.03f_Wmax=%.03f", T_ramp_max,
        V_MAX, W_MAX);
}

void HolonomicBlend::serializeFrom(
    mrpt::serialization::CArchive& in, uint8_t version)
{
    CParameterizedTrajectoryGenerator::internal_readFromStream(in);

    switch (version)
    {
        case 0:
        {
            CPTG_RobotShape_Circular::internal_shape_loadFromStream(in);
            in >> T_ramp_max >> V_MAX >> W_MAX >> turningRadiusReference;
            in >> expr_V >> expr_W >> expr_T_ramp;
        }
        break;

        default:
            MRPT_THROW_UNKNOWN_SERIALIZATION_VERSION(version);
    };
}

uint8_t HolonomicBlend::serializeGetVersion() const { return 0; }
void    HolonomicBlend::serializeTo(mrpt::serialization::CArchive& out) const
{
    CParameterizedTrajectoryGenerator::internal_writeToStream(out);
    CPTG_RobotShape_Circular::internal_shape_saveToStream(out);

    out << T_ramp_max << V_MAX << W_MAX << turningRadiusReference;
    out << expr_V << expr_W << expr_T_ramp;
}

bool HolonomicBlend::inverseMap_WS2TP(
    double x, double y, int& out_k, double& out_d,
    [[maybe_unused]] double tolerance_dist) const
{
    double dummy_T_ramp;
    return inverseMap_WS2TP_with_Tramp(x, y, out_k, out_d, dummy_T_ramp);
}

bool HolonomicBlend::inverseMap_WS2TP_with_Tramp(
    double x, double y, int& out_k, double& out_d, double& out_T_ramp) const
{
    PERFORMANCE_BENCHMARK;

    ASSERT_(x != 0 || y != 0);

    const double REL_SPEED_TO_CONSIDER_REACH_AND_STOP = 0.10 * 1.05 /*MARGIN*/;

    const double err_threshold = 1e-3;
    const double vxi           = m_nav_dyn_state.curVelLocal.vx,
                 vyi           = m_nav_dyn_state.curVelLocal.vy;

    // Use a Newton iterative non-linear optimizer to find the "exact" solution
    // for (t,alpha)
    // in each case: (1) t<T_ramp and (2) t>T_ramp

    // Initial value:
    mrpt::math::CVectorFixed<double, 4> q;  // [t vxf vyf T_r]
    q[0] = T_ramp_max * 1.1;
    q[1] = V_MAX * x / sqrt(x * x + y * y);
    q[2] = V_MAX * y / sqrt(x * x + y * y);
    q[3] = T_ramp_max;

    // Iterate: case (2) t > T_ramp
    double err_mod   = std::numeric_limits<double>::max();
    bool   sol_found = false;
    for (int iters = 0; !sol_found && iters < 25; iters++)
    {
        const double t   = q[0];
        const double vxf = q[1], vyf = q[2];
        const double alpha = atan2(vyf, vxf);

        const auto lambda_vel = [this](double dir) {
            auto lck                        = mrpt::lockHelper(m_expr_mtx);
            const_cast<double&>(m_expr_dir) = dir;
            return std::abs(m_expr_v.eval());
        };

        const double V_MAXsq = mrpt::square(lambda_vel(alpha));

        const bool stopAtTarget =
            V_MAXsq < mrpt::square(REL_SPEED_TO_CONSIDER_REACH_AND_STOP);

        const double T_ramp = q[3];

        const double TR_  = 1.0 / (T_ramp);
        const double TR2_ = 1.0 / (2 * T_ramp);

        // Eval residual:
        mrpt::math::CVectorFixed<double, 4> r;
        if (t >= T_ramp)
        {
            r[0] = 0.5 * T_ramp * (vxi + vxf) + (t - T_ramp) * vxf - x;
            r[1] = 0.5 * T_ramp * (vyi + vyf) + (t - T_ramp) * vyf - y;
        }
        else
        {
            r[0] = vxi * t + t * t * TR2_ * (vxf - vxi) - x;
            r[1] = vyi * t + t * t * TR2_ * (vyf - vyi) - y;
        }

        r[2] = vxf * vxf + vyf * vyf - V_MAXsq;
        if (stopAtTarget)  // "S.A.T."
                           // condition.
            r[3] = T_ramp - t;
        else
            r[3] = 0;

        // See doc/ for the Latex/PDF with
        // the exact formulas.
        //
        // Jacobian: q=[t vxf vyf T_r] q0=t
        // q1=vxf   q2=vyf q3=T_r
        //
        //  dx/dt    dx/dvxf    dx/dvyf
        //  dx/dTr dy/dt    dy/dvxf dy/dvyf
        //  dy/dTr dVF/dt   dVF/dvxf
        //  dVF/dvyf    dVR/dTr dSAT/dt
        //  dSAT/dvxf  dSAT/dvyf   dSAT/dTr
        //
        mrpt::math::CMatrixDouble44 J;  // all zeros
        if (t >= T_ramp)
        {
            J(0, 0) = vxf;
            J(0, 1) = 0.5 * T_ramp + t;
            // J(0, 2) = 0.0;
            J(1, 0) = vyf;
            // J(1, 1) = 0.0;
            J(1, 2) = 0.5 * T_ramp + t;

            if (stopAtTarget)
            {
                // Add derivatives wrt T_r
                J(0, 3) = 0.5 * (vxi - vxf);
                J(1, 3) = 0.5 * (vyi - vyf);
            }
            else
            {
                // make the Jacobian not to
                // depend on T_r so we used
                // the prescribed one:
                q[3]    = T_ramp_max;
                J(3, 3) = 1;
            }
        }
        else
        {
            // t<T_ramp case:
            // --------------------
            J(0, 0) = vxi + t * TR_ * (vxf - vxi);
            J(0, 1) = TR2_ * t * t;
            // J(0, 2) = 0.0;
            J(1, 0) = vyi + t * TR_ * (vyf - vyi);
            // J(1, 1) = 0.0;
            J(1, 2) = TR2_ * t * t;
            if (stopAtTarget)
            {
                // Add derivatives wrt T_r
                J(0, 3) = -t * t * TR2_ * (vxf - vxi);
                J(1, 3) = -t * t * TR2_ * (vyf - vyi);
            }
            else
            {
                // make the Jacobian not to
                // depend on T_r so we used
                // the prescribed one:
                q[3]    = T_ramp_max;
                J(3, 3) = 1;
            }
        }
        if (stopAtTarget)
        {
            // Impose "t=T_r"
            J(3, 0) = -1;
            J(3, 3) = 1;
        }

        J(2, 1) = 2 * vxf;
        J(2, 2) = 2 * vyf;

        mrpt::math::CVectorFixed<double, 4> q_incr = J.lu_solve(r);
        q -= q_incr;

        err_mod   = r.norm();
        sol_found = (err_mod < err_threshold);
    }

#if 0
    std::cout << "[inverseWS2TP] ws=(" << x << "," << y << ")"
              << " finalErr: " << err_mod << " q=" << q.inMatlabFormat()
              << "\n";
#endif

    if (sol_found && q[0] >= .0)
    {
        const double alpha = atan2(q[2], q[1]);
        out_k          = CParameterizedTrajectoryGenerator::alpha2index(alpha);
        out_T_ramp     = q[3];
        const double t = q[0];
        const double vxf = q[1], vyf = q[2];

        const double       solved_t    = t;
        const unsigned int solved_step = solved_t / PATH_TIME_STEP;
        const double       found_dist =
            internal_getPathDist(solved_step, out_T_ramp, vxf, vyf);

        out_d = found_dist / this->refDistance;

        return true;
    }
    else
    {
        return false;
    }
}

bool HolonomicBlend::PTG_IsIntoDomain(double x, double y) const
{
    int    k;
    double d;
    return inverseMap_WS2TP(x, y, k, d);
}

void HolonomicBlend::internal_deinitialize()
{
    // Nothing to do in a closed-form PTG.
}

mrpt::kinematics::CVehicleVelCmd::Ptr HolonomicBlend::directionToMotionCommand(
    uint16_t k) const
{
    const double dir_local = CParameterizedTrajectoryGenerator::index2alpha(k);

    const auto pp = internal_params_from_dir_and_dynstate(dir_local);

    auto* cmd      = new mrpt::kinematics::CVehicleVelCmd_Holo();
    cmd->vel       = pp.vf;
    cmd->dir_local = dir_local;
    cmd->ramp_time = pp.T_ramp;
    cmd->rot_speed = pp.wf;

    return mrpt::kinematics::CVehicleVelCmd::Ptr(cmd);
}

size_t HolonomicBlend::getPathStepCount(uint16_t k) const
{
    if (m_pathStepCountCache.size() > k && m_pathStepCountCache[k] > 0)
        return m_pathStepCountCache[k];

    uint32_t step;
    if (!getPathStepForDist(k, this->refDistance, step))
    {
        THROW_EXCEPTION_FMT(
            "Could not solve closed-form distance for k=%u",
            static_cast<unsigned>(k));
    }
    ASSERT_(step > 0);
    if (m_pathStepCountCache.size() != m_alphaValuesCount)
    { m_pathStepCountCache.assign(m_alphaValuesCount, -1); }
    m_pathStepCountCache[k] = step;
    return step;
}

mrpt::math::TPose2D HolonomicBlend::getPathPose(uint16_t k, uint32_t step) const
{
    const double t    = PATH_TIME_STEP * step;
    const double dir  = CParameterizedTrajectoryGenerator::index2alpha(k);
    const auto   _    = internal_params_from_dir_and_dynstate(dir);
    const double TR2_ = 1.0 / (2 * _.T_ramp);

    mrpt::math::TPose2D p;
    // Translational part:
    if (t < _.T_ramp)
    {
        p.x = _.vxi * t + t * t * TR2_ * (_.vxf - _.vxi);
        p.y = _.vyi * t + t * t * TR2_ * (_.vyf - _.vyi);
    }
    else
    {
        p.x = _.T_ramp * 0.5 * (_.vxi + _.vxf) + (t - _.T_ramp) * _.vxf;
        p.y = _.T_ramp * 0.5 * (_.vyi + _.vyf) + (t - _.T_ramp) * _.vyf;
    }

    // Rotational part:
    const double wi = m_nav_dyn_state.curVelLocal.omega;

    if (t < _.T_ramp)
    {
        // Time required to align completed?
        const double a = TR2_ * (_.wf - wi), b = (wi), c = -dir;

        // Solves equation `a*x^2 + b*x + c = 0`.
        double r1, r2;
        int    nroots = mrpt::math::solve_poly2(a, b, c, r1, r2);
        if (nroots != 2)
        {
            p.phi = .0;  // typical case: wi=wf=0
        }
        else
        {
            const double t_solve = std::max(r1, r2);
            if (t > t_solve)
                p.phi = dir;
            else
                p.phi = wi * t + t * t * TR2_ * (_.wf - wi);
        }
    }
    else
    {
        // Time required to align completed?
        const double t_solve =
            (dir - _.T_ramp * 0.5 * (wi + _.wf)) / _.wf + _.T_ramp;
        if (t > t_solve)
            p.phi = dir;
        else
            p.phi = _.T_ramp * 0.5 * (wi + _.wf) + (t - _.T_ramp) * _.wf;
    }
    return p;
}

double HolonomicBlend::getPathDist(uint16_t k, uint32_t step) const
{
    const auto pp = internal_params_from_dir_and_dynstate(
        CParameterizedTrajectoryGenerator::index2alpha(k));

    return internal_getPathDist(step, pp.T_ramp, pp.vxf, pp.vyf);
}

double HolonomicBlend::internal_getPathDist(
    uint32_t step, double T_ramp, double vxf, double vyf) const
{
    const double t    = PATH_TIME_STEP * step;
    const double TR2_ = 1.0 / (2 * T_ramp);

    const double vxi = m_nav_dyn_state.curVelLocal.vx;
    const double vyi = m_nav_dyn_state.curVelLocal.vy;

    const double k2 = (vxf - vxi) * TR2_;
    const double k4 = (vyf - vyi) * TR2_;

    if (t < T_ramp)
    { return calc_trans_distance_t_below_Tramp(k2, k4, vxi, vyi, t); }
    else
    {
        const double dist_trans =
            (t - T_ramp) * V_MAX +
            calc_trans_distance_t_below_Tramp(k2, k4, vxi, vyi, T_ramp);
        return dist_trans;
    }
}

bool HolonomicBlend::getPathStepForDist(
    uint16_t k, double dist, uint32_t& out_step) const
{
    PERFORMANCE_BENCHMARK;

    const double dir  = CParameterizedTrajectoryGenerator::index2alpha(k);
    const auto   _    = internal_params_from_dir_and_dynstate(dir);
    const double TR2_ = 1.0 / (2 * _.T_ramp);

    const double k2 = (_.vxf - _.vxi) * TR2_;
    const double k4 = (_.vyf - _.vyi) * TR2_;

    // --------------------------------------
    // Solution within  t >= T_ramp ??
    // --------------------------------------
    const double dist_trans_T_ramp =
        calc_trans_distance_t_below_Tramp(k2, k4, _.vxi, _.vyi, _.T_ramp);
    double t_solved = -1;

    if (dist >= dist_trans_T_ramp)
    {
        // Good solution:
        t_solved = _.T_ramp + (dist - dist_trans_T_ramp) / V_MAX;
    }
    else
    {
        // ------------------------------------
        // Solutions within t < T_ramp
        //
        // Cases:
        // 1) k2=k4=0  --> vi=vf. Path is straight line
        // 2) b=c=0     -> vi=0
        // 3) Otherwise, general case
        // ------------------------------------
        if (std::abs(k2) < eps && std::abs(k4) < eps)
        {
            // Case 1
            t_solved = (dist) / V_MAX;
        }
        else
        {
            const double a = ((k2 * k2) * 4.0 + (k4 * k4) * 4.0);
            const double b = (k2 * _.vxi * 4.0 + k4 * _.vyi * 4.0);
            const double c = (_.vxi * _.vxi + _.vyi * _.vyi);

            // Numerically-ill case: b=c=0 (initial vel=0)
            if (std::abs(b) < eps && std::abs(c) < eps)
            {
                // Case 2:
                t_solved = sqrt(2.0) * 1.0 / pow(a, 1.0 / 4.0) * sqrt(dist);
            }
            else
            {
                // Case 3: general case with non-linear equation:
                // dist = (t/2 + b/(4*a))*(a*t^2 + b*t + c)^(1/2) -
                // (b*c^(1/2))/(4*a) + (log((b/2 + a*t)/a^(1/2) + (a*t^2 +
                // b*t + c)^(1/2))*(- b^2/4 + a*c))/(2*a^(3/2)) - (log((b +
                // 2*a^(1/2)*c^(1/2))/(2*a^(1/2)))*(- b^2/4 +
                // a*c))/(2*a^(3/2)) dist =
                // (t*(1.0/2.0)+(b*(1.0/4.0))/a)*sqrt(c+b*t+a*(t*t))-(b*sqrt(c)*(1.0/4.0))/a+1.0/pow(a,3.0/2.0)*log(1.0/sqrt(a)*(b*(1.0/2.0)+a*t)+sqrt(c+b*t+a*(t*t)))*(a*c-(b*b)*(1.0/4.0))*(1.0/2.0)-1.0/pow(a,3.0/2.0)*log(1.0/sqrt(a)*(b+sqrt(a)*sqrt(c)*2.0)*(1.0/2.0))*(a*c-(b*b)*(1.0/4.0))*(1.0/2.0);

                // We must solve this by iterating:
                // Newton method:
                // Minimize f(t)-dist = 0
                //  with: f(t)=calc_trans_distance_t_below_Tramp_abc(t)
                //  and:  f'(t) = sqrt(a*t^2+b*t+c)

                t_solved = _.T_ramp * 0.6;  // Initial value for starting
                // interation inside the valid domain
                // of the function t=[0,T_ramp]
                for (int iters = 0; iters < 10; iters++)
                {
                    double err = calc_trans_distance_t_below_Tramp_abc(
                                     t_solved, a, b, c) -
                                 dist;
                    const double diff =
                        std::sqrt(a * t_solved * t_solved + b * t_solved + c);
                    ASSERT_(std::abs(diff) > 1e-40);
                    t_solved -= (err) / diff;
                    if (t_solved < 0) t_solved = .0;
                    if (std::abs(err) < 1e-3) break;  // Good enough!
                }
            }
        }
    }
    if (t_solved >= 0)
    {
        out_step = mrpt::round(t_solved / PATH_TIME_STEP);
        return true;
    }
    else
        return false;
}

void HolonomicBlend::updateTPObstacleSingle(
    double ox, double oy, uint16_t k, double& tp_obstacle_k) const
{
    const double R    = m_robotRadius;
    const double dir  = CParameterizedTrajectoryGenerator::index2alpha(k);
    const auto   _    = internal_params_from_dir_and_dynstate(dir);
    const double TR2_ = 1.0 / (2 * _.T_ramp);
    const double TR_2 = _.T_ramp * 0.5;
    const double T_ramp_thres099 = _.T_ramp * 0.99;
    const double T_ramp_thres101 = _.T_ramp * 1.01;

    double sol_t = -1.0;  // candidate solution for shortest time to collision

    // Note: It's tempting to try to solve first for t>T_ramp because it has
    // simpler (faster) equations,
    // but there are cases in which we will have valid collisions for
    // t>T_ramp but other valid ones for t<T_ramp as well, so the only SAFE
    // way to detect shortest distances is to check over increasing values
    // of "t".

    // Try to solve first for t<T_ramp:
    const double k2 = (_.vxf - _.vxi) * TR2_;
    const double k4 = (_.vyf - _.vyi) * TR2_;

    // equation: a*t^4 + b*t^3 + c*t^2 + d*t + e = 0
    const double a = (k2 * k2 + k4 * k4);
    const double b = (k2 * _.vxi * 2.0 + k4 * _.vyi * 2.0);
    const double c =
        -(k2 * ox * 2.0 + k4 * oy * 2.0 - _.vxi * _.vxi - _.vyi * _.vyi);
    const double d = -(ox * _.vxi * 2.0 + oy * _.vyi * 2.0);
    const double e = -R * R + ox * ox + oy * oy;

    double roots[4];
    int    num_real_sols = 0;
    if (std::abs(a) > eps)
    {
        // General case: 4th order equation
        // a * x^4 + b * x^3 + c * x^2 + d * x + e
        num_real_sols =
            mrpt::math::solve_poly4(roots, b / a, c / a, d / a, e / a);
    }
    else if (std::abs(b) > eps)
    {
        // Special case: k2=k4=0 (straight line path, no blend)
        // 3rd order equation:
        // b * x^3 + c * x^2 + d * x + e
        num_real_sols = mrpt::math::solve_poly3(roots, c / b, d / b, e / b);
    }
    else
    {
        // Special case: 2nd order equation (a=b=0)
        const double discr = d * d - 4 * c * e;  // c*t^2 + d*t + e = 0
        if (discr >= 0)
        {
            num_real_sols = 2;
            roots[0]      = (-d + sqrt(discr)) / (2 * c);
            roots[1]      = (-d - sqrt(discr)) / (2 * c);
        }
        else
        {
            num_real_sols = 0;
        }
    }

    for (int i = 0; i < num_real_sols; i++)
    {
        if (roots[i] == roots[i] &&  // not NaN
            std::isfinite(roots[i]) && roots[i] >= .0 &&
            roots[i] <= _.T_ramp * 1.01)
        {
            if (sol_t < 0)
                sol_t = roots[i];
            else
                mrpt::keep_min(sol_t, roots[i]);
        }
    }

    // Invalid with these equations?
    if (sol_t < 0 || sol_t > T_ramp_thres101)
    {
        // Now, attempt to solve with the equations for t>T_ramp:
        sol_t = -1.0;

        const double c1 = TR_2 * (_.vxi - _.vxf) - ox;
        const double c2 = TR_2 * (_.vyi - _.vyf) - oy;

        const double xa = _.vf * _.vf;
        const double xb = 2 * (c1 * _.vxf + c2 * _.vyf);
        const double xc = c1 * c1 + c2 * c2 - R * R;

        const double discr = xb * xb - 4 * xa * xc;
        if (discr >= 0)
        {
            const double sol_t0 = (-xb + sqrt(discr)) / (2 * xa);
            const double sol_t1 = (-xb - sqrt(discr)) / (2 * xa);

            // Identify the shortest valid collision time:
            if (sol_t0 < _.T_ramp && sol_t1 < _.T_ramp)
                sol_t = -1.0;
            else if (sol_t0 < _.T_ramp && sol_t1 >= T_ramp_thres099)
                sol_t = sol_t1;
            else if (sol_t1 < _.T_ramp && sol_t0 >= T_ramp_thres099)
                sol_t = sol_t0;
            else if (sol_t1 >= T_ramp_thres099 && sol_t0 >= T_ramp_thres099)
                sol_t = std::min(sol_t0, sol_t1);
        }
    }

    // Valid solution?
    if (sol_t < 0) return;
    // Compute the transversed distance:
    double dist;

    if (sol_t < _.T_ramp)
        dist = calc_trans_distance_t_below_Tramp(k2, k4, _.vxi, _.vyi, sol_t);
    else
        dist = (sol_t - _.T_ramp) * V_MAX + calc_trans_distance_t_below_Tramp(
                                                k2, k4, _.vxi, _.vyi, _.T_ramp);

    // Store in the output variable:
    internal_TPObsDistancePostprocess(ox, oy, dist, tp_obstacle_k);
}

void HolonomicBlend::updateTPObstacle(
    double ox, double oy, std::vector<double>& tp_obstacles) const
{
    PERFORMANCE_BENCHMARK;

    for (unsigned int k = 0; k < m_alphaValuesCount; k++)
    {
        updateTPObstacleSingle(ox, oy, k, tp_obstacles[k]);
    }  // end for each "k" alpha
}

void HolonomicBlend::internal_processNewRobotShape()
{
    // Nothing to do in a closed-form PTG.
}

mrpt::kinematics::CVehicleVelCmd::Ptr
    HolonomicBlend::getSupportedKinematicVelocityCommand() const
{
    return mrpt::kinematics::CVehicleVelCmd_Holo::Create();
}

bool   HolonomicBlend::supportVelCmdNOP() const { return true; }
double HolonomicBlend::maxTimeInVelCmdNOP(int path_k) const
{
    //	const double dir_local =
    // CParameterizedTrajectoryGenerator::index2alpha(path_k);

    const size_t nSteps = getPathStepCount(path_k);
    const double max_t =
		PATH_TIME_STEP *
		(nSteps *
		 0.7 /* leave room for obstacle detection ahead when we are far down the predicted PTG path */);
    return max_t;
}

double HolonomicBlend::getPathStepDuration() const { return PATH_TIME_STEP; }

HolonomicBlend::HolonomicBlend() { internal_construct_exprs(); }

HolonomicBlend::HolonomicBlend(
    const mrpt::config::CConfigFileBase& cfg, const std::string& sSection)
{
    internal_construct_exprs();
    HolonomicBlend::loadFromConfigFile(cfg, sSection);
}

HolonomicBlend::~HolonomicBlend() = default;

void HolonomicBlend::internal_construct_exprs()
{
    auto& nds = m_nav_dyn_state;

    const std::map<std::string, double*> vars = {
        {"trimmable_speed", &trimmableSpeed_},  // <== from base class
        {"dir", &m_expr_dir},
        {"target_dir", &m_expr_target_dir},
        {"target_dist", &m_expr_target_dist},
        {"V_MAX", &V_MAX},
        {"W_MAX", &W_MAX},
        {"T_ramp_max", &T_ramp_max},
        {"target_x", &nds.relTarget.x},
        {"target_y", &nds.relTarget.y},
        {"target_phi", &nds.relTarget.phi},
        {"vxi", &nds.curVelLocal.vx},
        {"vyi", &nds.curVelLocal.vy},
        {"wi", &nds.curVelLocal.omega},
        {"target_rel_speed", &nds.targetRelSpeed}};

    m_expr_v.register_symbol_table(vars);
    m_expr_w.register_symbol_table(vars);

    // Default expressions (can be overloaded by values in a config file)
    expr_V      = "V_MAX";
    expr_W      = "W_MAX";
    expr_T_ramp = "T_ramp_max";
}

void HolonomicBlend::internal_initialize(
    [[maybe_unused]] const std::string& cacheFilename,
    [[maybe_unused]] const bool         verbose)
{
    // No need to initialize anything, just do some params sanity checks:
    ASSERT_(T_ramp_max > 0);
    ASSERT_(V_MAX > 0);
    ASSERT_(W_MAX > 0);
    ASSERT_(m_alphaValuesCount > 0);
    ASSERT_(m_robotRadius > 0);

    // Compile user-given expressions:
    m_expr_v.compile(expr_V, {}, "expr_V");
    m_expr_w.compile(expr_W, {}, "expr_w");

#ifdef DO_PERFORMANCE_BENCHMARK
//    tl.dumpAllStats();
#endif

    m_pathStepCountCache.clear();
}

HolonomicBlend::InternalParams
    HolonomicBlend::internal_params_from_dir_and_dynstate(
        const double dir) const
{
    InternalParams p;

    // Default:
    p.T_ramp = T_ramp_max;

#if 0
    // Computes T_ramp, including the case of stop at target:
    const auto& trg     = m_nav_dyn_state.relTarget;
    int         dummy_k = 0;
    double      dummy_d = 0;
    if (trg.x != 0 && trg.y != 0 &&
        CParameterizedTrajectoryGenerator::alpha2index(atan2(trg.y, trg.x)) ==
            CParameterizedTrajectoryGenerator::alpha2index(dir))
    { inverseMap_WS2TP_with_Tramp(trg.x, trg.y, dummy_k, dummy_d, p.T_ramp); }
#endif

    auto lck = mrpt::lockHelper(m_expr_mtx);

    const_cast<double&>(m_expr_dir) = dir;
    p.vf                            = std::abs(m_expr_v.eval());
    p.wf  = mrpt::signWithZero(dir) * std::abs(m_expr_w.eval());
    p.vxi = m_nav_dyn_state.curVelLocal.vx;
    p.vyi = m_nav_dyn_state.curVelLocal.vy;
    p.vxf = p.vf * cos(dir);
    p.vyf = p.vf * sin(dir);

    return p;
}
