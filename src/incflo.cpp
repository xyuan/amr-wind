
#include <incflo.H>
#include <derive_F.H>
#include <param_mod_F.H>

// Need this for TagCutCells
#include <AMReX_EBAmrUtil.H>

DiffusionType incflo::m_diff_type = DiffusionType::Implicit;

// Constructor
// Note that geometry on all levels has already been defined in the AmrCore constructor,
// which the incflo class inherits from.
incflo::incflo()
  : m_bc_u(get_dim_bc()+1, 0)
  , m_bc_v(get_dim_bc()+1, 0)
  , m_bc_w(get_dim_bc()+1, 0)
  , m_bc_r(get_dim_bc()+1, 0)
  , m_bc_t(get_dim_bc()+1, 0)
  , m_bc_p(get_dim_bc()+1, 0)
{
    // NOTE: Geometry on all levels has just been defined in the AmrCore
    // constructor. No valid BoxArray and DistributionMapping have been defined.
    // But the arrays for them have been resized.

    // Read inputs file using ParmParse
    ReadParameters();

    // Initialize memory for data-array internals
    ResizeArrays();

    // This needs is needed before initializing level MultiFabs: ebfactories should
    // not change after the eb-dependent MultiFabs are allocated.
    MakeEBGeometry();
}

incflo::~incflo(){};

void incflo::InitData()
{
    BL_PROFILE("incflo::InitData()");

    // Either init from scratch or from the checkpoint file
    // In both cases, we call MakeNewLevelFromScratch():
    // - Set BA and DM
    // - Allocate arrays for level
    int restart_flag = 0;
    if(restart_file.empty())
    {
        // This tells the AmrMesh class not to iterate when creating the initial
        // grid hierarchy
        SetIterateToFalse();

        // This tells the Cluster routine to use the new chopping routine which
        // rejects cuts if they don't improve the efficiency
        SetUseNewChop();

        // This is an AmrCore member function which recursively makes new levels
        InitFromScratch(cur_time);
    }
    else
    {
        // Read starting configuration from chk file.
        ReadCheckpointFile();
        restart_flag = 1;
    }

    // Post-initialisation step
    // - Set BC types
    // - Initialize diffusive and projection operators
    // - Fill boundaries
    // - Create instance of MAC projection class
    // - Apply initial conditions
    // - Project initial velocity to make divergence free
    // - Perform dummy iterations to find pressure distribution
    PostInit(restart_flag);

    // Plot initial distribution
    if((plot_int > 0 || plot_per > 0) && !restart_flag)
    {
        WritePlotFile();
        last_plt = 0;
    }

    ParmParse pp("incflo");
    bool write_eb_surface = 0;
    pp.query("write_eb_surface", write_eb_surface);

    if (write_eb_surface)
    {
        amrex::Print() << "Writing the geometry to a vtp file.\n" << std::endl;
        WriteMyEBSurface();
    }
}

void incflo::Evolve()
{
    BL_PROFILE("incflo::Evolve()");

    bool do_not_evolve = ((max_step == 0) || ((stop_time >= 0.) && (cur_time > stop_time)) ||
   					     ((stop_time <= 0.) && (max_step <= 0))) && !steady_state;

    while(!do_not_evolve)
    {
        // // TODO: Necessary for dynamic meshing
        // /* if (regrid_int > 0)
        // {
        //     // Make sure we don't regrid on max_level
        //     for (int lev = 0; lev < max_level; ++lev)
        //     {
        //         // regrid is a member function of AmrCore
        //         if (nstep % regrid_int == 0)
        //         {
        //             regrid(lev, time);
        //             incflo_setup_solvers();
        //         }
        //     }
        // }*/

        // Advance to time t + dt
        Advance();
        nstep++;
        cur_time += dt;

        // Write plot and checkpoint files
        if((plot_int > 0 && (nstep % plot_int == 0)) ||
           (plot_per > 0 && (std::abs(remainder(cur_time, plot_per)) < 1.e-12)))
        {
            WritePlotFile();
            last_plt = nstep;
        }
        if(check_int > 0 && (nstep % check_int == 0))
        {
            WriteCheckPointFile();
            last_chk = nstep;
        }

        // Mechanism to terminate incflo normally.
        do_not_evolve = (steady_state && SteadyStateReached()) ||
                        ((stop_time > 0. && (cur_time >= stop_time - 1.e-12 * dt)) ||
                         (max_step >= 0 && nstep >= max_step));
    }

	// Output at the final time
    if(check_int > 0                  && nstep != last_chk) WriteCheckPointFile();
    if((plot_int > 0 || plot_per > 0) && nstep != last_plt) WritePlotFile();
}

