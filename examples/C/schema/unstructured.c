/* 
 * ADIOS is freely available under the terms of the BSD license described
 * in the COPYING file in the top level directory of this source distribution.
 *
 * Copyright (c) 2008 - 2009.  UT-BATTELLE, LLC. All rights reserved.
 */

/* ADIOS C Example: write variables along with an unstructured mesh. 
 */
#include <stdio.h>
#include <string.h>
#include "mpi.h"
#include "adios.h"


static char *reference = "http://people.sc.fsu.edu/~jburkardt/m_src/twod_to_vtk/twod_to_vtk.html";

//44 lines
static float points[] = {
    0.000,     0.000,
    0.000,     1.000,
    0.000,     2.000,
    0.000,     3.000,
    1.000,     0.000,
    1.000,     1.000,
    1.000,     2.000,
    1.000,     3.000,
    2.000,     0.000,
    2.000,     1.000,
    2.000,     2.000,
    2.000,     3.000,
    3.000,     0.000,
    3.000,     1.000,
    3.000,     2.000,
    3.000,     3.000,
    4.000,     0.000,
    4.000,     1.000,
    4.000,     2.000,
    4.000,     3.000,
    5.000,     0.000,
    5.000,     1.000,
    5.000,     2.000,
    5.000,     3.000,
    6.000,     0.000,
    6.000,     1.000,
    6.000,     2.000,
    6.000,     3.000,
    7.000,     0.000,
    7.000,     1.000,
    7.000,     2.000,
    7.000,     3.000,
    8.000,     0.000,
    8.000,     1.000,
    8.000,     2.000,
    8.000,     3.000,
    9.000,     0.000,
    9.000,     1.000,
    9.000,     2.000,
    9.000,     3.000,
    10.000,     0.000,
    10.000,     1.000,
    10.000,     2.000,
    10.000,     3.000
};

static float points_X[] = {
 0.000, 
 0.000, 
 0.000, 
 0.000, 
 1.000, 
 1.000, 
 1.000, 
 1.000, 
 2.000, 
 2.000, 
 2.000, 
 2.000, 
 3.000, 
 3.000, 
 3.000, 
 3.000, 
 4.000, 
 4.000, 
 4.000, 
 4.000, 
 5.000, 
 5.000, 
 5.000, 
 5.000, 
 6.000, 
 6.000, 
 6.000, 
 6.000, 
 7.000, 
 7.000, 
 7.000, 
 7.000, 
 8.000, 
 8.000, 
 8.000, 
 8.000, 
 9.000, 
 9.000, 
 9.000, 
 9.000, 
 10.000,
 10.000,
 10.000,
 10.000
};

static float points_Y[] = {
  0.000, 
  1.000, 
  2.000, 
  3.000, 
  0.000, 
  1.000, 
  2.000, 
  3.000, 
  0.000, 
  1.000, 
  2.000, 
  3.000, 
  0.000, 
  1.000, 
  2.000, 
  3.000, 
  0.000, 
  1.000, 
  2.000, 
  3.000, 
  0.000, 
  1.000, 
  2.000, 
  3.000, 
  0.000, 
  1.000, 
  2.000, 
  3.000, 
  0.000, 
  1.000, 
  2.000, 
  3.000, 
  0.000, 
  1.000, 
  2.000, 
  3.000, 
  0.000, 
  1.000, 
  2.000, 
  3.000, 
   0.000,
   1.000,
   2.000,
   3.000
};




//60 lines
static int cells[] = {
       1,   2,  12,
      13,  12,   2,
       2,   3,  13,
      14,  13,   3,
       3,   4,  14,
      15,  14,   4,
       4,   5,  15,
      16,  15,   5,
       5,   6,  16,
      17,  16,   6,
       6,   7,  17,
      18,  17,   7,
       7,   8,  18,
      19,  18,   8,
       8,   9,  19,
      20,  19,   9,
       9,  10,  20,
      21,  20,  10,
      10,  11,  21,
      22,  21,  11,
      12,  13,  23,
      24,  23,  13,
      13,  14,  24,
      25,  24,  14,
      14,  15,  25,
      26,  25,  15,
      15,  16,  26,
      27,  26,  16,
      16,  17,  27,
      28,  27,  17,
      17,  18,  28,
      29,  28,  18,
      18,  19,  29,
      30,  29,  19,
      19,  20,  30,
      31,  30,  20,
      20,  21,  31,
      32,  31,  21,
      21,  22,  32,
      33,  32,  22,
      23,  24,  34,
      35,  34,  24,
      24,  25,  35,
      36,  35,  25,
      25,  26,  36,
      37,  36,  26,
      26,  27,  37,
      38,  37,  27,
      27,  28,  38,
      39,  38,  28,
      28,  29,  39,
      40,  39,  29,
      29,  30,  40,
      41,  40,  30,
      30,  31,  41,
      42,  41,  31,
      31,  32,  42,
      43,  42,  32,
      32,  33,  43,
      44,  43,  33
};

