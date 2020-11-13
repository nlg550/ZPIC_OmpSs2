/*********************************************************************************************
 ZPIC
 particles.c

 Created by Ricardo Fonseca on 12/8/10.
 Modified by Nicolas Guidotti on 11/06/20

 Copyright 2010 Centro de Física dos Plasmas. All rights reserved.

 *********************************************************************************************/

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "particles.h"
#include "random.h"
#include "emf.h"
#include "current.h"
#include "zpic.h"
#include "zdf.h"
#include "timer.h"
#include "csv_handler.h"

#define LTRIM(x) (x >= 1.0f) - (x < 0.0f)

static double _spec_time = 0.0;
static double _spec_npush = 0.0;

/**
 * Returns the total time spent pushing particles (includes boundaries and moving window)
 * @return  Total time in seconds
 */
double spec_time(void)
{
	return _spec_time;
}

/**
 * Returns the performance achieved by the code (push time)
 * @return  Performance in seconds per particle
 */
double spec_perf(void)
{
	return (_spec_npush > 0) ? _spec_time / _spec_npush : 0.0;
}

/*********************************************************************************************
 Spec Sort
 *********************************************************************************************/

void prefix_sum_serial(int *restrict vector, const int size)
{
	int acc = 0;
	for (int i = 0; i < size; i++)
	{
		int temp = vector[i];
		vector[i] = acc;
		acc += temp;
	}
}

#if TARGET_GPU || TARGET_FPGA
void spec_sort(t_part_vector *part_vector, t_part_vector *temp_part, t_part_vector *new_part,
        int *restrict tile_offset, int *restrict np_per_tile, int *restrict sort_counter,
        int *target_idx, int *restrict temp_offset, const cl_int2 n_tiles, const int nx[2],
        const int moving_window, const int shift, const int ppc[2])
{
	const int size = part_vector->np;
	const int n_tiles_total = n_tiles.x * n_tiles.y;
	const int max_holes = MAX_LEAVING_PART * part_vector->np_max;

	#pragma omp task inout(tile_offset[0: n_tiles_total]) inout(np_per_tile[0: n_tiles_total]) \
	out(sort_counter[0; n_tiles_total]) inout(temp_offset[0; n_tiles_total])
	{
		if(moving_window && shift)
		{
			const int npc = TILE_SIZE * ppc[0] * ppc[1];
			for(int i = 0; i < n_tiles.y; i++)
				np_per_tile[(i + 1) * n_tiles.x - 1] += npc;
		}

		prefix_sum_serial(np_per_tile, n_tiles_total + 1);
		part_vector->np = np_per_tile[n_tiles_total];

		for(int i = 1; i < n_tiles_total; i++)
		{
			int diff = np_per_tile[i] - tile_offset[i];
			if(diff < 0) temp_offset[i] += -diff;
			else if(diff > 0) temp_offset[i - 1] += diff;
		}

		prefix_sum_serial(temp_offset, n_tiles_total);

		memcpy(sort_counter, temp_offset, n_tiles_total * sizeof(int));
		memcpy(tile_offset, np_per_tile, (n_tiles_total + 1) * sizeof(int));
	}

	spec_sort_1(part_vector->cell_idx, part_vector->position, part_vector->velocity,
	        temp_part->cell_idx, temp_part->position, temp_part->velocity, target_idx, sort_counter,
	        tile_offset, temp_offset, n_tiles, size, part_vector->np_max, max_holes, nx[0]);

	if (moving_window && shift)
		spec_inject_particles_opencl(temp_part->cell_idx, temp_part->position, temp_part->velocity,
				new_part->cell_idx, new_part->position, new_part->velocity,
				sort_counter, max_holes, new_part->np, n_tiles);

	spec_sort_2(part_vector->cell_idx, part_vector->position, part_vector->velocity,
	        temp_part->cell_idx, temp_part->position, temp_part->velocity, target_idx, sort_counter,
	        temp_offset, n_tiles, max_holes, part_vector->np_max);
}

#else
void spec_sort(t_part_vector *part_vector, t_part_vector *temp_part, t_part_vector *new_part,
        int *restrict tile_offset, int *restrict np_per_tile, int *restrict sort_counter,
        int *target_idx, int *restrict temp_offset, const cl_int2 n_tiles, const int nx[2],
        const int moving_window, const int shift, const int ppc[2])
{
	const int size = part_vector->np;
	const int n_tiles_total = n_tiles.x * n_tiles.y;
	const int max_holes = MAX_LEAVING_PART * part_vector->np_max;

	if (moving_window && shift)
	{
		const int npc = TILE_SIZE * ppc[0] * ppc[1];

		#pragma omp for
		for (int i = 0; i < n_tiles.y; i++)
			np_per_tile[(i + 1) * n_tiles.x - 1] += npc;
	}

	prefix_sum_serial(np_per_tile, n_tiles_total + 1);
	part_vector->np = np_per_tile[n_tiles_total];

	for (int i = 1; i < n_tiles_total; i++)
	{
		int diff = np_per_tile[i] - tile_offset[i];
		if (diff < 0) temp_offset[i] += -diff;
		else if (diff > 0) temp_offset[i - 1] += diff;
	}

	prefix_sum_serial(temp_offset, n_tiles_total);

	memcpy(sort_counter, temp_offset, n_tiles_total * sizeof(int));
	memcpy(tile_offset, np_per_tile, (n_tiles_total + 1) * sizeof(int));

	#pragma omp for schedule(static) collapse(2)
	for (int tile_y = 0; tile_y < n_tiles.y; tile_y++)
	{
		for (int tile_x = 0; tile_x < n_tiles.x; tile_x++)
		{
			const int tile_idx = tile_x + tile_y * n_tiles.x;
			const int begin = tile_offset[tile_idx];
			const int end = tile_offset[tile_idx + 1];
			int offset = temp_offset[tile_idx];

			for (int k = begin; k < end; k++)
			{
				int ix = part_vector->cell_idx[k].x / TILE_SIZE;
				int iy = part_vector->cell_idx[k].y / TILE_SIZE;
				int target_tile = ix + iy * n_tiles.x;
				int idx;

				if (part_vector->cell_idx[k].x < 0 || part_vector->cell_idx[k].x >= nx[0]
				        || k >= size) target_idx[offset++] = k;
				else if (target_tile != tile_idx)
				{
					target_idx[offset++] = k;

//					#pragma omp atomic capture
//					idx = sort_counter[target_tile]++;

					idx = __atomic_fetch_add(sort_counter + target_tile, 1, __ATOMIC_RELAXED);

					temp_part->cell_idx[idx] = part_vector->cell_idx[k];
					temp_part->position[idx] = part_vector->position[k];
					temp_part->velocity[idx] = part_vector->velocity[k];
				}
			}
		}
	}

	if(part_vector->np < size)
	{
		#pragma omp for
		for (int k = part_vector->np; k < size; k++)
		{
			int ix = part_vector->cell_idx[k].x / TILE_SIZE;
			int iy = part_vector->cell_idx[k].y / TILE_SIZE;
			int target_tile = ix + iy * n_tiles.x;
			int idx;

//			#pragma omp atomic capture
//			idx = sort_counter[target_tile]++;
			idx = __atomic_fetch_add(sort_counter + target_tile, 1, __ATOMIC_RELAXED);

			temp_part->cell_idx[idx] = part_vector->cell_idx[k];
			temp_part->position[idx] = part_vector->position[k];
			temp_part->velocity[idx] = part_vector->velocity[k];
		}
	}

	if (moving_window && shift)
	{
		#pragma omp for
		for (int k = 0; k < new_part->np; k++)
		{
			int ix = new_part->cell_idx[k].x / TILE_SIZE;
			int iy = new_part->cell_idx[k].y / TILE_SIZE;
			int target_tile = ix + iy * n_tiles.x;
			int idx;

//			#pragma omp atomic capture
//			idx = sort_counter[target_tile]++;
			idx = __atomic_fetch_add(sort_counter + target_tile, 1, __ATOMIC_RELAXED);

			temp_part->cell_idx[idx] = new_part->cell_idx[k];
			temp_part->position[idx] = new_part->position[k];
			temp_part->velocity[idx] = new_part->velocity[k];
		}
	}

	#pragma omp for collapse(2)
	for (int tile_y = 0; tile_y < n_tiles.y; tile_y++)
	{
		for (int tile_x = 0; tile_x < n_tiles.x; tile_x++)
		{
			const int current_tile = tile_x + tile_y * n_tiles.x;
			const int begin = temp_offset[current_tile];
			const int end = sort_counter[current_tile];

			for (int i = begin; i < end; i++)
			{
				const int target = target_idx[i];
				part_vector->cell_idx[target] = temp_part->cell_idx[i];
				part_vector->position[target] = temp_part->position[i];
				part_vector->velocity[target] = temp_part->velocity[i];

			}
		}
	}
}

