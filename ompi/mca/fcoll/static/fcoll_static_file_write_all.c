/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2017 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2016 University of Houston. All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2018      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "ompi_config.h"
#include "fcoll_static.h"

#include "mpi.h"
#include "ompi/constants.h"
#include "ompi/mca/fcoll/fcoll.h"
#include "ompi/mca/fcoll/base/fcoll_base_coll_array.h"
#include "ompi/mca/io/ompio/io_ompio.h"
#include "ompi/mca/io/io.h"
#include "math.h"
#include "ompi/mca/pml/pml.h"
#include <unistd.h>

#define DEBUG_ON 0

typedef struct mca_fcoll_static_local_io_array{
    OMPI_MPI_OFFSET_TYPE offset;
    MPI_Aint             length;
    int                  process_id;
}mca_fcoll_static_local_io_array;



static int local_heap_sort (mca_fcoll_static_local_io_array *io_array,
			    int num_entries,
			    int *sorted);

int find_next_index( int proc_index,
		     int c_index,
		     mca_io_ompio_file_t *fh,
		     mca_fcoll_static_local_io_array *global_iov_array,
		     int global_iov_count,
		     int *sorted);

int get_process_id (int rank,
		    mca_io_ompio_file_t *fh);


int
mca_fcoll_static_file_write_all (mca_io_ompio_file_t *fh,
                                 const void *buf,
                                 int count,
                                 struct ompi_datatype_t *datatype,
                                 ompi_status_public_t *status)
{



    size_t max_data = 0;
    MPI_Aint bytes_per_cycle=0;
    struct iovec *iov=NULL, *decoded_iov=NULL;
    uint32_t iov_count=0, iov_index=0;
    int i=0,j=0,l=0, temp_index;
    int ret=OMPI_SUCCESS, cycles, local_cycles, *bytes_per_process=NULL;
    int index, *disp_index=NULL, **blocklen_per_process=NULL;
    int *iovec_count_per_process=NULL, *displs=NULL;
    size_t total_bytes_written=0;
    MPI_Aint **displs_per_process=NULL, *memory_displacements=NULL;
    MPI_Aint bytes_to_write_in_cycle=0, global_iov_count=0, global_count=0;

    mca_fcoll_static_local_io_array *local_iov_array =NULL, *global_iov_array=NULL;
    mca_fcoll_static_local_io_array *file_offsets_for_agg=NULL;
    int *sorted=NULL, *sorted_file_offsets=NULL, temp_pindex, *temp_disp_index=NULL;
    char *send_buf=NULL, *global_buf=NULL;
    int iov_size=0, current_position=0, *current_index=NULL;
    int *bytes_remaining=NULL, entries_per_aggregator=0;
    ompi_datatype_t **recvtype = NULL;
    MPI_Request send_req=NULL, *recv_req=NULL;
    /* For creating datatype of type io_array */
    int blocklen[3] = {1, 1, 1};
    int static_num_io_procs=1;
    ptrdiff_t d[3], base;
    ompi_datatype_t *types[3];
    ompi_datatype_t *io_array_type=MPI_DATATYPE_NULL;
    int my_aggregator=-1;
    bool sendbuf_is_contiguous= false;
    size_t ftype_size;
    ptrdiff_t ftype_extent, lb;


    /*----------------------------------------------*/
#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
    double write_time = 0.0, start_write_time = 0.0, end_write_time = 0.0;
    double comm_time = 0.0, start_comm_time = 0.0, end_comm_time = 0.0;
    double exch_write = 0.0, start_exch = 0.0, end_exch = 0.0;
    mca_common_ompio_print_entry nentry;
#endif


#if DEBUG_ON
    MPI_Aint gc_in;
#endif

    opal_datatype_type_size ( &datatype->super, &ftype_size );
    opal_datatype_get_extent ( &datatype->super, &lb, &ftype_extent );

    /**************************************************************************
     ** 1.  In case the data is not contigous in memory, decode it into an iovec
     **************************************************************************/
    if ( ( ftype_extent == (ptrdiff_t) ftype_size)             &&
         opal_datatype_is_contiguous_memory_layout(&datatype->super,1) &&
         0 == lb ) {
        sendbuf_is_contiguous = true;
    }


    /* In case the data is not contigous in memory, decode it into an iovec */
    if (! sendbuf_is_contiguous ) {
        fh->f_decode_datatype ((struct mca_io_ompio_file_t *)fh,
                               datatype,
                               count,
                               buf,
                               &max_data,
                               &decoded_iov,
                               &iov_count);
    }
    else {
        max_data = count * datatype->super.size;
    }

    if ( MPI_STATUS_IGNORE != status ) {
	status->_ucount = max_data;
    }

    static_num_io_procs = fh->f_get_mca_parameter_value ( "num_aggregators", strlen ("num_aggregators"));
    if ( OMPI_ERR_MAX == static_num_io_procs ) {
        ret = OMPI_ERROR;
        goto exit;
    }
    fh->f_set_aggregator_props ((struct mca_io_ompio_file_t *)fh,
				static_num_io_procs,
				max_data);

    my_aggregator = fh->f_procs_in_group[fh->f_aggregator_index];

    /* io_array datatype  for using in communication*/
    types[0] = &ompi_mpi_long.dt;
    types[1] = &ompi_mpi_long.dt;
    types[2] = &ompi_mpi_int.dt;

    d[0] = (ptrdiff_t)&local_iov_array[0];
    d[1] = (ptrdiff_t)&local_iov_array[0].length;
    d[2] = (ptrdiff_t)&local_iov_array[0].process_id;
    base = d[0];
    for (i=0 ; i<3 ; i++) {
        d[i] -= base;
    }
    ompi_datatype_create_struct (3,
                                 blocklen,
                                 d,
                                 types,
                                 &io_array_type);
    ompi_datatype_commit (&io_array_type);
    /* #########################################################*/



    ret = fh->f_generate_current_file_view((struct mca_io_ompio_file_t *)fh,
                                           max_data,
                                           &iov,
                                           &iov_size);
    if (ret != OMPI_SUCCESS){
        fprintf(stderr,"Current File View Generation Error\n");
        goto exit;
    }

    if (0 == iov_size){
        iov_size  = 1;
    }

    local_iov_array = (mca_fcoll_static_local_io_array *)malloc (iov_size * sizeof(mca_fcoll_static_local_io_array));
    if ( NULL == local_iov_array){
        fprintf(stderr,"local_iov_array allocation error\n");
        ret = OMPI_ERR_OUT_OF_RESOURCE;
        goto exit;
    }


    for (j=0; j < iov_size; j++){
        local_iov_array[j].offset = (OMPI_MPI_OFFSET_TYPE)(intptr_t)
            iov[j].iov_base;
        local_iov_array[j].length = (size_t)iov[j].iov_len;
        local_iov_array[j].process_id = fh->f_rank;

    }

    bytes_per_cycle = fh->f_get_mca_parameter_value ("bytes_per_agg", strlen ("bytes_per_agg"));
    if ( OMPI_ERR_MAX == bytes_per_cycle ) {
        ret = OMPI_ERROR;
        goto exit;
    }
    local_cycles = ceil( ((double)max_data*fh->f_procs_per_group) /bytes_per_cycle);

#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
    start_exch = MPI_Wtime();
#endif
    ret = fh->f_comm->c_coll->coll_allreduce (&local_cycles,
                                             &cycles,
                                             1,
                                             MPI_INT,
                                             MPI_MAX,
                                             fh->f_comm,
                                             fh->f_comm->c_coll->coll_allreduce_module);

    if (OMPI_SUCCESS != ret){
        fprintf(stderr,"local cycles allreduce!\n");
        goto exit;
    }
#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
        end_comm_time = MPI_Wtime();
        comm_time += end_comm_time - start_comm_time;
#endif

    if (my_aggregator == fh->f_rank) {

        disp_index = (int *)malloc (fh->f_procs_per_group * sizeof (int));
        if (NULL == disp_index) {
            opal_output (1, "OUT OF MEMORY\n");
            ret = OMPI_ERR_OUT_OF_RESOURCE;
            goto exit;
        }

        bytes_per_process = (int *) malloc (fh->f_procs_per_group * sizeof(int ));
        if (NULL == bytes_per_process){
            opal_output (1, "OUT OF MEMORY\n");
            ret = OMPI_ERR_OUT_OF_RESOURCE;
            goto exit;
        }

        bytes_remaining = (int *) malloc (fh->f_procs_per_group * sizeof(int));
        if (NULL == bytes_remaining){
            opal_output (1, "OUT OF MEMORY\n");
            ret = OMPI_ERR_OUT_OF_RESOURCE;
            goto exit;
        }

        current_index = (int *) malloc (fh->f_procs_per_group * sizeof(int));
        if (NULL == current_index){
            opal_output (1, "OUT OF MEMORY\n");
            ret = OMPI_ERR_OUT_OF_RESOURCE;
            goto exit;
        }

        blocklen_per_process = (int **)malloc (fh->f_procs_per_group * sizeof (int*));
        if (NULL == blocklen_per_process) {
            opal_output (1, "OUT OF MEMORY\n");
            ret = OMPI_ERR_OUT_OF_RESOURCE;
            goto exit;
        }

        displs_per_process = (MPI_Aint **)
            malloc (fh->f_procs_per_group * sizeof (MPI_Aint*));

        if (NULL == displs_per_process) {
            opal_output (1, "OUT OF MEMORY\n");
            ret = OMPI_ERR_OUT_OF_RESOURCE;
            goto exit;
        }

        for(i=0;i<fh->f_procs_per_group;i++){
            current_index[i] = 0;
            bytes_remaining[i] =0;
            blocklen_per_process[i] = NULL;
            displs_per_process[i] = NULL;
        }
    }

    iovec_count_per_process = (int *) calloc (fh->f_procs_per_group, sizeof(int));
    if (NULL == iovec_count_per_process){
        opal_output (1, "OUT OF MEMORY\n");
        ret = OMPI_ERR_OUT_OF_RESOURCE;
        goto exit;
    }

    displs = (int *) calloc (fh->f_procs_per_group, sizeof(int));
    if (NULL == displs){
        opal_output (1, "OUT OF MEMORY\n");
        ret = OMPI_ERR_OUT_OF_RESOURCE;
        goto exit;
    }

#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
    start_exch = MPI_Wtime();
#endif
    ret = ompi_fcoll_base_coll_allgather_array (&iov_size,
                                           1,
                                           MPI_INT,
                                           iovec_count_per_process,
                                           1,
                                           MPI_INT,
                                           fh->f_aggregator_index,
                                           fh->f_procs_in_group,
                                           fh->f_procs_per_group,
                                           fh->f_comm);

    if( OMPI_SUCCESS != ret){
        fprintf(stderr,"iov size allgatherv array!\n");
        goto exit;
    }
#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
        end_comm_time = MPI_Wtime();
        comm_time += end_comm_time - start_comm_time;
#endif


    if (my_aggregator == fh->f_rank) {
        displs[0] = 0;
        global_iov_count = iovec_count_per_process[0];
        for (i=1 ; i<fh->f_procs_per_group ; i++) {
            global_iov_count += iovec_count_per_process[i];
            displs[i] = displs[i-1] + iovec_count_per_process[i-1];
        }
    }


    if (my_aggregator == fh->f_rank) {
        global_iov_array = (mca_fcoll_static_local_io_array *) malloc (global_iov_count *
                                                      sizeof(mca_fcoll_static_local_io_array));
        if (NULL == global_iov_array){
            opal_output (1, "OUT OF MEMORY\n");
            ret = OMPI_ERR_OUT_OF_RESOURCE;
            goto exit;
        }
    }

#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
    start_exch = MPI_Wtime();
#endif
    ret = ompi_fcoll_base_coll_gatherv_array (local_iov_array,
                                         iov_size,
                                         io_array_type,
                                         global_iov_array,
                                         iovec_count_per_process,
                                         displs,
                                         io_array_type,
                                         fh->f_aggregator_index,
                                         fh->f_procs_in_group,
                                         fh->f_procs_per_group,
                                         fh->f_comm);
    if (OMPI_SUCCESS != ret){
        fprintf(stderr,"global_iov_array gather error!\n");
        goto exit;
    }
#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
        end_comm_time = MPI_Wtime();
        comm_time += end_comm_time - start_comm_time;
#endif

    if (my_aggregator == fh->f_rank) {

        if ( 0 == global_iov_count){
            global_iov_count =  1;
        }

        sorted = (int *)malloc (global_iov_count * sizeof(int));
        if (NULL == sorted) {
            opal_output (1, "OUT OF MEMORY\n");
            ret = OMPI_ERR_OUT_OF_RESOURCE;
            goto exit;
        }
        local_heap_sort (global_iov_array, global_iov_count, sorted);

        recv_req = (MPI_Request *)malloc (fh->f_procs_per_group * sizeof(MPI_Request));
        if (NULL == recv_req){
            opal_output (1, "OUT OF MEMORY\n");
            ret = OMPI_ERR_OUT_OF_RESOURCE;
            goto exit;
        }
        if (NULL == recvtype){
            recvtype = (ompi_datatype_t **) malloc (fh->f_procs_per_group  * sizeof(ompi_datatype_t *));
            if (NULL == recvtype) {
                opal_output (1, "OUT OF MEMORY\n");
                ret = OMPI_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
        }
        for ( i=0; i < fh->f_procs_per_group; i++ ) {
            recvtype[i] = MPI_DATATYPE_NULL;
        }

    }

#if DEBUG_ON

    if (my_aggregator == fh->f_rank) {
        for (gc_in=0; gc_in<global_iov_count; gc_in++){
            printf("%d: Offset[%ld]: %lld, Length[%ld]: %ld\n",
                   global_iov_array[gc_in].process_id,
                   gc_in, global_iov_array[gc_in].offset,
                   gc_in, global_iov_array[gc_in].length);
        }
    }
#endif

#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
    start_exch = MPI_Wtime();
#endif


    for (index = 0; index < cycles; index++){

        if (my_aggregator == fh->f_rank) {
            fh->f_num_of_io_entries = 0;
            if (NULL != fh->f_io_array) {
                free (fh->f_io_array);
                fh->f_io_array = NULL;
            }
            if (NULL != global_buf) {
                free (global_buf);
                global_buf = NULL;
            }

            if ( NULL != recvtype ) {
                for ( i=0; i < fh->f_procs_per_group; i++ ) {
                    if (MPI_DATATYPE_NULL != recvtype[i] ) {
                        ompi_datatype_destroy(&recvtype[i]);
                    }
                }
            }

            for(l=0;l<fh->f_procs_per_group;l++){
                disp_index[l] =  1;
                if (NULL != blocklen_per_process[l]){
                    free(blocklen_per_process[l]);
                    blocklen_per_process[l] = NULL;
                }
                if (NULL != displs_per_process[l]){
                    free(displs_per_process[l]);
                    displs_per_process[l] = NULL;
                }
                blocklen_per_process[l] = (int *) calloc (1, sizeof(int));
                if (NULL == blocklen_per_process[l]) {
                    opal_output (1, "OUT OF MEMORY for blocklen\n");
                    ret = OMPI_ERR_OUT_OF_RESOURCE;
                    goto exit;
                }
                displs_per_process[l] = (MPI_Aint *) calloc (1, sizeof(MPI_Aint));
                if (NULL == displs_per_process[l]){
                    opal_output (1, "OUT OF MEMORY for displs\n");
                    ret = OMPI_ERR_OUT_OF_RESOURCE;
                    goto exit;
                }
            }
            if (NULL != sorted_file_offsets){
                free(sorted_file_offsets);
                sorted_file_offsets = NULL;
            }

            if(NULL != file_offsets_for_agg){
                free(file_offsets_for_agg);
                file_offsets_for_agg = NULL;
            }

            if (NULL != memory_displacements){
                free(memory_displacements);
                memory_displacements = NULL;
            }

        }
        if ( index < local_cycles ) {
            if ((index == local_cycles-1) && (max_data % (bytes_per_cycle/fh->f_procs_per_group)) ) {
                bytes_to_write_in_cycle = max_data - total_bytes_written;
            }
            else if (max_data <= (size_t) (bytes_per_cycle/fh->f_procs_per_group)) {
                bytes_to_write_in_cycle = max_data;
            }
            else {
                bytes_to_write_in_cycle = bytes_per_cycle/ fh->f_procs_per_group;
            }
        }
        else {
            bytes_to_write_in_cycle = 0;
        }
#if DEBUG_ON
        /*    if (my_aggregator == fh->f_rank) {*/
        printf ("***%d: CYCLE %d   Bytes %ld**********\n",
                fh->f_rank,
                index,
                bytes_to_write_in_cycle);
        /* }*/
#endif
        /**********************************************************
         **Gather the Data from all the processes at the writers **
         *********************************************************/

#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
        start_exch = MPI_Wtime();
#endif
        /* gather from each process how many bytes each will be sending */
        ret = ompi_fcoll_base_coll_gather_array (&bytes_to_write_in_cycle,
                                            1,
                                            MPI_INT,
                                            bytes_per_process,
                                            1,
                                            MPI_INT,
                                            fh->f_aggregator_index,
                                            fh->f_procs_in_group,
                                            fh->f_procs_per_group,
                                            fh->f_comm);

        if (OMPI_SUCCESS != ret){
            fprintf(stderr,"bytes_to_write_in_cycle gather error!\n");
            goto exit;
        }
#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
        end_comm_time = MPI_Wtime();
        comm_time += end_comm_time - start_comm_time;
#endif

        /*
          For each aggregator
          it needs to get bytes_to_write_in_cycle from each process
          in group which adds up to bytes_per_cycle

        */
        if (my_aggregator == fh->f_rank) {
            for (i=0;i<fh->f_procs_per_group; i++){

#if DEBUG_ON
                printf ("%d : bytes_per_process : %d\n",
                        fh->f_procs_in_group[i],
                        bytes_per_process[i]);
#endif

                while (bytes_per_process[i] > 0){
                    if (get_process_id(global_iov_array[sorted[current_index[i]]].process_id,
                                       fh) == i){ /* current id owns this entry!*/

                        /*Add and subtract length and create
                          blocklength and displs array*/
                        if (bytes_remaining[i]){ /*Remaining bytes in the current entry of
                                                   the global offset array*/
                            if (bytes_remaining[i] <= bytes_per_process[i]){
                                blocklen_per_process[i][disp_index[i] - 1] = bytes_remaining[i];
                                displs_per_process[i][disp_index[i] - 1] =
                                    global_iov_array[sorted[current_index[i]]].offset +
                                    (global_iov_array[sorted[current_index[i]]].length
                                     - bytes_remaining[i]);

                                blocklen_per_process[i] = (int *) realloc
                                    ((void *)blocklen_per_process[i], (disp_index[i]+1)*sizeof(int));
                                displs_per_process[i] = (MPI_Aint *)realloc
                                    ((void *)displs_per_process[i], (disp_index[i]+1)*sizeof(MPI_Aint));
                                bytes_per_process[i] -= bytes_remaining[i];
                                blocklen_per_process[i][disp_index[i]] = 0;
                                displs_per_process[i][disp_index[i]] = 0;
                                bytes_remaining[i] = 0;
                                disp_index[i] += 1;
                                /* This entry has been used up, we need to move to the
                                   next entry of this process and make current_index point there*/
                                current_index[i]  = find_next_index(i,
                                                                    current_index[i],
                                                                    fh,
                                                                    global_iov_array,
                                                                    global_iov_count,
                                                                    sorted);
                                if (current_index[i] == -1){
                                    /* No more entries left, so Its all done! exit!*/
                                    break;
                                }
                                continue;
                            }
                            else{
                                blocklen_per_process[i][disp_index[i] - 1] = bytes_per_process[i];
                                displs_per_process[i][disp_index[i] - 1] =
                                    global_iov_array[sorted[current_index[i]]].offset +
                                    (global_iov_array[sorted[current_index[i]]].length
                                     - bytes_remaining[i]);
                                bytes_remaining[i] -= bytes_per_process[i];
                                bytes_per_process[i] = 0;
                                break;
                            }
                        }
                        else{
                            if (bytes_per_process[i] <
                                global_iov_array[sorted[current_index[i]]].length){
                                blocklen_per_process[i][disp_index[i] - 1] =
                                    bytes_per_process[i];
                                displs_per_process[i][disp_index[i] - 1] =
                                    global_iov_array[sorted[current_index[i]]].offset;

                                bytes_remaining[i] =
                                    global_iov_array[sorted[current_index[i]]].length -
                                    bytes_per_process[i];
                                bytes_per_process[i] = 0;
                                break;
                            }
                            else {
                                blocklen_per_process[i][disp_index[i] - 1] =
                                    global_iov_array[sorted[current_index[i]]].length;
                                displs_per_process[i][disp_index[i] - 1] =
                                    global_iov_array[sorted[current_index[i]]].offset;
                                blocklen_per_process[i] =
                                    (int *) realloc ((void *)blocklen_per_process[i], (disp_index[i]+1)*sizeof(int));
                                displs_per_process[i] = (MPI_Aint *)realloc
                                    ((void *)displs_per_process[i], (disp_index[i]+1)*sizeof(MPI_Aint));
                                blocklen_per_process[i][disp_index[i]] = 0;
                                displs_per_process[i][disp_index[i]] = 0;
                                disp_index[i] += 1;
                                bytes_per_process[i] -=
                                    global_iov_array[sorted[current_index[i]]].length;
                                current_index[i] = find_next_index(i,
                                                                   current_index[i],
                                                                   fh,
                                                                   global_iov_array,
                                                                   global_iov_count,
                                                                   sorted);
                                if (current_index[i] == -1){
                                    break;
                                }
                            }
                        }
                    }
                    else{
                        current_index[i] = find_next_index(i,
                                                           current_index[i],
                                                           fh,
                                                           global_iov_array,
                                                           global_iov_count,
                                                           sorted);
                        if (current_index[i] == -1){
                            bytes_per_process[i] = 0; /* no more entries left
                                                         to service this request*/
                            continue;
                        }
                    }
                }
            }
            entries_per_aggregator=0;
            for (i=0;i<fh->f_procs_per_group;i++){
                for (j=0;j<disp_index[i];j++){
                    if (blocklen_per_process[i][j] > 0){
                        entries_per_aggregator++;
#if DEBUG_ON
                        printf("%d sends blocklen[%d]: %d, disp[%d]: %ld to %d\n",
                               fh->f_procs_in_group[i],j,
                               blocklen_per_process[i][j],j,
                               displs_per_process[i][j],
                               fh->f_rank);

#endif
                    }

                }
            }

            if (entries_per_aggregator > 0){
                file_offsets_for_agg = (mca_fcoll_static_local_io_array *)
                    malloc(entries_per_aggregator*sizeof(mca_fcoll_static_local_io_array));
                if (NULL == file_offsets_for_agg) {
                    opal_output (1, "OUT OF MEMORY\n");
                    ret = OMPI_ERR_OUT_OF_RESOURCE;
                    goto exit;
                }
                sorted_file_offsets = (int *)
                    malloc (entries_per_aggregator*sizeof(int));
                if (NULL == sorted_file_offsets){
                    opal_output (1, "OUT OF MEMORY\n");
                    ret =  OMPI_ERR_OUT_OF_RESOURCE;
                    goto exit;
                }
                temp_index = 0;
                for (i=0;i<fh->f_procs_per_group; i++){
                    for(j=0;j<disp_index[i];j++){
                        if (blocklen_per_process[i][j] > 0){
                            file_offsets_for_agg[temp_index].length =
                                blocklen_per_process[i][j];
                            file_offsets_for_agg[temp_index].process_id = i;
                            file_offsets_for_agg[temp_index].offset =
                                displs_per_process[i][j];
                            temp_index++;
                        }
                    }
                }
            }
            else{
                continue;
            }
            local_heap_sort (file_offsets_for_agg,
                             entries_per_aggregator,
                             sorted_file_offsets);

            memory_displacements = (MPI_Aint *) malloc
                (entries_per_aggregator * sizeof(MPI_Aint));
            memory_displacements[sorted_file_offsets[0]] = 0;
            for (i=1; i<entries_per_aggregator; i++){
                memory_displacements[sorted_file_offsets[i]] =
                    memory_displacements[sorted_file_offsets[i-1]] +
                    file_offsets_for_agg[sorted_file_offsets[i-1]].length;
            }

            temp_disp_index = (int *)calloc (1, fh->f_procs_per_group * sizeof (int));
            if (NULL == temp_disp_index) {
                opal_output (1, "OUT OF MEMORY\n");
                ret = OMPI_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
            global_count = 0;
            for (i=0;i<entries_per_aggregator;i++){
                temp_pindex =
                    file_offsets_for_agg[sorted_file_offsets[i]].process_id;
                displs_per_process[temp_pindex][temp_disp_index[temp_pindex]] =
                    memory_displacements[sorted_file_offsets[i]];
                if (temp_disp_index[temp_pindex] < disp_index[temp_pindex])
                    temp_disp_index[temp_pindex] += 1;
                else{
                    printf("temp_disp_index[%d]: %d is greater than disp_index[%d]: %d\n",
                           temp_pindex, temp_disp_index[temp_pindex],
                           temp_pindex, disp_index[temp_pindex]);
                }
                global_count +=
                    file_offsets_for_agg[sorted_file_offsets[i]].length;
            }
            if (NULL != temp_disp_index){
                free(temp_disp_index);
                temp_disp_index = NULL;
            }

#if DEBUG_ON
            printf("************Cycle: %d,  Aggregator: %d ***************\n",
                   index+1,fh->f_rank);
            for (i=0; i<entries_per_aggregator;i++){
                printf("%d: OFFSET: %lld   LENGTH: %ld, Mem-offset: %ld, disp : %d\n",
                       file_offsets_for_agg[sorted_file_offsets[i]].process_id,
                       file_offsets_for_agg[sorted_file_offsets[i]].offset,
                       file_offsets_for_agg[sorted_file_offsets[i]].length,
                       memory_displacements[sorted_file_offsets[i]],
                       disp_index[ file_offsets_for_agg[sorted_file_offsets[i]].process_id]);
            }
#endif

#if DEBUG_ON
            printf("%d: global_count : %ld, bytes_to_write_in_cycle : %ld, procs_per_group: %d\n",
                   fh->f_rank,
                   global_count,
                   bytes_to_write_in_cycle,
                   fh->f_procs_per_group);
#endif
#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
            start_comm_time = MPI_Wtime();
#endif
            global_buf  = (char *) malloc (global_count);
            if (NULL == global_buf){
                opal_output(1, "OUT OF MEMORY");
                ret = OMPI_ERR_OUT_OF_RESOURCE;
                goto exit;
            }

            for (i=0;i<fh->f_procs_per_group; i++){
                ompi_datatype_create_hindexed(disp_index[i],
                                              blocklen_per_process[i],
                                              displs_per_process[i],
                                              MPI_BYTE,
                                              &recvtype[i]);
                ompi_datatype_commit(&recvtype[i]);
                ret = MCA_PML_CALL(irecv(global_buf,
                                         1,
                                         recvtype[i],
                                         fh->f_procs_in_group[i],
                                         123,
                                         fh->f_comm,
                                         &recv_req[i]));
                if (OMPI_SUCCESS != ret){
                    fprintf(stderr,"irecv Error!\n");
                    goto exit;
                }
            }
        }

        if ( sendbuf_is_contiguous ) {
            send_buf = &((char*)buf)[total_bytes_written];
        }
        else if (bytes_to_write_in_cycle) {
            /* allocate a send buffer and copy the data that needs
               to be sent into it in case the data is non-contigous
               in memory */
            ptrdiff_t mem_address;
            size_t remaining = 0;
            size_t temp_position = 0;

            send_buf = malloc (bytes_to_write_in_cycle);
            if (NULL == send_buf) {
                opal_output (1, "OUT OF MEMORY\n");
                ret = OMPI_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
            remaining = bytes_to_write_in_cycle;

            while (remaining) {
                mem_address = (ptrdiff_t)
                    (decoded_iov[iov_index].iov_base) + current_position;

                if (remaining >=
                    (decoded_iov[iov_index].iov_len - current_position)) {
                    memcpy (send_buf+temp_position,
                            (IOVBASE_TYPE *)mem_address,
                            decoded_iov[iov_index].iov_len - current_position);
                    remaining = remaining -
                        (decoded_iov[iov_index].iov_len - current_position);
                    temp_position = temp_position +
                        (decoded_iov[iov_index].iov_len - current_position);
                    iov_index = iov_index + 1;
                    current_position = 0;
                }
                else {
                    memcpy (send_buf+temp_position,
                            (IOVBASE_TYPE *)mem_address,
                            remaining);
                    current_position = current_position + remaining;
                    remaining = 0;
                }
            }
        }
        total_bytes_written += bytes_to_write_in_cycle;

        ret = MCA_PML_CALL(isend(send_buf,
                                 bytes_to_write_in_cycle,
                                 MPI_BYTE,
                                 my_aggregator,
                                 123,
                                 MCA_PML_BASE_SEND_STANDARD,
                                 fh->f_comm,
                                 &send_req));
        
        if ( OMPI_SUCCESS != ret ){
            fprintf(stderr,"isend error!\n");
            goto exit;
        }

        ret = ompi_request_wait (&send_req, MPI_STATUS_IGNORE);
        if (OMPI_SUCCESS != ret){
            goto exit;
        }
        if ( !sendbuf_is_contiguous ) {
            if ( NULL != send_buf ) {
                free ( send_buf );
                send_buf = NULL;
            }
        }

        if (my_aggregator == fh->f_rank) {
            ret = ompi_request_wait_all (fh->f_procs_per_group,
                                         recv_req,
                                         MPI_STATUS_IGNORE);
            if (OMPI_SUCCESS != ret){
                goto exit;
            }

#if DEBUG_ON
            printf("************Cycle: %d,  Aggregator: %d ***************\n",
                   index+1,fh->f_rank);
            if (my_aggregator == fh->f_rank){
                for (i=0 ; i<global_count/4 ; i++)
                    printf (" RECV %d \n",((int *)global_buf)[i]);
            }
#endif
        }
#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
        end_comm_time = MPI_Wtime();
        comm_time += end_comm_time - start_comm_time;
#endif



        if (my_aggregator == fh->f_rank) {
            fh->f_io_array = (mca_io_ompio_io_array_t *) malloc
                (entries_per_aggregator * sizeof (mca_io_ompio_io_array_t));
            if (NULL == fh->f_io_array) {
                opal_output(1, "OUT OF MEMORY\n");
                ret = OMPI_ERR_OUT_OF_RESOURCE;
                goto exit;
            }
            fh->f_num_of_io_entries = 0;
            /*First entry for every aggregator*/
            fh->f_io_array[fh->f_num_of_io_entries].offset =
                (IOVBASE_TYPE *)(intptr_t)file_offsets_for_agg[sorted_file_offsets[0]].offset;
            fh->f_io_array[fh->f_num_of_io_entries].length =
                file_offsets_for_agg[sorted_file_offsets[0]].length;
            fh->f_io_array[fh->f_num_of_io_entries].memory_address =
                global_buf+memory_displacements[sorted_file_offsets[0]];
            fh->f_num_of_io_entries++;
            for (i=1;i<entries_per_aggregator;i++){
                if (file_offsets_for_agg[sorted_file_offsets[i-1]].offset +
                    file_offsets_for_agg[sorted_file_offsets[i-1]].length ==
                    file_offsets_for_agg[sorted_file_offsets[i]].offset){
                    fh->f_io_array[fh->f_num_of_io_entries - 1].length +=
                        file_offsets_for_agg[sorted_file_offsets[i]].length;
                }
                else {
                    fh->f_io_array[fh->f_num_of_io_entries].offset =
                        (IOVBASE_TYPE *)(intptr_t)file_offsets_for_agg[sorted_file_offsets[i]].offset;
                    fh->f_io_array[fh->f_num_of_io_entries].length =
                        file_offsets_for_agg[sorted_file_offsets[i]].length;
                    fh->f_io_array[fh->f_num_of_io_entries].memory_address =
                        global_buf+memory_displacements[sorted_file_offsets[i]];
                    fh->f_num_of_io_entries++;
                }
            }
#if DEBUG_ON
            printf("*************************** %d\n", fh->f_num_of_io_entries);
            for (i=0 ; i<fh->f_num_of_io_entries ; i++) {
                printf(" ADDRESS: %p  OFFSET: %ld   LENGTH: %ld\n",
                       fh->f_io_array[i].memory_address,
                       (ptrdiff_t)fh->f_io_array[i].offset,
                       fh->f_io_array[i].length);
            }
#endif

#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
            start_write_time = MPI_Wtime();
#endif

            if (fh->f_num_of_io_entries) {
                if ( 0 >  fh->f_fbtl->fbtl_pwritev (fh)) {
                    opal_output (1, "WRITE FAILED\n");
                    ret = OMPI_ERROR;
                    goto exit;
                }
            }

#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
            end_write_time = MPI_Wtime();
            write_time += end_write_time - start_write_time;
#endif

        }
    }

#if OMPIO_FCOLL_WANT_TIME_BREAKDOWN
    end_exch = MPI_Wtime();
    exch_write += end_exch - start_exch;
    nentry.time[0] = write_time;
    nentry.time[1] = comm_time;
    nentry.time[2] = exch_write;
    if (my_aggregator == fh->f_rank)
        nentry.aggregator = 1;
    else
        nentry.aggregator = 0;
    nentry.nprocs_for_coll = static_num_io_procs;
    if (!mca_common_ompio_full_print_queue(fh->f_coll_write_time)){
	mca_common_ompio_register_print_entry(fh->f_coll_write_time,
                                              nentry);
    }
#endif



exit:
    if (NULL != decoded_iov){
        free(decoded_iov);
        decoded_iov = NULL;
    }

    if (NULL != local_iov_array){
        free(local_iov_array);
        local_iov_array = NULL;
    }

    if (my_aggregator == fh->f_rank) {
        for(l=0;l<fh->f_procs_per_group;l++){
            if (NULL != blocklen_per_process[l]){
                free(blocklen_per_process[l]);
                blocklen_per_process[l] = NULL;
            }
            if (NULL != displs_per_process[l]){
                free(displs_per_process[l]);
                displs_per_process[l] = NULL;
            }
        }
    }

    if ( NULL != recv_req ) {
        free ( recv_req );
        recv_req = NULL;
    }
    if ( !sendbuf_is_contiguous ) {
        if (NULL != send_buf){
            free(send_buf);
            send_buf = NULL;
        }
    }

    if (NULL != global_buf){
        free(global_buf);
        global_buf = NULL;
    }

    if (NULL != recvtype){
        free(recvtype);
        recvtype = NULL;
    }

    if (NULL != sorted_file_offsets){
        free(sorted_file_offsets);
        sorted_file_offsets = NULL;
    }

    if (NULL != file_offsets_for_agg){
        free(file_offsets_for_agg);
        file_offsets_for_agg = NULL;
    }

    if (NULL != memory_displacements){
        free(memory_displacements);
        memory_displacements = NULL;
    }

    if (NULL != displs_per_process){
        free(displs_per_process);
        displs_per_process = NULL;
    }

    if (NULL != blocklen_per_process){
        free(blocklen_per_process);
        blocklen_per_process = NULL;
    }

    if(NULL != current_index){
        free(current_index);
        current_index = NULL;
    }

    if(NULL != bytes_remaining){
        free(bytes_remaining);
        bytes_remaining = NULL;
    }

    if (NULL != disp_index){
        free(disp_index);
        disp_index = NULL;
    }

    if (NULL != sorted) {
        free(sorted);
        sorted = NULL;
    }

    if (NULL != displs) {
        free(displs);
        displs = NULL;
    }

    return ret;
}



static int local_heap_sort (mca_fcoll_static_local_io_array *io_array,
			    int num_entries,
			    int *sorted)
{
    int i = 0;
    int j = 0;
    int left = 0;
    int right = 0;
    int largest = 0;
    int heap_size = num_entries - 1;
    int temp = 0;
    unsigned char done = 0;
    int* temp_arr = NULL;

    if( 0 == num_entries){
        num_entries = 1;
    }


    temp_arr = (int*)malloc(num_entries*sizeof(int));
    if (NULL == temp_arr) {
        opal_output (1, "OUT OF MEMORY\n");
        return OMPI_ERR_OUT_OF_RESOURCE;
    }
    temp_arr[0] = 0;
    for (i = 1; i < num_entries; ++i) {
        temp_arr[i] = i;
    }
    /* num_entries can be a large no. so NO RECURSION */
    for (i = num_entries/2-1 ; i>=0 ; i--) {
        done = 0;
        j = i;
        largest = j;

        while (!done) {
            left = j*2+1;
            right = j*2+2;
            if ((left <= heap_size) &&
                (io_array[temp_arr[left]].offset > io_array[temp_arr[j]].offset)) {
                largest = left;
            }
            else {
                largest = j;
            }
            if ((right <= heap_size) &&
                (io_array[temp_arr[right]].offset >
                 io_array[temp_arr[largest]].offset)) {
                largest = right;
            }
            if (largest != j) {
                temp = temp_arr[largest];
                temp_arr[largest] = temp_arr[j];
                temp_arr[j] = temp;
                j = largest;
            }
            else {
                done = 1;
            }
        }
    }

    for (i = num_entries-1; i >=1; --i) {
        temp = temp_arr[0];
        temp_arr[0] = temp_arr[i];
        temp_arr[i] = temp;
        heap_size--;
        done = 0;
        j = 0;
        largest = j;

        while (!done) {
            left =  j*2+1;
            right = j*2+2;

            if ((left <= heap_size) &&
                (io_array[temp_arr[left]].offset >
                 io_array[temp_arr[j]].offset)) {
                largest = left;
            }
            else {
                largest = j;
            }
            if ((right <= heap_size) &&
                (io_array[temp_arr[right]].offset >
                 io_array[temp_arr[largest]].offset)) {
                largest = right;
            }
            if (largest != j) {
                temp = temp_arr[largest];
                temp_arr[largest] = temp_arr[j];
                temp_arr[j] = temp;
                j = largest;
            }
            else {
                done = 1;
            }
        }
        sorted[i] = temp_arr[i];
    }
    sorted[0] = temp_arr[0];

    if (NULL != temp_arr) {
        free(temp_arr);
        temp_arr = NULL;
    }
    return OMPI_SUCCESS;
}

int find_next_index( int proc_index,
		     int c_index,
		     mca_io_ompio_file_t *fh,
		     mca_fcoll_static_local_io_array *global_iov_array,
		     int global_iov_count,
		     int *sorted){
    int i;

    for(i=c_index+1; i<global_iov_count;i++){
        if (get_process_id(global_iov_array[sorted[i]].process_id,
                           fh) == proc_index)
            return i;
    }
    return -1;
}


int get_process_id (int rank,
		    mca_io_ompio_file_t *fh){
    int i;
    for (i=0; i<fh->f_procs_per_group; i++){
        if (fh->f_procs_in_group[i] == rank){
            return i;
        }
    }
    return -1;
}