static double U[] = {
    0.0000000e+00,
    0.0000000e+00,
    0.0000000e+00,
    0.0000000e+00,
    5.8752800e-03,
    5.8752800e-03,
    5.8752800e-03,
    5.8752800e-03,
    9.5085900e-03,
    9.5085900e-03,
    9.5085900e-03,
    9.5085900e-03,
    9.5135100e-03,
    9.5135100e-03,
    9.5135100e-03,
    9.5135100e-03,
    5.8881600e-03,
    5.8881600e-03,
    5.8881600e-03,
    5.8881600e-03,
    1.5925500e-05,
    1.5925500e-05,
    1.5925500e-05,
    1.5925500e-05,
    5.8623800e-03,
    5.8623800e-03,
    5.8623800e-03,
    5.8623800e-03,
    9.5036500e-03,
    9.5036500e-03,
    9.5036500e-03,
    9.5036500e-03,
    9.5184100e-03,
    9.5184100e-03,
    9.5184100e-03,
    9.5184100e-03,
    5.9010200e-03,
    5.9010200e-03,
    5.9010200e-03,
    5.9010200e-03,
    3.1850900e-05,
    3.1850900e-05,
    3.1850900e-05,
    3.1850900e-05
};

static  double V[] = {
    0.0000000e+00,
    0.0000000e+00,
    0.0000000e+00,
    0.0000000e+00,
    -2.0000000e+00,
    -2.0000000e+00,
    -2.0000000e+00,
    -2.0000000e+00,
    -4.0000000e+00,
    -4.0000000e+00,
    -4.0000000e+00,
    -4.0000000e+00,
    -6.0000000e+00,
    -6.0000000e+00,
    -6.0000000e+00,
    -6.0000000e+00,
    -8.0000000e+00,
    -8.0000000e+00,
    -8.0000000e+00,
    -8.0000000e+00,
    -1.0000000e+01,
    -1.0000000e+01,
    -1.0000000e+01,
    -1.0000000e+01,
    -1.2000000e+01,
    -1.2000000e+01,
    -1.2000000e+01,
    -1.2000000e+01,
    -1.4000000e+01,
    -1.4000000e+01,
    -1.4000000e+01,
    -1.4000000e+01,
    -1.6000000e+01,
    -1.6000000e+01,
    -1.6000000e+01,
    -1.6000000e+01,
    -1.8000000e+01,
    -1.8000000e+01,
    -1.8000000e+01,
    -1.8000000e+01,
    -2.0000000e+01,
    -2.0000000e+01,
    -2.0000000e+01,
    -2.0000000e+01
};


static double T[] = {

    0.0000000e+00 ,
    2.0000000e+00 ,
    2.0000000e+00 ,
    0.0000000e+00 ,
    0.0000000e+00 ,
    2.0000000e+00 ,
    2.0000000e+00 ,
    0.0000000e+00 ,
    0.0000000e+00 ,
    2.0000000e+00 ,
    2.0000000e+00 ,
    0.0000000e+00 ,
    0.0000000e+00 ,
    2.0000000e+00 ,
    2.0000000e+00 ,
    0.0000000e+00 ,
    0.0000000e+00 ,
    2.0000000e+00 ,
    2.0000000e+00 ,
    0.0000000e+00 ,
    0.0000000e+00 ,
    2.0000000e+00 ,
    2.0000000e+00 ,
    0.0000000e+00 ,
    0.0000000e+00 ,
    2.0000000e+00 ,
    2.0000000e+00 ,
    0.0000000e+00 ,
    0.0000000e+00 ,
    2.0000000e+00 ,
    2.0000000e+00 ,
    0.0000000e+00 ,
    0.0000000e+00 ,
    2.0000000e+00 ,
    2.0000000e+00 ,
    0.0000000e+00 ,
    0.0000000e+00 ,
    2.0000000e+00 ,
    2.0000000e+00 ,
    0.0000000e+00 ,
    0.0000000e+00 ,
    2.0000000e+00 ,
    2.0000000e+00 ,
    0.0000000e+00
};


int main (int argc, char ** argv ) 
{
    char        filename [256];
    char        meshname [256] = "unstructured";
    char        xmlfilename[256];
    int         rank, size, i;
    MPI_Comm    comm = MPI_COMM_WORLD;

    int         npoints = 44;
    int         num_cells = 60;
    int         Nspace = 2;
	

    /* ADIOS variables declarations for matching gwrite_temperature.ch */
    int         adios_err;
    uint64_t    adios_groupsize, adios_totalsize;
    int64_t     adios_handle;

    MPI_Init (&argc, &argv);
    MPI_Comm_rank (comm, &rank);
    MPI_Comm_size (comm, &size);


    strcpy (filename, meshname);
    strcat (filename, ".bp");

    strcpy (xmlfilename,meshname);
    strcat (xmlfilename,".xml");

    for(i = 0; i < num_cells * 3; i ++)
    {
	cells[i] --;
    }

    adios_init (xmlfilename, comm);

    adios_open (&adios_handle, "channel", filename, "w", comm);

    adios_groupsize = 4 \
	+ 4 \
	+ 4 \
	+ 8 * (npoints) \
	+ 8 * (npoints) \
	+ 8 * (npoints) \
	+ 4 * (num_cells) * (3) \
	+ 4 * (npoints) * (2) \
	+ 4 * (npoints) \
	+ 4 * (npoints);

    adios_group_size (adios_handle, adios_groupsize, &adios_totalsize);
    adios_write (adios_handle, "npoints", &npoints);
    adios_write (adios_handle, "num_cells", &num_cells);
    adios_write (adios_handle, "Nspace", &Nspace);
    adios_write (adios_handle, "U", U);
    adios_write (adios_handle, "V", V);
    adios_write (adios_handle, "T", T);
    adios_write (adios_handle, "cells", cells);
    adios_write (adios_handle, "points", points);
    adios_write (adios_handle, "points_X", points_X);
    adios_write (adios_handle, "points_Y", points_Y);


    adios_close (adios_handle);

    MPI_Barrier (comm);

    adios_finalize (rank);

    MPI_Finalize ();
    return 0;
}