#endif

/*********************************************************************************************
 Initialization
 *********************************************************************************************/

// Set the momentum of the injected particles
void spec_set_u(cl_float3 *vector, const int start, const int end, const t_part_data ufl[3],
        const t_part_data uth[3])
{
	for (int i = start; i < end; i++)
	{
		vector[i].x = ufl[0] + uth[0] * rand_norm();
		vector[i].y = ufl[1] + uth[1] * rand_norm();
		vector[i].z = ufl[2] + uth[2] * rand_norm();
	}
}

// Set the position of the injected particles
void spec_set_x(t_part_vector *vector, const int range[][2], const int ppc[2],
        const t_density *part_density, const t_part_data dx[2], const int n_move)
{
	float *poscell;
	int start, end;

	// Calculate particle positions inside the cell
	const int npc = ppc[0] * ppc[1];
	t_part_data const dpcx = 1.0f / ppc[0];
	t_part_data const dpcy = 1.0f / ppc[1];

	poscell = malloc(2 * npc * sizeof(t_part_data));

	int ip = 0;
	for (int j = 0; j < ppc[1]; j++)
	{
		for (int i = 0; i < ppc[0]; i++)
		{
			poscell[ip] = dpcx * (i + 0.5);
			poscell[ip + 1] = dpcy * (j + 0.5);
			ip += 2;
		}
	}

	// Set position of particles in the specified grid range according to the density profile
	switch (part_density->type)
	{
		case STEP:    // Step like density profile

			// Get edge position normalized to cell size;
			start = part_density->start / dx[0] - n_move;
			if (range[0][0] > start) start = range[0][0];

			end = range[0][1];
			break;

		case SLAB:    // Slab like density profile
			// Get edge position normalized to cell size;
			start = part_density->start / dx[0] - n_move;
			end = part_density->end / dx[0] - n_move;

			if (start < range[0][0]) start = range[0][0];
			if (end > range[0][1]) end = range[0][1];
			break;

		default:    // Uniform density
			start = range[0][0];
			end = range[0][1];

	}

	ip = vector->np;

	for (int j = range[1][0]; j < range[1][1]; j++)
	{
		for (int i = start; i < end; i++)
		{
			for (int k = 0; k < npc; k++)
			{
				vector->cell_idx[ip].x = i;
				vector->cell_idx[ip].y = j;
				vector->position[ip].x = poscell[2 * k];
				vector->position[ip].y = poscell[2 * k + 1];
				ip++;
			}
		}
	}

	vector->np = ip;
	free(poscell);
}

// Inject the particles in the simulation
void spec_inject_particles(t_part_vector *part_vector, const int range[][2], const int ppc[2],
        const t_density *part_density, const t_part_data dx[2], const int n_move,
        const t_part_data ufl[3], const t_part_data uth[3])
{
	int start = part_vector->np;

	// Get maximum number of particles to inject
	int np_inj = (range[0][1] - range[0][0]) * (range[1][1] - range[1][0]) * ppc[0] * ppc[1];

	// Check if buffer is large enough and if not reallocate
	if (start + np_inj > part_vector->np_max)
	{
		part_vector->np_max = ((part_vector->np_max + np_inj) / 1024 + 1) * 1024;
		part_vector->cell_idx = realloc((void*) part_vector->cell_idx,
		        part_vector->np_max * sizeof(cl_int2));
		part_vector->velocity = realloc((void*) part_vector->cell_idx,
		        part_vector->np_max * sizeof(cl_float3));
		part_vector->position = realloc((void*) part_vector->cell_idx,
		        part_vector->np_max * sizeof(cl_float2));
	}

	// Set particle positions
	spec_set_x(part_vector, range, ppc, part_density, dx, n_move);

	// Set momentum of injected particles
	spec_set_u(part_vector->velocity, start, part_vector->np, ufl, uth);
}

