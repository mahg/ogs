/**
 * \copyright
 * Copyright (c) 2012-2017, OpenGeoSys Community (http://www.opengeosys.org)
 *            Distributed under a Modified BSD License.
 *              See accompanying file LICENSE.txt or
 *              http://www.opengeosys.org/project/license
 *
 */

#pragma once

#include <vector>

#include "HeatConductionProcessData.h"
#include "MathLib/LinAlg/Eigen/EigenMapTools.h"
#include "NumLib/Extrapolation/ExtrapolatableElement.h"
#include "NumLib/Fem/FiniteElement/TemplateIsoparametric.h"
#include "NumLib/Fem/ShapeMatrixPolicy.h"
#include "ProcessLib/LocalAssemblerInterface.h"
#include "ProcessLib/LocalAssemblerTraits.h"
#include "ProcessLib/Parameter/Parameter.h"
#include "ProcessLib/Utils/InitShapeMatrices.h"

// For coupling
#include "ProcessLib/LiquidFlow/LiquidFlowProcess.h"
#include "ProcessLib/LiquidFlow/LiquidFlowMaterialProperties.h"

namespace ProcessLib
{
namespace HeatConduction
{
const unsigned NUM_NODAL_DOF = 1;

class HeatConductionLocalAssemblerInterface
    : public ProcessLib::LocalAssemblerInterface,
      public NumLib::ExtrapolatableElement
{
public:
    virtual std::vector<double> const& getIntPtHeatFluxX(
        const double /*t*/,
        GlobalVector const& /*current_solution*/,
        NumLib::LocalToGlobalIndexMap const& /*dof_table*/,
        std::vector<double>& /*cache*/) const = 0;

    virtual std::vector<double> const& getIntPtHeatFluxY(
        const double /*t*/,
        GlobalVector const& /*current_solution*/,
        NumLib::LocalToGlobalIndexMap const& /*dof_table*/,
        std::vector<double>& /*cache*/) const = 0;

    virtual std::vector<double> const& getIntPtHeatFluxZ(
        const double /*t*/,
        GlobalVector const& /*current_solution*/,
        NumLib::LocalToGlobalIndexMap const& /*dof_table*/,
        std::vector<double>& /*cache*/) const = 0;
};

template <typename ShapeFunction, typename IntegrationMethod,
          unsigned GlobalDim>
class LocalAssemblerData : public HeatConductionLocalAssemblerInterface
{
    using ShapeMatricesType = ShapeMatrixPolicyType<ShapeFunction, GlobalDim>;
    using ShapeMatrices = typename ShapeMatricesType::ShapeMatrices;

    using LocalAssemblerTraits = ProcessLib::LocalAssemblerTraits<
        ShapeMatricesType, ShapeFunction::NPOINTS, NUM_NODAL_DOF, GlobalDim>;

    using NodalMatrixType = typename LocalAssemblerTraits::LocalMatrix;
    using NodalVectorType = typename LocalAssemblerTraits::LocalVector;
    using GlobalDimVectorType = typename ShapeMatricesType::GlobalDimVectorType;

public:
    /// The thermal_conductivity factor is directly integrated into the local
    /// element matrix.
    LocalAssemblerData(MeshLib::Element const& element,
                       std::size_t const local_matrix_size,
                       bool is_axially_symmetric,
                       unsigned const integration_order,
                       HeatConductionProcessData const& process_data)
        : _element(element),
          _process_data(process_data),
          _integration_method(integration_order),
          _shape_matrices(initShapeMatrices<ShapeFunction, ShapeMatricesType,
                                            IntegrationMethod, GlobalDim>(
              element, is_axially_symmetric, _integration_method)),
          _heat_fluxes(
              GlobalDim,
              std::vector<double>(_integration_method.getNumberOfPoints()))
    {
        // This assertion is valid only if all nodal d.o.f. use the same shape
        // matrices.
        assert(local_matrix_size == ShapeFunction::NPOINTS * NUM_NODAL_DOF);
        (void)local_matrix_size;
    }

