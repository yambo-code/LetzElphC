/*
This file contains helper functions that are use in wfc routines
*/
#include <mpi.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "common/numerical_func.h"
#include "common/omp_pragma_def.h"
#include "elphC.h"
#include "wfc.h"

/* rotate Gvectors */
void rotateGvecs(const ELPH_float* Gvecs, const ELPH_float* sym,
                 const ND_int ngvecs, const ELPH_float* lat_vec,
                 const bool inverse, const bool crystal, ELPH_float* G0,
                 ELPH_float* Gvec_out)
{
    /*
    this function rotates G vectors i.e \sum_j {Sij*Gj}
    Inputs :
    Gvecs -> unrotated gvecs ( in cart)
    sym -> 3x3 symmetric matrix
    ngvecs -> number of gvecs
    lat_vec -> lattice vectors a[i] = lat[:,i]
    inverse -> if true applies S^-1 instead of S
    crystal -> output in crystal coordinates. G0 will also be converted to
    crystal coordinates G0 --> (3) a const G vec all to all output vectors (in
    cart). Output : Gvec_out -> rotated Gvecs
    */

    /*
    need to do
    G@S^T-> cart
    G@S^T@lat_vec = G@(lat_vec^T@S)^T -> G_crys
    */

    for (ND_int i = 0; i < 3 * ngvecs; ++i)
    {
        Gvec_out[i] = 0;
    }

    ELPH_float sym_temp[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    const ELPH_float Iden3x3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    char transsym = 'N';
    if (inverse)
    {
        transsym = 'T';
    }

    const ELPH_float* lat_temp = Iden3x3;
    if (crystal)
    {
        lat_temp = lat_vec;
    }

    Gemm3x3f(lat_temp, 'T', sym, transsym, sym_temp);

    matmul_float('N', 'T', Gvecs, sym_temp, Gvec_out, 1.0, 0.0, 3, 3, 3, ngvecs,
                 3, 3);

    if (G0 != NULL)
    {
        if (crystal)
        {
            // convert G0 to crystal.  //
            ELPH_float G0_temp[3];
            memcpy(G0_temp, G0, sizeof(ELPH_float) * 3);
            MatVec3f(lat_vec, G0_temp, true, G0);
        }
        ELPH_OMP_PAR_FOR_SIMD
        for (ND_int i = 0; i < ngvecs; ++i)
        {
            Gvec_out[3 * i] += G0[0];
            Gvec_out[3 * i + 1] += G0[1];
            Gvec_out[3 * i + 2] += G0[2];
        }
    }
}