void spec_new(t_species *spec, char name[], const t_part_data m_q, const int ppc[],
        const t_part_data *ufl, const t_part_data *uth, const int nx[], t_part_data box[],
        const float dt, t_density *density)
{
	// Species name
	strncpy(spec->name, name, MAX_SPNAME_LEN);

	int npc = 1;
	// Store species data
	for (int i = 0; i < 2; i++)
	{
		spec->nx[i] = nx[i];
		spec->ppc[i] = ppc[i];
		npc *= ppc[i];

		spec->box[i] = box[i];
		spec->dx[i] = box[i] / nx[i];
	}

	spec->m_q = m_q;
	spec->q = copysign(1.0f, m_q) / npc;

	spec->dt = dt;
	spec->energy = 0;

	// Initialize particle buffer
	spec->part_vector.np_max = NP_MULTIPLY * nx[0] * nx[1] * npc;
	spec->part_vector.cell_idx = calloc(spec->part_vector.np_max, sizeof(cl_int2));
	spec->part_vector.position = calloc(spec->part_vector.np_max, sizeof(cl_float2));
	spec->part_vector.velocity = calloc(spec->part_vector.np_max, sizeof(cl_float3));

	assert(spec->part_vector.cell_idx);
	assert(spec->part_vector.position);
	assert(spec->part_vector.velocity);

	spec->incoming_part.cell_idx = NULL;
	spec->incoming_part.position = NULL;
	spec->incoming_part.velocity = NULL;
	spec->incoming_part.np_max = 0;
	spec->incoming_part.np = 0;

	// Initialize density profile
	if (density)
	{
		spec->density = *density;
		if (spec->density.n == 0.) spec->density.n = 1.0;
	} else
	{
		// Default values
		spec->density = (t_density ) { .type = UNIFORM, .n = 1.0 };
	}

	// Initialize temperature profile
	if (ufl)
	{
		for (int i = 0; i < 3; i++)
			spec->ufl[i] = ufl[i];
	} else
	{
		for (int i = 0; i < 3; i++)
			spec->ufl[i] = 0;
	}

	// Density multiplier
	spec->q *= fabsf(spec->density.n);

	if (uth)
	{
		for (int i = 0; i < 3; i++)
			spec->uth[i] = uth[i];
	} else
	{
		for (int i = 0; i < 3; i++)
			spec->uth[i] = 0;
	}

	// Reset iteration number
	spec->iter = 0;

	// Reset moving window information
	spec->moving_window = 0;
	spec->n_move = 0;

	// Inject initial particle distribution
	spec->part_vector.np = 0;
	const int range[][2] = { { 0, nx[0] }, { 0, nx[1] } };
	spec_inject_particles(&spec->part_vector, range, spec->ppc, &spec->density, spec->dx,
	        spec->n_move, ufl, uth);

	spec_init_tiles(spec, nx);
}

void spec_init_tiles(t_species *spec, const int nx[2])
{
	spec->n_tiles.x = ceil((float) nx[0] / TILE_SIZE);
	spec->n_tiles.y = ceil((float) nx[1] / TILE_SIZE);

	const int n_tiles_total = spec->n_tiles.x * spec->n_tiles.y;

	spec->tile_offset = calloc((n_tiles_total + 1), sizeof(int));
	spec->np_per_tile = calloc((n_tiles_total + 1), sizeof(int));

	spec->sort_counter = malloc(n_tiles_total * sizeof(int));
	spec->temp_offset = malloc(n_tiles_total * sizeof(int));

	int *restrict new_pos = malloc(spec->part_vector.np * sizeof(int));

	for (int k = 0; k < spec->part_vector.np; k++)
	{
		int ix = spec->part_vector.cell_idx[k].x / TILE_SIZE;
		int iy = spec->part_vector.cell_idx[k].y / TILE_SIZE;
		int target_idx = ix + iy * spec->n_tiles.x;

		new_pos[k] = spec->tile_offset[target_idx]++;
	}

	prefix_sum_serial(spec->tile_offset, n_tiles_total + 1);

	for (int k = 0; k < spec->part_vector.np; k++)
	{
		int ix = spec->part_vector.cell_idx[k].x / TILE_SIZE;
		int iy = spec->part_vector.cell_idx[k].y / TILE_SIZE;
		new_pos[k] += spec->tile_offset[ix + iy * spec->n_tiles.x];
	}

	t_part_vector temp;
	temp.cell_idx = malloc(spec->part_vector.np * sizeof(cl_int2));
	temp.position = malloc(spec->part_vector.np * sizeof(cl_float2));
	temp.velocity = malloc(spec->part_vector.np * sizeof(cl_float3));

	memcpy(temp.cell_idx, spec->part_vector.cell_idx, spec->part_vector.np * sizeof(cl_int2));
	memcpy(temp.position, spec->part_vector.position, spec->part_vector.np * sizeof(cl_float2));
	memcpy(temp.velocity, spec->part_vector.velocity, spec->part_vector.np * sizeof(cl_float3));

	for (int i = 0; i < spec->part_vector.np; i++)
	{
		spec->part_vector.cell_idx[new_pos[i]] = temp.cell_idx[i];
		spec->part_vector.position[new_pos[i]] = temp.position[i];
		spec->part_vector.velocity[new_pos[i]] = temp.velocity[i];
	}

	// Cleaning
	free(temp.cell_idx);
	free(temp.position);
	free(temp.velocity);
	free(new_pos);

	// Init temporary vector for particle sorting
	const int max_holes = MAX_LEAVING_PART * spec->part_vector.np_max;
	spec->target_idx = malloc(max_holes * sizeof(int));

	spec->temp_part.cell_idx = malloc(max_holes * sizeof(cl_int2));
	spec->temp_part.position = malloc(max_holes * sizeof(cl_float2));
	spec->temp_part.velocity = malloc(max_holes * sizeof(cl_float3));
	spec->temp_part.np = 0;
	spec->temp_part.np_max = max_holes;
}

void spec_set_moving_window(t_species *spec)
{
	spec->moving_window = 1;

	const int range[2][2] = { { spec->nx[0] - 1, spec->nx[0] }, { 0, spec->nx[1] } };
	const int np_inj = spec->nx[1] * spec->ppc[0] * spec->ppc[1];

	spec->incoming_part.cell_idx = malloc(np_inj * sizeof(cl_int2));
	spec->incoming_part.position = malloc(np_inj * sizeof(cl_float2));
	spec->incoming_part.velocity = malloc(np_inj * sizeof(cl_float3));
	spec->incoming_part.np_max = np_inj;
	spec->incoming_part.np = 0;

	spec_inject_particles(&spec->incoming_part, range, spec->ppc, &spec->density, spec->dx, 0,
	        spec->ufl, spec->uth);
}

void spec_delete(t_species *spec)
{
	free(spec->tile_offset);
	free(spec->np_per_tile);

	free(spec->part_vector.cell_idx);
	free(spec->part_vector.position);
	free(spec->part_vector.velocity);

	free(spec->temp_part.cell_idx);
	free(spec->temp_part.position);
	free(spec->temp_part.velocity);

	free(spec->target_idx);
	free(spec->sort_counter);
	free(spec->temp_offset);

	if (spec->moving_window)
	{
		free(spec->incoming_part.cell_idx);
		free(spec->incoming_part.position);
		free(spec->incoming_part.velocity);
	}

	spec->part_vector.np = -1;
}

