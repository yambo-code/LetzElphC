/* This function reads all the lattice, pseudo and wfcs data from SAVE DIR */

#include <math.h>
#include <mpi.h>
#include <netcdf.h>
#include <netcdf_par.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/ELPH_timers.h"
#include "common/constants.h"
#include "common/cwalk/cwalk.h"
#include "common/dtypes.h"
#include "common/error.h"
#include "common/numerical_func.h"
#include "common/parallel.h"
#include "dvloc/dvloc.h"
#include "elphC.h"
#include "io.h"
#include "io/qe/qe_io.h"
#include "mpi_bcast.h"
#include "nonloc/fcoeff.h"
#include "symmetries/symmetries.h"

/*static functions */
static void quick_read(const int ncid, char* var_name, void* data_out);

static void alloc_and_set_Gvec(
    ELPH_float* gvec, const ND_int ik, const ELPH_float* totalGvecs,
    const ND_int ng_total, const ELPH_float* Gvecidxs, const ND_int ng_shell,
    const ELPH_float* lat_param, const ND_int nG, const ND_int nG_shift);

static void quick_read_sub(const int ncid, char* var_name, const size_t* startp,
                           const size_t* countp, void* data_out);

static void get_wfc_from_save(ND_int spin_stride_len, ND_int ik, ND_int nkiBZ,
                              ND_int nspin, ND_int nspinor, ND_int start_band,
                              ND_int nbnds, ND_int nG, ND_int G_shift,
                              const char* save_dir, char* work_array,
                              const size_t work_array_len, ELPH_cmplx* out_wfc,
                              MPI_Comm comm);

static void free_phonon_data(struct Phonon* phonon);

static inline ND_int get_wf_io_pool(ND_int ik, ND_int q, ND_int r);

static bool check_ele_in_array(ND_int* arr_in, ND_int nelements,
                               ND_int check_ele);

/* Function body */
void read_and_alloc_save_data(char* SAVEdir, const struct ELPH_MPI_Comms* Comm,
                              ND_int start_band, ND_int end_band,
                              struct WFC** wfcs, char* ph_save_dir,
                              struct Lattice* lattice, struct Pseudo* pseudo,
                              struct Phonon* phonon,
                              enum ELPH_dft_code dft_code)
{
    /* This function allocates and reads data from SAVE dir.
    The following data is read : wfcs(in iBZ), lattice and pseudo
    start_band, end_band are give in fortran indices i.e 1st band starts
    from 1 instead of 0;
    // pseudo_pots  : list of pseudopotential files (for now only upf2 is
    supported) These variables are not allocated here
    ---
    Lattice :
    dimesnion : (read from input )
    ---
    pseudo :
    NOTE : ph_save_dir must be available on all processes
    */

    // Expect wfcs, all are read by the single IO and broadcasted

    ELPH_start_clock("Save I.O");

    int mpi_error;

    char** pseudo_pots = NULL;
    // pseudo_pots need to be defined only on 0-rank cpu of Comm->commW

    // first get the basic dft/dfpt data from dft code (code specific) before
    // anything
    char* pp_head = "ns.kb_pp_pwscf";  // Change this accordingly
    if (dft_code == DFT_CODE_QE)
    {
        // char* pp_head = "ns.kb_pp_pwscf";
        get_data_from_qe(lattice, phonon, ph_save_dir, &pseudo_pots, Comm);
    }
    else
    {
        error_msg("Only QE supported");
    }

    lattice->nfftz_loc = get_mpi_local_size_idx(
        lattice->fft_dims[2], &(lattice->nfftz_loc_shift), Comm->commK);

    if (lattice->nfftz_loc < 1)
    {
        error_msg(
            "Some cpus do not contain plane waves. Over parallelization !.");
    }

    int dbid, ppid, tempid, retval;  // file ids for ns.db1 , pp_pwscf*

    size_t temp_str_len = strlen(ph_save_dir) + strlen(SAVEdir) + 100;
    char* temp_str = malloc(temp_str_len);
    CHECK_ALLOC(temp_str);

    int nkBZ;  // total kpoints in BZ

