/*!
 * @file diffusion_problem.tpp
 * @brief Contains implementation of the main object.
 * @author Konrad Simon
 * @date August 2019
 */

#ifndef INCLUDE_DIFFUSION_PROBLEM_TPP_
#define INCLUDE_DIFFUSION_PROBLEM_TPP_

#include "diffusion_problem.hpp"


namespace DiffusionProblem
{
  using namespace dealii;

  template <int dim>
  DiffusionProblem<dim>::DiffusionProblem(unsigned int n_refine)
    : mpi_communicator(MPI_COMM_WORLD)
    , triangulation(mpi_communicator,
                    typename Triangulation<dim>::MeshSmoothing(
                      Triangulation<dim>::smoothing_on_refinement |
                      Triangulation<dim>::smoothing_on_coarsening))
    , fe(1)
    , dof_handler(triangulation)
    , pcout(std::cout,
            (Utilities::MPI::this_mpi_process(mpi_communicator) == 0))
    , computing_timer(mpi_communicator,
                      pcout,
                      TimerOutput::summary,
                      TimerOutput::wall_times)
    , n_refine(n_refine)
  {}


  template <int dim>
  void
  DiffusionProblem<dim>::make_grid()
  {
    TimerOutput::Scope t(computing_timer, "mesh generation");

    GridGenerator::hyper_cube(triangulation, 0, 1, /* colorize */ true);

    triangulation.refine_global(n_refine);
  }


  template <int dim>
  void
  DiffusionProblem<dim>::setup_system()
  {
    TimerOutput::Scope t(computing_timer, "system setup");

    dof_handler.distribute_dofs(fe);

    locally_owned_dofs = dof_handler.locally_owned_dofs();
    DoFTools::extract_locally_relevant_dofs(dof_handler, locally_relevant_dofs);

    locally_relevant_solution.reinit(locally_owned_dofs,
                                     locally_relevant_dofs,
                                     mpi_communicator);

    system_rhs.reinit(locally_owned_dofs, mpi_communicator);

    constraints.clear();
    constraints.reinit(locally_relevant_dofs);

    DoFTools::make_hanging_node_constraints(dof_handler, constraints);

    // Set up Dirichlet boundary conditions.
    const Coefficients::DirichletBC<dim> dirichlet_bc;
    for (unsigned int i = 0; i < dim; ++i)
      {
        VectorTools::interpolate_boundary_values(dof_handler,
                                                 /*boundary id*/ 2 *
                                                   i, // only even boundary id
                                                 dirichlet_bc,
                                                 constraints);
      }

    constraints.close();

    DynamicSparsityPattern dsp(locally_relevant_dofs);
    DoFTools::make_sparsity_pattern(dof_handler,
                                    dsp,
                                    constraints,
                                    /*keep_constrained_dofs =*/true);
    SparsityTools::distribute_sparsity_pattern(
      dsp,
      dof_handler.n_locally_owned_dofs_per_processor(),
      mpi_communicator,
      locally_relevant_dofs);

    system_matrix.reinit(locally_owned_dofs,
                         locally_owned_dofs,
                         dsp,
                         mpi_communicator);
  }


