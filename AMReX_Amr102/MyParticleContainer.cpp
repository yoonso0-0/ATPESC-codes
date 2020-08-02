
#include <MyParticleContainer.H>

namespace amrex {

//
// Initialize a random number of particles per cell determined by nppc
//
void
MyParticleContainer::InitParticles(int nppc, const MultiFab& phi, const MultiFab& ebvol)
{
    // Save the number of particles per cell we are using for the particle-mesh operations
    m_number_particles_per_cell = nppc;

    // Construct a ParticleInitData containing only zeros for the particle buffers and weights
    amrex::ParticleInitType<PIdx::NStructReal, 0, 0, 0> pdata {};

    if (nppc > 1) {
        // Create nppc random particles per cell, initialized with zero real struct data
        InitNRandomPerCell(nppc, pdata);
    } else if (nppc == 1) {
        // Or ... create just one particle per cell centered in the cell
        InitOnePerCell(0.5, 0.5, 0.5, pdata);
    } else {
        Print() << "No particles initialized.\n";
    }

    // Interpolate from density field phi to set particle weights
    InterpolateFromMesh(phi, Interpolation::CIC);

    // Set invalid particle IDs for particles from cells covered by the embedded geometry
    RemoveCoveredParticles(ebvol, Interpolation::CIC);

    // Redistribute to remove the EB-covered particles based on the invalid IDs 
    Redistribute();
}

//
// Uses midpoint method to advance particles using umac.
//
void
MyParticleContainer::AdvectWithUmac (MultiFab* umac, int lev, Real dt)
{
    BL_PROFILE("MyParticleContainer::AdvectWithUmac()");
    AMREX_ASSERT(OK(lev, lev, umac[0].nGrow()-1));
    AMREX_ASSERT(lev >= 0 && lev < GetParticles().size());

    AMREX_D_TERM(AMREX_ASSERT(umac[0].nGrow() >= 1);,
                 AMREX_ASSERT(umac[1].nGrow() >= 1);,
                 AMREX_ASSERT(umac[2].nGrow() >= 1););

    AMREX_D_TERM(AMREX_ASSERT(!umac[0].contains_nan());,
                 AMREX_ASSERT(!umac[1].contains_nan());,
                 AMREX_ASSERT(!umac[2].contains_nan()););

    const Real      strttime = amrex::second();
    const Geometry& geom     = m_gdb->Geom(lev);
    const auto          plo      = geom.ProbLoArray();
    const auto          dxi      = geom.InvCellSizeArray();

    Vector<std::unique_ptr<MultiFab> > raii_umac(AMREX_SPACEDIM);
    Vector<MultiFab*> umac_pointer(AMREX_SPACEDIM);
    if (OnSameGrids(lev, umac[0]))
    {
        for (int i = 0; i < AMREX_SPACEDIM; i++) {
	    umac_pointer[i] = &umac[i];
	}
    }
    else
    {
        for (int i = 0; i < AMREX_SPACEDIM; i++)
        {
	    int ng = umac[i].nGrow();
	    raii_umac[i].reset(new MultiFab(amrex::convert(m_gdb->ParticleBoxArray(lev),
                                                           IntVect::TheDimensionVector(i)),

					                   m_gdb->ParticleDistributionMap(lev),
					                   umac[i].nComp(), ng));


	    umac_pointer[i] = raii_umac[i].get();
	    umac_pointer[i]->copy(umac[i],0,0,umac[i].nComp(),ng,ng);
        }
    }

    for (int ipass = 0; ipass < 2; ipass++)
    {
#ifdef _OPENMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
        for (ParIterType pti(*this, lev); pti.isValid(); ++pti)
        {
            int grid    = pti.index();
            auto& ptile = ParticlesAt(lev, pti);
            auto& aos  = ptile.GetArrayOfStructs();
            const int n = aos.numParticles();
            auto p_pbox = aos().data();
            const FArrayBox* fab[AMREX_SPACEDIM] = { AMREX_D_DECL(&((*umac_pointer[0])[grid]),
                                                                  &((*umac_pointer[1])[grid]),
                                                                  &((*umac_pointer[2])[grid])) };

            //array of these pointers to pass to the GPU
            amrex::GpuArray<amrex::Array4<const Real>, AMREX_SPACEDIM>
                const umacarr {{AMREX_D_DECL((*fab[0]).array(),
                                             (*fab[1]).array(),
                                             (*fab[2]).array() )}};

            amrex::ParallelFor(n,
                               [=] AMREX_GPU_DEVICE (int i)
            {
                ParticleType& p = p_pbox[i];
                if (p.id() <= 0) return;
                Real v[AMREX_SPACEDIM];
                mac_interpolate(p, plo, dxi, umacarr, v);
                if (ipass == 0)
                {
                    for (int dim=0; dim < AMREX_SPACEDIM; dim++)
                    {
                        p.rdata(dim) = p.pos(dim);
                        p.pos(dim) += 0.5*dt*v[dim];
                    }
                }
                else
                {
                    for (int dim=0; dim < AMREX_SPACEDIM; dim++)
                    {
                        p.pos(dim) = p.rdata(dim) + dt*v[dim];
                        p.rdata(dim) = v[dim];
                    }
                }
            });
        }
    }

    if (m_verbose > 1)
    {
        Real stoptime = amrex::second() - strttime;

#ifdef AMREX_LAZY
	Lazy::QueueReduction( [=] () mutable {
#endif
                ParallelReduce::Max(stoptime, ParallelContext::IOProcessorNumberSub(),
                                    ParallelContext::CommunicatorSub());

                amrex::Print() << "MyParticleContainer::AdvectWithUmac() time: " << stoptime << '\n';
#ifdef AMREX_LAZY
	});
#endif
    }
}

//
// Deposit the particle weights to the mesh to set the number density phi
//
void
MyParticleContainer::DepositToMesh (MultiFab& phi, int interpolation)
{
    const auto geom = Geom(0);
    const auto plo = geom.ProbLoArray();
    const auto dxi = geom.InvCellSizeArray();
    const Real inv_cell_volume = dxi[0]*dxi[1]*dxi[2];

    amrex::ParticleToMesh(*this, phi, 0,
    [=] AMREX_GPU_DEVICE (const MyParticleContainer::ParticleType& p,
                          amrex::Array4<amrex::Real> const& phi_arr)
    {
        amrex::Real lx = (p.pos(0) - plo[0]) * dxi[0] + 0.5;
        amrex::Real ly = (p.pos(1) - plo[1]) * dxi[1] + 0.5;
        amrex::Real lz = (p.pos(2) - plo[2]) * dxi[2] + 0.5;

        int i = std::floor(lx);
        int j = std::floor(ly);
        int k = std::floor(lz);

        amrex::Real xint = lx - i;
        amrex::Real yint = ly - j;
        amrex::Real zint = lz - k;

        // Cloud In Cell interpolation
        amrex::Real sx[] = {1.-xint, xint};
        amrex::Real sy[] = {1.-yint, yint};
        amrex::Real sz[] = {1.-zint, zint};

        // Nearest Grid Point Interpolation
        if (interpolation == Interpolation::NGP) {
            sx[0] = 0.0;
            sy[0] = 0.0;
            sz[0] = 0.0;

            sx[1] = 1.0;
            sy[1] = 1.0;
            sz[1] = 1.0;
        }

        // Add up the number of physical particles represented by our particle weights to the grid
        // and divide by the cell volume to get the number density on the grid.
        for (int kk = 0; kk <= 1; ++kk) { 
        for (int jj = 0; jj <= 1; ++jj) { 
        for (int ii = 0; ii <= 1; ++ii) {
            amrex::Gpu::Atomic::Add(&phi_arr(i+ii-1, j+jj-1, k+kk-1), sx[ii]*sy[jj]*sz[kk] * p.rdata(PIdx::Weight) * inv_cell_volume);
        }}}
    });
}

//
// Interpolate number density phi to the particles to set their weights
//
void
MyParticleContainer::InterpolateFromMesh (const MultiFab& phi, int interpolation)
{
    const auto geom = Geom(0);
    const auto plo = geom.ProbLoArray();
    const auto dxi = geom.InvCellSizeArray();
    const auto dx  = geom.CellSizeArray();
    const Real cell_volume = dx[0]*dx[1]*dx[2];
    const Real volume_per_particle = cell_volume / NumParticlesPerCell();

    amrex::MeshToParticle(*this, phi, 0,
    [=] AMREX_GPU_DEVICE (MyParticleContainer::ParticleType& p,
                          amrex::Array4<const amrex::Real> const& phi_arr)
    {
        amrex::Real lx = (p.pos(0) - plo[0]) * dxi[0] + 0.5;
        amrex::Real ly = (p.pos(1) - plo[1]) * dxi[1] + 0.5;
        amrex::Real lz = (p.pos(2) - plo[2]) * dxi[2] + 0.5;

        int i = std::floor(lx);
        int j = std::floor(ly);
        int k = std::floor(lz);

        amrex::Real xint = lx - i;
        amrex::Real yint = ly - j;
        amrex::Real zint = lz - k;

        // Cloud In Cell interpolation (CIC)
        amrex::Real sx[] = {1.-xint, xint};
        amrex::Real sy[] = {1.-yint, yint};
        amrex::Real sz[] = {1.-zint, zint};

        // Nearest Grid Point Interpolation (NGP)
        if (interpolation == Interpolation::NGP) {
            sx[0] = 0.0;
            sy[0] = 0.0;
            sz[0] = 0.0;

            sx[1] = 1.0;
            sy[1] = 1.0;
            sz[1] = 1.0;
        }

        // The particle weight is the number of physical particles it represents.
        // This is set from the number density in the grid phi in 2 steps:

        // Step 1: interpolate number density phi using the particle shape factor for CIC or NGP
        amrex::Real interpolated_phi = 0.0;
        for (int kk = 0; kk <= 1; ++kk) { 
        for (int jj = 0; jj <= 1; ++jj) { 
        for (int ii = 0; ii <= 1; ++ii) {
            interpolated_phi += sx[ii]*sy[jj]*sz[kk] * phi_arr(i+ii-1,j+jj-1,k+kk-1);
        }}}

        // Step 2: scale interpolated number density by the volume per particle and set particle weight
        p.rdata(PIdx::Weight) = interpolated_phi * volume_per_particle;
    });
}

//
// Remove particles covered by the embedded geometry 
//
void
MyParticleContainer::RemoveCoveredParticles (const MultiFab& ebvol, int interpolation)
{
    const auto geom = Geom(0);
    const auto plo = geom.ProbLoArray();
    const auto dxi = geom.InvCellSizeArray();
    const auto dx  = geom.CellSizeArray();
    const Real cell_volume = dx[0]*dx[1]*dx[2];
    const Real volume_per_particle = cell_volume / NumParticlesPerCell();

    amrex::MeshToParticle(*this, ebvol, 0,
    [=] AMREX_GPU_DEVICE (MyParticleContainer::ParticleType& p,
                          amrex::Array4<const amrex::Real> const& vol_arr)
    {
        amrex::Real lx = (p.pos(0) - plo[0]) * dxi[0] + 0.5;
        amrex::Real ly = (p.pos(1) - plo[1]) * dxi[1] + 0.5;
        amrex::Real lz = (p.pos(2) - plo[2]) * dxi[2] + 0.5;

        int i = std::floor(lx);
        int j = std::floor(ly);
        int k = std::floor(lz);

        amrex::Real xint = lx - i;
        amrex::Real yint = ly - j;
        amrex::Real zint = lz - k;

        // Cloud In Cell interpolation (CIC)
        amrex::Real sx[] = {1.-xint, xint};
        amrex::Real sy[] = {1.-yint, yint};
        amrex::Real sz[] = {1.-zint, zint};

        // Nearest Grid Point Interpolation (NGP)
        if (interpolation == Interpolation::NGP) {
            sx[0] = 0.0;
            sy[0] = 0.0;
            sz[0] = 0.0;

            sx[1] = 1.0;
            sy[1] = 1.0;
            sz[1] = 1.0;
        }

        // Interpolate the EB volume fraction to the particle
        amrex::Real interpolated_vol = 0.0;
        for (int kk = 0; kk <= 1; ++kk) { 
        for (int jj = 0; jj <= 1; ++jj) { 
        for (int ii = 0; ii <= 1; ++ii) {
            interpolated_vol += sx[ii]*sy[jj]*sz[kk] * vol_arr(i+ii-1,j+jj-1,k+kk-1);
        }}}

        // If the interpolated volume = 0, then the particle is covered by the EB
        // so we want to delete the particle in the next call to Redistribute().
        // We also delete the particle if the weight is zero.
        if (interpolated_vol == 0.0 || p.rdata(PIdx::Weight) == 0.0) {
            p.id() = -1;
        }
    });
}

}