/**
 * ZPIC - em2d
 *
 * Weibel instability
 */

#include <stdlib.h>
#include "../../simulation.h"

void sim_init(t_simulation *sim, int n_regions)
{
	// Time step
	float dt = 0.035;
	float tmax = 70.0;

	// Simulation box
	int nx[2] = {3600, 3600};
	float box[2] = {180, 180};

	// Diagnostic frequency
	int ndump = 500;

	// Initialize particles
	const int n_species = 1;
	t_species *species = (t_species*) malloc(n_species * sizeof(t_species));

	// Use 8x8 particles per cell
	int ppc[] = {8, 8};

	// Initial fluid and thermal velocities
	t_part_data ufl[] = {0.0, 0.0, 0.0};
	t_part_data uth[] = {0.0, 0.0, 0.0};

	spec_new(&species[0], "electrons", -1.0, ppc, ufl, uth, nx, box, dt, NULL, nx[1]);

	// Initialize Simulation data
	sim_new(sim, nx, box, dt, tmax, ndump, species, n_species, "cold-2000-829M-3600-3600", n_regions);

	free(species);
}

void sim_report(t_simulation *sim)
{
	//sim_report_csv(sim);
	sim_report_energy(sim);

	// Bx, By, Bz
	sim_report_grid_zdf(sim, REPORT_BFLD, 0);
	sim_report_grid_zdf(sim, REPORT_BFLD, 1);
	sim_report_grid_zdf(sim, REPORT_BFLD, 2);

	// Jz
	sim_report_grid_zdf(sim, REPORT_CURRENT, 2);

	// electron and positron density
	sim_report_spec_zdf(sim, 0, CHARGE, NULL, NULL);
}
