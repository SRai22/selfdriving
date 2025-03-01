/* +------------------------------------------------------------------------+
   |                     Mobile Robot Programming Toolkit (MRPT)            |
   |                          https://www.mrpt.org/                         |
   |                                                                        |
   | Copyright (c) 2005-2023, Individual contributors, see AUTHORS file     |
   | See: https://www.mrpt.org/Authors - All rights reserved.               |
   | Released under BSD License. See: https://www.mrpt.org/License          |
   +------------------------------------------------------------------------+ */
#pragma once

#include <selfdriving/ptgs/DiffDriveCollisionGridBased.h>
#include <selfdriving/ptgs/SpeedTrimmablePTG.h>

namespace selfdriving::ptg
{
/** A PTG for circular paths ("C" type PTG in papers).
 * - **Compatible kinematics**: differential-driven / Ackermann steering
 * - **Compatible robot shape**: Arbitrary 2D polygon
 * - **PTG parameters**: Use the app `ptg-configurator`
 *
 * This PT generator functions are:
 *
 * \f[ v(\alpha) = V_{MAX} sign(K) \f]
 * \f[ \omega(\alpha) = \dfrac{\alpha}{\pi} W_{MAX} sign(K) \f]
 *
 * So, the radius of curvature of each trajectory is constant for each "alpha"
 * value (the trajectory parameter):
 *
 *  \f[ R(\alpha) = \dfrac{v}{\omega} = \dfrac{V_{MAX}}{W_{MAX}}
 * \dfrac{\pi}{\alpha} \f]
 *
 * from which a minimum radius of curvature can be set by selecting the
 * appropriate values of V_MAX and W_MAX,
 * knowning that \f$ \alpha \in (-\pi,\pi) \f$.
 *
 *  ![C-PTG path examples](PTG1_paths.png)
 *
 * \note [Before MRPT 1.5.0 this was named CPTG1]
 *  \ingroup nav_tpspace
 */
class DiffDrive_C : public DiffDriveCollisionGridBased, public SpeedTrimmablePTG
{
    DEFINE_SERIALIZABLE(DiffDrive_C, selfdriving::ptg)
   public:
    DiffDrive_C() = default;
    DiffDrive_C(
        const mrpt::config::CConfigFileBase& cfg, const std::string& sSection)
    {
        DiffDrive_C::loadFromConfigFile(cfg, sSection);
    }
    void loadFromConfigFile(
        const mrpt::config::CConfigFileBase& cfg,
        const std::string&                   sSection) override;
    void saveToConfigFile(
        mrpt::config::CConfigFileBase& cfg,
        const std::string&             sSection) const override;

    std::string getDescription() const override;
    bool        inverseMap_WS2TP(
               double x, double y, int& out_k, double& out_d,
               double tolerance_dist = 0.10) const override;
    bool PTG_IsIntoDomain(double x, double y) const override;
    void ptgDiffDriveSteeringFunction(
        float alpha, float t, float x, float y, float phi, float& v,
        float& w) const override;
    void loadDefaultParams() override;

   protected:
    /** A generation parameter */
    double K{0};
};
}  // namespace selfdriving::ptg