    char* elements = malloc(3 * 104);  // coded 104 elements
    CHECK_ALLOC(elements);
    {
        char* temp =
            "NA\0H \0He\0Li\0Be\0B \0C \0N \0O "
            "\0F \0Ne\0Na\0Mg\0Al\0Si\0P \0S \0Cl\0Ar\0K \0Ca"
            "\0Sc\0Ti\0V \0Cr\0Mn\0Fe\0Co\0Ni\0Cu\0Zn\0Ga\0Ge"
            "\0As\0Se\0Br\0Kr\0Rb\0Sr\0Y \0Zr\0Nb\0Mo\0Tc\0Ru"
            "\0Rh\0Pd\0Ag\0Cd\0In\0Sn\0Sb\0Te\0I \0Xe\0Cs\0Ba"
            "\0La\0Ce\0Pr\0Nd\0Pm\0Sm\0Eu\0Gd\0Tb\0Dy\0Ho\0Er"
            "\0Tm\0Yb\0Lu\0Hf\0Ta\0W \0Re\0Os\0Ir\0Pt\0Au\0Hg"
            "\0Tl\0Pb\0Bi\0Po\0At\0Rn\0Fr\0Ra\0Ac\0Th\0Pa\0U "
            "\0Np\0Pu\0Am\0Cm\0Bk\0Cf\0Es\0Fm\0Md\0No\0Lr\0";
        memcpy(elements, temp, 3 * 104);
        // printf(elements+3*Z) will give symbol for Z
    }
    /*****/
    if (Comm->commW_rank == 0)
    {
        cwk_path_join(SAVEdir, "ndb.kindx", temp_str, temp_str_len);

        if ((retval = nc_open(temp_str, NC_NOWRITE, &tempid)))
        {
            ERR(retval);
        }

#if defined(YAMBO_LT_5_1)
        ELPH_float kindx_pars[7];
        quick_read(tempid, "PARS", kindx_pars);
        nkBZ = (int)rint(kindx_pars[0]);  // FIX ME !! or kindx_pars[5] ?
#else
        int nkBZ_read;
        quick_read(tempid, "nXkbz", &nkBZ_read);
        nkBZ = nkBZ_read;
#endif

        if ((retval = nc_close(tempid)))
        {
            ERR(retval);
        }
    }
    /* broad cast (int nkBZ) */
    mpi_error = MPI_Bcast(&nkBZ, 1, MPI_INT, 0, Comm->commW);
    MPI_error_msg(mpi_error);
    /*******/
    if (nkBZ / Comm->nkpools < 1)
    {
        error_msg(
            "There are no kpoints in some cpus, Make sure nkpool < # of "
            "kpoints in full BZ.");
    }
    // set nBZ
    lattice->nkpts_BZ = nkBZ;
    // printf("Debug-%d \n",1);
    ELPH_float dimensions[18];
    if (Comm->commW_rank == 0)
    {
        cwk_path_join(SAVEdir, "ns.db1", temp_str, temp_str_len);
        if ((retval = nc_open(temp_str, NC_NOWRITE, &dbid)))
        {
            ERR(retval);
        }
        quick_read(dbid, "DIMENSIONS", dimensions);
    }
    /* bcast ELPH_float dimensions[18] */
    mpi_error = MPI_Bcast(dimensions, 18, ELPH_MPI_float, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    lattice->nspinor = rint(dimensions[11]);
    lattice->nspin = rint(dimensions[12]);
    lattice->timerev = rint(dimensions[9]);
    lattice->total_bands = rint(dimensions[5]);
    lattice->nsym = rint(dimensions[10]);
    lattice->nkpts_iBZ = rint(dimensions[6]);
    ;

    int nibz = lattice->nkpts_iBZ;

    if (start_band < 1 || end_band < 1)
    {
        if (Comm->commW_rank == 0)
        {
            fprintf(stderr,
                    "Warning : invalid bands used in calculation. computing "
                    "matrix elements for all bands, "
                    "Bands index belong to [1,nbnds] \n");
        }
        start_band = 1;
        end_band = lattice->total_bands;
    }
    if (start_band > lattice->total_bands || end_band > lattice->total_bands ||
        start_band >= end_band)
    {
        if (Comm->commW_rank == 0)
        {
            fprintf(
                stderr,
                "Warning : invalid bands used in calculation. computing matrix "
                "elements for all bands \n");
        }
        start_band = 1;
        end_band = lattice->total_bands;
    }

    lattice->start_band = start_band;
    lattice->end_band = end_band;
    lattice->nbnds = end_band - start_band + 1;

    ELPH_float lat_param[3];
    if (Comm->commW_rank == 0)
    {
        quick_read(dbid, "LATTICE_PARAMETER", lat_param);
    }
    /*Bcast ELPH_float lat_param[3] */
    mpi_error = MPI_Bcast(lat_param, 3, ELPH_MPI_float, 0, Comm->commW);
    MPI_error_msg(mpi_error);
    // alloc memory for kpots, symm,
    /* read */

    lattice->kpt_iredBZ = malloc(sizeof(ELPH_float) * 3 * nibz);
    CHECK_ALLOC(lattice->kpt_iredBZ);

    lattice->syms = malloc(sizeof(struct symmetry) * lattice->nsym);
    CHECK_ALLOC(lattice->syms);

    if (Comm->commW_rank == 0)
    {
        ELPH_float* sym_temp =
            malloc(sizeof(ELPH_float) * 12 * lattice->nsym);  // free
        CHECK_ALLOC(sym_temp);
        // (9+3) 9 for symms 3 for tau
        ELPH_float* tau = sym_temp + 9 * lattice->nsym;

        ELPH_float* kiBZtmp = malloc(sizeof(ELPH_float) * 3 * nibz);
        CHECK_ALLOC(kiBZtmp);

        quick_read(dbid, "LATTICE_VECTORS", lattice->alat_vec);
        quick_read(dbid, "SYMMETRY",
                   sym_temp);                   // transpose is read (nsym, 3,3)
        quick_read(dbid, "K-POINTS", kiBZtmp);  // (3,nibz)

        // for now yambo does not support frac. trans. so set it to 0
        /*
            FIX ME: For time reversal symmetry, we set tau = -tau. This is just
           a convention used in the entire code. This is because, the rotation
           matrix already include a negative sign and this must be compensated
        */
        for (ND_int i = 0; i < (3 * lattice->nsym); ++i)
        {
            tau[i] = 0;
        }

        /* Get kpoints to cart coordinates */
        for (ND_int i = 0; i < nibz; ++i)
        {
            kiBZtmp[i + 0 * nibz] /= lat_param[0];
            kiBZtmp[i + 1 * nibz] /= lat_param[1];
            kiBZtmp[i + 2 * nibz] /= lat_param[2];

            /* Transpose to (nibz,3)*/
            lattice->kpt_iredBZ[0 + i * 3] = kiBZtmp[i + 0 * nibz];
            lattice->kpt_iredBZ[1 + i * 3] = kiBZtmp[i + 1 * nibz];
            lattice->kpt_iredBZ[2 + i * 3] = kiBZtmp[i + 2 * nibz];
        }

        // fill the symmetry array

        /* Transpose symmetries as yambo stores in fill the symmetry struct */
        for (ND_int i = 0; i < lattice->nsym; ++i)
        {
            transpose3x3f(sym_temp + 9 * i, lattice->syms[i].Rmat);  // set Rmat

            memcpy(lattice->syms[i].tau, tau + 3 * i,
                   sizeof(ELPH_float) * 3);  // set tau

            /* fill the time-rev array*/
            if (i >= (lattice->nsym / (lattice->timerev + 1)))
            {
                lattice->syms[i].time_rev = true;
            }
            else
            {
                lattice->syms[i].time_rev = false;
            }
        }

        free(kiBZtmp);
        free(sym_temp);
    }

    Bcast_symmetries(lattice->nsym, lattice->syms, 0, Comm->commW);
    mpi_error = MPI_Bcast(lattice->kpt_iredBZ, 3 * nibz, ELPH_MPI_float, 0,
                          Comm->commW);
    MPI_error_msg(mpi_error);

    mpi_error = MPI_Bcast(lattice->alat_vec, 9, ELPH_MPI_float, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    // compute reciprocal vectors and volume
    reciprocal_vecs(lattice->alat_vec, lattice->blat_vec);
    // b[:,i]  are blat. blat comes with 2*pi factor
    lattice->volume = fabs(det3x3(lattice->alat_vec));

    lattice->kpt_fullBZ_crys = calloc(3 * nkBZ, sizeof(ELPH_float));
    CHECK_ALLOC(lattice->kpt_fullBZ_crys);

    lattice->kpt_fullBZ = calloc(3 * nkBZ, sizeof(ELPH_float));
    CHECK_ALLOC(lattice->kpt_fullBZ);

    lattice->kmap = malloc(sizeof(int) * nkBZ * 2);
    CHECK_ALLOC(lattice->kmap);

    ND_int tmp_nkBZ = bz_expand(nibz, lattice->nsym, lattice->kpt_iredBZ,
                                lattice->syms, lattice->alat_vec,
                                lattice->kpt_fullBZ_crys, NULL, lattice->kmap);
    if (tmp_nkBZ != nkBZ)
    {
        error_msg("K-point expansion over full BZ failed.");
    }

    // convert to cart units
    matmul_float('N', 'T', lattice->kpt_fullBZ_crys, lattice->blat_vec,
                 lattice->kpt_fullBZ, 1.0f / (2.0f * ELPH_PI), 0.0, 3, 3, 3,
                 nkBZ, 3, 3);

    // read number of atomic types
    ELPH_float ntype;
    if (Comm->commW_rank == 0)
    {
        quick_read(dbid, "number_of_atom_species", &ntype);
    }
    /* Bcast ELPH_float ntype */
    mpi_error = MPI_Bcast(&ntype, 1, ELPH_MPI_float, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    pseudo->ntype = rint(ntype);

    /* Read atomic positions */
    char* atom_symbols = NULL;  // only needs to defined at W_rank = 0;

    if (Comm->commW_rank == 0)
    {
        ELPH_float* natom_per_type =
            malloc(sizeof(ELPH_float) * 2 * pseudo->ntype);
        CHECK_ALLOC(natom_per_type);

        ELPH_float* atomic_numbers = natom_per_type + pseudo->ntype;

        quick_read(dbid, "N_ATOMS", natom_per_type);
        quick_read(dbid, "atomic_numbers", atomic_numbers);

        lattice->natom = 0;  // Bcast
        for (ND_int ia = 0; ia < pseudo->ntype; ++ia)
        {
            lattice->natom += (int)rint(natom_per_type[ia]);
        }
        ND_int nspec_max = rint(find_maxfloat(natom_per_type, pseudo->ntype));

        ELPH_float* atomic_map =
            malloc(sizeof(ELPH_float) * pseudo->ntype * nspec_max);
        CHECK_ALLOC(atomic_map);

        ELPH_float* atom_pos_temp =
            malloc(sizeof(ELPH_float) * pseudo->ntype * nspec_max * 3);
        CHECK_ALLOC(atom_pos_temp);

        quick_read(dbid, "ATOM_MAP", atomic_map);
        quick_read(dbid, "ATOM_POS", atom_pos_temp);

        lattice->atom_type = malloc(sizeof(int) * lattice->natom);  // Bcast
        CHECK_ALLOC(lattice->atom_type);

        atom_symbols = malloc(3 * pseudo->ntype);
        CHECK_ALLOC(atom_symbols);

        //
        lattice->atomic_pos =
            malloc(sizeof(ELPH_float) * 3 * lattice->natom);  // Bcast
        CHECK_ALLOC(lattice->atomic_pos);

        for (ND_int it = 0; it < pseudo->ntype; ++it)
        {
            ND_int ia_no = rint(atomic_numbers[it]);
            memcpy(atom_symbols + 3 * it, elements + 3 * ia_no,
                   3 * sizeof(char));

            ND_int nspec = rint(natom_per_type[it]);
            for (ND_int ispec = 0; ispec < nspec; ++ispec)
            {
                ND_int iatom = rint(atomic_map[ispec + it * nspec_max] - 1);
                ELPH_float* get_ptr =
                    atom_pos_temp + 3 * (ispec + it * nspec_max);
                ELPH_float* set_ptr = lattice->atomic_pos + 3 * iatom;
                memcpy(set_ptr, get_ptr, 3 * sizeof(ELPH_float));
                lattice->atom_type[iatom] = it;
            }
        }
        free(atomic_map);
        free(atom_pos_temp);
        free(natom_per_type);
    }

    // Bcast variables
    mpi_error = MPI_Bcast(&lattice->natom, 1, MPI_INT, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    lattice->nmodes = 3 * lattice->natom;

    if (Comm->commW_rank != 0)
    {
        lattice->atom_type = malloc(sizeof(int) * lattice->natom);  // Bcast
        CHECK_ALLOC(lattice->atom_type);

        lattice->atomic_pos =
            malloc(sizeof(ELPH_float) * 3 * lattice->natom);  // Bcast
        CHECK_ALLOC(lattice->atomic_pos);
    }

    mpi_error =
        MPI_Bcast(lattice->atom_type, lattice->natom, MPI_INT, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    mpi_error = MPI_Bcast(lattice->atomic_pos, 3 * lattice->natom,
                          ELPH_MPI_float, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    ELPH_float* nGmax =
        malloc(sizeof(ELPH_float) *
               nibz);  // max number of gvectors for each wfc in iBZ
    CHECK_ALLOC(nGmax);

    *wfcs = malloc(sizeof(struct WFC) * nibz);  // wfcs in iBZ
    CHECK_ALLOC(*wfcs);

    struct WFC* wfc_temp = *wfcs;

    /* allocate arrays of arrays for wfc, gvsc, Fk */
    if (Comm->commW_rank == 0)
    {
        quick_read(dbid, "WFC_NG", nGmax);
    }
    /* Bcast ELPH_float * nGmax */
    mpi_error = MPI_Bcast(nGmax, nibz, ELPH_MPI_float, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    // dimensions[]
    ND_int ng_total = rint(dimensions[7]);
    ND_int ng_shell = rint(dimensions[8]);
    ELPH_float* totalGvecs = malloc(sizeof(ELPH_float) * 3 * ng_total);
    CHECK_ALLOC(totalGvecs);

    ELPH_float* Gvecidxs = malloc(sizeof(ELPH_float) * nibz * ng_shell);
    CHECK_ALLOC(Gvecidxs);

    if (Comm->commW_rank == 0)
    {
        quick_read(dbid, "G-VECTORS", totalGvecs);
        quick_read(dbid, "WFC_GRID", Gvecidxs);
        if ((retval = nc_close(dbid)))
        {
            ERR(retval);
        }
    }
    // Need to Bcast totalGvecs and Gvecidxs
    mpi_error =
        MPI_Bcast(totalGvecs, 3 * ng_total, ELPH_MPI_float, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    mpi_error =
        MPI_Bcast(Gvecidxs, nibz * ng_shell, ELPH_MPI_float, 0, Comm->commW);
    MPI_error_msg(mpi_error);
    // ! Warning, Only read only mode for opening files

    // ------------------
    if (Comm->commW_rank == 0)
    {
        cwk_path_join(SAVEdir, pp_head, temp_str, temp_str_len);
        if ((retval = nc_open(temp_str, NC_NOWRITE, &ppid)))
        {
            ERR(retval);
        }

        {
            ELPH_float kb_pars[4];
            quick_read(ppid, "PARS", kb_pars);
            pseudo->nltimesj = rint(kb_pars[2]);
            pseudo->lmax = rint(kb_pars[3]);  // yambo stores lmax+1 ! FIX ME
        }
    }

    mpi_error =
        MPI_Bcast(&(pseudo->nltimesj), 1, ELPH_MPI_ND_INT, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    mpi_error = MPI_Bcast(&(pseudo->lmax), 1, MPI_INT, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    pseudo->PP_table =
        malloc(sizeof(ELPH_float) * pseudo->ntype * pseudo->nltimesj * 3);
    // (nltimesj,natom_types,3) (l+1,2j,pp_spin)
    CHECK_ALLOC(pseudo->PP_table);

    pseudo->Fsign =
        malloc(sizeof(ELPH_float) * pseudo->ntype * pseudo->nltimesj);
    // (nlj_max, natom_types)
    CHECK_ALLOC(pseudo->Fsign);

    if (Comm->commW_rank == 0)
    {
        quick_read(ppid, "PP_TABLE", pseudo->PP_table);
        quick_read(ppid, "PP_KBS", pseudo->Fsign);

        if ((retval = nc_close(ppid)))
        {
            ERR(retval);
        }
    }
    /* Bcast PP_table and Fsign */
    // ------------------
    mpi_error =
        MPI_Bcast(pseudo->PP_table, pseudo->nltimesj * pseudo->ntype * 3,
                  ELPH_MPI_float, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    mpi_error = MPI_Bcast(pseudo->Fsign, pseudo->nltimesj * pseudo->ntype,
                          ELPH_MPI_float, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    // We read the wfcs from one k pool and then broad cast them over rest of
    // the pools. different pools read different wfcs
    // NOte if we change how we divided them here, also edit the get_wf_io_pool
    // function accordingly. better not to touch the distribution.
    ND_int total_kpools = Comm->nkpools * Comm->nqpools;
    ND_int q_kpool_read = nibz / total_kpools;
    ND_int r_kpool_read = nibz % total_kpools;
    //
    for (ND_int ik = 0; ik < nibz; ++ik)
    {
        /*set total pws */
        (wfc_temp + ik)->npw_total = rint(nGmax[ik]);

        ND_int G_shift, pw_this_cpu;
        pw_this_cpu = get_mpi_local_size_idx((wfc_temp + ik)->npw_total,
                                             &G_shift, Comm->commK);

        /* set the local number of pw's */
        (wfc_temp + ik)->npw_loc = pw_this_cpu;
        (wfc_temp + ik)->gvec = malloc(sizeof(ELPH_float) * 3 * pw_this_cpu);
        CHECK_ALLOC((wfc_temp + ik)->gvec);

        alloc_and_set_Gvec((wfc_temp + ik)->gvec, ik, totalGvecs, ng_total,
                           Gvecidxs, ng_shell, lat_param, pw_this_cpu, G_shift);

        /* initiate, allocate and load wfcs*/
        ND_int spin_stride_len =
            lattice->nbnds * lattice->nspinor * pw_this_cpu;
        (wfc_temp + ik)->wfc =
            malloc(sizeof(ELPH_cmplx) * lattice->nspin * spin_stride_len);
        CHECK_ALLOC((wfc_temp + ik)->wfc);
        // (nspin, bands, nspinor, npw)
        // only one K pool must read the wfc, we then bcast to rest of the k
        // pools
        // The pool read this wfc is given by
        ND_int ipool_iibz = get_wf_io_pool(ik, q_kpool_read, r_kpool_read);
        if (Comm->commR_rank == ipool_iibz)
        {
            get_wfc_from_save(spin_stride_len, ik, nibz, lattice->nspin,
                              lattice->nspinor, lattice->start_band,
                              lattice->nbnds, pw_this_cpu, G_shift, SAVEdir,
                              temp_str, temp_str_len, (wfc_temp + ik)->wfc,
                              Comm->commK);
        }

        /* initiate, allocate and load Fk (Kleinbylander Coefficients)*/
        //(nltimesj, ntype, npw_loc)
        (wfc_temp + ik)->Fk = malloc(sizeof(ELPH_float) * pseudo->nltimesj *
                                     pseudo->ntype * pw_this_cpu);
        CHECK_ALLOC((wfc_temp + ik)->Fk);

        char small_buf[64];

        snprintf(small_buf, 64, "%s_fragment_%d", pp_head, (int)(ik + 1));
        cwk_path_join(SAVEdir, small_buf, temp_str, temp_str_len);

        /* Abinit has a aditional spin dimension instead of 2*n projectors */
        if (Comm->commR_rank == ipool_iibz)
        {
            if ((retval = nc_open_par(temp_str, NC_NOWRITE, Comm->commK,
                                      MPI_INFO_NULL, &ppid)))
            {
                ERR(retval);
            }

            snprintf(temp_str, temp_str_len, "PP_KB_K%d", (int)(ik + 1));

            size_t startppkb[] = {0, 0, G_shift};
            size_t countppkb[] = {pseudo->nltimesj, pseudo->ntype, pw_this_cpu};
            quick_read_sub(ppid, temp_str, startppkb, countppkb,
                           (wfc_temp + ik)->Fk);

            if ((retval = nc_close(ppid)))
            {
                ERR(retval);
            }
        }
    }
    //
    //
    // Now we broad cast wfcs to all pools
    //
    for (ND_int ik = 0; ik < nibz; ++ik)
    {
        ND_int ipool_iibz = get_wf_io_pool(ik, q_kpool_read, r_kpool_read);

        ND_int pw_this_cpu = (wfc_temp + ik)->npw_loc;
        ND_int spin_stride_len =
            lattice->nbnds * lattice->nspinor * pw_this_cpu;
        // Bcast the wfc
        mpi_error =
            MPI_Bcast((wfc_temp + ik)->wfc, lattice->nspin * spin_stride_len,
                      ELPH_MPI_cmplx, ipool_iibz, Comm->commR);
        MPI_error_msg(mpi_error);

        // broadcast kb projector
        mpi_error = MPI_Bcast((wfc_temp + ik)->Fk,
                              pseudo->nltimesj * pseudo->ntype * pw_this_cpu,
                              ELPH_MPI_float, ipool_iibz, Comm->commR);
        MPI_error_msg(mpi_error);
    }
    //
    /* Free temp gvec arrays */
    free(totalGvecs);
    free(Gvecidxs);

    // compute f-coeffs for SOC
    alloc_and_Compute_f_Coeff(lattice, pseudo);

    /* Read upf data */
    /* First get the pseudo pots order */
    ND_int* pseudo_order = malloc(sizeof(ND_int) * pseudo->ntype);
    CHECK_ALLOC(pseudo_order);

    for (ND_int ipot1 = 0; ipot1 < pseudo->ntype; ++ipot1)
    {
        pseudo_order[ipot1] = -1;
    }

    if (Comm->commW_rank == 0)
    {
        for (ND_int ipot1 = 0; ipot1 < pseudo->ntype; ++ipot1)
        {
            char temp_ele[3];

            cwk_path_join(ph_save_dir, pseudo_pots[ipot1], temp_str,
                          temp_str_len);

            /* read elements from pseudo pots */
            get_upf_element(temp_str, temp_ele);  // only single process !
            bool found = false;

            for (ND_int ipot2 = 0; ipot2 < pseudo->ntype; ++ipot2)
            {
                if (strcmp(temp_ele, atom_symbols + 3 * ipot2) == 0 &&
                    !check_ele_in_array(pseudo_order, pseudo->ntype, ipot2))
                {
                    found = true;
                    pseudo_order[ipot1] = ipot2;
                    break;
                }
            }
            if (!found)
            {
                fprintf(stderr, "Pseudo for element %s not found \n", temp_ele);
                error_msg("Missing pseudopotential");
            }
        }
    }
    // Bcast pseudo_order[pseudo->ntype]
    mpi_error =
        MPI_Bcast(pseudo_order, pseudo->ntype, ELPH_MPI_ND_INT, 0, Comm->commW);
    MPI_error_msg(mpi_error);

    pseudo->loc_pseudo = malloc(pseudo->ntype * sizeof(struct local_pseudo));
    CHECK_ALLOC(pseudo->loc_pseudo);
    /* Get data from upfs */
    if (Comm->commW_rank == 0)
    {
        for (ND_int ipot = 0; ipot < pseudo->ntype; ++ipot)
        {
            ND_int iorder = pseudo_order[ipot];

            cwk_path_join(ph_save_dir, pseudo_pots[ipot], temp_str,
                          temp_str_len);

            parse_upf(temp_str, pseudo->loc_pseudo + iorder);
        }
    }

    // Bcast all the pseudo information
    for (ND_int itype = 0; itype < pseudo->ntype; ++itype)
    {
        Bcast_local_pseudo(pseudo->loc_pseudo + itype, true, 0, Comm->commW);
    }

    // reuse pseudo_order buffer to find the ngrid max
    for (ND_int ipot = 0; ipot < pseudo->ntype; ++ipot)
    {
        pseudo_order[ipot] = pseudo->loc_pseudo[ipot].ngrid;
    }

    pseudo->ngrid_max = find_maxint(pseudo_order, pseudo->ntype);
    lattice->npw_max = find_maxfloat(nGmax, nibz);
    // find the max number of pws i.e max(nGmax)

    // in the end compute the Vlocg table
    //// first find the qmax for vloc table
    ELPH_float qmax_val = fabs(phonon->qpts_iBZ[0]);
    for (ND_int imax = 0; imax < (phonon->nq_iBZ * 3); ++imax)
    {
        if (fabs(phonon->qpts_iBZ[imax]) > qmax_val)
        {
            qmax_val = fabs(phonon->qpts_iBZ[imax]);
        }
    }
    // Note this needs to be set before compute the Vlocg table
    pseudo->vloc_table->qmax_abs = ceil(fabs(qmax_val)) + 1;
    // Note this must be called in the last else U.B
    create_vlocg_table(lattice, pseudo, Comm);

    // free all buffers
    if (Comm->commW_rank == 0)
    {
        if (pseudo_pots != NULL)
        {
            for (ND_int ipot = 0; ipot < pseudo->ntype; ++ipot)
            {
                free(pseudo_pots[ipot]);
            }
            free(pseudo_pots);
        }
    }

    free(pseudo_order);
    free(atom_symbols);
    free(elements);
    free(nGmax);
    free(temp_str);
    ELPH_stop_clock("Save I.O");
}

// ============ free's

void free_phonon_data(struct Phonon* phonon)
{
    free(phonon->qpts_iBZ);
    free(phonon->qpts_BZ);
    free(phonon->ph_syms);
    free(phonon->qmap);
    free(phonon->nqstar);
    free(phonon->epsilon);
    free(phonon->Zborn);
    free(phonon->Qpole);
}

void free_save_data(struct WFC* wfcs, struct Lattice* lattice,
                    struct Pseudo* pseudo, struct Phonon* phonon)
{
    // free pseudo data
    free_f_Coeff(lattice, pseudo);
    // free fCoeff
    free_vlocg_table(pseudo->vloc_table);
    // free the local interpolation interpolation table
    free(pseudo->Fsign);
    free(pseudo->PP_table);

    if (pseudo->loc_pseudo)
    {
        for (ND_int itype = 0; itype < pseudo->ntype; ++itype)
        {
            free(pseudo->loc_pseudo[itype].Vloc_atomic);
            free(pseudo->loc_pseudo[itype].r_grid);
            free(pseudo->loc_pseudo[itype].rab_grid);
        }
    }
    free(pseudo->loc_pseudo);

    // free wfcs
    ND_int nkiBZ = lattice->nkpts_iBZ;
    /* Free wavefunctions */
    for (ND_int ik = 0; ik < nkiBZ; ++ik)
    {
        free((wfcs + ik)->wfc);
        free((wfcs + ik)->gvec);
        free((wfcs + ik)->Fk);
    }
    free(wfcs);

    // free lattice data
    free(lattice->atomic_pos);
    free(lattice->atom_type);
    free(lattice->kpt_iredBZ);
    free(lattice->kpt_fullBZ);
    free(lattice->kpt_fullBZ_crys);
    free(lattice->kmap);
    free(lattice->syms);

    // free the phonon data
    free_phonon_data(phonon);
}

// ----

// ============ static functions
static void alloc_and_set_Gvec(
    ELPH_float* gvec, const ND_int ik, const ELPH_float* totalGvecs,
    const ND_int ng_total, const ELPH_float* Gvecidxs, const ND_int ng_shell,
    const ELPH_float* lat_param, const ND_int nG, const ND_int nG_shift)
{
    // sets the gvecs for each wfc
    const ELPH_float* gidx_temp = Gvecidxs + ik * ng_shell;

    for (ND_int ig = 0; ig < nG; ++ig)
    {
        ND_int gidx = rint(gidx_temp[ig + nG_shift] - 1);
        if (gidx < 0)
        {
            error_msg("Wrong g indices");
        }
        ELPH_float* gvec_temp = gvec + ig * 3;

        for (int i = 0; i < 3; ++i)
        {
            gvec_temp[i] = totalGvecs[ng_total * i + gidx] / lat_param[i];
        }
    }
}

static void get_wfc_from_save(ND_int spin_stride_len, ND_int ik, ND_int nkiBZ,
                              ND_int nspin, ND_int nspinor, ND_int start_band,
                              ND_int nbnds, ND_int nG, ND_int G_shift,
                              const char* save_dir, char* work_array,
                              const size_t work_array_len, ELPH_cmplx* out_wfc,
                              MPI_Comm comm)
{
    int wfID, retval;
    // NO OPENMP !! , Not thread safe
    for (ND_int is = 0; is < nspin; ++is)
    {
        char tmp_buf[64];
        snprintf(tmp_buf, 64, "ns.wf_fragments_%d_1",
                 (int)(is * nkiBZ + (ik + 1)));
        cwk_path_join(save_dir, tmp_buf, work_array, work_array_len);

        if ((retval = nc_open_par(work_array, NC_NOWRITE, comm, MPI_INFO_NULL,
                                  &wfID)))
        {
            ERR(retval);
        }

        snprintf(work_array, work_array_len,
                 "WF_COMPONENTS_@_SP_POL%d_K%d_BAND_GRP_1", (int)(is + 1),
                 (int)(ik + 1));

        //// (nspin, bands, nspinor, npw)
        size_t startp[4] = {start_band - 1, 0, G_shift, 0};
        size_t countp[4] = {nbnds, nspinor, nG, 2};
        quick_read_sub(wfID, work_array, startp, countp,
                       out_wfc + is * spin_stride_len);

        if ((retval = nc_close(wfID)))
        {
            ERR(retval);
        }
    }
}

static void quick_read(const int ncid, char* var_name, void* data_out)
{ /*  Serial read
      load the entire varible data to data_out pointer
  */
    int varid, retval;

    if ((retval = nc_inq_varid(ncid, var_name, &varid)))
    {
        ERR(retval);  // get the varible id of the file
    }

    if ((retval = nc_get_var(ncid, varid, data_out)))
    {
        ERR(retval);  // get data in floats
    }
}

static void quick_read_sub(const int ncid, char* var_name, const size_t* startp,
                           const size_t* countp, void* data_out)
{ /*  Serial read
      load the slice of varible data to data_out pointer
  */
    int varid, retval;

    if ((retval = nc_inq_varid(ncid, var_name, &varid)))
    {
        ERR(retval);  // get the varible id of the file
    }

    // collective IO
    if ((retval = nc_var_par_access(ncid, varid, NC_COLLECTIVE)))
    {
        ERR(retval);  // NC_COLLECTIVE or NC_INDEPENDENT
    }

    if ((retval = nc_get_vara(ncid, varid, startp, countp, data_out)))
    {
        ERR(retval);  // get data in floats
    }
}

static bool check_ele_in_array(ND_int* arr_in, ND_int nelements,
                               ND_int check_ele)
{
    bool found = false;
    for (int ii = 0; ii < nelements; ++ii)
    {
        if (arr_in[ii] == check_ele)
        {
            found = true;
            break;
        }
    }
    return found;
}

static inline ND_int get_wf_io_pool(ND_int ik, ND_int q, ND_int r)
{
    // a tiny helper function to find which pool should read the wfc
    // We donot do any checks ik in [0,n)
    ND_int offset = r * (q + 1);
    if (ik < offset)
    {
        return ik / (q + 1);
    }
    else
    {
        return (ik - r) / q;
    }
}