  template <int dim>
  void
  DiffusionProblem<dim>::assemble_system()
  {
    TimerOutput::Scope t(computing_timer, "assembly");

    QGauss<dim>     quadrature_formula(fe.degree + 1);
    QGauss<dim - 1> face_quadrature_formula(fe.degree + 1);

    FEValues<dim> fe_values(fe,
                            quadrature_formula,
                            update_values | update_gradients |
                              update_quadrature_points | update_JxW_values);

    FEFaceValues<dim> fe_face_values(fe,
                                     face_quadrature_formula,
                                     update_values | update_quadrature_points |
                                       update_normal_vectors |
                                       update_JxW_values);

    const unsigned int dofs_per_cell   = fe.dofs_per_cell;
    const unsigned int n_q_points      = quadrature_formula.size();
    const unsigned int n_face_q_points = face_quadrature_formula.size();

    FullMatrix<double> cell_matrix(dofs_per_cell, dofs_per_cell);
    Vector<double>     cell_rhs(dofs_per_cell);

    std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

    /*
     * Matrix coefficient and vector to store the values.
     */
    const Coefficients::MatrixCoeff<dim> matrix_coeff;
    std::vector<Tensor<2, dim>>          matrix_coeff_values(n_q_points);

    /*
     * Right hand side and vector to store the values.
     */
    const Coefficients::RightHandSide<dim> right_hand_side;
    std::vector<double>                    rhs_values(n_q_points);

    /*
     * Neumann BCs and vector to store the values.
     */
    const Coefficients::NeumannBC<dim> neumann_bc;
    std::vector<double>                neumann_values(n_face_q_points);

    /*
     * Integration over cells.
     */
    for (const auto &cell : dof_handler.active_cell_iterators())
      {
        if (cell->is_locally_owned())
          {
            cell_matrix = 0;
            cell_rhs    = 0;

            fe_values.reinit(cell);

            // Now actually fill with values.
            matrix_coeff.value_list(fe_values.get_quadrature_points(),
                                    matrix_coeff_values);
            right_hand_side.value_list(fe_values.get_quadrature_points(),
                                       rhs_values);

            for (unsigned int q_index = 0; q_index < n_q_points; ++q_index)
              {
                for (unsigned int i = 0; i < dofs_per_cell; ++i)
                  {
                    for (unsigned int j = 0; j < dofs_per_cell; ++j)
                      {
                        cell_matrix(i, j) += fe_values.shape_grad(i, q_index) *
                                             matrix_coeff_values[q_index] *
                                             fe_values.shape_grad(j, q_index) *
                                             fe_values.JxW(q_index);
                      } // end ++j

                    cell_rhs(i) += fe_values.shape_value(i, q_index) *
                                   rhs_values[q_index] * fe_values.JxW(q_index);
                  } // end ++i
              }     // end ++q_index

            /*
             * Boundary integral for Neumann values for odd boundary_id.
             */
            for (unsigned int face_number = 0;
                 face_number < GeometryInfo<dim>::faces_per_cell;
                 ++face_number)
              {
                if (cell->face(face_number)->at_boundary() &&
                    ((cell->face(face_number)->boundary_id() == 1) ||
                     (cell->face(face_number)->boundary_id() == 3) ||
                     (cell->face(face_number)->boundary_id() == 5)))
                  {
                    fe_face_values.reinit(cell, face_number);

                    /*
                     * Fill in values at this particular face.
                     */
                    neumann_bc.value_list(
                      fe_face_values.get_quadrature_points(), neumann_values);

                    for (unsigned int q_face_point = 0;
                         q_face_point < n_face_q_points;
                         ++q_face_point)
                      {
                        for (unsigned int i = 0; i < dofs_per_cell; ++i)
                          {
                            cell_rhs(i) +=
                              neumann_values[q_face_point] // g(x_q)
                              * fe_face_values.shape_value(
                                  i, q_face_point)                // phi_i(x_q)
                              * fe_face_values.JxW(q_face_point); // dS
                          }                                       // end ++i
                      } // end ++q_face_point
                  }     // end if
              }         // end ++face_number


            // get global indices
            cell->get_dof_indices(local_dof_indices);
            /*
             * Now add the cell matrix and rhs to the right spots
             * in the global matrix and global rhs. Constraints will
             * be taken care of later.
             */
            constraints.distribute_local_to_global(cell_matrix,
                                                   cell_rhs,
                                                   local_dof_indices,
                                                   system_matrix,
                                                   system_rhs);
          } // end if (cell->is_locally_owned())
      }     // end ++cell

    system_matrix.compress(VectorOperation::add);
    system_rhs.compress(VectorOperation::add);
  }