/*********************************************************************************************
 Current Deposition
 *********************************************************************************************/

void dep_current_esk(int ix0, int iy0, int di, int dj, t_part_data x0, t_part_data y0,
        t_part_data x1, t_part_data y1, t_part_data qvx, t_part_data qvy, t_part_data qvz,
        t_current *current)
{

	int i, j;
	t_fld S0x[4], S0y[4], S1x[4], S1y[4], DSx[4], DSy[4];
	t_fld Wx[16], Wy[16], Wz[16];

	S0x[0] = 0.0f;
	S0x[1] = 1.0f - x0;
	S0x[2] = x0;
	S0x[3] = 0.0f;

	S0y[0] = 0.0f;
	S0y[1] = 1.0f - y0;
	S0y[2] = y0;
	S0y[3] = 0.0f;

	for (i = 0; i < 4; i++)
	{
		S1x[i] = 0.0f;
		S1y[i] = 0.0f;
	}

	S1x[1 + di] = 1.0f - x1;
	S1x[2 + di] = x1;

	S1y[1 + dj] = 1.0f - y1;
	S1y[2 + dj] = y1;

	for (i = 0; i < 4; i++)
	{
		DSx[i] = S1x[i] - S0x[i];
		DSy[i] = S1y[i] - S0y[i];
	}

	for (j = 0; j < 4; j++)
	{
		for (i = 0; i < 4; i++)
		{
			Wx[i + 4 * j] = DSx[i] * (S0y[j] + DSy[j] / 2.0f);
			Wy[i + 4 * j] = DSy[j] * (S0x[i] + DSx[i] / 2.0f);
			Wz[i + 4 * j] = S0x[i] * S0y[j] + DSx[i] * S0y[j] / 2.0f + S0x[i] * DSy[j] / 2.0f
			        + DSx[i] * DSy[j] / 3.0f;
		}
	}

	// jx
	const int nrow = current->nrow;
	t_vfld *restrict const J = current->J;

	for (j = 0; j < 4; j++)
	{
		t_fld c;

		c = -qvx * Wx[4 * j];
		J[ix0 - 1 + (iy0 - 1 + j) * nrow].x += c;
		for (i = 1; i < 4; i++)
		{
			c -= qvx * Wx[i + 4 * j];
			J[ix0 + i - 1 + (iy0 - 1 + j) * nrow].x += c;
		}
	}

	// jy
	for (i = 0; i < 4; i++)
	{
		t_fld c;

		c = -qvy * Wy[i];
		J[ix0 + i - 1 + (iy0 - 1) * nrow].y += c;
		for (j = 1; j < 4; j++)
		{
			c -= qvy * Wy[i + 4 * j];
			J[ix0 + i - 1 + (iy0 - 1 + j) * nrow].y += c;
		}
	}

	// jz
	for (j = 0; j < 4; j++)
	{
		for (i = 0; i < 4; i++)
		{
			J[ix0 + i - 1 + (iy0 - 1 + j) * nrow].z += qvz * Wz[i + 4 * j];
		}
	}

}

void dep_current_zamb(int ix, int iy, int di, int dj, float x0, float y0, float dx, float dy,
        float qnx, float qny, float qvz, t_vfld *restrict J, const int nrow)
{
	// Split the particle trajectory
	typedef struct {
		float x0, x1, y0, y1, dx, dy, qvz;
		int ix, iy;
	} t_vp;

	t_vp vp[3];
	int vnp = 1;

	// split
	vp[0].x0 = x0;
	vp[0].y0 = y0;

	vp[0].dx = dx;
	vp[0].dy = dy;

	vp[0].x1 = x0 + dx;
	vp[0].y1 = y0 + dy;

	vp[0].qvz = qvz / 2.0;

	vp[0].ix = ix;
	vp[0].iy = iy;

	// x split
	if (di != 0)
	{
		//int ib = ( di+1 )>>1;
		int ib = (di == 1);

		float delta = (x0 + dx - ib) / dx;

		// Add new particle
		vp[1].x0 = 1 - ib;
		vp[1].x1 = (x0 + dx) - di;
		vp[1].dx = dx * delta;
		vp[1].ix = ix + di;

		float ycross = y0 + dy * (1.0f - delta);

		vp[1].y0 = ycross;
		vp[1].y1 = vp[0].y1;
		vp[1].dy = dy * delta;
		vp[1].iy = iy;

		vp[1].qvz = vp[0].qvz * delta;

		// Correct previous particle
		vp[0].x1 = ib;
		vp[0].dx *= (1.0f - delta);

		vp[0].dy *= (1.0f - delta);
		vp[0].y1 = ycross;

		vp[0].qvz *= (1.0f - delta);

		vnp++;
	}

	// ysplit
	if (dj != 0)
	{
		int isy = 1 - (vp[0].y1 < 0.0f || vp[0].y1 >= 1.0f);

		// int jb = ( dj+1 )>>1;
		int jb = (dj == 1);

		// The static analyser gets confused by this but it is correct
		float delta = (vp[isy].y1 - jb) / vp[isy].dy;

		// Add new particle
		vp[vnp].y0 = 1 - jb;
		vp[vnp].y1 = vp[isy].y1 - dj;
		vp[vnp].dy = vp[isy].dy * delta;
		vp[vnp].iy = vp[isy].iy + dj;

		float xcross = vp[isy].x0 + vp[isy].dx * (1.0f - delta);

		vp[vnp].x0 = xcross;
		vp[vnp].x1 = vp[isy].x1;
		vp[vnp].dx = vp[isy].dx * delta;
		vp[vnp].ix = vp[isy].ix;

		vp[vnp].qvz = vp[isy].qvz * delta;

		// Correct previous particle
		vp[isy].y1 = jb;
		vp[isy].dy *= (1.0f - delta);

		vp[isy].dx *= (1.0f - delta);
		vp[isy].x1 = xcross;

		vp[isy].qvz *= (1.0f - delta);

		// Correct extra vp if needed
		if (isy < vnp - 1)
		{
			vp[1].y0 -= dj;
			vp[1].y1 -= dj;
			vp[1].iy += dj;
		}
		vnp++;
	}

	// Deposit virtual particle currents
	for (int k = 0; k < vnp; k++)
	{
		float S0x[2], S1x[2], S0y[2], S1y[2];
		float wl1, wl2;
		float wp1[2], wp2[2];

		S0x[0] = 1.0f - vp[k].x0;
		S0x[1] = vp[k].x0;

		S1x[0] = 1.0f - vp[k].x1;
		S1x[1] = vp[k].x1;

		S0y[0] = 1.0f - vp[k].y0;
		S0y[1] = vp[k].y0;

		S1y[0] = 1.0f - vp[k].y1;
		S1y[1] = vp[k].y1;

		wl1 = qnx * vp[k].dx;
		wl2 = qny * vp[k].dy;

		wp1[0] = 0.5f * (S0y[0] + S1y[0]);
		wp1[1] = 0.5f * (S0y[1] + S1y[1]);

		wp2[0] = 0.5f * (S0x[0] + S1x[0]);
		wp2[1] = 0.5f * (S0x[1] + S1x[1]);

		J[vp[k].ix + nrow * vp[k].iy].x += wl1 * wp1[0];
		J[vp[k].ix + nrow * (vp[k].iy + 1)].x += wl1 * wp1[1];

		J[vp[k].ix + nrow * vp[k].iy].y += wl2 * wp2[0];
		J[vp[k].ix + 1 + nrow * vp[k].iy].y += wl2 * wp2[1];

		J[vp[k].ix + nrow * vp[k].iy].z += vp[k].qvz
				* (S0x[0] * S0y[0] + S1x[0] * S1y[0] + (S0x[0] * S1y[0] - S1x[0] * S0y[0]) / 2.0f);
		J[vp[k].ix + 1 + nrow * vp[k].iy].z += vp[k].qvz
				* (S0x[1] * S0y[0] + S1x[1] * S1y[0] + (S0x[1] * S1y[0] - S1x[1] * S0y[0]) / 2.0f);
		J[vp[k].ix + nrow * (vp[k].iy + 1)].z += vp[k].qvz
				* (S0x[0] * S0y[1] + S1x[0] * S1y[1] + (S0x[0] * S1y[1] - S1x[0] * S0y[1]) / 2.0f);
		J[vp[k].ix + 1 + nrow * (vp[k].iy + 1)].z += vp[k].qvz
				* (S0x[1] * S0y[1] + S1x[1] * S1y[1] + (S0x[1] * S1y[1] - S1x[1] * S0y[1]) / 2.0f);
	}
}

