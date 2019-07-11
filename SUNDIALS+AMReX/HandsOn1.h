#ifndef HANDSON1_H
#define HANDSON1_H

#include <AMReX_Geometry.H>
#include <AMReX_MultiFab.H>
#include <AMReX_Array.H>
#include "Utilities.h"

// user-data structure for problem options
struct ProblemOpt
{
   int plot_int;
   int arkode_order;
   amrex::Real rtol;
   amrex::Real atol;
   amrex::Real fixed_dt;
   amrex::Real tfinal;
   amrex::Real dtout;
   int max_steps;
   int write_diag;
};

// Run problem
void DoProblem();

// Parse the problem input file
void ParseInputs(ProblemOpt& prob_opt, ProblemData& prob_data);

// Advance the solution in time with ARKode ARKStep
void ComputeSolutionARK(N_Vector nv_sol, ProblemOpt* prob_opt,
                        ProblemData* prob_data);

#endif