  template <int dim>
  void
  DiffusionProblem<dim>::solve_iterative()
  {
    TimerOutput::Scope t(computing_timer, "iterative solver");

    LA::MPI::Vector completely_distributed_solution(locally_owned_dofs,
                                                    mpi_communicator);

    SolverControl solver_control(dof_handler.n_dofs(), 1e-12);

#ifdef USE_PETSC_LA
    LA::SolverCG solver(solver_control, mpi_communicator);
#else
    LA::SolverCG solver(solver_control);
#endif

    LA::MPI::PreconditionAMG                 preconditioner;
    LA::MPI::PreconditionAMG::AdditionalData data;

#ifdef USE_PETSC_LA
    data.symmetric_operator = true;
#else
    /* Trilinos defaults are good */
#endif

    preconditioner.initialize(system_matrix, data);

    solver.solve(system_matrix,
                 completely_distributed_solution,
                 system_rhs,
                 preconditioner);

    pcout << "   Solved in " << solver_control.last_step() << " iterations."
          << std::endl;

    constraints.distribute(completely_distributed_solution);
    locally_relevant_solution = completely_distributed_solution;
  }


  template <int dim>
  void
  DiffusionProblem<dim>::output_results() const
  {
    std::string filename = (dim == 2 ? "solution-std_2d" : "solution-std_3d");

    DataOut<dim> data_out;
    data_out.attach_dof_handler(dof_handler);
    data_out.add_data_vector(locally_relevant_solution, "u");

    Vector<float> subdomain(triangulation.n_active_cells());
    for (unsigned int i = 0; i < subdomain.size(); ++i)
      {
        subdomain(i) = triangulation.locally_owned_subdomain();
      }

    data_out.add_data_vector(subdomain, "subdomain");

    data_out.build_patches();

    std::string filename_slave(filename);
    filename_slave +=
      "_refinements-" + Utilities::int_to_string(n_refine, 1) + "." +
      Utilities::int_to_string(triangulation.locally_owned_subdomain(), 4) +
      ".vtu";

    std::ofstream output(filename_slave.c_str());
    data_out.write_vtu(output);


    if (Utilities::MPI::this_mpi_process(mpi_communicator) == 0)
      {
        std::vector<std::string> file_list;
        for (unsigned int i = 0;
             i < Utilities::MPI::n_mpi_processes(mpi_communicator);
             ++i)
          {
            file_list.push_back(filename + "_refinements-" +
                                Utilities::int_to_string(n_refine, 1) + "." +
                                Utilities::int_to_string(i, 4) + ".vtu");
          }

        std::string filename_master(filename);

        filename_master +=
          "_refinements-" + Utilities::int_to_string(n_refine, 1) + ".pvtu";

        std::ofstream master_output(filename_master);
        data_out.write_pvtu_record(master_output, file_list);
      }
  }


  template <int dim>
  void
  DiffusionProblem<dim>::run()
  {
    pcout << std::endl
          << "===========================================" << std::endl
          << "Solving >> STANDARD << problem in " << dim << "D." << std::endl;

    pcout << "Running with "
#ifdef USE_PETSC_LA
          << "PETSc"
#else
          << "Trilinos"
#endif
          << " on " << Utilities::MPI::n_mpi_processes(mpi_communicator)
          << " MPI rank(s)..." << std::endl;

    make_grid();

    setup_system();

    pcout << "   Number of active cells:       "
          << triangulation.n_global_active_cells() << std::endl
          << "   Number of degrees of freedom: " << dof_handler.n_dofs()
          << std::endl;

    assemble_system();

    // Now solve
    solve_iterative();

    if (Utilities::MPI::n_mpi_processes(mpi_communicator) <= 32)
      {
        TimerOutput::Scope t(computing_timer, "output vtu");
        output_results();
      }

    computing_timer.print_summary();
    computing_timer.reset();

    pcout << std::endl
          << "===========================================" << std::endl;
  }

} // end namespace DiffusionProblem


#endif /* INCLUDE_DIFFUSION_PROBLEM_TPP_ */
