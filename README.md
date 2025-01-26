# Parallel ZPIC

[ZPIC](https://github.com/zpic-plasma/zpic) is a sequential 2D EM-PIC kinetic plasma simulator based on OSIRIS [1], implementing the same core algorithm and features. From the ZPIC code (em2d variant), we developed several parallel versions to explore asynchronous tasking with [OmpSs-2](https://pm.bsc.es/ompss-2), GPUs with [OpenACC](https://www.openacc.org/)) and RDMA with [GASPI](http://www.gpi-site.com/).

## Versions

|                  | Platform           | Programming Model(s) | Asynchronous |
|------------------|--------------------|----------------------|--------------|
| `serial`         | CPU                | None                 | No           |
| `ompss2`         | CPU                | OmpSs-2              | Yes          |
| `openacc`        | (multi) GPU        | OpenMP + OpenACC     | No           |
| `ompss2_openacc` | (multi) GPU        | OmpSs-2 + OpenACC    | Yes          |
| `mpi_ompss2`     | multiple CPU nodes | MPI + OmpSs-2        | Yes          |
| `gaspi_ompss2`   | multiple CPU nodes | GASPI + OmpSs-2      | Yes          |

## Related Papers and Thesis

N. Guidotti, “Harnessing the power of modern multi-core and GPU systems with tasks with data dependencies,” Instituto Superior Técnico, Lisboa, 2021. [Online]. Available: https://fenix.tecnico.ulisboa.pt/cursos/meic-a/dissertacao/1128253548921901

N. Guidotti et al., “Particle-In-Cell Simulation Using Asynchronous Tasking,” in Euro-Par 2021: Parallel Processing, L. Sousa, N. Roma, and P. Tomás, Eds., in Lecture Notes in Computer Science. Cham: Springer International Publishing, 2021, pp. 482–498. doi: 10.1007/978-3-030-85665-6_30.

O. Korakitis, S. G. de Gonzalo, N. Guidotti, J. Barreto, J. Monteiro, and A. J. Peña, “OmpSs-2 and OpenACC Interoperation,” in 2022 Workshop on Accelerator Programming Using Directives (WACCPD), Nov. 2022, pp. 11–21. doi: 10.1109/WACCPD56842.2022.00007.

O. Korakitis, S. G. De Gonzalo, N. Guidotti, J. P. Barreto, J. C. Monteiro, and A. J. Peña, “Towards OmpSs-2 and OpenACC interoperation,” in Proceedings of the 27th ACM SIGPLAN Symposium on Principles and Practice of Parallel Programming, in PPoPP ’22. New York, NY, USA: Association for Computing Machinery, Mar. 2022, pp. 433–434. doi: 10.1145/3503221.3508401.

## Plasma Experiments / Input
Please check for the [ZPIC documentation](https://github.com/ricardo-fonseca/zpic/blob/master/doc/Documentation.md) for more information for setting up the simulation parameters. The LWFA (Laser Wakefield Acceleration) and Weibel (Instability) simulations are already included in all versions.

For organization purpose, each file is named after the simulation parameters according to the following scheme:
```
<experiment type> - <number of time steps> - <number of particles per species> - <grid size x> - <grid size y>
```

## Output

Like the original ZPIC, all versions report the simulation parameters in the ZDF format. The simulation timing and relevant information are displayed in the terminal after the simulation is completed.

## Compilation and Execution

### Requirements:

OmpSs-based versions:
- [Nanos6 Runtime](https://github.com/bsc-pm/nanos6) (last tested: Nov. 2021)
- [Mercurium Compiler](https://github.com/bsc-pm/mcxx) (last tested: Nov. 2021)

**Important:** This code only works with `regions` dependency model instead of the default a `discrete` model. The dependency model can be changed in the `nanos6.toml` at `$INSTALLATION_PREFIX/share/doc/nanos6/scripts` (installation-wide). Alternatively, the environment variable `NANOS6_CONFIG_OVERRIDE="version.dependencies=regions"` can be used for override the default configuration (specified in the `nanos6.toml`).

OpenACC:
- PGI Compiler 19.10 or newer (later renamed as NVIDIA HPC SDK)
- CUDA v9.0 or newer
- Pascal or newer GPUs
- With OmpSs-2, use this experimental version of the [Nanos6 Runtime](https://github.com/epeec/nanos6-openacc) (get-queue-affinity-update2021 branch)

### Commands

Within the folder, compile ZPIC with

```
make <option> -j
```
Then, run it as
```
./zpic <number of regions>
```
for `serial`, `ompss2`, `openacc` and `ompss2_openacc`. Or

```
mpirun -np <number of processes> ./zpic <number of regions>
```
for `mpi_ompss2` or `gaspi_ompss2`.

### Compilation Flags

`-DTEST`: Print the simulation timing and other information in a CSV friendly format. Disable all reporting and other terminal outputs

`-DENABLE_ADVISE` (`ON` by default): Enable CUDA MemAdvise routines to guide the Unified Memory System. All OpenACC versions

`-DENABLE_PREFETCH` (or `make prefetch`): Enable CUDA MemPrefetch routines (experimental). Pure OpenACC only.

`-DENABLE_AFFINITY` (or `make affinity`): Enable the use of device affinity (the runtime schedule openacc tasks based on the data location). Otherwise, Nanos6 runtime only uses 1 GPU. Only supported by OmpSs@OpenACC