    void assemble(double const t, std::vector<double> const& local_x,
                  std::vector<double>& local_M_data,
                  std::vector<double>& local_K_data,
                  std::vector<double>& /*local_b_data*/) override
    {
        auto const local_matrix_size = local_x.size();
        // This assertion is valid only if all nodal d.o.f. use the same shape
        // matrices.
        assert(local_matrix_size == ShapeFunction::NPOINTS * NUM_NODAL_DOF);

        auto local_M = MathLib::createZeroedMatrix<NodalMatrixType>(
            local_M_data, local_matrix_size, local_matrix_size);
        auto local_K = MathLib::createZeroedMatrix<NodalMatrixType>(
            local_K_data, local_matrix_size, local_matrix_size);

        unsigned const n_integration_points =
            _integration_method.getNumberOfPoints();

        SpatialPosition pos;
        pos.setElementID(_element.getID());

        for (unsigned ip = 0; ip < n_integration_points; ip++)
        {
            pos.setIntegrationPoint(ip);
            auto const& sm = _shape_matrices[ip];
            auto const& wp = _integration_method.getWeightedPoint(ip);
            auto const k = _process_data.thermal_conductivity(t, pos)[0];
            auto const heat_capacity = _process_data.heat_capacity(t, pos)[0];
            auto const density = _process_data.density(t, pos)[0];

            local_K.noalias() += sm.dNdx.transpose() * k * sm.dNdx * sm.detJ *
                                 wp.getWeight() * sm.integralMeasure;
            local_M.noalias() += sm.N.transpose() * density * heat_capacity *
                                 sm.N * sm.detJ * wp.getWeight() *
                                 sm.integralMeasure;
        }
    }

    void assembleWithCoupledTerm(
        double const t, std::vector<double> const& local_x,
        std::vector<double>& local_M_data, std::vector<double>& local_K_data,
        std::vector<double>& /*local_b_data*/,
        LocalCouplingTerm const& coupled_term) override;

    void computeSecondaryVariableConcrete(
        const double t, std::vector<double> const& local_x) override
    {
        auto const local_matrix_size = local_x.size();
        // This assertion is valid only if all nodal d.o.f. use the same shape
        // matrices.
        assert(local_matrix_size == ShapeFunction::NPOINTS * NUM_NODAL_DOF);

        unsigned const n_integration_points =
            _integration_method.getNumberOfPoints();

        SpatialPosition pos;
        pos.setElementID(_element.getID());
        const auto local_x_vec =
            MathLib::toVector<NodalVectorType>(local_x, local_matrix_size);

        for (unsigned ip = 0; ip < n_integration_points; ip++)
        {
            pos.setIntegrationPoint(ip);
            auto const& sm = _shape_matrices[ip];
            auto const k = _process_data.thermal_conductivity(t, pos)[0];
            // heat flux only computed for output.
            GlobalDimVectorType const heat_flux = -k * sm.dNdx * local_x_vec;

            for (unsigned d = 0; d < GlobalDim; ++d)
            {
                _heat_fluxes[d][ip] = heat_flux[d];
            }
        }
    }

    Eigen::Map<const Eigen::RowVectorXd> getShapeMatrix(
        const unsigned integration_point) const override
    {
        auto const& N = _shape_matrices[integration_point].N;

        // assumes N is stored contiguously in memory
        return Eigen::Map<const Eigen::RowVectorXd>(N.data(), N.size());
    }

    std::vector<double> const& getIntPtHeatFluxX(
        const double /*t*/,
        GlobalVector const& /*current_solution*/,
        NumLib::LocalToGlobalIndexMap const& /*dof_table*/,
        std::vector<double>& /*cache*/) const override
    {
        assert(!_heat_fluxes.empty());
        return _heat_fluxes[0];
    }