/*********************************************************************************************
 Particle advance
 *********************************************************************************************/

void interpolate_fld(const t_vfld *restrict const E, const t_vfld *restrict const B, const int nrow,
        const int ix, const int iy, const float x, const float y, t_vfld *restrict const Ep,
        t_vfld *restrict const Bp)
{
	const int ih = ix + ((x < 0.5f) ? -1 : 0);
	const int jh = iy + ((y < 0.5f) ? -1 : 0);

	const float w1h = x + ((x < 0.5f) ? 0.5f : -0.5f);
	const float w2h = y + ((y < 0.5f) ? 0.5f : -0.5f);

	Ep->x = (E[ih + iy * nrow].x * (1.0f - w1h) + E[ih + 1 + iy * nrow].x * w1h) * (1.0f - y)
	        + (E[ih + (iy + 1) * nrow].x * (1.0f - w1h) + E[ih + 1 + (iy + 1) * nrow].x * w1h) * y;
	Ep->y = (E[ix + jh * nrow].y * (1.0f - x) + E[ix + 1 + jh * nrow].y * x) * (1.0f - w2h)
	        + (E[ix + (jh + 1) * nrow].y * (1.0f - x) + E[ix + 1 + (jh + 1) * nrow].y * x) * w2h;
	Ep->z = (E[ix + iy * nrow].z * (1.0f - x) + E[ix + 1 + iy * nrow].z * x) * (1.0f - y)
	        + (E[ix + (iy + 1) * nrow].z * (1.0f - x) + E[ix + 1 + (iy + 1) * nrow].z * x) * y;

	Bp->x = (B[ix + jh * nrow].x * (1.0f - x) + B[ix + 1 + jh * nrow].x * x) * (1.0f - w2h)
	        + (B[ix + (jh + 1) * nrow].x * (1.0f - x) + B[ix + 1 + (jh + 1) * nrow].x * x) * w2h;
	Bp->y = (B[ih + iy * nrow].y * (1.0f - w1h) + B[ih + 1 + iy * nrow].y * w1h) * (1.0f - y)
	        + (B[ih + (iy + 1) * nrow].y * (1.0f - w1h) + B[ih + 1 + (iy + 1) * nrow].y * w1h) * y;
	Bp->z = (B[ih + jh * nrow].z * (1.0f - w1h) + B[ih + 1 + jh * nrow].z * w1h) * (1.0f - w2h)
	        + (B[ih + (jh + 1) * nrow].z * (1.0f - w1h) + B[ih + 1 + (jh + 1) * nrow].z * w1h)
	                * w2h;
}

#if TARGET_GPU || TARGET_FPGA
void spec_advance(t_species *spec, t_emf *emf, t_current *current)
{
	// Advance internal iteration number
	spec->iter += 1;

	const int shift = (spec->iter * spec->dt > spec->dx[0] * (spec->n_move + 1));
	const t_part_data tem = 0.5 * spec->dt / spec->m_q;
	const t_part_data dt_dx = spec->dt / spec->dx[0];
	const t_part_data dt_dy = spec->dt / spec->dx[1];

	// Auxiliary values for current deposition
	t_part_data qnx = spec->q * spec->dx[0] / spec->dt;
	t_part_data qny = spec->q * spec->dx[1] / spec->dt;

	memset(spec->temp_offset, 0, spec->n_tiles.x * spec->n_tiles.y * sizeof(int));
	memset(spec->np_per_tile, 0, (spec->n_tiles.x * spec->n_tiles.y + 1) * sizeof(int));

	spec_advance_opencl(spec->part_vector.cell_idx, spec->part_vector.position,
	        spec->part_vector.velocity, spec->tile_offset, spec->np_per_tile, spec->temp_offset,
	        spec->part_vector.np_max, emf->E_buf, emf->B_buf, current->J_buf, emf->nrow,
	        emf->total_size, tem, dt_dx, dt_dy, qnx, qny, spec->q, spec->nx[0], spec->nx[1],
	        spec->n_tiles, spec->moving_window, shift);

	// Increase moving window counter
	if (spec->moving_window && shift) spec->n_move++;

	spec_sort(&spec->part_vector, &spec->temp_part, &spec->incoming_part, spec->tile_offset,
	        spec->np_per_tile, spec->sort_counter, spec->target_idx, spec->temp_offset,
	        spec->n_tiles, spec->nx, spec->moving_window, shift, spec->ppc);
}