// tag cells for refinement
// overrides the pure virtual function in AmrCore
void incflo::ErrorEst(int lev,
                      TagBoxArray& tags,
                      Real time,
                      int ngrow)
{
    BL_PROFILE("incflo::ErrorEst()");

    const char   tagval = TagBox::SET;
    const char clearval = TagBox::CLEAR;

    auto const& factory = dynamic_cast<EBFArrayBoxFactory const&>(vel[lev]->Factory());
    auto const& flags = factory.getMultiEBCellFlagFab();

    const Real* dx      = geom[lev].CellSize();
    const Real* prob_lo = geom[lev].ProbLo();

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(*vel[lev],true); mfi.isValid(); ++mfi)
    {
        const Box& bx  = mfi.tilebox();
        const auto& flag = flags[mfi];
        const FabType typ = flag.getType(bx);
        if (typ != FabType::covered)
        {
            TagBox&     tagfab  = tags[mfi];

            // tag cells for refinement
            state_error(BL_TO_FORTRAN_BOX(bx),
                        BL_TO_FORTRAN_ANYD(tagfab),
                        BL_TO_FORTRAN_ANYD((ebfactory[lev]->getVolFrac())[mfi]),
                        &tagval, &clearval,
                        AMREX_ZFILL(dx), AMREX_ZFILL(prob_lo), &time);
        }
    }

    refine_cutcells = true;
    // Refine on cut cells
    if (refine_cutcells)
    {
        const MultiFab* volfrac = &(ebfactory[lev] -> getVolFrac());
        amrex::TagCutCells(tags, *vel[lev]);
    }
}

// Make a new level from scratch using provided BoxArray and DistributionMapping.
// Only used during initialization.
// overrides the pure virtual function in AmrCore
void incflo::MakeNewLevelFromScratch(int lev,
                                     Real time,
                                     const BoxArray& new_grids,
                                     const DistributionMapping& new_dmap)
{
    BL_PROFILE("incflo::MakeNewLevelFromScratch()");

    if(incflo_verbose > 0)
    {
        amrex::Print() << "Making new level " << lev << std::endl;
        amrex::Print() << "with BoxArray " << new_grids << std::endl;
    }

	SetBoxArray(lev, new_grids);
	SetDistributionMap(lev, new_dmap);

	// Allocate the fluid data, NOTE: this depends on the ebfactories.
    AllocateArrays(lev);
}

// Make a new level using provided BoxArray and DistributionMapping and
// fill with interpolated coarse level data.
// overrides the pure virtual function in AmrCore
void incflo::MakeNewLevelFromCoarse (int lev,
                                     Real time,
                                     const BoxArray& ba,
                                     const DistributionMapping& dm)
{
    BL_PROFILE("incflo::MakeNewLevelFromCoarse()");

    amrex::Print() << "ABORT: incflo::MakeNewLevelFromCoarse() not yet implemented. " << std::endl;
    amrex::Abort();
}

// Remake an existing level using provided BoxArray and DistributionMapping and
// fill with existing fine and coarse data.
// overrides the pure virtual function in AmrCore
void incflo::RemakeLevel (int lev, Real time, const BoxArray& ba,
			 const DistributionMapping& dm)
{
    BL_PROFILE("incflo::RemakeLevel()");

    amrex::Print() << "ABORT: incflo::RemakeLevel() not yet implemented. " << std::endl;
    amrex::Abort();
}

// Delete level data
// overrides the pure virtual function in AmrCore
void incflo::ClearLevel (int lev)
{
    BL_PROFILE("incflo::ClearLevel()");

    amrex::Print() << "ABORT: incflo::ClearLevel() not yet implemented. " << std::endl;
    amrex::Abort();
}

// Set covered coarse cells to be the average of overlying fine cells
// TODO: Move somewhere else, for example setup/incflo_arrays.cpp
void incflo::AverageDown()
{
    BL_PROFILE("incflo::AverageDown()");

    for (int lev = finest_level - 1; lev >= 0; --lev)
    {
        AverageDownTo(lev);
    }
}

void incflo::AverageDownTo(int crse_lev)
{
    BL_PROFILE("incflo::AverageDownTo()");

    IntVect rr = refRatio(crse_lev);

    amrex::EB_average_down(*vel[crse_lev+1],        *vel[crse_lev],        0, AMREX_SPACEDIM, rr);
    amrex::EB_average_down( *gp[crse_lev+1],         *gp[crse_lev],        0, AMREX_SPACEDIM, rr);

    if (!constant_density)
       amrex::EB_average_down(*density[crse_lev+1], *density[crse_lev],    0, 1, rr);

    if (advect_tracer)
       amrex::EB_average_down(*tracer[crse_lev+1],  *tracer[crse_lev],     0, ntrac, rr);

    amrex::EB_average_down(*eta[crse_lev+1],        *eta[crse_lev],        0, 1, rr);
    amrex::EB_average_down(*strainrate[crse_lev+1], *strainrate[crse_lev], 0, 1, rr);
    amrex::EB_average_down(*vort[crse_lev+1],       *vort[crse_lev],       0, 1, rr);
}