    std::vector<double> const& getIntPtHeatFluxY(
        const double /*t*/,
        GlobalVector const& /*current_solution*/,
        NumLib::LocalToGlobalIndexMap const& /*dof_table*/,
        std::vector<double>& /*cache*/) const override
    {
        assert(_heat_fluxes.size() > 1);
        return _heat_fluxes[1];
    }

    std::vector<double> const& getIntPtHeatFluxZ(
        const double /*t*/,
        GlobalVector const& /*current_solution*/,
        NumLib::LocalToGlobalIndexMap const& /*dof_table*/,
        std::vector<double>& /*cache*/) const override
    {
        assert(_heat_fluxes.size() > 2);
        return _heat_fluxes[2];
    }

private:
    MeshLib::Element const& _element;
    HeatConductionProcessData const& _process_data;

    IntegrationMethod const _integration_method;
    std::vector<ShapeMatrices, Eigen::aligned_allocator<ShapeMatrices>>
        _shape_matrices;

    std::vector<std::vector<double>> _heat_fluxes;

    /**
     * @brief Assemble local matrices and vectors of the equations of
     *        heat transport process in porous media with full saturated liquid
     *        flow.
     *
     * @param t                          Time.
     * @param material_id                Material ID of the element.
     * @param pos                        Position (t, x) of the element.
     * @param gravitational_axis_id      The ID of the axis, which indicates the
     *                                   direction of gravity portion of the
     *                                   Darcy's velocity.
     * @param gravitational_acceleration Gravitational acceleration, 9.81 in the
     *                                   SI unit standard.
     * @param permeability               Intrinsic permeability of liquid
     *                                   in porous media.
     * @param liquid_flow_properties     Liquid flow properties.
     * @param local_x                    Local vector of unknowns, e.g.
     *                                   nodal temperatures of the element.
     * @param local_p                    Local vector of nodal pore pressures.
     * @param local_M_data               Local mass matrix.
     * @param local_K_data               Local Laplace matrix.
     */
    template <typename LiquidFlowVelocityCalculator>
    void assembleHeatTransportLiquidFlow(
        double const t, int const material_id, SpatialPosition& pos,
        int const gravitational_axis_id,
        double const gravitational_acceleration,
        Eigen::MatrixXd const& permeability,
        ProcessLib::LiquidFlow::LiquidFlowMaterialProperties const&
            liquid_flow_properties,
        std::vector<double> const& local_x, std::vector<double> const& local_p,
        std::vector<double>& local_M_data, std::vector<double>& local_K_data);

    /// Calculator of liquid flow velocity for isotropic permeability
    /// tensor
    struct IsotropicLiquidFlowVelocityCalculator
    {
        static GlobalDimVectorType calculateVelocity(
            Eigen::Map<const NodalVectorType> const& local_p,
            ShapeMatrices const& sm, Eigen::MatrixXd const& permeability,
            double const mu, double const rho_g,
            int const gravitational_axis_id)
        {
            const double K = permeability(0, 0) / mu;
            // Compute the velocity
            GlobalDimVectorType darcy_velocity = -K * sm.dNdx * local_p;
            // gravity term
            if (gravitational_axis_id >= 0)
                darcy_velocity[gravitational_axis_id] -= K * rho_g;
            return darcy_velocity;
        }
    };

    /// Calculator of liquid flow velocity for anisotropic permeability
    /// tensor
    struct AnisotropicLiquidFlowVelocityCalculator
    {
        static GlobalDimVectorType calculateVelocity(
            Eigen::Map<const NodalVectorType> const& local_p,
            ShapeMatrices const& sm, Eigen::MatrixXd const& permeability,
            double const mu, double const rho_g,
            int const gravitational_axis_id)
        {
            GlobalDimVectorType darcy_velocity =
                -permeability * sm.dNdx * local_p / mu;
            if (gravitational_axis_id >= 0)
            {
                darcy_velocity.noalias() -=
                    rho_g * permeability.col(gravitational_axis_id) / mu;
            }
            return darcy_velocity;
        }
    };
};

}  // namespace HeatConduction
}  // namespace ProcessLib

#include "HeatConductionFEM-impl.h"