#else
void spec_advance(t_species *spec, t_emf *emf, t_current *current)
{
	uint64_t t0 = timer_ticks();

	// Advance internal iteration number
	spec->iter += 1;

	const int shift = (spec->iter * spec->dt > spec->dx[0] * (spec->n_move + 1));

	const t_part_data tem = 0.5 * spec->dt / spec->m_q;
	const t_part_data dt_dx = spec->dt / spec->dx[0];
	const t_part_data dt_dy = spec->dt / spec->dx[1];

	// Auxiliary values for current deposition
	t_part_data qnx = spec->q * spec->dx[0] / spec->dt;
	t_part_data qny = spec->q * spec->dx[1] / spec->dt;

	const int nx0 = spec->nx[0];
	const int nx1 = spec->nx[1];

	memset(spec->temp_offset, 0, spec->n_tiles.x * spec->n_tiles.y * sizeof(int));
	memset(spec->np_per_tile, 0, (spec->n_tiles.x * spec->n_tiles.y + 1) * sizeof(int));

	// Advance particles
	#pragma omp for schedule(static) collapse(2)
	for (int tile_y = 0; tile_y < spec->n_tiles.y; tile_y++)
	{
		for (int tile_x = 0; tile_x < spec->n_tiles.x; tile_x++)
		{
			const int current_tile = tile_x + tile_y * spec->n_tiles.x;
			const int begin = spec->tile_offset[current_tile];
			const int end = spec->tile_offset[current_tile + 1];

			const int begin_x = tile_x * TILE_SIZE;
			const int begin_y = tile_y * TILE_SIZE;

			const int local_nj = TILE_SIZE + 3;
			t_vfld local_J[local_nj * local_nj] = { 0 };

			for (int i = begin; i < end; i++)
			{
				t_vfld Ep, Bp;

				int ix, iy, local_ix, local_iy;

				t_part_data utx, uty, utz;
				t_part_data ux, uy, uz, rg;
				t_part_data utsq, qvz;
				t_part_data gtem, otsq;

				t_part_data x0, y0;
				t_part_data x1, y1;

				int di, dj;
				float dx, dy;

				x0 = spec->part_vector.position[i].x;
				y0 = spec->part_vector.position[i].y;

				ix = spec->part_vector.cell_idx[i].x;
				iy = spec->part_vector.cell_idx[i].y;

				local_ix = ix - begin_x + 1;
				local_iy = iy - begin_y + 1;

				// Load particle momenta
				ux = spec->part_vector.velocity[i].x;
				uy = spec->part_vector.velocity[i].y;
				uz = spec->part_vector.velocity[i].z;

				// interpolate fields
				interpolate_fld(emf->E, emf->B, emf->nrow, ix, iy, x0, y0, &Ep, &Bp);

				// advance u using Boris scheme
				Ep.x *= tem;
				Ep.y *= tem;
				Ep.z *= tem;

				utx = ux + Ep.x;
				uty = uy + Ep.y;
				utz = uz + Ep.z;

				// Perform first half of the rotation
				utsq = utx * utx + uty * uty + utz * utz;
				gtem = tem / sqrtf(1.0f + utsq);

				Bp.x *= gtem;
				Bp.y *= gtem;
				Bp.z *= gtem;

				otsq = 2.0f / (1.0f + Bp.x * Bp.x + Bp.y * Bp.y + Bp.z * Bp.z);

				ux = utx + uty * Bp.z - utz * Bp.y;
				uy = uty + utz * Bp.x - utx * Bp.z;
				uz = utz + utx * Bp.y - uty * Bp.x;

				// Perform second half of the rotation
				Bp.x *= otsq;
				Bp.y *= otsq;
				Bp.z *= otsq;

				utx += uy * Bp.z - uz * Bp.y;
				uty += uz * Bp.x - ux * Bp.z;
				utz += ux * Bp.y - uy * Bp.x;

				// Perform second half of electric field acceleration
				ux = utx + Ep.x;
				uy = uty + Ep.y;
				uz = utz + Ep.z;

				// push particle
				rg = 1.0f / sqrtf(1.0f + ux * ux + uy * uy + uz * uz);

				dx = dt_dx * rg * ux;
				dy = dt_dy * rg * uy;

				x1 = x0 + dx;
				y1 = y0 + dy;

				di = LTRIM(x1);
				dj = LTRIM(y1);

				qvz = spec->q * uz * rg;

				// deposit current using Eskirepov method
				//		dep_current_esk(spec->part_vector.cell_idx[i].x, spec->part_vector.cell_idx[i].y, di, dj, spec->part_vector.position[i].x, spec->part_vector.position[i].y, x1, y1, qnx, qny,
				//				qvz, current);

				dep_current_zamb(local_ix, local_iy, di, dj, x0, y0, dx, dy, qnx, qny, qvz, local_J,
				        local_nj);

				ix += di;
				iy += dj;

				// First shift particle left (if applicable), then check for particles leaving the box
				if (!spec->moving_window) ix += ((ix < 0) ? nx0 : 0) - ((ix >= nx0) ? nx0 : 0);
				else if (spec->moving_window && shift) ix--;

				iy += ((iy < 0) ? nx1 : 0) - ((iy >= nx1) ? nx1 : 0);

				// Store results
				spec->part_vector.velocity[i].x = ux;
				spec->part_vector.velocity[i].y = uy;
				spec->part_vector.velocity[i].z = uz;
				spec->part_vector.position[i].x = x1 - di;
				spec->part_vector.position[i].y = y1 - dj;
				spec->part_vector.cell_idx[i].x = ix;
				spec->part_vector.cell_idx[i].y = iy;

				int target_tile = (ix / TILE_SIZE) + (iy / TILE_SIZE) * spec->n_tiles.x;

				if (ix >= 0 && ix < nx0)
				{
					#pragma omp atomic
					spec->np_per_tile[target_tile]++;

					if (target_tile != current_tile)
						spec->temp_offset[current_tile]++;
				} else spec->temp_offset[current_tile]++;
			}

			for (int j = 0; j < local_nj; j++)
			{
				for (int i = 0; i < local_nj; i++)
				{
					const int global_idx = begin_x + i + (begin_y + j) * current->nrow;

					#pragma omp atomic
					current->J_buf[global_idx].x += local_J[i + j * local_nj].x;

					#pragma omp atomic
					current->J_buf[global_idx].y += local_J[i + j * local_nj].y;

					#pragma omp atomic
					current->J_buf[global_idx].z += local_J[i + j * local_nj].z;
				}
			}
		}
	}

	// Increase moving window counter
	if (spec->moving_window && shift) spec->n_move++;

	spec_sort(&spec->part_vector, &spec->temp_part, &spec->incoming_part, spec->tile_offset,
	        spec->np_per_tile, spec->sort_counter, spec->target_idx, spec->temp_offset,
	        spec->n_tiles, spec->nx, spec->moving_window, shift, spec->ppc);

	_spec_npush += spec->part_vector.np;
	_spec_time += timer_interval_seconds(t0, timer_ticks());
}

