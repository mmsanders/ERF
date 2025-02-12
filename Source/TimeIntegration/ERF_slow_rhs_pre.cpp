#include <AMReX_MultiFab.H>
#include <AMReX_ArrayLim.H>
#include <AMReX_BCRec.H>
#include <AMReX_GpuContainers.H>
#include <ERF_Constants.H>
#include <Advection.H>
#include <Diffusion.H>
#include <TimeIntegration.H>
#include <EOS.H>
#include <ERF.H>

#include <TerrainMetrics.H>
#include <IndexDefines.H>
#include <PlaneAverage.H>

using namespace amrex;

void erf_slow_rhs_pre (int /*level*/, int nrk,
                       BoxArray& grids_to_evolve,
                       Vector<MultiFab>& S_rhs,
                       Vector<MultiFab>& S_data,
                       const MultiFab& S_prim,
                       Vector<MultiFab>& S_scratch,
                       const MultiFab& xvel,
                       const MultiFab& yvel,
                       const MultiFab& zvel,
                       std::unique_ptr<MultiFab>& z_t_mf,
                       MultiFab& Omega,
                       const MultiFab& source,
                       MultiFab* Tau11, MultiFab* Tau22, MultiFab* Tau33,
                       MultiFab* Tau12, MultiFab* Tau13, MultiFab* Tau21,
                       MultiFab* Tau23, MultiFab* Tau31, MultiFab* Tau32,
                       MultiFab* eddyDiffs,
#ifdef ERF_USE_MOISTURE
                       const MultiFab& qvapor,
                       const MultiFab& qcloud,
                       const MultiFab& qice,
#endif
                       const amrex::Geometry geom,
                       const SolverChoice& solverChoice,
                       std::unique_ptr<ABLMost>& most,
                       const Gpu::DeviceVector<amrex::BCRec> domain_bcs_type_d,
                       const Vector<amrex::BCRec> domain_bcs_type,
                       std::unique_ptr<MultiFab>& z_phys_nd, std::unique_ptr<MultiFab>& dJ,
                       const MultiFab* r0, const MultiFab* p0,
                       std::unique_ptr<MultiFab>& mapfac_m,
                       std::unique_ptr<MultiFab>& mapfac_u,
                       std::unique_ptr<MultiFab>& mapfac_v,
                       const amrex::Real* dptr_rayleigh_tau, const amrex::Real* dptr_rayleigh_ubar,
                       const amrex::Real* dptr_rayleigh_vbar, const amrex::Real* dptr_rayleigh_thetabar)
{
    BL_PROFILE_REGION("erf_slow_rhs_pre()");

    const MultiFab* t_mean_mf = nullptr;
    if (most) t_mean_mf = most->get_mac_avg(0,2);

    int start_comp = 0;
    int   num_comp = 2;

    const int  l_spatial_order  = solverChoice.spatial_order;
    const bool l_use_terrain    = solverChoice.use_terrain;
    const bool l_moving_terrain = (solverChoice.terrain_type == 1);
    if (l_moving_terrain) AMREX_ALWAYS_ASSERT (l_use_terrain);

    bool       l_use_diff       = ( (solverChoice.molec_diff_type != MolecDiffType::None) ||
                                    (solverChoice.les_type        !=       LESType::None) ||
                                    (solverChoice.pbl_type        !=       PBLType::None) );
    bool       cons_visc        = ( (solverChoice.molec_diff_type == MolecDiffType::Constant) ||
                                    (solverChoice.molec_diff_type == MolecDiffType::ConstantAlpha) );
    bool       l_use_turb       = ( solverChoice.les_type == LESType::Smagorinsky ||
                                    solverChoice.les_type == LESType::Deardorff   ||
                                    solverChoice.pbl_type == PBLType::MYNN25 );

    const amrex::BCRec* bc_ptr   = domain_bcs_type_d.data();
    const amrex::BCRec* bc_ptr_h = domain_bcs_type.data();

    const Box& domain = geom.Domain();
    const int domhi_z = domain.bigEnd()[2];

    const GpuArray<Real, AMREX_SPACEDIM> dxInv = geom.InvCellSizeArray();

#ifdef ERF_USE_MOISTURE
    PlaneAverage state_ave(&(S_data[IntVar::cons]), geom, 2);
    PlaneAverage prim_ave(&S_prim, geom, 2);
    PlaneAverage qv_ave(&qvapor, geom, 2);
    PlaneAverage qc_ave(&qcloud, geom, 2);
    PlaneAverage qi_ave(&qice, geom, 2);
    state_ave.compute_averages(ZDir(), state_ave.field());
    prim_ave.compute_averages(ZDir(), prim_ave.field());
    qv_ave.compute_averages(ZDir(), qv_ave.field());
    qc_ave.compute_averages(ZDir(), qc_ave.field());
    qi_ave.compute_averages(ZDir(), qi_ave.field());

    // get plane averaged data
    int ncell = state_ave.ncell_line();

    Gpu::HostVector<Real> rho_h(ncell), theta_h(ncell),
                          qp_h(ncell), qv_h(ncell),
                          qi_h(ncell), qc_h(ncell);


    state_ave.line_average(Rho_comp, rho_h);
    prim_ave.line_average(PrimTheta_comp, theta_h);
    prim_ave.line_average(PrimQp_comp, qp_h);
    qv_ave.line_average(0, qv_h);
    qi_ave.line_average(0, qi_h);
    qc_ave.line_average(0, qc_h);

    // copy data to device
    Gpu::DeviceVector<Real>   rho_d(ncell);
    Gpu::DeviceVector<Real> theta_d(ncell);
    Gpu::DeviceVector<Real>    qp_d(ncell);
    Gpu::DeviceVector<Real>    qv_d(ncell);
    Gpu::DeviceVector<Real>    qc_d(ncell);
    Gpu::DeviceVector<Real>    qi_d(ncell);

    Gpu::copyAsync(Gpu::hostToDevice, rho_h.begin(), rho_h.end(), rho_d.begin());
    Gpu::copyAsync(Gpu::hostToDevice, theta_h.begin(), theta_h.end(), theta_d.begin());
    Gpu::copyAsync(Gpu::hostToDevice, qp_h.begin(), qp_h.end(), qp_d.begin());
    Gpu::copyAsync(Gpu::hostToDevice, qv_h.begin(), qv_h.end(), qv_d.begin());
    Gpu::copyAsync(Gpu::hostToDevice, qi_h.begin(), qi_h.end(), qi_d.begin());
    Gpu::copyAsync(Gpu::hostToDevice, qc_h.begin(), qc_h.end(), qc_d.begin());
    Gpu::streamSynchronize();

    Real*   rho_d_ptr =   rho_d.data();
    Real* theta_d_ptr = theta_d.data();
    Real*    qp_d_ptr =    qp_d.data();
    Real*    qv_d_ptr =    qv_d.data();
    Real*    qc_d_ptr =    qc_d.data();
    Real*    qi_d_ptr =    qi_d.data();
#endif

    // *************************************************************************
    // Combine external forcing terms
    // *************************************************************************
    const    Array<Real,AMREX_SPACEDIM> grav{0.0, 0.0, -solverChoice.gravity};
    const GpuArray<Real,AMREX_SPACEDIM> grav_gpu{grav[0], grav[1], grav[2]};

    const GpuArray<Real, AMREX_SPACEDIM> ext_forcing = {
       -solverChoice.abl_pressure_grad[0] + solverChoice.abl_geo_forcing[0],
       -solverChoice.abl_pressure_grad[1] + solverChoice.abl_geo_forcing[1],
       -solverChoice.abl_pressure_grad[2] + solverChoice.abl_geo_forcing[2]};

    // *************************************************************************
    // Pre-computed quantities
    // *************************************************************************
    int nvars                     = S_data[IntVar::cons].nComp();
    const BoxArray& ba            = S_data[IntVar::cons].boxArray();
    const DistributionMapping& dm = S_data[IntVar::cons].DistributionMap();

    MultiFab pprime(ba, dm, 1, 1);

    MultiFab* expr    = nullptr;
    MultiFab* dflux_x = nullptr;
    MultiFab* dflux_y = nullptr;
    MultiFab* dflux_z = nullptr;

    if (l_use_diff) {
        expr    = new MultiFab(ba  , dm, 1, IntVect(1,1,0));
        dflux_x = new MultiFab(convert(ba,IntVect(1,0,0)), dm, nvars, 0);
        dflux_y = new MultiFab(convert(ba,IntVect(0,1,0)), dm, nvars, 0);
        dflux_z = new MultiFab(convert(ba,IntVect(0,0,1)), dm, nvars, 0);
    }

    // *************************************************************************
    // Define updates and fluxes in the current RK stage
    // *************************************************************************
#ifdef _OPENMP
#pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
#endif
    for ( MFIter mfi(S_data[IntVar::cons],TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        const Box& valid_bx = grids_to_evolve[mfi.index()];

        // Construct intersection of current tilebox and valid region for updating
        Box bx = mfi.tilebox() & valid_bx;

        Box tbx = surroundingNodes(bx,0);
        Box tby = surroundingNodes(bx,1);
        Box tbz = surroundingNodes(bx,2);

        // We don't compute a source term for z-momentum on the bottom or top boundary
        tbz.growLo(2,-1);
        tbz.growHi(2,-1);

        const Array4<const Real> & cell_data  = S_data[IntVar::cons].array(mfi);
        const Array4<const Real> & cell_prim  = S_prim.array(mfi);
        const Array4<Real> &       cell_rhs   = S_rhs[IntVar::cons].array(mfi);
        const Array4<const Real> & source_fab = source.const_array(mfi);
#ifdef ERF_USE_MOISTURE
        const Array4<const Real> & qv_data = qvapor.array(mfi);
        const Array4<const Real> & qc_data = qcloud.array(mfi);
        const Array4<const Real> & qi_data = qice.array(mfi);
#endif

        // We must initialize these to zero each RK step
        S_scratch[IntVar::xmom][mfi].template setVal<RunOn::Device>(0.);
        S_scratch[IntVar::ymom][mfi].template setVal<RunOn::Device>(0.);
        S_scratch[IntVar::zmom][mfi].template setVal<RunOn::Device>(0.);

        Array4<Real> avg_xmom = S_scratch[IntVar::xmom].array(mfi);
        Array4<Real> avg_ymom = S_scratch[IntVar::ymom].array(mfi);
        Array4<Real> avg_zmom = S_scratch[IntVar::zmom].array(mfi);

        const Array4<const Real> & u = xvel.array(mfi);
        const Array4<const Real> & v = yvel.array(mfi);
        const Array4<const Real> & w = zvel.array(mfi);

        const Array4<const Real>& rho_u = S_data[IntVar::xmom].array(mfi);
        const Array4<const Real>& rho_v = S_data[IntVar::ymom].array(mfi);
        const Array4<const Real>& rho_w = S_data[IntVar::zmom].array(mfi);

        // Map factors
        const Array4<const Real>& mf_m   = mapfac_m->const_array(mfi);
        const Array4<const Real>& mf_u   = mapfac_u->const_array(mfi);
        const Array4<const Real>& mf_v   = mapfac_v->const_array(mfi);

        const Array4<      Real>& omega_arr = Omega.array(mfi);

        Array4<const Real> z_t;
        if (z_t_mf)
            z_t = z_t_mf->array(mfi);
        else
            z_t = Array4<const Real>{};

        const Array4<Real>& rho_u_rhs = S_rhs[IntVar::xmom].array(mfi);
        const Array4<Real>& rho_v_rhs = S_rhs[IntVar::ymom].array(mfi);
        const Array4<Real>& rho_w_rhs = S_rhs[IntVar::zmom].array(mfi);

        const Array4<Real const>& mu_turb = l_use_turb ? eddyDiffs->const_array(mfi) : Array4<const Real>{};

        // Terrain metrics
        const Array4<const Real>& z_nd   = l_use_terrain ? z_phys_nd->const_array(mfi) : Array4<const Real>{};
        const Array4<const Real>& detJ   = l_use_terrain ?        dJ->const_array(mfi) : Array4<const Real>{};

        // Base state
        const Array4<const Real>& r0_arr = r0->const_array(mfi);
        const Array4<const Real>& p0_arr = p0->const_array(mfi);

        const Box& gbx = mfi.growntilebox({1,1,0});
        const Array4<Real> & pp_arr  = pprime.array(mfi);
        {
        BL_PROFILE("slow_rhs_pre_pprime");
        amrex::ParallelFor(gbx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
            //if (cell_data(i,j,k,RhoTheta_comp) < 0.) printf("BAD THETA AT %d %d %d %e %e \n",
            //    i,j,k,cell_data(i,j,k,RhoTheta_comp),cell_data(i,j,k+1,RhoTheta_comp));
            AMREX_ASSERT(cell_data(i,j,k,RhoTheta_comp) > 0.);
            pp_arr(i,j,k) = getPgivenRTh(cell_data(i,j,k,RhoTheta_comp)) - p0_arr(i,j,k);
        });
        } // end profile

        Array4<Real> er_arr;
        if (expr)
            er_arr = expr->array(mfi);
        else
            er_arr = Array4<Real>{};
        {
        BL_PROFILE("slow_rhs_making_er");
        if (l_use_diff)
        {
            const Box& gbx2 = mfi.growntilebox(IntVect(1,1,0));

            if (l_use_terrain) {
                // First create Omega using velocity (not momentum)
                Box gbxo = mfi.nodaltilebox(2);gbxo.grow(IntVect(1,1,0));
                amrex::ParallelFor(gbxo, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                    omega_arr(i,j,k) = (k == 0) ? 0. : OmegaFromW(i,j,k,w(i,j,k),u,v,z_nd,dxInv);
                });

                amrex::ParallelFor(gbx2, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {

                    Real met_u_h_zeta_hi = Compute_h_zeta_AtIface(i+1, j  , k, dxInv, z_nd);
                    Real met_u_h_zeta_lo = Compute_h_zeta_AtIface(i  , j  , k, dxInv, z_nd);

                    Real met_v_h_zeta_hi = Compute_h_zeta_AtJface(i  , j+1, k, dxInv, z_nd);
                    Real met_v_h_zeta_lo = Compute_h_zeta_AtJface(i  , j  , k, dxInv, z_nd);

                    Real Omega_hi = omega_arr(i,j,k+1);
                    Real Omega_lo = omega_arr(i,j,k  );

                    Real mfsq = mf_m(i,j,0)*mf_m(i,j,0);

                    Real expansionRate = (u(i+1,j  ,k)/mf_u(i+1,j,0)*met_u_h_zeta_hi - u(i,j,k)/mf_u(i,j,0)*met_u_h_zeta_lo)*dxInv[0]*mfsq +
                                         (v(i  ,j+1,k)/mf_v(i,j+1,0)*met_v_h_zeta_hi - v(i,j,k)/mf_v(i,j,0)*met_v_h_zeta_lo)*dxInv[1]*mfsq +
                                         (Omega_hi - Omega_lo)*dxInv[2];

                    er_arr(i,j,k) = expansionRate / detJ(i,j,k);
                });

            } else {
                amrex::ParallelFor(gbx2, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                    Real mfsq = mf_m(i,j,0)*mf_m(i,j,0);
                    er_arr(i,j,k) = (u(i+1, j  , k  )/mf_u(i+1,j,0) - u(i, j, k)/mf_u(i,j,0))*dxInv[0]*mfsq +
                                    (v(i  , j+1, k  )/mf_v(i,j+1,0) - v(i, j, k)/mf_v(i,j,0))*dxInv[1]*mfsq +
                                    (w(i  , j  , k+1) - w(i, j, k))*dxInv[2];
                });
            }
        }
        } // end profile

        {
        BL_PROFILE("slow_rhs_making_omega");
            Box gbxo = mfi.nodaltilebox(2);gbxo.grow(IntVect(1,1,0));
            // Now create Omega with momentum (not velocity) with z_t subtracted if moving terrain
            if (l_use_terrain) {
                if (z_t) {
                    amrex::ParallelFor(gbxo, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                        Real rho_at_face = 0.5 * (cell_data(i,j,k,Rho_comp) + cell_data(i,j,k-1,Rho_comp));
                        omega_arr(i,j,k) = (k == 0) ? 0. : OmegaFromW(i,j,k,rho_w(i,j,k),rho_u,rho_v,z_nd,dxInv) -
                            rho_at_face * z_t(i,j,k);
                    });
                } else {
                    amrex::ParallelFor(gbxo, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                        omega_arr(i,j,k) = (k == 0) ? 0. : OmegaFromW(i,j,k,rho_w(i,j,k),rho_u,rho_v,z_nd,dxInv);
                    });
                }
            } else {
                amrex::ParallelFor(gbxo, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                    omega_arr(i,j,k) = rho_w(i,j,k);
                });
            }
        } // end profile

        // No terrain diffusion
        Array4<Real> tau11,tau22,tau33;
        Array4<Real> tau12,tau13,tau23;
        if (Tau11) {
            tau11 = Tau11->array(mfi);
            tau22 = Tau22->array(mfi);
            tau33 = Tau33->array(mfi);
            tau12 = Tau12->array(mfi);
            tau13 = Tau13->array(mfi);
            tau23 = Tau23->array(mfi);
        } else {
            tau11 = Array4<Real>{};
            tau22 = Array4<Real>{};
            tau33 = Array4<Real>{};
            tau12 = Array4<Real>{};
            tau13 = Array4<Real>{};
            tau23 = Array4<Real>{};
        }
        // Terrain diffusion
        Array4<Real> tau21,tau31,tau32;
        if (Tau21) {
            tau21 = Tau21->array(mfi);
            tau31 = Tau31->array(mfi);
            tau32 = Tau32->array(mfi);
        } else {
            tau21 = Array4<Real>{};
            tau31 = Array4<Real>{};
            tau32 = Array4<Real>{};
        }
        {
        BL_PROFILE("slow_rhs_making_strain");
        if (nrk>0 && l_use_diff) {
            Box bxcc  = mfi.growntilebox(IntVect(1,1,0));
            Box tbxxy = bx; tbxxy.convert(IntVect(1,1,0));
            Box tbxxz = bx; tbxxz.convert(IntVect(1,0,1));
            Box tbxyz = bx; tbxyz.convert(IntVect(0,1,1));

            // Fill strain ghost cells for building mu_turb
            tbxxy.growLo(0,1);tbxxy.growLo(1,1);
            tbxxz.growLo(0,1);tbxxz.growLo(1,1);
            tbxyz.growLo(0,1);tbxyz.growLo(1,1);
            tbxxy.growHi(0,1);tbxxy.growHi(1,1);
            tbxxz.growHi(0,1);tbxxz.growHi(1,1);
            tbxyz.growHi(0,1);tbxyz.growHi(1,1);

            if (l_use_terrain) {
                ComputeStrain_T(bxcc, tbxxy, tbxxz, tbxyz,
                                u, v, w,
                                tau11, tau22, tau33,
                                tau12, tau13,
                                tau21, tau23,
                                tau31, tau32,
                                z_nd, bc_ptr_h, dxInv,
                                mf_m, mf_u, mf_v);
            } else {
                ComputeStrain_N(bxcc, tbxxy, tbxxz, tbxyz,
                                u, v, w,
                                tau11, tau22, tau33,
                                tau12, tau13, tau23,
                                bc_ptr_h, dxInv,
                                mf_m, mf_u, mf_v);
            }
        } // l_use_diff
        } // profile

        {
        BL_PROFILE("slow_rhs_making_strain_most");
        // Need to recompute strain at bottom with MOST
        if (nrk==0 && l_use_diff && most) {
            Box tbxxz = bx; tbxxz.convert(IntVect(1,0,1));
            Box tbxyz = bx; tbxyz.convert(IntVect(0,1,1));

            // Only operate on bottom layer
            tbxxz.setBig(2,0);
            tbxyz.setBig(2,0);

            if (l_use_terrain) {
                amrex::ParallelFor(tbxxz,tbxyz,
                [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                    Real GradWz = 0.5 * dxInv[2] * ( w(i  ,j  ,k+1) + w(i-1,j  ,k+1)
                                                   - w(i  ,j  ,k  ) - w(i-1,j  ,k  ) );

                    Real met_h_xi,met_h_zeta;
                    met_h_xi   = Compute_h_xi_AtEdgeCenterJ  (i,j,k,dxInv,z_nd);
                    met_h_zeta = Compute_h_zeta_AtEdgeCenterJ(i,j,k,dxInv,z_nd);

                    tau13(i,j,k) = 0.5 * ( (u(i, j, k) - u(i  , j, k-1))*dxInv[2]/met_h_zeta
                                         + (w(i, j, k) - w(i-1, j, k  ))*dxInv[0]
                                         - (met_h_xi/met_h_zeta)*GradWz );
                    tau31(i,j,k) = tau13(i,j,k);
                },
                [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                    Real GradWz = 0.5 * dxInv[2] * ( w(i  ,j  ,k+1) + w(i  ,j-1,k+1)
                                                   - w(i  ,j  ,k  ) - w(i  ,j-1,k  ) );

                    Real met_h_eta,met_h_zeta;
                    met_h_eta  = Compute_h_eta_AtEdgeCenterI (i,j,k,dxInv,z_nd);
                    met_h_zeta = Compute_h_zeta_AtEdgeCenterI(i,j,k,dxInv,z_nd);

                    tau23(i,j,k) = 0.5 * ( (v(i, j, k) - v(i, j  , k-1))*dxInv[2]/met_h_zeta
                                         + (w(i, j, k) - w(i, j-1, k  ))*dxInv[1]
                                         - (met_h_eta/met_h_zeta)*GradWz );
                    tau32(i,j,k) = tau23(i,j,k);
                });
            } else {
                amrex::ParallelFor(tbxxz,tbxyz,
                [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                    tau13(i,j,k) = 0.5 * ( (u(i, j, k) - u(i, j, k-1))*dxInv[2] + (w(i, j, k) - w(i-1, j, k))*dxInv[0] );
                },
                [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                    tau23(i,j,k) = 0.5 * ( (v(i, j, k) - v(i, j, k-1))*dxInv[2] + (w(i, j, k) - w(i, j-1, k))*dxInv[1] );
                });
            }
        } // nrk==0 && l_use_diff && m_most
        } // profile

        {
        BL_PROFILE("slow_rhs_making_stress");
        if (l_use_diff) {
            Box bxcc  = mfi.growntilebox(IntVect(1,1,0));
            Box tbxxy = bx; tbxxy.convert(IntVect(1,1,0));
            Box tbxxz = bx; tbxxz.convert(IntVect(1,0,1));
            Box tbxyz = bx; tbxyz.convert(IntVect(0,1,1));

            Real mu_eff = 0.;
            if (cons_visc)
                mu_eff += 2.0 * solverChoice.dynamicViscosity;

            if (l_use_terrain) {
                if (cons_visc) {
                    ComputeStressConsVisc_T(bxcc, tbxxy, tbxxz, tbxyz, mu_eff,
                                            tau11, tau22, tau33,
                                            tau12, tau13,
                                            tau21, tau23,
                                            tau31, tau32,
                                            er_arr, z_nd, dxInv);
                } else {
                    ComputeStressVarVisc_T(bxcc, tbxxy, tbxxz, tbxyz, mu_eff, mu_turb,
                                           tau11, tau22, tau33,
                                           tau12, tau13,
                                           tau21, tau23,
                                           tau31, tau32,
                                           er_arr, z_nd, dxInv);
                }
            } else {
                if (cons_visc) {
                    ComputeStressConsVisc_N(bxcc, tbxxy, tbxxz, tbxyz, mu_eff,
                                            tau11, tau22, tau33,
                                            tau12, tau13, tau23,
                                            er_arr);
                } else {
                    ComputeStressVarVisc_N(bxcc, tbxxy, tbxxz, tbxyz, mu_eff, mu_turb,
                                           tau11, tau22, tau33,
                                           tau12, tau13, tau23,
                                           er_arr);
                }
            }
        }
        } // profile

        // **************************************************************************
        // Define updates in the RHS of continuity, temperature, and scalar equations
        // **************************************************************************
        Real fac = 1.0;

        AdvectionSrcForRhoAndTheta(bx, valid_bx, cell_rhs,       // these are being used to build the fluxes
                                   rho_u, rho_v, omega_arr, fac,
                                   avg_xmom, avg_ymom, avg_zmom, // these are being defined from the rho fluxes
                                   cell_prim, z_nd, detJ,
                                   dxInv, mf_m, mf_u, mf_v, l_spatial_order, l_use_terrain);

        if (l_use_diff) {
            Array4<Real> diffflux_x = dflux_x->array(mfi);
            Array4<Real> diffflux_y = dflux_y->array(mfi);
            Array4<Real> diffflux_z = dflux_z->array(mfi);;

            const Array4<const Real> tm_arr = t_mean_mf ? t_mean_mf->const_array(mfi) : Array4<const Real>{};

            // NOTE: No diffusion for continuity, so n starts at 1.
            //       KE calls moved inside DiffSrcForState.
            int n_start = amrex::max(start_comp,RhoTheta_comp);
            int n_end   = start_comp + num_comp - 1;

            if (l_use_terrain) {
                DiffusionSrcForState_T(bx, domain, n_start, n_end, u, v, w,
                                       cell_data, cell_prim, source_fab, cell_rhs,
                                       diffflux_x, diffflux_y, diffflux_z, z_nd, detJ,
                                       dxInv, mf_m, mf_u, mf_v,
                                       mu_turb, solverChoice, tm_arr, grav_gpu, bc_ptr);
            } else {
                DiffusionSrcForState_N(bx, domain, n_start, n_end, u, v, w,
                                       cell_data, cell_prim, source_fab, cell_rhs,
                                       diffflux_x, diffflux_y, diffflux_z,
                                       dxInv, mf_m, mf_u, mf_v,
                                       mu_turb, solverChoice, tm_arr, grav_gpu, bc_ptr);
            }
        }

        // Add Rayleigh damping
        if (solverChoice.use_rayleigh_damping) {
            int n  = RhoTheta_comp;
            int nr = Rho_comp;
            int np = PrimTheta_comp;
            amrex::ParallelFor(bx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
            {
                Real theta = cell_prim(i,j,k,np);
                cell_rhs(i, j, k, n) -= dptr_rayleigh_tau[k] * (theta - dptr_rayleigh_thetabar[k]) * cell_data(i,j,k,nr);
            });
        }

        // Multiply the slow RHS for rho and rhotheta by detJ here so we don't have to later
        if (l_use_terrain && l_moving_terrain) {
            amrex::ParallelFor(bx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
            {
                cell_rhs(i,j,k,Rho_comp)      *= detJ(i,j,k);
                cell_rhs(i,j,k,RhoTheta_comp) *= detJ(i,j,k);
            });
        }

        // *********************************************************************
        // Define updates in the RHS of {x, y, z}-momentum equations
        // *********************************************************************

        AdvectionSrcForMom(tbx, tby, tbz,
                           rho_u_rhs, rho_v_rhs, rho_w_rhs, u, v, w,
                           rho_u    , rho_v    , omega_arr,
                           z_nd, detJ, dxInv, mf_m, mf_u, mf_v,
                           l_spatial_order, l_use_terrain, domhi_z);

        if (l_use_diff) {
            if (l_use_terrain) {
                DiffusionSrcForMom_T(tbx, tby, tbz,
                                     rho_u_rhs, rho_v_rhs, rho_w_rhs,
                                     tau11, tau22, tau33,
                                     tau12, tau13,
                                     tau21, tau23,
                                     tau31, tau32,
                                     cell_data, detJ, solverChoice, dxInv,
                                     mf_m, mf_u, mf_v);
            } else {
                DiffusionSrcForMom_N(tbx, tby, tbz,
                                     rho_u_rhs, rho_v_rhs, rho_w_rhs,
                                     tau11, tau22, tau33,
                                     tau12, tau13, tau23,
                                     cell_data, solverChoice, dxInv,
                                     mf_m, mf_u, mf_v);
            }
        }

        {
        BL_PROFILE("slow_rhs_pre_xmom");
        // ******************************************************************
        // TERRAIN VERSION
        // ******************************************************************
        if (l_use_terrain) {
          amrex::ParallelFor(tbx,
          [=] AMREX_GPU_DEVICE (int i, int j, int k)
          { // x-momentum equation
            // Add pressure gradient
            amrex::Real gpx;

            Real met_h_xi   = Compute_h_xi_AtIface  (i, j, k, dxInv, z_nd);
            Real met_h_zeta = Compute_h_zeta_AtIface(i, j, k, dxInv, z_nd);

            //Note : mx/my == 1, so no map factor needed here
            Real gp_xi = dxInv[0] * (pp_arr(i,j,k) - pp_arr(i-1,j,k));
            Real gp_zeta_on_iface;
            if (k==0) {
                gp_zeta_on_iface = 0.5 * dxInv[2] * (
                                                    pp_arr(i-1,j,k+1) + pp_arr(i,j,k+1)
                                                  - pp_arr(i-1,j,k  ) - pp_arr(i,j,k  ) );
            } else if (k==domhi_z) {
                gp_zeta_on_iface = 0.5 * dxInv[2] * (
                                                    pp_arr(i-1,j,k  ) + pp_arr(i,j,k  )
                                                  - pp_arr(i-1,j,k-1) - pp_arr(i,j,k-1) );
            } else {
                gp_zeta_on_iface = 0.25 * dxInv[2] * (
                                                     pp_arr(i-1,j,k+1) + pp_arr(i,j,k+1)
                                                   - pp_arr(i-1,j,k-1) - pp_arr(i,j,k-1) );
            }
            gpx = gp_xi - (met_h_xi/ met_h_zeta) * gp_zeta_on_iface;
            gpx *= mf_u(i,j,0);

#ifdef ERF_USE_MOISTURE
            Real q = 0.5 * ( cell_prim(i,j,k,PrimQt_comp) + cell_prim(i-1,j,k,PrimQt_comp)
                            +cell_prim(i,j,k,PrimQp_comp) + cell_prim(i-1,j,k,PrimQp_comp) );
            rho_u_rhs(i, j, k) -= gpx / (1.0 + q);
#else
            rho_u_rhs(i, j, k) -= gpx;
#endif
            // Add external drivers
            rho_u_rhs(i, j, k) += ext_forcing[0];

            // Add Coriolis forcing (that assumes east is +x, north is +y)
            if (solverChoice.use_coriolis)
            {
                Real rho_v_loc = 0.25 * (rho_v(i,j+1,k) + rho_v(i,j,k) + rho_v(i-1,j+1,k) + rho_v(i-1,j,k));
                Real rho_w_loc = 0.25 * (rho_w(i,j,k+1) + rho_w(i,j,k) + rho_w(i,j-1,k+1) + rho_w(i,j-1,k));
                rho_u_rhs(i, j, k) += solverChoice.coriolis_factor *
                        (rho_v_loc * solverChoice.sinphi - rho_w_loc * solverChoice.cosphi);
            }

            // Add Rayleigh damping
            if (solverChoice.use_rayleigh_damping)
            {
                Real uu = rho_u(i,j,k) / cell_data(i,j,k,Rho_comp);
                rho_u_rhs(i, j, k) -= dptr_rayleigh_tau[k] * (uu - dptr_rayleigh_ubar[k]) * cell_data(i,j,k,Rho_comp);
            }

            if (l_moving_terrain) {
                rho_u_rhs(i, j, k) *= 0.5 * (detJ(i,j,k) + detJ(i-1,j,k));
            }
        });

        } else {
        // ******************************************************************
        // NON-TERRAIN VERSION
        // ******************************************************************
          amrex::ParallelFor(tbx,
          [=] AMREX_GPU_DEVICE (int i, int j, int k)
          { // x-momentum equation

              Real gpx = dxInv[0] * (pp_arr(i,j,k) - pp_arr(i-1,j,k));
              gpx *= mf_u(i,j,0);

#ifdef ERF_USE_MOISTURE
              Real q = 0.5 * ( cell_prim(i,j,k,PrimQt_comp) + cell_prim(i-1,j,k,PrimQt_comp)
                              +cell_prim(i,j,k,PrimQp_comp) + cell_prim(i-1,j,k,PrimQp_comp) );
              rho_u_rhs(i, j, k) -= gpx / (1.0 + q);
#else
              rho_u_rhs(i, j, k) -= gpx;
#endif
              // Add external drivers
              rho_u_rhs(i, j, k) += ext_forcing[0];

              // Add Coriolis forcing (that assumes east is +x, north is +y)
              if (solverChoice.use_coriolis)
              {
                  Real rho_v_loc = 0.25 * (rho_v(i,j+1,k) + rho_v(i,j,k) + rho_v(i-1,j+1,k) + rho_v(i-1,j,k));
                  Real rho_w_loc = 0.25 * (rho_w(i,j,k+1) + rho_w(i,j,k) + rho_w(i,j-1,k+1) + rho_w(i,j-1,k));
                  rho_u_rhs(i, j, k) += solverChoice.coriolis_factor *
                          (rho_v_loc * solverChoice.sinphi - rho_w_loc * solverChoice.cosphi);
              }

              // Add Rayleigh damping
              if (solverChoice.use_rayleigh_damping)
              {
                  Real uu = rho_u(i,j,k) / cell_data(i,j,k,Rho_comp);
                  rho_u_rhs(i, j, k) -= dptr_rayleigh_tau[k] * (uu - dptr_rayleigh_ubar[k]) * cell_data(i,j,k,Rho_comp);
              }
          });
        } // no terrain
        } // end profile

        {
        BL_PROFILE("slow_rhs_pre_ymom");
        // ******************************************************************
        // TERRAIN VERSION
        // ******************************************************************
        if (l_use_terrain) {
          amrex::ParallelFor(tby,
          [=] AMREX_GPU_DEVICE (int i, int j, int k)
          { // y-momentum equation

              Real met_h_eta  = Compute_h_eta_AtJface (i, j, k, dxInv, z_nd);
              Real met_h_zeta = Compute_h_zeta_AtJface(i, j, k, dxInv, z_nd);

              //Note : mx/my == 1, so no map factor needed here
              Real gp_eta = dxInv[1] * (pp_arr(i,j,k) - pp_arr(i,j-1,k));
              Real gp_zeta_on_jface;
              if(k==0) {
                  gp_zeta_on_jface = 0.5 * dxInv[2] * (
                                                      pp_arr(i,j,k+1) + pp_arr(i,j-1,k+1)
                                                    - pp_arr(i,j,k  ) - pp_arr(i,j-1,k  ) );
              } else if (k==domhi_z) {
                  gp_zeta_on_jface = 0.5 * dxInv[2] * (
                                                      pp_arr(i,j,k  ) + pp_arr(i,j-1,k  )
                                                    - pp_arr(i,j,k-1) - pp_arr(i,j-1,k-1) );
              } else {
                  gp_zeta_on_jface = 0.25 * dxInv[2] * (
                                                       pp_arr(i,j,k+1) + pp_arr(i,j-1,k+1)
                                                     - pp_arr(i,j,k-1) - pp_arr(i,j-1,k-1) );
              }

              Real gpy = gp_eta - (met_h_eta / met_h_zeta) * gp_zeta_on_jface;
              gpy *= mf_v(i,j,0);

#ifdef ERF_USE_MOISTURE
              Real q = 0.5 * ( cell_prim(i,j,k,PrimQt_comp) + cell_prim(i,j-1,k,PrimQt_comp)
                              +cell_prim(i,j,k,PrimQp_comp) + cell_prim(i,j-1,k,PrimQp_comp) );
              rho_v_rhs(i, j, k) -= gpy / (1.0_rt + q);
#else
              rho_v_rhs(i, j, k) -= gpy;
#endif
              // Add external drivers
              rho_v_rhs(i, j, k) += ext_forcing[1];

              // Add Coriolis forcing (that assumes east is +x, north is +y) if (solverChoice.use_coriolis)
              {
                  Real rho_u_loc = 0.25 * (rho_u(i+1,j,k) + rho_u(i,j,k) + rho_u(i+1,j-1,k) + rho_u(i,j-1,k));
                  rho_v_rhs(i, j, k) += -solverChoice.coriolis_factor * rho_u_loc * solverChoice.sinphi;
              }

              // Add Rayleigh damping
              if (solverChoice.use_rayleigh_damping)
              {
                  Real vv = rho_v(i,j,k) / cell_data(i,j,k,Rho_comp);
                  rho_v_rhs(i, j, k) -= dptr_rayleigh_tau[k] * (vv - dptr_rayleigh_vbar[k]) * cell_data(i,j,k,Rho_comp);
              }

              if (l_moving_terrain) {
                  rho_v_rhs(i, j, k) *= 0.5 * (detJ(i,j,k) + detJ(i,j-1,k));
              }
          });

        // ******************************************************************
        // NON-TERRAIN VERSION
        // ******************************************************************
        } else {
          amrex::ParallelFor(tby,
          [=] AMREX_GPU_DEVICE (int i, int j, int k)
          { // y-momentum equation

              Real gpy = dxInv[1] * (pp_arr(i,j,k) - pp_arr(i,j-1,k));
              gpy *= mf_v(i,j,0);

#ifdef ERF_USE_MOISTURE
              Real q = 0.5 * ( cell_prim(i,j,k,PrimQt_comp) + cell_prim(i,j-1,k,PrimQt_comp)
                              +cell_prim(i,j,k,PrimQp_comp) + cell_prim(i,j-1,k,PrimQp_comp) );
              rho_v_rhs(i, j, k) -= gpy / (1.0_rt + q);
#else
              rho_v_rhs(i, j, k) -= gpy;
#endif

              // Add external drivers
              rho_v_rhs(i, j, k) += ext_forcing[1];

              // Add Coriolis forcing (that assumes east is +x, north is +y)
              if (solverChoice.use_coriolis)
              {
                  Real rho_u_loc = 0.25 * (rho_u(i+1,j,k) + rho_u(i,j,k) + rho_u(i+1,j-1,k) + rho_u(i,j-1,k));
                  rho_v_rhs(i, j, k) += -solverChoice.coriolis_factor * rho_u_loc * solverChoice.sinphi;
              }

              // Add Rayleigh damping
              if (solverChoice.use_rayleigh_damping)
              {
                  Real vv = rho_v(i,j,k) / cell_data(i,j,k,Rho_comp);
                  rho_v_rhs(i, j, k) -= dptr_rayleigh_tau[k] * (vv - dptr_rayleigh_vbar[k]) * cell_data(i,j,k,Rho_comp);
              }
          });
        } // no terrain
        } // end profile

        {
        BL_PROFILE("slow_rhs_pre_zmom_2d");
        amrex::Box b2d = tbz;
        b2d.setSmall(2,0);
        b2d.setBig(2,0);
        // Enforce no forcing term at top and bottom boundaries
        amrex::ParallelFor(b2d, [=] AMREX_GPU_DEVICE (int i, int j, int) {
            rho_w_rhs(i,j,        0) = 0.;
            rho_w_rhs(i,j,domhi_z+1) = 0.;
        });
        } // end profile

        {
        BL_PROFILE("slow_rhs_pre_zmom");
        // ******************************************************************
        // TERRAIN VERSION
        // ******************************************************************
        if (l_use_terrain) {
          amrex::ParallelFor(tbz,
          [=] AMREX_GPU_DEVICE (int i, int j, int k) { // z-momentum equation

                Real met_h_zeta = Compute_h_zeta_AtKface(i, j, k, dxInv, z_nd);
                Real gpz = dxInv[2] * ( pp_arr(i,j,k)-pp_arr(i,j,k-1) )  / met_h_zeta;

#ifdef ERF_USE_MOISTURE
                Real q = 0.5 * ( cell_prim(i,j,k,PrimQt_comp) + cell_prim(i,j,k-1,PrimQt_comp)
                                +cell_prim(i,j,k,PrimQp_comp) + cell_prim(i,j,k-1,PrimQp_comp) );
                rho_w_rhs(i, j, k) -= gpz / (1.0_rt + q);
#else
                rho_w_rhs(i, j, k) -= gpz;
#endif

                // Add buoyancy term
#ifdef ERF_USE_MOISTURE
                Real tempp1d = getTgivenRandRTh(rho_d_ptr[k  ], rho_d_ptr[k  ]*theta_d_ptr[k  ]);
                Real tempm1d = getTgivenRandRTh(rho_d_ptr[k-1], rho_d_ptr[k-1]*theta_d_ptr[k-1]);

                Real tempp3d  = getTgivenRandRTh(cell_data(i,j,k  ,Rho_comp), cell_data(i,j,k  ,RhoTheta_comp));
                Real tempm3d  = getTgivenRandRTh(cell_data(i,j,k-1,Rho_comp), cell_data(i,j,k-1,RhoTheta_comp));

                Real qplus = 0.61* ( qv_data(i,j,k)-qv_d_ptr[k]) -
                                    (qc_data(i,j,k)-qc_d_ptr[k]+
                                     qi_data(i,j,k)-qi_d_ptr[k]+
                                     cell_prim(i,j,k,PrimQp_comp)-qp_d_ptr[k])
                           + (tempp3d-tempp1d)/tempp1d*(Real(1.0) + Real(0.61)*qv_d_ptr[k]-qc_d_ptr[k]-qi_d_ptr[k]-qp_d_ptr[k]);

                Real qminus = 0.61 *( qv_data(i,j,k-1)-qv_d_ptr[k-1]) -
                                     (qc_data(i,j,k-1)-qc_d_ptr[k-1]+
                                      qi_data(i,j,k-1)-qi_d_ptr[k-1]+
                                      cell_prim(i,j,k-1,PrimQp_comp)-qp_d_ptr[k-1])
                           + (tempm3d-tempm1d)/tempm1d*(Real(1.0) + Real(0.61)*qv_d_ptr[k-1]-qi_d_ptr[k-1]-qc_d_ptr[k-1]-qp_d_ptr[k-1]);

                Real qavg  = Real(0.5) * (qplus + qminus);
                Real r0avg = Real(0.5) * (r0_arr(i,j,k) + r0_arr(i,j,k-1));

                rho_w_rhs(i, j, k) -= qavg * r0avg * grav_gpu[2];
#else
                rho_w_rhs(i, j, k) += grav_gpu[2] * 0.5 * ( cell_data(i,j,k) + cell_data(i,j,k-1)
                                                             - r0_arr(i,j,k) -    r0_arr(i,j,k-1) );
#endif

                // Add external drivers
                rho_w_rhs(i, j, k) += ext_forcing[2];

                // Add Coriolis forcing (that assumes east is +x, north is +y)
                if (solverChoice.use_coriolis)
                {
                    Real rho_u_loc = 0.25 * (rho_u(i+1,j,k) + rho_u(i,j,k) + rho_u(i+1,j,k-1) + rho_u(i,j,k-1));
                    rho_w_rhs(i, j, k) += solverChoice.coriolis_factor * rho_u_loc * solverChoice.cosphi;
                }

                // Add Rayleigh damping
                if (solverChoice.use_rayleigh_damping)
                {
                    rho_w_rhs(i, j, k) -= dptr_rayleigh_tau[k] * rho_w(i,j,k);
                }

                if (l_use_terrain && l_moving_terrain) {
                     rho_w_rhs(i, j, k) *= 0.5 * (detJ(i,j,k) + detJ(i,j,k-1));
                }
          });

        // ******************************************************************
        // NON-TERRAIN VERSION
        // ******************************************************************
        } else {
          amrex::ParallelFor(tbz,
          [=] AMREX_GPU_DEVICE (int i, int j, int k)
          { // z-momentum equation

                Real gpz = dxInv[2] * ( pp_arr(i,j,k)-pp_arr(i,j,k-1) );

#ifdef ERF_USE_MOISTURE
                Real q = 0.5 * ( cell_prim(i,j,k,PrimQt_comp) + cell_prim(i,j,k-1,PrimQt_comp)
                                +cell_prim(i,j,k,PrimQp_comp) + cell_prim(i,j,k-1,PrimQp_comp) );
                rho_w_rhs(i, j, k) -= gpz / (1.0_rt + q);
#else
                rho_w_rhs(i, j, k) -= gpz;
#endif
                // Add buoyancy term
#ifdef ERF_USE_MOISTURE
                Real tempp1d = getTgivenRandRTh(rho_d_ptr[k  ], rho_d_ptr[k  ]*theta_d_ptr[k  ]);
                Real tempm1d = getTgivenRandRTh(rho_d_ptr[k-1], rho_d_ptr[k-1]*theta_d_ptr[k-1]);

                Real tempp3d  = getTgivenRandRTh(cell_data(i,j,k  ,Rho_comp), cell_data(i,j,k  ,RhoTheta_comp));
                Real tempm3d  = getTgivenRandRTh(cell_data(i,j,k-1,Rho_comp), cell_data(i,j,k-1,RhoTheta_comp));

                Real qplus = 0.61* ( qv_data(i,j,k)-qv_d_ptr[k]) -
                                    (qc_data(i,j,k)-qc_d_ptr[k]+
                                     qi_data(i,j,k)-qi_d_ptr[k]+
                                     cell_prim(i,j,k,PrimQp_comp)-qp_d_ptr[k])
                           + (tempp3d-tempp1d)/tempp1d*(Real(1.0) + Real(0.61)*qv_d_ptr[k]-qc_d_ptr[k]-qi_d_ptr[k]-qp_d_ptr[k]);

                Real qminus = 0.61 *( qv_data(i,j,k-1)-qv_d_ptr[k-1]) -
                                     (qc_data(i,j,k-1)-qc_d_ptr[k-1]+
                                      qi_data(i,j,k-1)-qi_d_ptr[k-1]+
                                      cell_prim(i,j,k-1,PrimQp_comp)-qp_d_ptr[k-1])
                           + (tempm3d-tempm1d)/tempm1d*(Real(1.0) + Real(0.61)*qv_d_ptr[k-1]-qi_d_ptr[k-1]-qc_d_ptr[k-1]-qp_d_ptr[k-1]);

                Real qavg  = Real(0.5) * (qplus + qminus);
                Real r0avg = Real(0.5) * (r0_arr(i,j,k) + r0_arr(i,j,k-1));

                rho_w_rhs(i, j, k) -= qavg * r0avg * grav_gpu[2];
#else
                rho_w_rhs(i, j, k) += grav_gpu[2] * 0.5 * ( cell_data(i,j,k) + cell_data(i,j,k-1)
                                                             - r0_arr(i,j,k) -    r0_arr(i,j,k-1) );
#endif
                // Add external drivers
                rho_w_rhs(i, j, k) += ext_forcing[2];

                // Add Coriolis forcing (that assumes east is +x, north is +y)
                if (solverChoice.use_coriolis)
                {
                    Real rho_u_loc = 0.25 * (rho_u(i+1,j,k) + rho_u(i,j,k) + rho_u(i+1,j,k-1) + rho_u(i,j,k-1));
                    rho_w_rhs(i, j, k) += solverChoice.coriolis_factor * rho_u_loc * solverChoice.cosphi;
                }

                // Add Rayleigh damping
                if (solverChoice.use_rayleigh_damping)
                {
                    rho_w_rhs(i, j, k) -= dptr_rayleigh_tau[k] * rho_w(i,j,k);
                }
        });
        } // no terrain
        } // end profile
    } // mfi

    if (l_use_diff) {
        delete expr;
        delete dflux_x;
        delete dflux_y;
        delete dflux_z;
    }
}
