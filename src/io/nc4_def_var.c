#include <netcdf.h>
#include <netcdf_par.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/error.h"
#include "io.h"

/* ND_function to create a varible to netCDF */
void def_ncVar(const int ncid, int* varid, ND_int rank, nc_type xtype,
               ND_int* dims, const char* var_name, char** dim_names,
               size_t* chunksize)
{
    // creates a netcdf variable and returns varid

    int retval;     // error iD
    int tmp_varid;  // var iDs // return value

    if ((retval = nc_inq(ncid, NULL, NULL, NULL, NULL)))
    {
        ERR(retval);  // check the ncid
    }

    int* dimids = malloc(rank * sizeof(int));
    CHECK_ALLOC(dimids);
    //
    for (ND_int i = 0; i < rank; ++i)
    {
        if ((retval = nc_inq_dimid(ncid, dim_names[i], dimids + i)))
        {
            if ((retval = nc_def_dim(ncid, dim_names[i], dims[i], dimids + i)))
            {
                ERR(retval);
            }
            // get ids for the omega dimensions
        }
        /* Check is dim len are consistant */
        size_t temp_dim;
        if ((retval = nc_inq_dimlen(ncid, dimids[i], &temp_dim)))
        {
            ERR(retval);
        }
        if ((ND_int)temp_dim != dims[i])
        {
            error_msg(
                "Dimension name already exist with different dimension length");
        }
    }

    if ((retval = nc_def_var(ncid, var_name, xtype, rank, dimids, &tmp_varid)))
    {
        ERR(retval);
    }

    // Set chunking if there
    if (!chunksize)
    {
        if ((retval =
                 nc_def_var_chunking(ncid, tmp_varid, NC_CONTIGUOUS, NULL)))
        {
            ERR(retval);
        }
    }
    else
    {
        if ((retval =
                 nc_def_var_chunking(ncid, tmp_varid, NC_CHUNKED, chunksize)))
        {
            ERR(retval);
        }
    }
    free(dimids);

    *varid = tmp_varid;

    // Note: In the end, we call the nc_put_var() with 0 counts to avoid
    // deadlocks
    /* // when sometimes, some cpus donot perform writes. This is enforced by
     * the netcdf. */

    /* size_t * sp_tmp = calloc( rank, sizeof(*sp_tmp)); */
    /* CHECK_ALLOC(sp_tmp); */

    size_t* sp_tmp = malloc(sizeof(*sp_tmp) * rank);
    CHECK_ALLOC(sp_tmp);
    // let the compiler optimize it to calloc
    for (ND_int irank = 0; irank < rank; ++irank)
    {
        sp_tmp[irank] = 0;
    }

    // no-op call
    if ((retval = nc_put_vara(ncid, tmp_varid, sp_tmp, sp_tmp, NULL)))
    {
        ERR(retval);
    }
    free(sp_tmp);
}
