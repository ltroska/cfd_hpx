#include <hpx/include/iostreams.hpp>
#include <hpx/lcos/gather.hpp>
#include <hpx/lcos/broadcast.hpp>

#include <cmath>

#include "stepper_server.hpp"

#include "io/vtk_writer.hpp"

typedef stepper::server::stepper_server stepper_component;
typedef hpx::components::component<stepper_component> stepper_server_type;

HPX_REGISTER_COMPONENT_MODULE();

HPX_REGISTER_COMPONENT(stepper_server_type, stepper_component);
HPX_REGISTER_ACTION(stepper::server::stepper_server::setup_action, stepper_server_setup_action);

HPX_REGISTER_GATHER(RealType, stepper_server_space_gatherer);

namespace stepper { namespace server {

stepper_server::stepper_server(uint nl)
    : num_localities(nl)
{}

void stepper_server::setup(io::config&& cfg)
{
    if (num_localities == 2)
    {
        num_localities_x = 2;
        num_localities_y = 1;
    }
    else
    {
        num_localities_x = static_cast<uint>(sqrt(num_localities));
        num_localities_y = num_localities_x;
    }

    c = cfg;

  //  std::cout << "forcing to 1x1 partitions" << std::endl;
  //  c.i_res = (c.i_max + 2)/num_localities_x;
  //  c.j_res = c.j_max + 2;

    initialize_parameters();
    initialize_grids();
    initialize_communication();

    std::cout << cfg << std::endl;

    hpx::cout << "stepper on " << hpx::get_locality_id() << " with " <<  params.num_partitions_x-2 << "x"<< params.num_partitions_y-2 << " partitions, "
             << params.num_cells_per_partition_x << "x" <<  params.num_cells_per_partition_y << " cells each, " << "dx=" << params.dx << " dy=" << params.dx
             << hpx::endl << hpx::flush;

    //communicate_uv_grid(0);

    if ((c.output_skip_size != 0 || c.delta_vec != 0) && c.vtk)
        write_vtk(0);

    if (hpx::get_locality_id() == 0)
        do_work();
}

void stepper_server::initialize_parameters()
{
    params.num_cells_per_partition_x = c.i_res;
    params.num_cells_per_partition_y = c.j_res;

    params.num_partitions_x = ((c.i_max + 2) / num_localities_x) / c.i_res + 2;
    params.num_partitions_y = ((c.j_max + 2) / num_localities_y) / c.j_res + 2;
    
    HPX_ASSERT((params.num_partitions_x - 2) * num_localities_x * c.i_res == c.i_max + 2);
    HPX_ASSERT((params.num_partitions_y - 2) * num_localities_y * c.j_res == c.j_max + 2);

    params.re = c.re;
    params.pr = c.pr;
    params.alpha = c.alpha;
    params.omega = c.omega;
    params.dx = c.x_length / c.i_max;
    params.dy = c.y_length / c.j_max;

    params.i_max = c.i_max;
    params.j_max = c.j_max;

    t = 0;
    next_write = 0;
    out_iter = 1;
}

void stepper_server::initialize_grids()
{
    index_grid.resize(params.num_partitions_x * params.num_partitions_y);

    uv_grid.resize(params.num_partitions_x * params.num_partitions_y);
    uv_temp_grid.resize(params.num_partitions_x * params.num_partitions_y);

    fg_grid.resize(params.num_partitions_x * params.num_partitions_y);
    fg_temp_grid.resize(params.num_partitions_x * params.num_partitions_y);

    p_grid.resize(params.num_partitions_x * params.num_partitions_y);
    p_temp_grid.resize(params.num_partitions_x * params.num_partitions_y);

    rhs_grid.resize(params.num_partitions_x * params.num_partitions_y);

    temperature_grid.resize(params.num_partitions_x * params.num_partitions_y);
    temperature_temp_grid.resize(params.num_partitions_x * params.num_partitions_y);

    stream_grid.resize(params.num_partitions_x * params.num_partitions_y);
    vorticity_grid.resize(params.num_partitions_x * params.num_partitions_y);
    heat_grid.resize(params.num_partitions_x * params.num_partitions_y);

    flag_grid.resize(params.num_partitions_x * params.num_partitions_y);

    scalar_dummy = scalar_partition(hpx::find_here(), 1, 1);
    vector_dummy = vector_partition(hpx::find_here(), 1, 1);

    for (uint l = 0; l < params.num_partitions_y; l++)
        for (uint k = 0; k < params.num_partitions_x; k++)
        {
            index_grid[get_index(k, l)] =
                std::pair<RealType, RealType>
                    (
                    (hpx::get_locality_id() % num_localities_x) * (params.num_partitions_x - 2) * params.num_cells_per_partition_x + (k - 1) * params.num_cells_per_partition_x,
                    (hpx::get_locality_id() / num_localities_x) * (params.num_partitions_y - 2) * params.num_cells_per_partition_y + (l - 1) * params.num_cells_per_partition_y
                    );

            fg_grid[get_index(k, l)] = vector_partition(hpx::find_here(), params.num_cells_per_partition_x, params.num_cells_per_partition_y);
            p_grid[get_index(k, l)] = scalar_partition(hpx::find_here(), params.num_cells_per_partition_x, params.num_cells_per_partition_y);
            rhs_grid[get_index(k, l)] = scalar_partition(hpx::find_here(), params.num_cells_per_partition_x, params.num_cells_per_partition_y);
            temperature_grid[get_index(k, l)] = scalar_partition(hpx::find_here(), params.num_cells_per_partition_x, params.num_cells_per_partition_y, c.ti);
            stream_grid[get_index(k, l)] = scalar_partition(hpx::find_here(), params.num_cells_per_partition_x, params.num_cells_per_partition_y);
            vorticity_grid[get_index(k, l)] = scalar_partition(hpx::find_here(), params.num_cells_per_partition_x, params.num_cells_per_partition_y);
            heat_grid[get_index(k, l)] = scalar_partition(hpx::find_here(), params.num_cells_per_partition_x, params.num_cells_per_partition_y);

            std::vector<std::bitset<5> > local_flags(params.num_cells_per_partition_x*params.num_cells_per_partition_y);

            for (uint j = 0; j < params.num_cells_per_partition_y; j++)
                for (uint i = 0; i < params.num_cells_per_partition_x; i++)
                {
                    local_flags[j*params.num_cells_per_partition_x + i] =
                        c.flag_grid[(params.j_max + 2 - 1 - index_grid[get_index(k, l)].second - j)*(params.i_max + 2)
                                            + (index_grid[get_index(k, l)].first + i)];
                }

            flag_grid[get_index(k, l)] = std::move(local_flags);

            if (k > 0 && k < params.num_partitions_x - 1 && l > 0 && l < params.num_partitions_y - 1)
            {
                if (c.with_initial_uv_grid)
                {
                    vector_data curr_data(params.num_cells_per_partition_x, params.num_cells_per_partition_y);

                    for (uint j = 0; j < params.num_cells_per_partition_y; j++)
                        for (uint i = 0; i < params.num_cells_per_partition_x; i++)
                        {
                            vector_cell& curr_cell = curr_data.get_cell_ref(i, j);
                            curr_cell.first = c.initial_uv_grid[(params.j_max + 2 - 1 - index_grid[get_index(k, l)].second - j)*(params.i_max + 2)
                                            + (index_grid[get_index(k, l)].first + i)].first;
                            curr_cell.second = c.initial_uv_grid[(params.j_max + 2 - 1 - index_grid[get_index(k, l)].second - j)*(params.i_max + 2)
                                            + (index_grid[get_index(k, l)].first + i)].second;

                        }

                    uv_grid[get_index(k, l)] = vector_partition(hpx::find_here(), curr_data);
                }
                else
                {
                    uv_grid[get_index(k, l)] = vector_partition(hpx::find_here(), params.num_cells_per_partition_x, params.num_cells_per_partition_y);
                }
            }
            else
            {
                uv_grid[get_index(k, l)] = vector_dummy;
            }

        }
}

void stepper_server::initialize_communication()
{
    has_neighbor[LEFT] = !(hpx::get_locality_id() % num_localities_x == 0);
    has_neighbor[RIGHT] = !(hpx::get_locality_id() % num_localities_x == num_localities_x - 1);
    has_neighbor[BOTTOM] = !(hpx::get_locality_id() / num_localities_x == 0);
    has_neighbor[TOP] = !(hpx::get_locality_id() / num_localities_x == num_localities_y - 1);
    has_neighbor[BOTTOM_LEFT] = (has_neighbor[BOTTOM] && has_neighbor[LEFT]);
    has_neighbor[BOTTOM_RIGHT] = (has_neighbor[BOTTOM] && has_neighbor[RIGHT]);
    has_neighbor[TOP_LEFT] = (has_neighbor[TOP] && has_neighbor[LEFT]);
    has_neighbor[TOP_RIGHT] = (has_neighbor[TOP] && has_neighbor[RIGHT]);

    for (int directionInt = LEFT; directionInt != NUM_DIRECTIONS; directionInt++)
    {
        neighbor_steppers_[directionInt] = hpx::find_from_basename(stepper_basename,
                                                                   get_neighbor_id(hpx::get_locality_id(), static_cast<direction>(directionInt), num_localities));
    }

    for (uint loc = 0; loc < num_localities; loc++)
                localities.push_back(hpx::find_from_basename(stepper_basename, loc).get());
}

void stepper_server::do_work()
{
    std::pair<RealType, RealType> max_uv(0, 0);
    RealType dt = c.dt;

    for(uint step = 1; t + dt < c.t_end; step++)
    {
        hpx::future<std::vector<std::pair<RealType, RealType> > > local_max_uvs = hpx::lcos::broadcast<do_timestep_action> (localities, step, dt);

        hpx::future<std::pair<RealType, RealType> > max_uv_fut =
                                local_max_uvs.then([](hpx::future<std::vector<std::pair<RealType, RealType> > > fut) -> std::pair<RealType, RealType>
                                                    {
                                                        auto local_max_uvs = fut.get();

                                                        std::pair<RealType, RealType> res(0, 0);

                                                        for (auto max_uv : local_max_uvs)
                                                        {
                                                            res.first = (max_uv.first > res.first ? max_uv.first : res.first);
                                                            res.second = (max_uv.second > res.second ? max_uv.second : res.second);
                                                        }

                                                        return res;
                                                    });
                                                    
        max_uv = max_uv_fut.get();

        RealType tmp = std::min(c.re/2. * 1./(1./std::pow(params.dx, 2) + 1./std::pow(params.dy, 2)),
                                        std::min(params.dx/max_uv.first, params.dy/max_uv.second));

        if (c.temp_data_type.left != -1)
            tmp = std::min(tmp, (c.re*c.pr)/2. * 1./(1./std::pow(params.dx, 2) + 1./std::pow(params.dy, 2)));

        dt = c.tau * tmp;
    }
}

std::pair<RealType, RealType> stepper_server::do_timestep(uint step, RealType dt)
{
    hpx::util::high_resolution_timer t1;
    // SET BOUNDARY
    for (uint l = 0; l < params.num_partitions_y; l++)
        for (uint k = 0; k < params.num_partitions_x; k++)
        {
            if ( k != 0 && k != params.num_partitions_x - 1 && l != 0 && l != params.num_partitions_x - 1)
            {
                uint global_i = index_grid[get_index(k, l)].first;
                uint global_j = index_grid[get_index(k, l)].second;

                uv_temp_grid[get_index(k, l)] =
                    hpx::dataflow(
                        hpx::launch::async,
                        &strategy::set_velocity_for_boundary_and_obstacles,
                        uv_grid[get_index(k, l)],
                        uv_grid[get_index(k-1, l)],
                        uv_grid[get_index(k+1, l)],
                        uv_grid[get_index(k, l-1)],
                        uv_grid[get_index(k, l+1)],
                        flag_grid[get_index(k, l)],
                        c.data_type,
                        c.u_bnd,
                        c.v_bnd,
                        global_i,
                        global_j,
                        params.i_max,
                        params.j_max                    
                    );

                temperature_temp_grid[get_index(k, l)] =
                    hpx::dataflow(
                        hpx::launch::async,
                        &strategy::set_temperature_for_boundary_and_obstacles,
                        temperature_grid[get_index(k, l)],
                        temperature_grid[get_index(k-1, l)],
                        temperature_grid[get_index(k+1, l)],
                        temperature_grid[get_index(k, l-1)],
                        temperature_grid[get_index(k, l+1)],
                        flag_grid[get_index(k, l)],
                        c.temp_data_type,
                        c.temp_bnd,
                        global_i,
                        global_j,
                        params.i_max,
                        params.j_max,
                        params.dx,
                        params.dy
                    );

            }
            else
            {
                uv_temp_grid[get_index(k, l)] = uv_grid[get_index(k, l)];
                temperature_temp_grid[get_index(k, l)] = temperature_grid[get_index(k, l)];
            }
        }

    uv_grid = uv_temp_grid;
    temperature_grid = temperature_temp_grid;
    //print_grid(temperature_grid);
   // print_grid(uv_grid);
   // print_grid(temperature_grid);

   // communicate_uv_grid(step);

    // COMPUTE TEMPERATURE
    for (uint l = 0; l < params.num_partitions_y; l++)
        for (uint k = 0; k < params.num_partitions_x; k++)
        {
            if ( k != 0 && k != params.num_partitions_x - 1 && l != 0 && l != params.num_partitions_x - 1)
            {
                temperature_temp_grid[get_index(k, l)] =
                    hpx::dataflow(
                        hpx::launch::async,
                        &strategy::compute_temperature_on_fluid_cells,
                        temperature_grid[get_index(k, l)],
                        temperature_grid[get_index(k-1, l)],
                        temperature_grid[get_index(k+1, l)],
                        temperature_grid[get_index(k, l-1)],
                        temperature_grid[get_index(k, l+1)],
                        uv_grid[get_index(k, l)],
                        uv_grid[get_index(k-1, l)],
                        uv_grid[get_index(k, l-1)],
                        flag_grid[get_index(k, l)],
                        params.re, c.pr, params.dx, params.dy, dt, c.alpha
                );
            }
            else
            {
                temperature_temp_grid[get_index(k, l)] = temperature_grid[get_index(k, l)];
            }
        }


    temperature_grid = temperature_temp_grid;
  //  print_grid(temperature_grid);

    //COMPUTE FG
    for (uint l = 1; l < params.num_partitions_y - 1; l++)
        for (uint k = 1; k < params.num_partitions_x - 1; k++)
        {
                fg_grid[get_index(k, l)]
                    = hpx::dataflow(
                        hpx::launch::async,
                        &strategy::compute_fg_on_fluid_cells,
                        uv_grid[get_index(k, l)],
                        uv_grid[get_index(k-1, l)],
                        uv_grid[get_index(k+1, l)],
                        uv_grid[get_index(k, l-1)],
                        uv_grid[get_index(k, l+1)],
                        uv_grid[get_index(k+1, l-1)],
                        uv_grid[get_index(k-1, l+1)],
                        temperature_grid[get_index(k, l)],
                        temperature_grid[get_index(k+1, l)],
                        temperature_grid[get_index(k, l+1)],
                        flag_grid[get_index(k, l)],
                        c.re, c.gx, c.gy, c.beta, params.dx, params.dy, dt, c.alpha
                    );
        }

   // print_grid(fg_grid);
   // communicate_fg_grid(step); 
    
    //COMPUTE RHS
    for (uint l = 1; l < params.num_partitions_y - 1; l++)
        for (uint k = 1; k < params.num_partitions_x - 1; k++)
        {
            rhs_grid[get_index(k, l)] =
                hpx::dataflow(
                    hpx::launch::async,
                    &strategy::compute_right_hand_side_on_fluid_cells,
                    fg_grid[get_index(k, l)],
                    fg_grid[get_index(k-1, l)],
                    fg_grid[get_index(k, l-1)],
                    flag_grid[get_index(k, l)],
                    params.dx,
                    params.dy,
                    dt
                );
        }

   // print_grid(uv_grid);
   // print_grid(temperature_grid);
   // print_grid(fg_grid);
   // print_grid(rhs_grid);

    RealType t1_elapsed = t1.elapsed();
    
    hpx::util::high_resolution_timer t2;
    uint iter = 0;
    RealType res = 0;

//    RealType average = 0;
    do
    {
            //    hpx::util::high_resolution_timer t5;

     /*  for (uint l = 1; l < params.num_partitions_y - 1; l++)
            for (uint k = 1; k < params.num_partitions_x - 1; k++)
            {
                    uint global_i = index_grid[get_index(k, l)].first;
                    uint global_j = index_grid[get_index(k, l)].second;

                    scalar_data p_center = p_grid[get_index(k, l)].get_data(CENTER).get();
                    scalar_data p_left = p_grid[get_index(k-1, l)].get_data(LEFT).get();
                    scalar_data p_right = p_grid[get_index(k+1, l)].get_data(RIGHT).get();
                    scalar_data p_bottom = p_grid[get_index(k, l-1)].get_data(BOTTOM).get();
                    scalar_data p_top = p_grid[get_index(k, l+1)].get_data(TOP).get();

                   strategy::set_pressure_on_boundary(p_center, p_left, p_right, p_bottom, p_top, flag_grid[get_index(k, l)], global_i, global_j, params.i_max, params.j_max);
            }*/
        
        for (uint l = 0; l < params.num_partitions_y; l++)
            for (uint k = 0; k < params.num_partitions_x; k++)
            {
                if ( k != 0 && k != params.num_partitions_x - 1 && l != 0 && l != params.num_partitions_x - 1)
                {
                    p_temp_grid[get_index(k, l)] =
                        hpx::dataflow(
                            hpx::launch::async,
                            &strategy::set_pressure_on_boundary_and_obstacles,
                            p_grid[get_index(k, l)],
                            p_grid[get_index(k - 1, l)],
                            p_grid[get_index(k + 1, l)],
                            p_grid[get_index(k, l - 1)],
                            p_grid[get_index(k, l + 1)],
                            flag_grid[get_index(k, l)]
                        );
                }
                else
                    p_temp_grid[get_index(k, l)] = p_grid[get_index(k, l)];
            }           

        // communicate_p_grid(step*c.iter_max + iter);

        for (uint l = 1; l < params.num_partitions_y - 1; l++)
            for (uint k = 1; k < params.num_partitions_x - 1; k++)
            {
                p_grid[get_index(k, l)] =
                        hpx::dataflow(
                            hpx::launch::async,
                            &strategy::sor_cycle,
                            p_temp_grid[get_index(k, l)],
                            p_grid[get_index(k - 1, l)],
                            p_temp_grid[get_index(k + 1, l)],
                            p_grid[get_index(k, l - 1)],
                            p_temp_grid[get_index(k, l + 1)],
                            rhs_grid[get_index(k, l)],
                            flag_grid[get_index(k, l)],
                            c.omega, params.dx, params.dy
                        );                  
            }
        
        std::vector<hpx::future<RealType> > residuals;
        
        for (uint l = 1; l < params.num_partitions_y - 1; l++)
            for (uint k = 1; k < params.num_partitions_x - 1; k++)
            {
                residuals.push_back(
                        hpx::dataflow(
                            hpx::launch::async,
                            &strategy::compute_residual,
                            p_grid[get_index(k, l)],
                            p_grid[get_index(k - 1, l)],
                            p_grid[get_index(k + 1, l)],
                            p_grid[get_index(k, l - 1)],
                            p_grid[get_index(k, l + 1)],
                            rhs_grid[get_index(k, l)],
                            flag_grid[get_index(k, l)],
                            params.dx, params.dy
                        )
                );                  
            }
        
            hpx::future<RealType> residual_fut = 
                hpx::when_all(residuals).then(
                    hpx::util::unwrapped(
                        [](std::vector< hpx::future<RealType> > residuals)
                        -> RealType
                        {
                            RealType sum;

                            for (auto& r : residuals)
                                sum += r.get();

                            return sum;
                        }));

        iter++;

        if (hpx::get_locality_id() == 0)
        {
            hpx::future<std::vector<RealType> > local_residuals =
                hpx::lcos::gather_here(gather_basename, std::move(residual_fut), num_localities, step*c.iter_max + iter);

            hpx::future<RealType> residual =
                local_residuals.then(
                    [](hpx::future<std::vector<RealType>> local_residuals) -> RealType
                    {
                        RealType result = 0;
                        std::vector<RealType> local_res = local_residuals.get();

                        for (RealType res : local_res)
                            result += res;

                        return result;
                    });

            res = residual.get() / (params.i_max * params.j_max);


            hpx::lcos::broadcast_apply<set_keep_running_action> (localities, step*c.iter_max + iter, (iter < c.iter_max && res > c.eps_sq));
        }
        else
        {
            hpx::lcos::gather_there(gather_basename, std::move(residual_fut), step*c.iter_max + iter).wait();
        }

    } while(keep_running.receive(step*c.iter_max + iter).get());
    RealType t2_elapsed = t2.elapsed();
    
    hpx::util::high_resolution_timer t3;

    
    for (uint l = 1; l < params.num_partitions_y - 1; l++)
        for (uint k = 1; k < params.num_partitions_x - 1; k++)
        {
                uv_grid[get_index(k, l)] =
                    hpx::dataflow(
                        hpx::launch::async,
                        &strategy::update_velocities,
                        uv_grid[get_index(k, l)],
                        p_grid[get_index(k, l)],
                        p_grid[get_index(k + 1, l)],
                        p_grid[get_index(k, l + 1)],
                        fg_grid[get_index(k, l)],
                        flag_grid[get_index(k, l)],
                        params.dx, params.dy, dt
                    );
        }

    t += dt;

    std::vector<hpx::future<std::pair<RealType, RealType> > > max_uvs;

    for (uint l = 1; l < params.num_partitions_y - 1; l++)
        for (uint k = 1; k < params.num_partitions_x - 1; k++)
        { 
            max_uvs.push_back(
                hpx::dataflow(
                    hpx::launch::async,
                    &strategy::max_velocity,
                    uv_grid[get_index(k, l)]
                )
            );
        }
    
    hpx::future<std::pair<RealType, RealType> > max_uv = 
                hpx::when_all(max_uvs).then(
                    hpx::util::unwrapped(
                        [](std::vector< hpx::future<std::pair<RealType, RealType> > > max_uvs)
                        -> std::pair<RealType, RealType>
                        {
                            std::pair<RealType, RealType> max_uv(0, 0);

                            for (auto& m_uv_f : max_uvs)
                            {
                                auto m_uv = m_uv_f.get();
                                
                                max_uv.first =
                                    (m_uv.first > max_uv.first ? 
                                     m_uv.first : max_uv.first);
                                
                                max_uv.second =
                                    (m_uv.second > max_uv.second ?
                                     m_uv.second : max_uv.second);
                            }
                            
                            return max_uv;
                        }));
    
    if ( (c.output_skip_size != 0 && (step % c.output_skip_size == 0)) || (c.delta_vec != 0 && next_write <= t))
    {
        if (c.delta_vec != 0)
            next_write = t + c.delta_vec;

        if (c.vtk)
            write_vtk(out_iter);

        if (hpx::get_locality_id() == 0)
            std::cout << "t " << t << " | dt " << dt << " | iterations: " << iter << " | residual squared " << res 
                      << " | before SOR = " << t1_elapsed << " | SOR = " << t2_elapsed << " | after SOR = " << t3.elapsed() <<  std::endl;

        out_iter++;
    }


   // std::pair<RealType, RealType> max_uv(0, 0);
    return max_uv.get();
}

void stepper_server::do_sor_cycle()
{

}

void stepper_server::sor()
{

}

// ---------------------------------------- COMMUNICATION ---------------------------------------- //
void stepper_server::set_keep_running(uint iter, bool kr)
{
    keep_running.store_received(iter, std::move(kr));
}

void stepper_server::communicate_p_grid(uint iter)
{
    //SEND
    for (uint l = 1; l < params.num_partitions_y - 1; l++)
    {
        send_p_to_neighbor(iter * params.num_partitions_y + l, p_grid[get_index(1, l)], LEFT);
        send_p_to_neighbor(iter * params.num_partitions_y + l, p_grid[get_index(params.num_partitions_x - 2, l)], RIGHT);
    }

    for (uint k = 1; k < params.num_partitions_x - 1; k++)
    {
        send_p_to_neighbor(iter * params.num_partitions_x + k, p_grid[get_index(k, 1)], BOTTOM);
        send_p_to_neighbor(iter * params.num_partitions_x + k, p_grid[get_index(k, params.num_partitions_y - 2)], TOP);
    }

    //RECEIVE
    for(uint l = 1; l < params.num_partitions_y - 1; l++)
    {
        p_grid[get_index(0, l)] = receive_p_from_neighbor(iter * params.num_partitions_y+l, LEFT);
        p_grid[get_index(params.num_partitions_x - 1, l)] = receive_p_from_neighbor(iter * params.num_partitions_y + l, RIGHT);
    }

    for(uint k = 1; k < params.num_partitions_x - 1; k++)
    {
        p_grid[get_index(k, 0)] = receive_p_from_neighbor(iter * params.num_partitions_x + k, BOTTOM);
        p_grid[get_index(k, params.num_partitions_y - 1)] = receive_p_from_neighbor(iter * params.num_partitions_x + k, TOP);
    }
}

void stepper_server::receive_p_action_(uint t, scalar_partition p, direction to_dir)
{
    //direction is now opposite.
    p_recv_buffs_[NUM_DIRECTIONS - to_dir - 1].store_received(t, std::move(p));
}

void stepper_server::send_p_to_neighbor(uint t, scalar_partition p, direction dir)
{
    if (has_neighbor[dir])
    {
        receive_p_action act;
        hpx::async(act, neighbor_steppers_[dir].get(), t, p, dir);
    }
}

scalar_partition stepper_server::receive_p_from_neighbor(uint t, direction dir)
{
    if (has_neighbor[dir])
        return p_recv_buffs_[dir].receive(t);
    else
        return scalar_dummy;
}

void stepper_server::communicate_fg_grid(uint step)
{
    //SEND
     for (uint l = 1; l < params.num_partitions_y - 1; l++)
        send_fg_to_neighbor(step * params.num_partitions_y + l, fg_grid[get_index(params.num_partitions_x - 2, l)], RIGHT);

    for (uint k = 1; k < params.num_partitions_x - 1; k++)
        send_fg_to_neighbor(step * params.num_partitions_x + k, fg_grid[get_index(k, params.num_partitions_y - 2)], TOP);

    //RECEIVE
    for(uint l = 1; l < params.num_partitions_y - 1; l++)
        fg_grid[get_index(0, l)] = receive_fg_from_neighbor(step * params.num_partitions_y + l, LEFT);

    for(uint k = 1; k < params.num_partitions_x-1; k++)
        fg_grid[get_index(k, 0)] = receive_fg_from_neighbor(step * params.num_partitions_x + k, BOTTOM);
}

void stepper_server::receive_fg_action_(uint t, vector_partition fg, direction to_dir)
{
    //direction is now opposite.
    fg_recv_buffs_[NUM_DIRECTIONS - to_dir - 1].store_received(t, std::move(fg));
}

void stepper_server::send_fg_to_neighbor(uint t, vector_partition fg, direction dir)
{
    if (has_neighbor[dir])
    {
        receive_fg_action act;
        hpx::async(act, neighbor_steppers_[dir].get(), t, fg, dir);
    }
}

vector_partition stepper_server::receive_fg_from_neighbor(uint t, direction dir)
{
    if (has_neighbor[dir])
        return fg_recv_buffs_[dir].receive(t);
    else
        return vector_dummy;
}

uint stepper_server::get_index(uint k, uint l) const
{
    return l * params.num_partitions_x + k;
}

void stepper_server::communicate_uv_grid(uint step)
{
    // SEND
     for (uint l = 1; l < params.num_partitions_y - 1; l++)
    {
        if (l == 1)
            send_uv_to_neighbor(step, uv_grid[get_index(params.num_partitions_x - 2, l)], BOTTOM_RIGHT);

        if (l == params.num_partitions_y - 2)
            send_uv_to_neighbor(step, uv_grid[get_index(1, l)], TOP_LEFT);

        send_uv_to_neighbor(step * params.num_partitions_y + l, uv_grid[get_index(1, l)], LEFT);
        send_uv_to_neighbor(step * params.num_partitions_y + l, uv_grid[get_index(params.num_partitions_x - 2, l)], RIGHT);
    }

    for (uint k = 1; k < params.num_partitions_x - 1; k++)
    {
        if (k == 1)
                send_uv_to_neighbor(step, uv_grid[get_index(k, 1)], BOTTOM_LEFT);

        if (k == params.num_partitions_x- 2)
                send_uv_to_neighbor(step, uv_grid[get_index(k, params.num_partitions_y - 2)], TOP_RIGHT);

        send_uv_to_neighbor(step * params.num_partitions_x + k, uv_grid[get_index(k, 1)], BOTTOM);
        send_uv_to_neighbor(step * params.num_partitions_x + k, uv_grid[get_index(k, params.num_partitions_y - 2)], TOP);
    }

    // RECEIVE
    for(uint l = 1; l < params.num_partitions_y - 1; l++)
    {
        uv_grid[get_index(0, l)] = receive_uv_from_neighbor(step * params.num_partitions_y + l, LEFT);
        uv_grid[get_index(params.num_partitions_x - 1, l)] = receive_uv_from_neighbor(step * params.num_partitions_y + l, RIGHT);
    }

    for(uint k = 1; k < params.num_partitions_x - 1; k++)
    {
        uv_grid[get_index(k, 0)] = receive_uv_from_neighbor(step * params.num_partitions_x + k, BOTTOM);
        uv_grid[get_index(k, params.num_partitions_y - 1)] = receive_uv_from_neighbor(step * params.num_partitions_x + k, TOP);
    }

    uv_grid[get_index(0, params.num_partitions_y - 1)] = receive_uv_from_neighbor(step, TOP_LEFT);
    uv_grid[get_index(params.num_partitions_x - 1, params.num_partitions_y -1)] = receive_uv_from_neighbor(step, TOP_RIGHT);
    uv_grid[get_index(0, 0)] = receive_uv_from_neighbor(step, BOTTOM_LEFT);
    uv_grid[get_index(params.num_partitions_x - 1, 0)] = receive_uv_from_neighbor(step, BOTTOM_RIGHT);
}

void stepper_server::receive_uv_action_(uint t, vector_partition uv, direction to_dir)
{
    //direction is now opposite.
    uv_recv_buffs_[NUM_DIRECTIONS - to_dir - 1].store_received(t, std::move(uv));
}

void stepper_server::send_uv_to_neighbor(uint t, vector_partition uv, direction dir)
{
    if (has_neighbor[dir])
    {
        receive_uv_action act;
        hpx::async(act, neighbor_steppers_[dir].get(), t, uv, dir);
    }
}

vector_partition stepper_server::receive_uv_from_neighbor(uint t, direction dir)
{
    if (has_neighbor[dir])
        return uv_recv_buffs_[dir].receive(t);
    else
        return vector_dummy;
}

template<typename T>
void stepper_server::print_grid(std::vector<grid::partition<T> > const& grid, const std::string message) const
{
    std::vector<std::vector<grid::partition_data<T> > > data;

    data.resize(params.num_partitions_x - 2);

    for (uint k = 1; k < params.num_partitions_x - 1; k++)
    {
        data[k-1].resize(params.num_partitions_y - 2);
        for (uint l = 1; l < params.num_partitions_y - 1; l++)
        {
            grid::partition_data<T> base = grid[get_index(k, l)].get_data(CENTER).get();
            data[k-1][l-1] = grid::partition_data<T>(base);
        }
    }

    boost::shared_ptr<hpx::lcos::local::promise<int> > p = boost::make_shared<hpx::lcos::local::promise<int> >();
    io::do_async_print(data, message, params.num_partitions_x - 2, params.num_partitions_y - 2, params.num_cells_per_partition_x, params.num_cells_per_partition_y, p);
}

void stepper_server::write_vtk(uint step)
{
    for (uint l = 1; l < params.num_partitions_y - 1; l++)
        for (uint k = 1; k < params.num_partitions_x - 1; k++)
        {
            uint global_i = index_grid[get_index(k, l)].first;
            uint global_j = index_grid[get_index(k, l)].second;

            scalar_data stream_center = stream_grid[get_index(k, l)].get_data(CENTER).get();
            scalar_data stream_bottom = stream_grid[get_index(k, l-1)].get_data(BOTTOM).get();

            scalar_data heat_center = heat_grid[get_index(k, l)].get_data(CENTER).get();
            scalar_data heat_bottom = heat_grid[get_index(k, l-1)].get_data(BOTTOM).get();

            scalar_data vorticity_center = vorticity_grid[get_index(k, l)].get_data(CENTER).get();

            vector_data uv_center = uv_grid[get_index(k, l)].get_data(CENTER).get();
            vector_data uv_right = uv_grid[get_index(k+1, l)].get_data(RIGHT).get();
            vector_data uv_top = uv_grid[get_index(k, l+1)].get_data(TOP).get();

            scalar_data temp_center = temperature_grid[get_index(k, l)].get_data(CENTER).get();
            scalar_data temp_right = temperature_grid[get_index(k+1, l)].get_data(RIGHT).get();

            strategy::compute_stream_vorticity_heat(stream_center, vorticity_center, heat_center, stream_bottom, heat_bottom, uv_center,
                                                    uv_right, uv_top, temp_center, temp_right, flag_grid[get_index(k, l)],
                                                    global_i, global_j, params.i_max,
                                                    params.j_max, c.re, c.pr, params.dx, params.dy);

            stream_grid[get_index(k, l)] = scalar_partition(hpx::find_here(), stream_center);
            vorticity_grid[get_index(k, l)] = scalar_partition(hpx::find_here(), vorticity_center);
            heat_grid[get_index(k, l)] = scalar_partition(hpx::find_here(), heat_center);
        }

    std::vector<std::vector<scalar_data> > p_data;

    p_data.resize(params.num_partitions_x);

    for (uint k = 0; k < params.num_partitions_x; k++)
    {
        p_data[k].resize(params.num_partitions_y);
        for (uint l = 0; l < params.num_partitions_y; l++)
        {
            scalar_data base = p_grid[get_index(k, l)].get_data(CENTER).get();
            p_data[k][l] = scalar_data(base);
        }
    }

    std::vector<std::vector<vector_data> > uv_data;

    uv_data.resize(params.num_partitions_x);

    for (uint k = 0; k < params.num_partitions_x; k++)
    {
        uv_data[k].resize(params.num_partitions_y);
        for (uint l = 0; l < params.num_partitions_y; l++)
        {
            vector_data base = uv_grid[get_index(k, l)].get_data(CENTER).get();
            uv_data[k][l] = vector_data(base);
        }
    }

    std::vector<std::vector<scalar_data> > stream_data;

    stream_data.resize(params.num_partitions_x );

    for (uint k = 0; k < params.num_partitions_x ; k++)
    {
        stream_data[k].resize(params.num_partitions_y);
        for (uint l = 0; l < params.num_partitions_y; l++)
        {
            scalar_data base = stream_grid[get_index(k, l)].get_data(CENTER).get();
            stream_data[k][l] = scalar_data(base);
        }
    }

    std::vector<std::vector<scalar_data> > vorticity_data;

    vorticity_data.resize(params.num_partitions_x);

    for (uint k = 0; k < params.num_partitions_x; k++)
    {
        vorticity_data[k].resize(params.num_partitions_y);
        for (uint l = 0; l < params.num_partitions_y; l++)
        {
            scalar_data base = vorticity_grid[get_index(k, l)].get_data(CENTER).get();
            vorticity_data[k][l] = scalar_data(base);
        }
    }

    std::vector<std::vector<scalar_data> > heat_data;

    heat_data.resize(params.num_partitions_x);

    for (uint k = 0; k < params.num_partitions_x; k++)
    {
        heat_data[k].resize(params.num_partitions_y);
        for (uint l = 0; l < params.num_partitions_y; l++)
        {
            scalar_data base = heat_grid[get_index(k, l)].get_data(CENTER).get();
            heat_data[k][l] = scalar_data(base);
        }
    }

    std::vector<std::vector<scalar_data> > temp_data;

    temp_data.resize(params.num_partitions_x);

    for (uint k = 0; k < params.num_partitions_x; k++)
    {
        temp_data[k].resize(params.num_partitions_y);
        for (uint l = 0; l < params.num_partitions_y; l++)
        {
            scalar_data base = temperature_grid[get_index(k, l)].get_data(CENTER).get();
            temp_data[k][l] = scalar_data(base);
        }
    }

  //  hpx::async(write_vtk_action(), hpx::find_here(), p_data, uv_data, stream_data, vorticity_data, heat_data, temp_data, params.dx, params.dy, step, params.i_max, params.j_max, params.num_partitions_x - 2,
 //                                       params.num_partitions_y - 2, params.num_cells_per_partition_x, params.num_cells_per_partition_y);
    boost::shared_ptr<hpx::lcos::local::promise<int> > p = boost::make_shared<hpx::lcos::local::promise<int> >();

    io::do_write_vtk(p_data, uv_data, stream_data, vorticity_data, heat_data, temp_data, flag_grid, params.dx, params.dy, step, params.i_max, params.j_max, params.num_partitions_x,
                                        params.num_partitions_y, params.num_cells_per_partition_x, params.num_cells_per_partition_y, p);

}

RealType stepper_server::compute_new_dt(std::pair<RealType, RealType> max_uv) const
{
    return c.tau * std::min(c.re/2. * 1./(1./(params.dx * params.dx) + 1./(params.dy * params.dy)), std::min(params.dx/max_uv.first, params.dy/max_uv.second));
}

}//namespace server
}//namespace stepper