#endif

/*********************************************************************************************
 Charge Deposition
 *********************************************************************************************/

void spec_deposit_charge(const t_species *spec, t_part_data *charge)
{
	int i, j;

	// Charge array is expected to have 1 guard cell at the upper boundary
	int nrow = spec->nx[0] + 1;
	t_part_data q = spec->q;

	for (i = 0; i < spec->part_vector.np; i++)
	{
		int idx = spec->part_vector.cell_idx[i].x + nrow * spec->part_vector.cell_idx[i].y;

		t_fld w1, w2;

		w1 = spec->part_vector.position[i].x;
		w2 = spec->part_vector.position[i].y;

		charge[idx] += (1.0f - w1) * (1.0f - w2) * q;
		charge[idx + 1] += (w1) * (1.0f - w2) * q;
		charge[idx + nrow] += (1.0f - w1) * (w2) * q;
		charge[idx + 1 + nrow] += (w1) * (w2) * q;
	}

	// Correct boundary values

	// x
	if (!spec->moving_window)
	{
		for (j = 0; j < spec->nx[1] + 1; j++)
		{
			charge[0 + j * nrow] += charge[spec->nx[0] + j * nrow];
		}
	}

	// y - Periodic boundaries
	for (i = 0; i < spec->nx[0] + 1; i++)
	{
		charge[i + 0] += charge[i + spec->nx[1] * nrow];
	}

}

/*********************************************************************************************
 Diagnostics
 *********************************************************************************************/

void spec_rep_particles(const t_species *spec, const char path[128])
{

	t_zdf_file part_file;

	int i;

	const char *quants[] = { "x1", "x2", "u1", "u2", "u3" };

	const char *units[] = { "c/\\omega_p", "c/\\omega_p", "c", "c", "c" };

	t_zdf_iteration iter = { .n = spec->iter, .t = spec->iter * spec->dt,
	        .time_units = "1/\\omega_p" };

	// Allocate buffer for positions

	t_zdf_part_info info = { .name = (char*) spec->name, .nquants = 5, .quants = (char**) quants,
	        .units = (char**) units, .np = spec->part_vector.np };

	// Create file and add description
	zdf_part_file_open(&part_file, &info, &iter, path);

	// Add positions and generalized velocities
	size_t size = (spec->part_vector.np) * sizeof(float);
	float *data = malloc(size);

	// x1
	for (i = 0; i < spec->part_vector.np; i++)
		data[i] = (spec->n_move + spec->part_vector.cell_idx[i].x + spec->part_vector.position[i].x)
		        * spec->dx[0];
	zdf_part_file_add_quant(&part_file, quants[0], data, spec->part_vector.np);

	// x2
	for (i = 0; i < spec->part_vector.np; i++)
		data[i] = (spec->part_vector.cell_idx[i].y + spec->part_vector.position[i].y) * spec->dx[1];
	zdf_part_file_add_quant(&part_file, quants[1], data, spec->part_vector.np);

	// ux
	for (i = 0; i < spec->part_vector.np; i++)
		data[i] = spec->part_vector.velocity[i].x;
	zdf_part_file_add_quant(&part_file, quants[2], data, spec->part_vector.np);

	// uy
	for (i = 0; i < spec->part_vector.np; i++)
		data[i] = spec->part_vector.velocity[i].y;
	zdf_part_file_add_quant(&part_file, quants[3], data, spec->part_vector.np);

	// uz
	for (i = 0; i < spec->part_vector.np; i++)
		data[i] = spec->part_vector.velocity[i].z;
	zdf_part_file_add_quant(&part_file, quants[4], data, spec->part_vector.np);

	free(data);

	zdf_close_file(&part_file);
}

void spec_rep_charge(const t_species *spec, const char path[128])
{
	t_part_data *buf, *charge, *b, *c;
	size_t size;
	int i, j;

	// Add 1 guard cell to the upper boundary
	size = (spec->nx[0] + 1) * (spec->nx[1] + 1) * sizeof(t_part_data);
	charge = malloc(size);
	memset(charge, 0, size);

	// Deposit the charge
	spec_deposit_charge(spec, charge);

	// Compact the data to save the file (throw away guard cells)
	size = (spec->nx[0]) * (spec->nx[1]);
	buf = malloc(size * sizeof(float));

	b = buf;
	c = charge;
	for (j = 0; j < spec->nx[1]; j++)
	{
		for (i = 0; i < spec->nx[0]; i++)
		{
			b[i] = c[i];
		}
		b += spec->nx[0];
		c += spec->nx[0] + 1;
	}

	free(charge);

	t_zdf_grid_axis axis[2];
	axis[0] = (t_zdf_grid_axis ) { .min = 0.0, .max = spec->box[0], .label = "x_1",
			        .units = "c/\\omega_p" };

	axis[1] = (t_zdf_grid_axis ) { .min = 0.0, .max = spec->box[1], .label = "x_2",
			        .units = "c/\\omega_p" };

	t_zdf_grid_info info = { .ndims = 2, .label = "charge", .units = "n_e", .axis = axis };

	info.nx[0] = spec->nx[0];
	info.nx[1] = spec->nx[1];

	t_zdf_iteration iter = { .n = spec->iter, .t = spec->iter * spec->dt,
	        .time_units = "1/\\omega_p" };

	zdf_save_grid(buf, &info, &iter, path);

	free(buf);
}

void spec_pha_axis(const t_species *spec, int i0, int np, int quant, float *axis)
{
	int i;

	switch (quant)
	{
		case X1:
			for (i = 0; i < np; i++)
				axis[i] = (spec->part_vector.position[i0 + i].x
				        + spec->part_vector.cell_idx[i0 + i].x) * spec->dx[0];
			break;
		case X2:
			for (i = 0; i < np; i++)
				axis[i] = (spec->part_vector.position[i0 + i].y
				        + spec->part_vector.cell_idx[i0 + i].y) * spec->dx[1];
			break;
		case U1:
			for (i = 0; i < np; i++)
				axis[i] = spec->part_vector.velocity[i0 + i].x;
			break;
		case U2:
			for (i = 0; i < np; i++)
				axis[i] = spec->part_vector.velocity[i0 + i].y;
			break;
		case U3:
			for (i = 0; i < np; i++)
				axis[i] = spec->part_vector.velocity[i0 + i].z;
			break;
	}
}

