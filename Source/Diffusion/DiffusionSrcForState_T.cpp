#include <Diffusion.H>
#include <EddyViscosity.H>
#include <TerrainMetrics.H>
#include <ComputeQKESourceTerm.H>

using namespace amrex;

void
DiffusionSrcForState_T (const amrex::Box& bx, const amrex::Box& domain, int n_start, int n_end,
                        const Array4<const Real>& u,
                        const Array4<const Real>& v,
                        const Array4<const Real>& w,
                        const Array4<const Real>& cell_data,
                        const Array4<const Real>& cell_prim,
                        const Array4<const Real>& source_fab,
                        const Array4<Real>& cell_rhs,
                        const Array4<Real>& xflux,
                        const Array4<Real>& yflux,
                        const Array4<Real>& zflux,
                        const Array4<const Real>& z_nd,
                        const Array4<const Real>& detJ,
                        const amrex::GpuArray<Real, AMREX_SPACEDIM>& dxInv,
                        const Array4<const Real>& mf_m,
                        const Array4<const Real>& mf_u,
                        const Array4<const Real>& mf_v,
                        const Array4<const Real>& mu_turb,
                        const SolverChoice &solverChoice,
                        const Array4<const Real>& tm_arr,
                        const amrex::GpuArray<Real,AMREX_SPACEDIM> grav_gpu,
                        const amrex::BCRec* bc_ptr)
{
    BL_PROFILE_VAR("DiffusionSrcForState_T()",DiffusionSrcForState_T);

    const Real dx_inv = dxInv[0];
    const Real dy_inv = dxInv[1];
    const Real dz_inv = dxInv[2];

    bool l_use_QKE       = solverChoice.use_QKE && solverChoice.advect_QKE;
    bool l_use_deardorff = (solverChoice.les_type == LESType::Deardorff);
    Real l_Delta         = std::pow(dx_inv * dy_inv * dz_inv,-1./3.);
    Real l_C_e           = solverChoice.Ce;

    bool l_consA  = (solverChoice.molec_diff_type == MolecDiffType::ConstantAlpha);
    bool l_turb   = ( (solverChoice.les_type == LESType::Smagorinsky) ||
                      (solverChoice.les_type == LESType::Deardorff  ) ||
                      (solverChoice.pbl_type == PBLType::MYNN25     ) );

    int l_use_terrain = solverChoice.use_terrain;

    const Box xbx = surroundingNodes(bx,0);
    const Box ybx = surroundingNodes(bx,1);
    const Box zbx = surroundingNodes(bx,2);
    Box zbx3 = zbx;

    const int ncomp      = n_end - n_start + 1;
    const int qty_offset = RhoTheta_comp;

    // Theta, KE, QKE, Scalar
    Vector<Real>    alpha_eff(NUM_PRIM, 0.0);
    if (l_consA) {
    for (int i = 0; i < NUM_PRIM; ++i) {
       switch (i) {
           case PrimTheta_comp:
            alpha_eff[PrimTheta_comp] = solverChoice.alpha_T;
            break;
           case PrimScalar_comp:
            alpha_eff[PrimScalar_comp] = solverChoice.alpha_C;
            break;
#ifdef ERF_USE_MOISTURE
           case PrimQt_comp:
            alpha_eff[PrimQt_comp] = solverChoice.alpha_C;
            break;
           case PrimQp_comp:
            alpha_eff[PrimQp_comp] = solverChoice.alpha_C;
            break;
#endif
           default:
            alpha_eff[i] = 0.0;
            break;
      }
       }
    } else {
        for (int i = 0; i < NUM_PRIM; ++i) {
           switch (i) {
               case PrimTheta_comp:
                    alpha_eff[PrimTheta_comp] = solverChoice.rhoAlpha_T;
                    break;
               case PrimScalar_comp:
                    alpha_eff[PrimScalar_comp] = solverChoice.rhoAlpha_C;
                    break;
#ifdef ERF_USE_MOISTURE
               case PrimQt_comp:
                    alpha_eff[PrimQt_comp] = solverChoice.rhoAlpha_C;
                    break;
               case PrimQp_comp:
                    alpha_eff[PrimQp_comp] = solverChoice.rhoAlpha_C;
                    break;
#endif
               default:
                    alpha_eff[i] = 0.0;
                    break;
          }
       }
    }

    Vector<int> eddy_diff_idx{EddyDiff::Theta_h, EddyDiff::KE_h, EddyDiff::QKE_h, EddyDiff::Scalar_h};
    Vector<int> eddy_diff_idy{EddyDiff::Theta_h, EddyDiff::KE_h, EddyDiff::QKE_h, EddyDiff::Scalar_h};
    Vector<int> eddy_diff_idz{EddyDiff::Theta_v, EddyDiff::KE_v, EddyDiff::QKE_v, EddyDiff::Scalar_v};

    // Device vectors
    Gpu::AsyncVector<Real> alpha_eff_d;
    Gpu::AsyncVector<int>  eddy_diff_idx_d,eddy_diff_idy_d,eddy_diff_idz_d;
    alpha_eff_d.resize(alpha_eff.size());
    eddy_diff_idx_d.resize(eddy_diff_idx.size());
    eddy_diff_idy_d.resize(eddy_diff_idy.size());
    eddy_diff_idz_d.resize(eddy_diff_idz.size());

    Gpu::copy(Gpu::hostToDevice, alpha_eff.begin(), alpha_eff.end(), alpha_eff_d.begin());
    Gpu::copy(Gpu::hostToDevice, eddy_diff_idx.begin(), eddy_diff_idx.end(), eddy_diff_idx_d.begin());
    Gpu::copy(Gpu::hostToDevice, eddy_diff_idy.begin(), eddy_diff_idy.end(), eddy_diff_idy_d.begin());
    Gpu::copy(Gpu::hostToDevice, eddy_diff_idz.begin(), eddy_diff_idz.end(), eddy_diff_idz_d.begin());

    // Capture pointers for device code
    Real* d_alpha_eff     = alpha_eff_d.data();
    int*  d_eddy_diff_idx = eddy_diff_idx_d.data();
    int*  d_eddy_diff_idy = eddy_diff_idy_d.data();
    int*  d_eddy_diff_idz = eddy_diff_idz_d.data();

    // RhoAlpha & Turb model
    if (l_consA && l_turb) {
        amrex::ParallelFor(xbx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real rhoFace  = 0.5 * ( cell_data(i, j, k, Rho_comp) + cell_data(i-1, j, k, Rho_comp) );
            Real rhoAlpha = rhoFace * d_alpha_eff[prim_index];
            rhoAlpha += 0.5 * ( mu_turb(i  , j, k, d_eddy_diff_idx[prim_index])
                              + mu_turb(i-1, j, k, d_eddy_diff_idx[prim_index]) );

            Real met_h_xi,met_h_zeta;
            met_h_xi   = Compute_h_xi_AtIface  (i,j,k,dxInv,z_nd);
            met_h_zeta = Compute_h_zeta_AtIface(i,j,k,dxInv,z_nd);

            Real GradCz = 0.25 * dz_inv * ( cell_prim(i, j, k+1, prim_index) + cell_prim(i-1, j, k+1, prim_index)
                                          - cell_prim(i, j, k-1, prim_index) - cell_prim(i-1, j, k-1, prim_index) );
            Real GradCx =        dx_inv * ( cell_prim(i, j, k  , prim_index) - cell_prim(i-1, j, k  , prim_index) );

            xflux(i,j,k,qty_index) = rhoAlpha * mf_u(i,j,0) * ( GradCx - (met_h_xi/met_h_zeta)*GradCz );
        });
        amrex::ParallelFor(ybx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real rhoFace  = 0.5 * ( cell_data(i, j, k, Rho_comp) + cell_data(i, j-1, k, Rho_comp) );
            Real rhoAlpha = rhoFace * d_alpha_eff[prim_index];
            rhoAlpha += 0.5 * ( mu_turb(i, j  , k, d_eddy_diff_idy[prim_index])
                              + mu_turb(i, j-1, k, d_eddy_diff_idy[prim_index]) );

            Real met_h_eta,met_h_zeta;
            met_h_eta  = Compute_h_eta_AtJface (i,j,k,dxInv,z_nd);
            met_h_zeta = Compute_h_zeta_AtJface(i,j,k,dxInv,z_nd);

            Real GradCz = 0.25 * dz_inv * ( cell_prim(i, j, k+1, prim_index) + cell_prim(i, j-1, k+1, prim_index)
                                          - cell_prim(i, j, k-1, prim_index) - cell_prim(i, j-1, k-1, prim_index) );
            Real GradCy =        dy_inv * ( cell_prim(i, j, k  , prim_index) - cell_prim(i, j-1, k  , prim_index) );

            yflux(i,j,k,qty_index) = rhoAlpha * mf_v(i,j,0) * ( GradCy - (met_h_eta/met_h_zeta)*GradCz );
        });
        amrex::ParallelFor(zbx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real rhoFace  = 0.5 * ( cell_data(i, j, k, Rho_comp) + cell_data(i, j, k-1, Rho_comp) );
            Real rhoAlpha = rhoFace * d_alpha_eff[prim_index];
            rhoAlpha += 0.5 * ( mu_turb(i, j, k  , d_eddy_diff_idz[prim_index])
                              + mu_turb(i, j, k-1, d_eddy_diff_idz[prim_index]) );

            Real met_h_zeta;
            met_h_zeta = Compute_h_zeta_AtKface(i,j,k,dxInv,z_nd);

            Real GradCz = dz_inv * ( cell_prim(i, j, k, prim_index) - cell_prim(i, j, k-1, prim_index) );

            zflux(i,j,k,qty_index) = rhoAlpha * GradCz / met_h_zeta;
        });
    // Alpha & Turb model
    } else if (l_turb) {
        amrex::ParallelFor(xbx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real Alpha = d_alpha_eff[prim_index];
            Alpha += 0.5 * ( mu_turb(i  , j, k, d_eddy_diff_idx[prim_index])
                           + mu_turb(i-1, j, k, d_eddy_diff_idx[prim_index]) );

            Real met_h_xi,met_h_zeta;
            met_h_xi   = Compute_h_xi_AtIface  (i,j,k,dxInv,z_nd);
            met_h_zeta = Compute_h_zeta_AtIface(i,j,k,dxInv,z_nd);

            Real GradCz = 0.25 * dz_inv * ( cell_prim(i, j, k+1, prim_index) + cell_prim(i-1, j, k+1, prim_index)
                                          - cell_prim(i, j, k-1, prim_index) - cell_prim(i-1, j, k-1, prim_index) );
            Real GradCx =        dx_inv * ( cell_prim(i, j, k  , prim_index) - cell_prim(i-1, j, k  , prim_index) );

            xflux(i,j,k,qty_index) = Alpha * mf_u(i,j,0) * ( GradCx - (met_h_xi/met_h_zeta)*GradCz );
        });
        amrex::ParallelFor(ybx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real Alpha = d_alpha_eff[prim_index];
            Alpha += 0.5 * ( mu_turb(i, j  , k, d_eddy_diff_idy[prim_index])
                           + mu_turb(i, j-1, k, d_eddy_diff_idy[prim_index]) );

            Real met_h_eta,met_h_zeta;
            met_h_eta  = Compute_h_eta_AtJface (i,j,k,dxInv,z_nd);
            met_h_zeta = Compute_h_zeta_AtJface(i,j,k,dxInv,z_nd);

            Real GradCz = 0.25 * dz_inv * ( cell_prim(i, j, k+1, prim_index) + cell_prim(i, j-1, k+1, prim_index)
                                          - cell_prim(i, j, k-1, prim_index) - cell_prim(i, j-1, k-1, prim_index) );
            Real GradCy =        dy_inv * ( cell_prim(i, j, k  , prim_index) - cell_prim(i, j-1, k  , prim_index) );

            yflux(i,j,k,qty_index) = Alpha * mf_v(i,j,0) * ( GradCy - (met_h_eta/met_h_zeta)*GradCz );
        });
        amrex::ParallelFor(zbx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real Alpha = d_alpha_eff[prim_index];

            Alpha += 0.5 * ( mu_turb(i, j, k  , d_eddy_diff_idz[prim_index])
                           + mu_turb(i, j, k-1, d_eddy_diff_idz[prim_index]) );

            Real met_h_zeta;
            met_h_zeta = Compute_h_zeta_AtKface(i,j,k,dxInv,z_nd);

            Real GradCz = dz_inv * ( cell_prim(i, j, k, prim_index) - cell_prim(i, j, k-1, prim_index) );

            zflux(i,j,k,qty_index) = Alpha * GradCz / met_h_zeta;
        });
    // RhoAlpha
    } else if(l_consA) {
        amrex::ParallelFor(xbx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real rhoFace  = 0.5 * ( cell_data(i, j, k, Rho_comp) + cell_data(i-1, j, k, Rho_comp) );
            Real rhoAlpha = rhoFace * d_alpha_eff[prim_index];

            Real met_h_xi,met_h_zeta;
            met_h_xi   = Compute_h_xi_AtIface  (i,j,k,dxInv,z_nd);
            met_h_zeta = Compute_h_zeta_AtIface(i,j,k,dxInv,z_nd);

            Real GradCz = 0.25 * dz_inv * ( cell_prim(i, j, k+1, prim_index) + cell_prim(i-1, j, k+1, prim_index)
                                          - cell_prim(i, j, k-1, prim_index) - cell_prim(i-1, j, k-1, prim_index) );
            Real GradCx =        dx_inv * ( cell_prim(i, j, k  , prim_index) - cell_prim(i-1, j, k  , prim_index) );

            xflux(i,j,k,qty_index) = rhoAlpha * mf_u(i,j,0) * ( GradCx - (met_h_xi/met_h_zeta)*GradCz );
        });
        amrex::ParallelFor(ybx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real rhoFace  = 0.5 * ( cell_data(i, j, k, Rho_comp) + cell_data(i, j-1, k, Rho_comp) );
            Real rhoAlpha = rhoFace * d_alpha_eff[prim_index];

            Real met_h_eta,met_h_zeta;
            met_h_eta  = Compute_h_eta_AtJface (i,j,k,dxInv,z_nd);
            met_h_zeta = Compute_h_zeta_AtJface(i,j,k,dxInv,z_nd);

            Real GradCz = 0.25 * dz_inv * ( cell_prim(i, j, k+1, prim_index) + cell_prim(i, j-1, k+1, prim_index)
                                          - cell_prim(i, j, k-1, prim_index) - cell_prim(i, j-1, k-1, prim_index) );
            Real GradCy =        dy_inv * ( cell_prim(i, j, k  , prim_index) - cell_prim(i, j-1, k  , prim_index) );

            yflux(i,j,k,qty_index) = rhoAlpha * mf_v(i,j,0) * ( GradCy - (met_h_eta/met_h_zeta)*GradCz );
        });
        amrex::ParallelFor(zbx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real rhoFace  = 0.5 * ( cell_data(i, j, k, Rho_comp) + cell_data(i, j, k-1, Rho_comp) );
            Real rhoAlpha = rhoFace * d_alpha_eff[prim_index];

            Real met_h_zeta;
            met_h_zeta = Compute_h_zeta_AtKface(i,j,k,dxInv,z_nd);

            Real GradCz = dz_inv * ( cell_prim(i, j, k, prim_index) - cell_prim(i, j, k-1, prim_index) );

            zflux(i,j,k,qty_index) = rhoAlpha * GradCz / met_h_zeta;
        });
    // Alpha
    } else {
        amrex::ParallelFor(xbx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real Alpha = d_alpha_eff[prim_index];

            Real met_h_xi,met_h_zeta;
            met_h_xi   = Compute_h_xi_AtIface  (i,j,k,dxInv,z_nd);
            met_h_zeta = Compute_h_zeta_AtIface(i,j,k,dxInv,z_nd);

            Real GradCz = 0.25 * dz_inv * ( cell_prim(i, j, k+1, prim_index) + cell_prim(i-1, j, k+1, prim_index)
                                          - cell_prim(i, j, k-1, prim_index) - cell_prim(i-1, j, k-1, prim_index) );
            Real GradCx =        dx_inv * ( cell_prim(i, j, k  , prim_index) - cell_prim(i-1, j, k  , prim_index) );

            xflux(i,j,k,qty_index) = Alpha * mf_u(i,j,0) * ( GradCx - (met_h_xi/met_h_zeta)*GradCz );
        });
        amrex::ParallelFor(ybx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real Alpha = d_alpha_eff[prim_index];

            Real met_h_eta,met_h_zeta;
            met_h_eta  = Compute_h_eta_AtJface (i,j,k,dxInv,z_nd);
            met_h_zeta = Compute_h_zeta_AtJface(i,j,k,dxInv,z_nd);

            Real GradCz = 0.25 * dz_inv * ( cell_prim(i, j, k+1, prim_index) + cell_prim(i, j-1, k+1, prim_index)
                                          - cell_prim(i, j, k-1, prim_index) - cell_prim(i, j-1, k-1, prim_index) );
            Real GradCy =        dy_inv * ( cell_prim(i, j, k  , prim_index) - cell_prim(i, j-1, k  , prim_index) );

            yflux(i,j,k,qty_index) = Alpha * mf_v(i,j,0) * ( GradCy - (met_h_eta/met_h_zeta)*GradCz );
        });
        amrex::ParallelFor(zbx, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
        {
            const int  qty_index = n_start + n;
            const int prim_index = qty_index - qty_offset;

            Real Alpha = d_alpha_eff[prim_index];

            Real met_h_zeta;
            met_h_zeta = Compute_h_zeta_AtKface(i,j,k,dxInv,z_nd);

            Real GradCz = dz_inv * ( cell_prim(i, j, k, prim_index) - cell_prim(i, j, k-1, prim_index) );

            zflux(i,j,k,qty_index) = Alpha * GradCz / met_h_zeta;
        });
    }

    // Linear combinations for z-flux with terrain
    //-----------------------------------------------------------------------------------
    // Extrapolate top and bottom cells
    {
      Box planexy = zbx; planexy.setBig(2, planexy.smallEnd(2) );
      int k_lo = zbx.smallEnd(2); int k_hi = zbx.bigEnd(2);
      zbx3.growLo(2,-1); zbx3.growHi(2,-1);
      amrex::ParallelFor(planexy, ncomp, [=] AMREX_GPU_DEVICE (int i, int j, int , int n) noexcept
      {
          const int  qty_index = n_start + n;
          Real met_h_xi,met_h_eta;

          { // Bottom face
            met_h_xi  = Compute_h_xi_AtKface (i,j,k_lo,dxInv,z_nd);
            met_h_eta = Compute_h_eta_AtKface(i,j,k_lo,dxInv,z_nd);

            Real xfluxlo  = 0.5 * ( xflux(i  , j  , k_lo  , qty_index) + xflux(i+1, j  , k_lo  , qty_index) );
            Real xfluxhi  = 0.5 * ( xflux(i  , j  , k_lo+1, qty_index) + xflux(i+1, j  , k_lo+1, qty_index) );
            Real xfluxbar = 1.5*xfluxlo - 0.5*xfluxhi;

            Real yfluxlo  = 0.5 * ( yflux(i  , j  , k_lo  , qty_index) + yflux(i  , j+1, k_lo  , qty_index) );
            Real yfluxhi  = 0.5 * ( yflux(i  , j  , k_lo+1, qty_index) + yflux(i  , j+1, k_lo+1, qty_index) );
            Real yfluxbar = 1.5*yfluxlo - 0.5*yfluxhi;

            zflux(i,j,k_lo,qty_index) -= met_h_xi*xfluxbar + met_h_eta*yfluxbar;
          }

          { // Top face
            met_h_xi  = Compute_h_xi_AtKface (i,j,k_hi,dxInv,z_nd);
            met_h_eta = Compute_h_eta_AtKface(i,j,k_hi,dxInv,z_nd);

            Real xfluxlo  = 0.5 * ( xflux(i  , j  , k_hi-2, qty_index) + xflux(i+1, j  , k_hi-2, qty_index) );
            Real xfluxhi  = 0.5 * ( xflux(i  , j  , k_hi-1, qty_index) + xflux(i+1, j  , k_hi-1, qty_index) );
            Real xfluxbar = 1.5*xfluxhi - 0.5*xfluxlo;

            Real yfluxlo  = 0.5 * ( yflux(i  , j  , k_hi-2, qty_index) + yflux(i  , j+1, k_hi-2, qty_index) );
            Real yfluxhi  = 0.5 * ( yflux(i  , j  , k_hi-1, qty_index) + yflux(i  , j+1, k_hi-1, qty_index) );
            Real yfluxbar = 1.5*yfluxhi - 0.5*yfluxlo;

            zflux(i,j,k_hi,qty_index) -= met_h_xi*xfluxbar + met_h_eta*yfluxbar;
          }
      });
    }
    // Average interior cells
    amrex::ParallelFor(zbx3, ncomp,[=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
      const int  qty_index = n_start + n;

      Real met_h_xi,met_h_eta;
      met_h_xi  = Compute_h_xi_AtKface (i,j,k,dxInv,z_nd);
      met_h_eta = Compute_h_eta_AtKface(i,j,k,dxInv,z_nd);

      Real xfluxbar = 0.25 * ( xflux(i  , j  , k  , qty_index) + xflux(i+1, j  , k  , qty_index)
                             + xflux(i  , j  , k-1, qty_index) + xflux(i+1, j  , k-1, qty_index) );
      Real yfluxbar = 0.25 * ( yflux(i  , j  , k  , qty_index) + yflux(i  , j+1, k  , qty_index)
                             + yflux(i  , j  , k-1, qty_index) + yflux(i  , j+1, k-1, qty_index) );

      zflux(i,j,k,qty_index) -= met_h_xi*xfluxbar + met_h_eta*yfluxbar;
    });
    // Multiply h_zeta by x/y-fluxes
    amrex::ParallelFor(xbx, ncomp, [=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
      const int  qty_index = n_start + n;
      Real met_h_zeta      = Compute_h_zeta_AtIface(i,j,k,dxInv,z_nd);
      xflux(i,j,k,qty_index) *= met_h_zeta;
    });
    amrex::ParallelFor(ybx, ncomp, [=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept
    {
      const int  qty_index = n_start + n;
      Real met_h_zeta      = Compute_h_zeta_AtJface(i,j,k,dxInv,z_nd);
      yflux(i,j,k,qty_index) *= met_h_zeta;
    });


    // Use fluxes to compute RHS
    //-----------------------------------------------------------------------------------
    for (int qty_index = n_start; qty_index <= n_end; qty_index++)
    {
        amrex::ParallelFor(bx,[=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
        {
            Real stateContrib = (xflux(i+1,j  ,k  ,qty_index) - xflux(i, j, k, qty_index)) * dx_inv * mf_m(i,j,0)  // Diffusive flux in x-dir
                               +(yflux(i  ,j+1,k  ,qty_index) - yflux(i, j, k, qty_index)) * dy_inv * mf_m(i,j,0)  // Diffusive flux in y-dir
                               +(zflux(i  ,j  ,k+1,qty_index) - zflux(i, j, k, qty_index)) * dz_inv;  // Diffusive flux in z-dir

            // Add source terms. TODO: Put this under an if condition when we implement source term
            stateContrib += source_fab(i,j,k,qty_index);

            stateContrib /= detJ(i,j,k);

            cell_rhs(i,j,k,qty_index) += stateContrib;
        });
    }

    // Using Deardorff
    if (l_use_deardorff && n_end >= RhoKE_comp) {
        int qty_index = RhoKE_comp;
        amrex::ParallelFor(bx,[=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
        {
            // Add Buoyancy Source
            Real theta     = cell_prim(i,j,k,PrimTheta_comp);
            Real dtheta_dz = 0.5*(cell_prim(i,j,k+1,PrimTheta_comp)-cell_prim(i,j,k-1,PrimTheta_comp))*dz_inv;
            Real E         = cell_prim(i,j,k,PrimKE_comp);

            Real met_h_zeta = Compute_h_zeta_AtCellCenter(i,j,k,dxInv,z_nd);
            dtheta_dz /= met_h_zeta;

            Real length;
            if (dtheta_dz <= 0.) {
                length = l_Delta;
            } else {
                length = 0.76*std::sqrt(E)*(grav_gpu[2]/theta)*dtheta_dz;
            }
            Real KH   = 0.1 * (1.+2.*length/l_Delta) * std::sqrt(E);
            cell_rhs(i,j,k,qty_index) += cell_data(i,j,k,Rho_comp) * grav_gpu[2] * KH * dtheta_dz;

            // Add TKE production
            cell_rhs(i,j,k,qty_index) += ComputeTKEProduction(i,j,k,u,v,w,mu_turb,dxInv,domain,bc_ptr,l_use_terrain);

            // Add dissipation
            if (std::abs(E) > 0.) {
                cell_rhs(i,j,k,qty_index) += cell_data(i,j,k,Rho_comp) * l_C_e *
                    std::pow(E,1.5) / length;
            }
        });
    }

    // Using Deardorff
    if (l_use_QKE && n_end >= RhoQKE_comp) {
        int qty_index = RhoQKE_comp;
        amrex::ParallelFor(bx,[=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
        {
            cell_rhs(i, j, k, qty_index) += ComputeQKESourceTerms(i,j,k,u,v,cell_data,cell_prim,
                                                                  mu_turb,dxInv,domain,solverChoice,tm_arr(i,j,0));
        });
    }
}