const char* spec_pha_axis_units(int quant)
{
	switch (quant)
	{
		case X1:
		case X2:
			return ("c/\\omega_p");
			break;
		case U1:
		case U2:
		case U3:
			return ("m_e c");
	}
	return ("");
}

void spec_deposit_pha(const t_species *spec, const int rep_type, const int pha_nx[],
        const float pha_range[][2], float *restrict buf)
{
	const int BUF_SIZE = 1024;
	float pha_x1[BUF_SIZE], pha_x2[BUF_SIZE];

	const int nrow = pha_nx[0];

	const int quant1 = rep_type & 0x000F;
	const int quant2 = (rep_type & 0x00F0) >> 4;

	const float x1min = pha_range[0][0];
	const float x2min = pha_range[1][0];

	const float rdx1 = pha_nx[0] / (pha_range[0][1] - pha_range[0][0]);
	const float rdx2 = pha_nx[1] / (pha_range[1][1] - pha_range[1][0]);

	for (int i = 0; i < spec->part_vector.np; i += BUF_SIZE)
	{
		int np = (i + BUF_SIZE > spec->part_vector.np) ? spec->part_vector.np - i : BUF_SIZE;

		spec_pha_axis(spec, i, np, quant1, pha_x1);
		spec_pha_axis(spec, i, np, quant2, pha_x2);

		for (int k = 0; k < np; k++)
		{

			float nx1 = (pha_x1[k] - x1min) * rdx1;
			float nx2 = (pha_x2[k] - x2min) * rdx2;

			int i1 = (int) (nx1 + 0.5f);
			int i2 = (int) (nx2 + 0.5f);

			float w1 = nx1 - i1 + 0.5f;
			float w2 = nx2 - i2 + 0.5f;

			int idx = i1 + nrow * i2;

			if (i2 >= 0 && i2 < pha_nx[1])
			{

				if (i1 >= 0 && i1 < pha_nx[0])
				{
					buf[idx] += (1.0f - w1) * (1.0f - w2) * spec->q;
				}

				if (i1 + 1 >= 0 && i1 + 1 < pha_nx[0])
				{
					buf[idx + 1] += w1 * (1.0f - w2) * spec->q;
				}
			}

			idx += nrow;
			if (i2 + 1 >= 0 && i2 + 1 < pha_nx[1])
			{

				if (i1 >= 0 && i1 < pha_nx[0])
				{
					buf[idx] += (1.0f - w1) * w2 * spec->q;
				}

				if (i1 + 1 >= 0 && i1 + 1 < pha_nx[0])
				{
					buf[idx + 1] += w1 * w2 * spec->q;
				}
			}

		}

	}
}

void spec_rep_pha(const t_species *spec, const int rep_type, const int pha_nx[],
        const float pha_range[][2], const char path[128])
{

	char const *const pha_ax_name[] = { "x1", "x2", "x3", "u1", "u2", "u3" };
	char pha_name[64];

	// Allocate phasespace buffer
	float *restrict buf = malloc(pha_nx[0] * pha_nx[1] * sizeof(float));
	memset(buf, 0, pha_nx[0] * pha_nx[1] * sizeof(float));

	// Deposit the phasespace
	spec_deposit_pha(spec, rep_type, pha_nx, pha_range, buf);

	// save the data in hdf5 format
	int quant1 = rep_type & 0x000F;
	int quant2 = (rep_type & 0x00F0) >> 4;

	const char *pha_ax1_units = spec_pha_axis_units(quant1);
	const char *pha_ax2_units = spec_pha_axis_units(quant2);

	sprintf(pha_name, "%s%s", pha_ax_name[quant1 - 1], pha_ax_name[quant2 - 1]);

	t_zdf_grid_axis axis[2];
	axis[0] = (t_zdf_grid_axis ) { .min = pha_range[0][0], .max = pha_range[0][1],
			        .label = (char*) pha_ax_name[quant1 - 1], .units = (char*) pha_ax1_units };

	axis[1] = (t_zdf_grid_axis ) { .min = pha_range[1][0], .max = pha_range[1][1],
			        .label = (char*) pha_ax_name[quant2 - 1], .units = (char*) pha_ax2_units };

	t_zdf_grid_info info = { .ndims = 2, .label = pha_name, .units = "a.u.", .axis = axis };

	info.nx[0] = pha_nx[0];
	info.nx[1] = pha_nx[1];

	t_zdf_iteration iter = { .n = spec->iter, .t = spec->iter * spec->dt,
	        .time_units = "1/\\omega_p" };

	zdf_save_grid(buf, &info, &iter, path);

	// Free temp. buffer
	free(buf);

}

void spec_report(const t_species *spec, const int rep_type, const int pha_nx[],
        const float pha_range[][2], const char path[128])
{

	switch (rep_type & 0xF000)
	{
		case CHARGE:
			spec_rep_charge(spec, path);
			break;

		case PHA:
			spec_rep_pha(spec, rep_type, pha_nx, pha_range, path);
			break;

		case PARTICLES:
			spec_rep_particles(spec, path);
			break;
	}

}

void spec_report_csv(const t_species *spec, const char sim_name[64])
{
	t_part_data *buf, *charge, *b, *c;
	size_t size;
	int i, j;

	// Add 1 guard cell to the upper boundary
	size = (spec->nx[0] + 1) * (spec->nx[1] + 1) * sizeof(t_part_data);
	charge = malloc(size);
	memset(charge, 0, size);

	// Deposit the charge
	spec_deposit_charge(spec, charge);

	// Compact the data to save the file (throw away guard cells)
	size = (spec->nx[0]) * (spec->nx[1]);
	buf = malloc(size * sizeof(float));

	b = buf;
	c = charge;
	for (j = 0; j < spec->nx[1]; j++)
	{
		for (i = 0; i < spec->nx[0]; i++)
		{
			b[i] = c[i];
		}
		b += spec->nx[0];
		c += spec->nx[0] + 1;
	}

	char filename[128];
	sprintf(filename, "%s_charge_map_%d.csv", spec->name, spec->iter);
	save_data_csv(buf, spec->nx[0], spec->nx[1], filename, sim_name);

	free(charge);
	free(buf);
}

void spec_calculate_energy(t_species *restrict spec)
{
	spec->energy = 0;

	for (int i = 0; i < spec->part_vector.np; i++)
	{
		t_part_data usq = spec->part_vector.velocity[i].x * spec->part_vector.velocity[i].x
		        + spec->part_vector.velocity[i].y * spec->part_vector.velocity[i].y
		        + spec->part_vector.velocity[i].z * spec->part_vector.velocity[i].z;
		t_part_data gamma = sqrtf(1 + usq);
		spec->energy += usq / (gamma + 1);
	}
}