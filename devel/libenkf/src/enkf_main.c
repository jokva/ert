/*
   Copyright (C) 2011  Statoil ASA, Norway. 
   The file 'enkf_main.c' is part of ERT - Ensemble based Reservoir Tool. 
    
   ERT is free software: you can redistribute it and/or modify 
   it under the terms of the GNU General Public License as published by 
   the Free Software Foundation, either version 3 of the License, or 
   (at your option) any later version. 
   
   ERT is distributed in the hope that it will be useful, but WITHOUT ANY 
   WARRANTY; without even the implied warranty of MERCHANTABILITY or 
   FITNESS FOR A PARTICULAR PURPOSE.   
    
   See the GNU General Public License at <http://www.gnu.org/licenses/gpl.html> 
   for more details. 
*/


#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <stdbool.h>
#include <util.h>
#include <hash.h>
#include <enkf_config_node.h>
#include <ecl_util.h>
#include <path_fmt.h>
#include <enkf_types.h>
#include <thread_pool.h>
#include <obs_data.h>
#include <history.h>
#include <meas_data.h>
#include <enkf_state.h>
#include <enkf_obs.h>
#include <sched_file.h>
#include <enkf_fs.h>
#include <arg_pack.h>
#include <history.h>
#include <node_ctype.h>
#include <pthread.h>
#include <job_queue.h>
#include <msg.h>
#include <stringlist.h>
#include <enkf_main.h>
#include <enkf_serialize.h>
#include <config.h>
#include <local_driver.h>
#include <rsh_driver.h>
#include <lsf_driver.h>
#include <history.h>
#include <enkf_sched.h>
#include <set.h>
#include <ecl_io_config.h>
#include <ecl_config.h>
#include <ensemble_config.h>
#include <model_config.h>
#include <site_config.h>
#include <active_config.h>
#include <forward_model.h>
#include <enkf_analysis.h>
#include <local_ministep.h>
#include <local_updatestep.h>
#include <local_config.h>
#include <local_dataset.h>
#include <misfit_table.h>
#include <log.h>
#include <plot_config.h>
#include <ert_template.h>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>
#include <job_queue.h>
#include <queue_driver.h>
#include <subst_list.h>
#include <subst_func.h>
#include <int_vector.h>
#include <ert_build_info.h>
#include <bool_vector.h>
#include <rng.h>
#include <rng_config.h>
#include <analysis_module.h>
#include <analysis_table.h>
#include "enkf_defaults.h"
#include "config_keys.h"

/**
   This object should contain **everything** needed to run a enkf
   simulation. A way to wrap up all available information/state and
   pass it around. An attempt has been made to collect various pieces
   of related information together in a couple of objects
   (model_config, ecl_config, site_config and ensemble_config). When
   it comes to these holding objects the following should be observed:

    1. It not always obvious where a piece of information should be
       stored, i.e. the grid is a property of the model, however it is
       an eclipse grid, and hence also belongs to eclipse
       configuration?? [In this case ecl_config wins out.]

    2. The information stored in these objects is typically passed on
       to the enkf_state object, where it is used.

    3. At enkf_state level it is not really consequent - in some cases
       the enkf_state object takes a scalar copy (i.e. keep_runpath),
       and in other cases only a pointer down to the underlying
       enkf_main object is taken. In the former case it is no way to
       change global behaviour by modifying the enkf_main objects.

       In the enkf_state object the fields of the member_config,
       ecl_config, site_config and ensemble_config objects are mixed
       and matched into other small holding objects defined in
       enkf_state.c.

*/

#define ENKF_MAIN_ID              8301
#define ENKF_MOUNT_MAP           "enkf_mount_info"

struct enkf_main_struct {
  UTIL_TYPE_ID_DECLARATION;
  enkf_fs_type         * dbase;              /* The internalized information. */
  ensemble_config_type * ensemble_config;    /* The config objects for the various enkf nodes.*/
  model_config_type    * model_config;
  ecl_config_type      * ecl_config;
  site_config_type     * site_config;
  analysis_config_type * analysis_config;
  local_config_type    * local_config;       /* Holding all the information about local analysis. */
  ert_templates_type   * templates;          /* Run time templates */
  log_type             * logh;               /* Handle to an open log file. */
  plot_config_type     * plot_config;        /* Information about plotting. */
  rng_config_type      * rng_config;
  rng_type             * rng;

  /*---------------------------*/            /* Variables related to substitution. */
  subst_func_pool_type * subst_func_pool;
  subst_list_type      * subst_list;         /* A parent subst_list instance - common to all ensemble members. */
  /*-------------------------*/
  
  int_vector_type      * keep_runpath;       /* HACK: This is only used in the initialization period - afterwards the data is held by the enkf_state object. */
  bool                   pre_clear_runpath;  /* HACK: This is only used in the initialization period - afterwards the data is held by the enkf_state object. */

  char                 * site_config_file;
  char                 * user_config_file;   
  char                 * rft_config_file;       /* File giving the configuration to the RFTwells*/  
  enkf_obs_type        * obs;
  misfit_table_type    * misfit_table;     /* An internalization of misfit results - used for ranking according to various criteria. */
  enkf_state_type     ** ensemble;         /* The ensemble ... */
  int                    ens_size;         /* The size of the ensemble */  
};




/*****************************************************************/

void enkf_main_init_internalization( enkf_main_type *  , run_mode_type  );




/*****************************************************************/

UTIL_SAFE_CAST_FUNCTION(enkf_main , ENKF_MAIN_ID)

analysis_config_type * enkf_main_get_analysis_config(const enkf_main_type * enkf_main) {
  return enkf_main->analysis_config;
}

bool enkf_main_get_pre_clear_runpath( const enkf_main_type * enkf_main ) {
  return enkf_state_get_pre_clear_runpath( enkf_main->ensemble[0] );
}

void enkf_main_set_pre_clear_runpath( enkf_main_type * enkf_main , bool pre_clear_runpath) {
  const int ens_size = enkf_main_get_ensemble_size( enkf_main );
  int iens;
  for (iens = 0; iens < ens_size; iens++)
    enkf_state_set_pre_clear_runpath( enkf_main->ensemble[iens] , pre_clear_runpath );
}


void enkf_main_set_eclbase( enkf_main_type * enkf_main , const char * eclbase_fmt) {
  ecl_config_set_eclbase( enkf_main->ecl_config , eclbase_fmt);
  for (int iens = 0; iens < enkf_main->ens_size; iens++) 
    enkf_state_update_eclbase( enkf_main->ensemble[iens] );
}

void enkf_main_set_refcase( enkf_main_type * enkf_main , const char * refcase_path) {
  ecl_config_load_refcase( enkf_main->ecl_config , refcase_path );
  model_config_set_refcase( enkf_main->model_config , ecl_config_get_refcase( enkf_main->ecl_config ));
  ensemble_config_set_refcase( enkf_main->ensemble_config , ecl_config_get_refcase( enkf_main->ecl_config ));
}


void enkf_main_set_user_config_file( enkf_main_type * enkf_main , const char * user_config_file ) {
  enkf_main->user_config_file = util_realloc_string_copy( enkf_main->user_config_file , user_config_file );
}

void enkf_main_set_rft_config_file( enkf_main_type * enkf_main , const char * rft_config_file ) {
  enkf_main->rft_config_file = util_realloc_string_copy( enkf_main->rft_config_file , rft_config_file );
} 

void enkf_main_set_site_config_file( enkf_main_type * enkf_main , const char * site_config_file ) {
  enkf_main->site_config_file = util_realloc_string_copy( enkf_main->site_config_file , site_config_file );
}

const char * enkf_main_get_user_config_file( const enkf_main_type * enkf_main ) {
  return enkf_main->user_config_file;
}

const char * enkf_main_get_site_config_file( const enkf_main_type * enkf_main ) {
  return enkf_main->site_config_file;
}

const char * enkf_main_get_rft_config_file( const enkf_main_type * enkf_main ) {
  return enkf_main->rft_config_file;
}

ensemble_config_type * enkf_main_get_ensemble_config(const enkf_main_type * enkf_main) {
  return enkf_main->ensemble_config;
}

site_config_type * enkf_main_get_site_config( const enkf_main_type * enkf_main ) {
  return enkf_main->site_config;
}


subst_list_type * enkf_main_get_data_kw( const enkf_main_type * enkf_main ) {
  return enkf_main->subst_list;
}


local_config_type * enkf_main_get_local_config( const enkf_main_type * enkf_main ) {
  return enkf_main->local_config;
}

model_config_type * enkf_main_get_model_config( const enkf_main_type * enkf_main ) {
  return enkf_main->model_config;
}

log_type * enkf_main_get_logh( const enkf_main_type * enkf_main ) {
  return enkf_main->logh;
}

plot_config_type * enkf_main_get_plot_config( const enkf_main_type * enkf_main ) {
  return enkf_main->plot_config;
}

ecl_config_type *enkf_main_get_ecl_config(const enkf_main_type * enkf_main) {
        return enkf_main->ecl_config;
}

int enkf_main_get_history_length( const enkf_main_type * enkf_main) {
  return model_config_get_last_history_restart( enkf_main->model_config);
}

bool enkf_main_has_prediction( const enkf_main_type * enkf_main ) {
  return model_config_has_prediction( enkf_main->model_config );
}

enkf_fs_type * enkf_main_get_fs(const enkf_main_type * enkf_main) {
  return enkf_main->dbase;
}


enkf_obs_type * enkf_main_get_obs(const enkf_main_type * enkf_main) {
  return enkf_main->obs;
}


/**
   Will do a forced reload of the observtaions; if the user has edited
   the content of the observation file while the ERT instance is
   running.
*/

void enkf_main_reload_obs( enkf_main_type * enkf_main) {
  enkf_obs_reload(enkf_main->obs , ecl_config_get_sched_file( enkf_main->ecl_config ) , enkf_main->ensemble_config );
}


/**
   Will not reload the observations if the input config file
   @obs_config_file is equal to the currently set config_file. If you
   want to force a reload of the observations use the function
   enkf_main_reload_obs().
*/

void enkf_main_load_obs( enkf_main_type * enkf_main , const char * obs_config_file ) {
  if (!util_string_equal( obs_config_file , enkf_obs_get_config_file( enkf_main->obs )))
    enkf_obs_load(enkf_main->obs , obs_config_file , ecl_config_get_sched_file( enkf_main->ecl_config ), enkf_main->ensemble_config );
}


/**
   This function should be called when a new data_file has been set.
*/

static void enkf_main_update_num_cpu( enkf_main_type * enkf_main ) {
  /**
     This is how the number of CPU's are passed on to the forward models:
  */
  {
    char * num_cpu_key     = enkf_util_alloc_tagged_string( "NUM_CPU" );
    char * num_cpu_string  = util_alloc_sprintf( "%d" , ecl_config_get_num_cpu( enkf_main->ecl_config ));
    
    subst_list_append_owned_ref( enkf_main->subst_list , num_cpu_key , num_cpu_string , NULL );
    free( num_cpu_key );
  }
}


void enkf_main_set_data_file( enkf_main_type * enkf_main , const char * data_file ) {
  ecl_config_set_data_file( enkf_main->ecl_config , data_file );
  enkf_main_update_num_cpu( enkf_main );
}



misfit_table_type * enkf_main_get_misfit(const enkf_main_type * enkf_main) {
  return enkf_main->misfit_table;
}



static void enkf_main_free_ensemble( enkf_main_type * enkf_main ) {
  if (enkf_main->ensemble != NULL) {
    const int ens_size = enkf_main->ens_size;
    int i;
    for (i=0; i < ens_size; i++)
      enkf_state_free( enkf_main->ensemble[i] );
    free(enkf_main->ensemble);
    enkf_main->ensemble = NULL;
  }
}


void enkf_main_free(enkf_main_type * enkf_main) {
  rng_free( enkf_main->rng );
  rng_config_free( enkf_main->rng_config );
  enkf_obs_free(enkf_main->obs);
  enkf_main_free_ensemble( enkf_main );
  if (enkf_main->dbase != NULL) enkf_fs_free( enkf_main->dbase );

  log_add_message( enkf_main->logh , false , NULL , "Exiting ert application normally - all is fine(?)" , false);
  log_close( enkf_main->logh );
  analysis_config_free(enkf_main->analysis_config);
  ecl_config_free(enkf_main->ecl_config);
  model_config_free( enkf_main->model_config);
  site_config_free( enkf_main->site_config);
  ensemble_config_free( enkf_main->ensemble_config );
  local_config_free( enkf_main->local_config );
  if (enkf_main->misfit_table != NULL)
    misfit_table_free( enkf_main->misfit_table );
  int_vector_free( enkf_main->keep_runpath );
  plot_config_free( enkf_main->plot_config );
  ert_templates_free( enkf_main->templates );
  
  subst_func_pool_free( enkf_main->subst_func_pool );
  subst_list_free( enkf_main->subst_list );
  util_safe_free( enkf_main->user_config_file );
  util_safe_free( enkf_main->site_config_file );
  util_safe_free( enkf_main->rft_config_file );
  free(enkf_main);
}



/*****************************************************************/



static void enkf_main_load_sub_ensemble(enkf_main_type * enkf_main , int mask , int report_step , state_enum state, int iens1 , int iens2) {
  int iens;
  for (iens = iens1; iens < iens2; iens++)
    enkf_state_fread(enkf_main->ensemble[iens] , mask , report_step , state );
}


static void * enkf_main_load_sub_ensemble__(void * __arg) {
  arg_pack_type * arg_pack   = arg_pack_safe_cast(__arg);
  enkf_main_type * enkf_main = arg_pack_iget_ptr(arg_pack , 0);
  int mask                   = arg_pack_iget_int(arg_pack , 1);
  int report_step            = arg_pack_iget_int(arg_pack , 2);
  state_enum state           = arg_pack_iget_int(arg_pack , 3);
  int iens1                  = arg_pack_iget_int(arg_pack , 4);
  int iens2                  = arg_pack_iget_int(arg_pack , 5);

  enkf_main_load_sub_ensemble(enkf_main , mask , report_step , state , iens1 , iens2);
  return NULL;
}



void enkf_main_load_ensemble(enkf_main_type * enkf_main , int mask , int report_step , state_enum state) {
  const   int cpu_threads = 4;
  int     sub_ens_size    = enkf_main_get_ensemble_size(enkf_main) / cpu_threads;
  int     icpu;
  thread_pool_type * tp          = thread_pool_alloc( cpu_threads , true );
  arg_pack_type ** arg_pack_list = util_malloc( cpu_threads * sizeof * arg_pack_list , __func__);

  for (icpu = 0; icpu < cpu_threads; icpu++) {
    arg_pack_type * arg = arg_pack_alloc();
    arg_pack_append_ptr(arg , enkf_main);
    arg_pack_append_int(arg , mask);
    arg_pack_append_int(arg , report_step);
    arg_pack_append_int(arg , state);

    {
      int iens1 =  icpu * sub_ens_size;
      int iens2 = iens1 + sub_ens_size;

      if (icpu == (cpu_threads - 1))
        iens2 = enkf_main_get_ensemble_size(enkf_main);

      arg_pack_append_int(arg ,  iens1);
      arg_pack_append_int(arg ,  iens2);
    }
    arg_pack_list[icpu] = arg;
    arg_pack_lock( arg );
    thread_pool_add_job( tp , enkf_main_load_sub_ensemble__ , arg);
  }
  thread_pool_join( tp );
  thread_pool_free( tp );

  for (icpu = 0; icpu < cpu_threads; icpu++)
    arg_pack_free( arg_pack_list[icpu] );
  free(arg_pack_list);
}





static void enkf_main_fwrite_sub_ensemble(enkf_main_type * enkf_main , int mask , int report_step , state_enum state, int iens1 , int iens2) {
  int iens;
  for (iens = iens1; iens < iens2; iens++)
    enkf_state_fwrite(enkf_main->ensemble[iens] , mask , report_step , state);
}


static void * enkf_main_fwrite_sub_ensemble__(void *__arg) {
  arg_pack_type * arg_pack   = arg_pack_safe_cast(__arg);
  enkf_main_type * enkf_main = arg_pack_iget_ptr(arg_pack , 0);
  int mask                   = arg_pack_iget_int(arg_pack , 1);
  int report_step            = arg_pack_iget_int(arg_pack , 2);
  state_enum state           = arg_pack_iget_int(arg_pack , 3);
  int iens1                  = arg_pack_iget_int(arg_pack , 4);
  int iens2                  = arg_pack_iget_int(arg_pack , 5);

  enkf_main_fwrite_sub_ensemble(enkf_main , mask , report_step , state , iens1 , iens2);
  return NULL;
}


void enkf_main_fwrite_ensemble(enkf_main_type * enkf_main , int mask , int report_step , state_enum state) {
  const   int cpu_threads = 4;
  int     sub_ens_size    = enkf_main_get_ensemble_size(enkf_main) / cpu_threads;
  int     icpu;
  thread_pool_type * tp = thread_pool_alloc( cpu_threads , true );
  arg_pack_type ** arg_pack_list = util_malloc( cpu_threads * sizeof * arg_pack_list , __func__);

  for (icpu = 0; icpu < cpu_threads; icpu++) {
    arg_pack_type * arg = arg_pack_alloc();
    arg_pack_append_ptr(arg , enkf_main);
    arg_pack_append_int(arg , mask);
    arg_pack_append_int(arg , report_step);
    arg_pack_append_int(arg , state);

    {
      int iens1 =  icpu * sub_ens_size;
      int iens2 = iens1 + sub_ens_size;

      if (icpu == (cpu_threads - 1))
        iens2 = enkf_main_get_ensemble_size(enkf_main);

      arg_pack_append_int(arg , iens1);
      arg_pack_append_int(arg , iens2);
    }
    arg_pack_list[icpu] = arg;
    arg_pack_lock( arg );
    thread_pool_add_job( tp , enkf_main_fwrite_sub_ensemble__ , arg);
  }
  thread_pool_join( tp );
  thread_pool_free( tp );

  for (icpu = 0; icpu < cpu_threads; icpu++)
    arg_pack_free( arg_pack_list[icpu]);
  free(arg_pack_list);
}




/**
   This function returns a (enkf_node_type ** ) pointer, which points
   to all the instances with the same keyword, i.e.

   enkf_main_get_node_ensemble(enkf_main , "PRESSURE");

   Will return an ensemble of pressure nodes. Observe that apart from
   the list of pointers, *now new storage* is allocated, all the
   pointers point in to the underlying enkf_node instances under the
   enkf_main / enkf_state objects. Consequently there is no designated
   free() function to match this, just free() the result.

   Example:

   enkf_node_type ** pressure_nodes = enkf_main_get_node_ensemble(enkf_main , "PRESSURE");

   Do something with the pressure nodes ...

   free(pressure_nodes);

*/

enkf_node_type ** enkf_main_get_node_ensemble(const enkf_main_type * enkf_main , const char * key , int report_step , state_enum load_state) {
  enkf_fs_type * fs               = enkf_main_get_fs( enkf_main );
  const int ens_size              = enkf_main_get_ensemble_size( enkf_main );
  enkf_node_type ** node_ensemble = util_malloc(ens_size * sizeof * node_ensemble , __func__ );
  int iens;

  for (iens = 0; iens < ens_size; iens++) {
    node_ensemble[iens] = enkf_state_get_node(enkf_main->ensemble[iens] , key);
    enkf_fs_fread_node( fs , node_ensemble[iens] , report_step , iens ,load_state);
  }
  return node_ensemble;
}

/*****************************************************************/




enkf_state_type * enkf_main_iget_state(const enkf_main_type * enkf_main , int iens) {
  return enkf_main->ensemble[iens];
}


member_config_type * enkf_main_iget_member_config(const enkf_main_type * enkf_main , int iens) {
  return enkf_state_get_member_config( enkf_main->ensemble[iens] );
}



void enkf_main_node_mean( const enkf_node_type ** ensemble , int ens_size , enkf_node_type * mean ) {
  int iens;
  enkf_node_clear( mean );
  for (iens = 0; iens < ens_size; iens++)
    enkf_node_iadd( mean , ensemble[iens] );

  enkf_node_scale( mean , 1.0 / ens_size );
}


/**
   This function calculates the node standard deviation from the
   ensemble. The mean can be NULL, in which case it is assumed that
   the mean has already been shifted away from the ensemble.
*/


void enkf_main_node_std( const enkf_node_type ** ensemble , int ens_size , const enkf_node_type * mean , enkf_node_type * std) {
  int iens;
  enkf_node_clear( std );
  for (iens = 0; iens < ens_size; iens++)
    enkf_node_iaddsqr( std , ensemble[iens] );
  enkf_node_scale(std , 1.0 / ens_size );

  if (mean != NULL) {
    enkf_node_scale( std , -1 );
    enkf_node_iaddsqr( std , mean );
    enkf_node_scale( std , -1 );
  }

  enkf_node_sqrt( std );
}


void enkf_main_inflate_node(enkf_main_type * enkf_main , int report_step , const char * key , const enkf_node_type * min_std) {
  int ens_size                              = enkf_main_get_ensemble_size(enkf_main);
  enkf_node_type ** ensemble                = enkf_main_get_node_ensemble( enkf_main , key , report_step , ANALYZED );
  enkf_node_type * mean                     = enkf_node_copyc( ensemble[0] );
  enkf_node_type * std                      = enkf_node_copyc( ensemble[0] );
  int iens;
  
  /* Shifting away the mean */
  enkf_main_node_mean( (const enkf_node_type **) ensemble , ens_size , mean );
   enkf_node_scale( mean , -1 );
  for (iens = 0; iens < ens_size; iens++)
    enkf_node_iadd( ensemble[iens] , mean );
  enkf_node_scale( mean , -1 );

  /*****************************************************************/
  /*
    Now we have the ensemble represented as a mean and an ensemble of
    deviations from the mean. This is the form suitable for actually
    doing the inflation.
  */
  {
    enkf_node_type * inflation = enkf_node_copyc( ensemble[0] );
    enkf_node_set_inflation( inflation , std , min_std  );

    for (iens = 0; iens < ens_size; iens++)
      enkf_node_imul( ensemble[iens] , inflation );

    enkf_node_free( inflation );
  }

  /* Add the mean back in - and store the updated node to disk.*/
  for (iens = 0; iens < ens_size; iens++) {
    enkf_node_iadd( ensemble[iens] , mean );
    enkf_fs_fwrite_node( enkf_main_get_fs( enkf_main ) , ensemble[iens] , report_step , iens , ANALYZED );
  }

  enkf_node_free( mean );
  enkf_node_free( std );
  free( ensemble );
}




void enkf_main_inflate(enkf_main_type * enkf_main , int report_step , hash_type * use_count) {
  stringlist_type * keys = ensemble_config_alloc_keylist_from_var_type( enkf_main->ensemble_config , PARAMETER + DYNAMIC_STATE);
  msg_type * msg = msg_alloc("Inflating:" , false);

  msg_show( msg );
  for (int ikey = 0; ikey < stringlist_get_size( keys ); ikey++) {
    const char * key = stringlist_iget( keys  , ikey );
    if (hash_get_counter(use_count , key) > 0) {
      const enkf_config_node_type * config_node = ensemble_config_get_node( enkf_main->ensemble_config , key );
      const enkf_node_type * min_std            = enkf_config_node_get_min_std( config_node );
      
      if (min_std != NULL) {
        msg_update( msg , key );
        enkf_main_inflate_node(enkf_main , report_step , key , min_std );
      }
    }
  }
  stringlist_free( keys );
  msg_free( msg , true );
}




static int __get_active_size(enkf_main_type * enkf_main , const char * key, int report_step , const active_list_type * active_list) {
  const enkf_config_node_type * config_node = ensemble_config_get_node( enkf_main->ensemble_config , key );
  /**
     This is very awkward; the problem is that for the GEN_DATA
     type the config object does not really own the size. Instead
     the size is pushed (on load time) from gen_data instances to
     the gen_data_config instance. Therefor we have to assert
     that at least one gen_data instance has been loaded (and
     consequently updated the gen_data_config instance) before we
     query for the size.
  */
  {
    if (enkf_config_node_get_impl_type( config_node ) == GEN_DATA) {
      enkf_node_type * node = enkf_state_get_node( enkf_main->ensemble[0] , key);
      enkf_fs_fread_node( enkf_main->dbase , node , report_step , 0 , FORECAST);
    }
  }

  {
    active_mode_type active_mode = active_list_get_mode( active_list );
    int active_size;
    if (active_mode == INACTIVE)
      active_size = 0;
    else if (active_mode == ALL_ACTIVE)
      active_size = enkf_config_node_get_data_size( config_node , report_step );
    else if (active_mode == PARTLY_ACTIVE)
      active_size = active_list_get_active_size( active_list , -1 );
    else {
      util_abort("%s: internal error .. \n",__func__);
      active_size = -1; /* Compiler shut up */
    }
    return active_size;
  }
}


/*****************************************************************/
/**
   Helper struct used to pass information to the multithreaded 
   serialize / deserialize functions.
*/

typedef struct {
  enkf_fs_type            * fs; 
  enkf_state_type        ** ensemble;
  int                       iens1;    /* Inclusive lower limit. */
  int                       iens2;    /* NOT inclusive upper limit. */
  const char              * key;
  int                       report_step;
  state_enum                load_state;
  int                       row_offset;
  const active_list_type  * active_list;
  matrix_type             * A;
} serialize_info_type;



static void serialize_node( enkf_fs_type * fs , 
                            enkf_state_type ** ensemble , 
                            int iens , 
                            const char * key , 
                            int report_step , 
                            state_enum load_state , 
                            int row_offset , 
                            const active_list_type * active_list,
                            matrix_type * A) {

  enkf_node_type * node = enkf_state_get_node( ensemble[iens] , key);
  enkf_fs_fread_node( fs , node , report_step , iens , load_state);
  enkf_node_serialize( node , active_list , A , row_offset , iens);
}


static void * serialize_nodes_mt( void * arg ) {
  serialize_info_type * info = (serialize_info_type *) arg;
  int iens;
  for (iens = info->iens1; iens < info->iens2; iens++) 
    serialize_node( info->fs , info->ensemble , iens , info->key , info->report_step , info->load_state , info->row_offset , info->active_list , info->A );
  
  return NULL;
}


static void enkf_main_serialize_node( const char * node_key , 
                                      state_enum load_state , 
                                      const active_list_type * active_list , 
                                      int row_offset , 
                                      thread_pool_type * work_pool , 
                                      serialize_info_type * serialize_info) {

  /* Multithreaded serializing*/
  const int num_cpu_threads = thread_pool_get_max_running( work_pool );
  int icpu;
  
  thread_pool_restart( work_pool );
  for (icpu = 0; icpu < num_cpu_threads; icpu++) {
    serialize_info[icpu].key         = node_key;
    serialize_info[icpu].active_list = active_list;
    serialize_info[icpu].load_state  = load_state;
    serialize_info[icpu].row_offset  = row_offset;
    
    thread_pool_add_job( work_pool , serialize_nodes_mt , &serialize_info[icpu]);
  }
  thread_pool_join( work_pool );
}



/**
   The return value is the number of rows in the serialized
   A matrix. 
*/

static int enkf_main_serialize_dataset( enkf_main_type * enkf_main, 
                                        const local_dataset_type * dataset ,
                                        int report_step,
                                        hash_type * use_count ,  
                                        int * active_size , 
                                        int * row_offset,
                                        thread_pool_type * work_pool,
                                        serialize_info_type * serialize_info ) {

  matrix_type * A   = serialize_info->A;
  stringlist_type * update_keys = local_dataset_alloc_keys( dataset );
  const int num_kw  = stringlist_get_size( update_keys );
  int ens_size      = matrix_get_columns( A );
  int current_row   = 0;

  for (int ikw=0; ikw < num_kw; ikw++) {
    const char             * key              = stringlist_iget(update_keys , ikw);
    const active_list_type * active_list      = local_dataset_get_node_active_list( dataset , key );
      
    active_size[ikw] = __get_active_size( enkf_main , key , report_step , active_list );
    row_offset[ikw]  = current_row;

    {
      int matrix_rows = matrix_get_rows( A );
      if ((active_size[ikw] + current_row) > matrix_rows) 
        matrix_resize( A , matrix_rows + 2 * active_size[ikw] , ens_size , true );
    }
    
    if (active_size[ikw] > 0) {
      state_enum load_state;
      
      if (hash_inc_counter( use_count , key) == 0)
        load_state = FORECAST;   /* This is the first time this keyword is updated for this reportstep */
      else
        load_state = ANALYZED;
      
      enkf_main_serialize_node( key , load_state , active_list , row_offset[ikw] , work_pool , serialize_info );
      current_row += active_size[ikw];
    }
  } 
  matrix_shrink_header( A , current_row , ens_size );
  stringlist_free( update_keys ); 
  return matrix_get_rows( A );
}




static void deserialize_node( enkf_fs_type            * fs, 
                              enkf_state_type ** ensemble , 
                              int iens, 
                              const char * key , 
                              int report_step , 
                              int row_offset , 
                              const active_list_type * active_list,
                              matrix_type * A) {
  
  enkf_node_type * node = enkf_state_get_node( ensemble[iens] , key);
  enkf_node_deserialize(node , active_list , A , row_offset , iens);
  enkf_fs_fwrite_node( fs , node , report_step , iens , ANALYZED);
  
}



static void * deserialize_nodes_mt( void * arg ) {
  serialize_info_type * info = (serialize_info_type *) arg;
  int iens;
  for (iens = info->iens1; iens < info->iens2; iens++) 
    deserialize_node( info->fs , info->ensemble , iens , info->key , info->report_step , info->row_offset , info->active_list , info->A );

  return NULL;
}


static void enkf_main_deserialize_dataset( const local_dataset_type * dataset , 
                                           const int * active_size , 
                                           const int * row_offset , 
                                           serialize_info_type * serialize_info , 
                                           thread_pool_type * work_pool ) {
  
  int num_cpu_threads = thread_pool_get_max_running( work_pool );
  stringlist_type * update_keys = local_dataset_alloc_keys( dataset );
  for (int i = 0; i < stringlist_get_size( update_keys ); i++) {
    if (active_size[i] > 0) {
      const char             * key              = stringlist_iget(update_keys , i);
      const active_list_type * active_list      = local_dataset_get_node_active_list( dataset , key );

      {
        /* Multithreaded */
        int icpu;
        thread_pool_restart( work_pool );
        for (icpu = 0; icpu < num_cpu_threads; icpu++) {
          serialize_info[icpu].key         = key;
          serialize_info[icpu].active_list = active_list;
          serialize_info[icpu].row_offset  = row_offset[i];
          
          thread_pool_add_job( work_pool , deserialize_nodes_mt , &serialize_info[icpu]);
        }
        thread_pool_join( work_pool );
      }
    }
  }
  stringlist_free( update_keys );
}


static void serialize_info_free( serialize_info_type * serialize_info ) {
  free( serialize_info );
}

static serialize_info_type * serialize_info_alloc( enkf_main_type * enkf_main , int report_step , matrix_type * A , int num_cpu_threads ) {
  serialize_info_type * serialize_info = util_malloc( num_cpu_threads * sizeof * serialize_info , __func__ );
  int ens_size = matrix_get_columns( A );
  int icpu;
  int iens_offset = 0;
  enkf_fs_type * fs = enkf_main_get_fs( enkf_main );
  for (icpu = 0; icpu < num_cpu_threads; icpu++) {
    serialize_info[icpu].fs          = fs;
    serialize_info[icpu].ensemble    = enkf_main->ensemble;
    serialize_info[icpu].report_step = report_step;
    serialize_info[icpu].A           = A;
    serialize_info[icpu].iens1       = iens_offset;
    serialize_info[icpu].iens2       = iens_offset + (ens_size - iens_offset) / (num_cpu_threads - icpu);
    iens_offset = serialize_info[icpu].iens2;
  }
  serialize_info[num_cpu_threads - 1].iens2 = ens_size;
  return serialize_info;
}








// Bootstrap + CVvoid enkf_main_update_mulX_prin_comp_cv(enkf_main_type * enkf_main , const local_dataset_type * dataset , int report_step , hash_type * use_count , meas_data_type * meas_data , obs_data_type * obs_data) {
// Bootstrap + CV  const int num_cpu_threads          = 4;
// Bootstrap + CV  int       matrix_size              = 1000;  /* Starting with this */
// Bootstrap + CV  const int ens_size                 = enkf_main_get_ensemble_size(enkf_main);
// Bootstrap + CV  matrix_type * A = matrix_alloc(matrix_size , ens_size);
// Bootstrap + CV  msg_type  * msg = msg_alloc("Updating: " , false);
// Bootstrap + CV  stringlist_type * update_keys = local_dataset_alloc_keys( dataset );
// Bootstrap + CV  const int num_kw  = stringlist_get_size( update_keys );
// Bootstrap + CV  int * active_size = util_malloc( num_kw * sizeof * active_size , __func__);
// Bootstrap + CV  int * row_offset  = util_malloc( num_kw * sizeof * row_offset  , __func__);
// Bootstrap + CV  bool complete     = false;
// Bootstrap + CV  serialize_info_type * serialize_info;
// Bootstrap + CV  thread_pool_type * work_pool = thread_pool_alloc( num_cpu_threads , false );
// Bootstrap + CV  
// Bootstrap + CV  int nrobs                = obs_data_get_active_size(obs_data);
// Bootstrap + CV  int nrmin                = util_int_min( ens_size , nrobs);
// Bootstrap + CV  enkf_mode_type enkf_mode = analysis_config_get_enkf_mode( enkf_main->analysis_config );
// Bootstrap + CV  double truncation                     = analysis_config_get_truncation( enkf_main->analysis_config );  
// Bootstrap + CV  matrix_type * S , *R , *E , *D , *innov;
// Bootstrap + CV  matrix_type * randrot       = NULL;
// Bootstrap + CV
// Bootstrap + CV  
// Bootstrap + CV  /*Allocate matrix for principal components and reduced error covariance matrix Rp */
// Bootstrap + CV  matrix_type * Z    = matrix_alloc( nrmin , ens_size    );
// Bootstrap + CV  matrix_type * Rp   = matrix_alloc( nrmin , nrmin       );
// Bootstrap + CV  matrix_type * Dp   = matrix_alloc( nrmin , ens_size    );
// Bootstrap + CV
// Bootstrap + CV  
// Bootstrap + CV  /*
// Bootstrap + CV    Pre-processing step: Returns matrices V0T, Z, eig, U0, needed for
// Bootstrap + CV    local CV below. This step is only performed once for each
// Bootstrap + CV    ministep, whereas the the update code below (can) go in several
// Bootstrap + CV    steps if memory is tight.
// Bootstrap + CV  */
// Bootstrap + CV
// Bootstrap + CV
// Bootstrap + CV  enkf_analysis_alloc_matrices( enkf_main->rng , meas_data , obs_data , enkf_mode , &S , &R , &innov , &E , &D , true ); 
// Bootstrap + CV  enkf_analysis_init_principal_components( truncation ,  S , R , innov , E , D , Z , Rp , Dp);
// Bootstrap + CV  
// Bootstrap + CV  if (analysis_config_get_random_rotation( enkf_main->analysis_config ))
// Bootstrap + CV    randrot = enkf_analysis_alloc_mp_randrot( ens_size , enkf_main->rng );
// Bootstrap + CV  
// Bootstrap + CV  serialize_info = serialize_info_alloc( enkf_main , report_step , A , num_cpu_threads );
// Bootstrap + CV  msg_show( msg );
// Bootstrap + CV  {
// Bootstrap + CV    int ikw1 = 0;
// Bootstrap + CV    do {
// Bootstrap + CV      int ikw2;
// Bootstrap + CV      int current_row_offset   = 0;
// Bootstrap + CV      /* Recover full matrix size - after matrix_shrink_header() has been called. */
// Bootstrap + CV      matrix_resize( A , matrix_size , ens_size , false);  
// Bootstrap + CV      
// Bootstrap + CV      ikw2 = enkf_main_serialize(enkf_main , 
// Bootstrap + CV                                 A , 
// Bootstrap + CV                                 report_step , 
// Bootstrap + CV                                 use_count , 
// Bootstrap + CV                                 ikw1 , 
// Bootstrap + CV                                 active_size , 
// Bootstrap + CV                                 row_offset , 
// Bootstrap + CV                                 update_keys , 
// Bootstrap + CV                                 dataset,
// Bootstrap + CV                                 work_pool , 
// Bootstrap + CV                                 serialize_info , 
// Bootstrap + CV                                 &current_row_offset , 
// Bootstrap + CV                                 msg );
// Bootstrap + CV      
// Bootstrap + CV      if (current_row_offset > 0) {
// Bootstrap + CV        /* The actual update */
// Bootstrap + CV        
// Bootstrap + CV        /*Get the optimal update matrix - observe that the A matrix is input.*/
// Bootstrap + CV        { 
// Bootstrap + CV          matrix_type * X5 = matrix_alloc( ens_size , ens_size );
// Bootstrap + CV
// Bootstrap + CV          enkf_analysis_initX_principal_components_cv( analysis_config_get_nfolds_CV( enkf_main->analysis_config ),
// Bootstrap + CV                                                       analysis_config_get_penalised_press( enkf_main->analysis_config ),
// Bootstrap + CV                                                       X5 , 
// Bootstrap + CV                                                       enkf_main->rng, 
// Bootstrap + CV                                                       A  , 
// Bootstrap + CV                                                       Z  , 
// Bootstrap + CV                                                       Rp , 
// Bootstrap + CV                                                       Dp );   
// Bootstrap + CV          
// Bootstrap + CV          msg_update(msg , " matrix multiplication");
// Bootstrap + CV          matrix_inplace_matmul_mt( A , X5 , num_cpu_threads );  
// Bootstrap + CV          matrix_free( X5 );
// Bootstrap + CV        }
// Bootstrap + CV        
// Bootstrap + CV        enkf_main_deserialize( update_keys , ikw1 , ikw2 , active_size , row_offset , dataset , serialize_info , num_cpu_threads , work_pool , msg );
// Bootstrap + CV      }
// Bootstrap + CV      ikw1 = ikw2;
// Bootstrap + CV      if (ikw2 == num_kw)
// Bootstrap + CV        complete = true;
// Bootstrap + CV    } while ( !complete );
// Bootstrap + CV  } 
// Bootstrap + CV  
// Bootstrap + CV  thread_pool_free( work_pool );
// Bootstrap + CV  serialize_info_free( serialize_info );
// Bootstrap + CV  free( active_size );
// Bootstrap + CV  free( row_offset );
// Bootstrap + CV  msg_free( msg , true );
// Bootstrap + CV
// Bootstrap + CV  matrix_free( R );
// Bootstrap + CV  matrix_free( S );
// Bootstrap + CV  matrix_free( innov );
// Bootstrap + CV  
// Bootstrap + CV  matrix_safe_free( E );
// Bootstrap + CV  matrix_safe_free( D );
// Bootstrap + CV  matrix_free( A );
// Bootstrap + CV  matrix_free( Rp );
// Bootstrap + CV  matrix_free( Dp );
// Bootstrap + CV  matrix_free( Z );
// Bootstrap + CV  matrix_safe_free( randrot );
// Bootstrap + CV}
// Bootstrap + CV
// Bootstrap + CV
// Bootstrap + CV
// Bootstrap + CV
// Bootstrap + CV
// Bootstrap + CV
// Bootstrap + CV
// Bootstrap + CV
// Bootstrap + CV
// Bootstrap + CV
// Bootstrap + CV
// Bootstrap + CV/* 
// Bootstrap + CV   Perform Cross-Validation and update ensemble members one at a time
// Bootstrap + CV   using BOOTSTRAP estimates for ensemble covariance for localised 
// Bootstrap + CV   EnKF updating schemes.  Here we create the update matrix X5 within 
// Bootstrap + CV   the function, rather than sending it as an input.
// Bootstrap + CV*/
// Bootstrap + CV
// Bootstrap + CVvoid enkf_main_update_mulX_bootstrap(enkf_main_type * enkf_main , 
// Bootstrap + CV                                     const local_dataset_type * dataset , 
// Bootstrap + CV                                     const int_vector_type * step_list , 
// Bootstrap + CV                                     hash_type * use_count , 
// Bootstrap + CV                                     meas_data_type * meas_data , 
// Bootstrap + CV                                     obs_data_type * obs_data, 
// Bootstrap + CV                                     double std_cutoff, 
// Bootstrap + CV                                     double alpha) {
// Bootstrap + CV  const int num_cpu_threads          = 4;
// Bootstrap + CV
// Bootstrap + CV  int       matrix_size              = 1000;  /* Starting with this */
// Bootstrap + CV  const int ens_size                 = enkf_main_get_ensemble_size(enkf_main);
// Bootstrap + CV  matrix_type * A = matrix_alloc(matrix_size , ens_size);
// Bootstrap + CV  msg_type  * msg = msg_alloc("Updating: " , false);
// Bootstrap + CV  stringlist_type * update_keys = local_dataset_alloc_keys( dataset );
// Bootstrap + CV  const int num_kw  = stringlist_get_size( update_keys );
// Bootstrap + CV  int * active_size = util_malloc( num_kw * sizeof * active_size , __func__);
// Bootstrap + CV  int * row_offset  = util_malloc( num_kw * sizeof * row_offset  , __func__);
// Bootstrap + CV  bool complete     = false;
// Bootstrap + CV  serialize_info_type * serialize_info = serialize_info_alloc( enkf_main , report_step , A , num_cpu_threads );
// Bootstrap + CV  thread_pool_type * work_pool = thread_pool_alloc( num_cpu_threads , false );
// Bootstrap + CV  
// Bootstrap + CV  int nrobs                = obs_data_get_active_size(obs_data);
// Bootstrap + CV  int nrmin                = util_int_min( ens_size , nrobs);
// Bootstrap + CV  int report_step          = int_vector_get_last( step_list );
// Bootstrap + CV  
// Bootstrap + CV  matrix_type * randrot       = NULL;
// Bootstrap + CV  
// Bootstrap + CV  /*
// Bootstrap + CV    Generating a matrix with random integers to be used for sampling
// Bootstrap + CV    across serializing. This matrix will hold double values, but an
// Bootstrap + CV    integer cast will give a random integer between 0 and ens_size-1.
// Bootstrap + CV  */
// Bootstrap + CV
// Bootstrap + CV  matrix_type * randints = matrix_alloc( ens_size , ens_size);
// Bootstrap + CV  for (int i = 0; i < ens_size; i++){
// Bootstrap + CV    for (int j = 0; j < ens_size; j++){
// Bootstrap + CV      double r = 1.0 * rng_get_int( enkf_main->rng , ens_size );
// Bootstrap + CV      matrix_iset(randints, i , j , r);
// Bootstrap + CV    }
// Bootstrap + CV  }
// Bootstrap + CV
// Bootstrap + CV  if (analysis_config_get_random_rotation( enkf_main->analysis_config ))
// Bootstrap + CV    randrot = enkf_analysis_alloc_mp_randrot( ens_size  , enkf_main->rng );
// Bootstrap + CV  
// Bootstrap + CV  msg_show( msg );
// Bootstrap + CV  {
// Bootstrap + CV    int ikw1 = 0;
// Bootstrap + CV    do {
// Bootstrap + CV      int ikw2;
// Bootstrap + CV      int current_row_offset   = 0;
// Bootstrap + CV      matrix_resize( A , matrix_size , ens_size , false);  /* Recover full matrix size - after matrix_shrink_header() has been called. */
// Bootstrap + CV
// Bootstrap + CV      ikw2 = enkf_main_serialize(enkf_main , 
// Bootstrap + CV                                 A , 
// Bootstrap + CV                                 report_step , 
// Bootstrap + CV                                 use_count , 
// Bootstrap + CV                                 ikw1 , 
// Bootstrap + CV                                 active_size , 
// Bootstrap + CV                                 row_offset , 
// Bootstrap + CV                                 update_keys , 
// Bootstrap + CV                                 dataset ,
// Bootstrap + CV                                 work_pool , 
// Bootstrap + CV                                 serialize_info , 
// Bootstrap + CV                                 &current_row_offset , 
// Bootstrap + CV                                 msg );
// Bootstrap + CV  
// Bootstrap + CV      if (current_row_offset > 0) {
// Bootstrap + CV        /* The actual update */
// Bootstrap + CV        
// Bootstrap + CV        /* The updates are performed one at a time when using bootstrap. Hence the followin loop*/
// Bootstrap + CV        { 
// Bootstrap + CV          int ensemble_members_loop;
// Bootstrap + CV          matrix_type * work_A                     = matrix_alloc_copy( A ); // Huge memory requirement. Is needed such that we do not resample updated ensemble members from A
// Bootstrap + CV          meas_data_type * meas_data_resampled = meas_data_alloc_copy( meas_data );
// Bootstrap + CV          matrix_type      * A_resampled       = matrix_alloc( matrix_get_rows(work_A) , matrix_get_columns( work_A ));
// Bootstrap + CV
// Bootstrap + CV          for ( ensemble_members_loop = 0; ensemble_members_loop < ens_size; ensemble_members_loop++) { 
// Bootstrap + CV            int ensemble_counter;
// Bootstrap + CV            /* Resample A and meas_data. Here we are careful to resample the working copy.*/
// Bootstrap + CV            int_vector_type * bootstrap_components = int_vector_alloc( ens_size , 0);
// Bootstrap + CV            for (ensemble_counter  = 0; ensemble_counter < ens_size; ensemble_counter++) {
// Bootstrap + CV              double random_col = matrix_iget( randints , ensemble_members_loop , ensemble_counter );
// Bootstrap + CV              int random_column = (int)random_col;
// Bootstrap + CV              int_vector_iset( bootstrap_components , ensemble_counter , random_column );
// Bootstrap + CV              matrix_copy_column( A_resampled , work_A , ensemble_counter , random_column );
// Bootstrap + CV              meas_data_assign_vector( meas_data_resampled, meas_data , ensemble_counter , random_column);
// Bootstrap + CV            }
// Bootstrap + CV            int_vector_select_unique( bootstrap_components );
// Bootstrap + CV            int unique_bootstrap_components = int_vector_size( bootstrap_components );
// Bootstrap + CV            int_vector_free( bootstrap_components);
// Bootstrap + CV            if (analysis_config_get_do_local_cross_validation( enkf_main->analysis_config )) { /* Bootstrapping and CV*/
// Bootstrap + CV              matrix_type * U0   = matrix_alloc( nrobs , nrmin    ); /* Left singular vectors.  */
// Bootstrap + CV              matrix_type * V0T  = matrix_alloc( nrmin , ens_size ); /* Right singular vectors. */
// Bootstrap + CV              matrix_type * Z    = matrix_alloc( nrmin , nrmin    );
// Bootstrap + CV              double      * eig  = util_malloc( sizeof * eig * nrmin , __func__);
// Bootstrap + CV              /*
// Bootstrap + CV                Pre-processing step: Returns matrices V0T, Z, eig, U0, needed for
// Bootstrap + CV                local CV below. 
// Bootstrap + CV              */
// Bootstrap + CV              enkf_analysis_local_pre_cv( enkf_main->analysis_config , enkf_main->rng , meas_data_resampled , obs_data ,  V0T , Z , eig , U0 , meas_data );
// Bootstrap + CV              matrix_type * X5_boot_cv = enkf_analysis_allocX_pre_cv( enkf_main->analysis_config , 
// Bootstrap + CV                                                                      enkf_main->rng , 
// Bootstrap + CV                                                                      meas_data_resampled , 
// Bootstrap + CV                                                                      obs_data , 
// Bootstrap + CV                                                                      randrot, 
// Bootstrap + CV                                                                      A_resampled , 
// Bootstrap + CV                                                                      V0T ,
// Bootstrap + CV                                                                      Z , 
// Bootstrap + CV                                                                      eig, 
// Bootstrap + CV                                                                      U0, 
// Bootstrap + CV                                                                      meas_data, 
// Bootstrap + CV                                                                      unique_bootstrap_components);
// Bootstrap + CV              
// Bootstrap + CV              msg_update(msg , " matrix multiplication");
// Bootstrap + CV              matrix_inplace_matmul_mt( A_resampled , X5_boot_cv , num_cpu_threads );
// Bootstrap + CV              matrix_inplace_add( A_resampled , work_A ); 
// Bootstrap + CV              matrix_copy_column( A , A_resampled, ensemble_members_loop, ensemble_members_loop);
// Bootstrap + CV              matrix_free( U0 );
// Bootstrap + CV              matrix_free( V0T );
// Bootstrap + CV              matrix_free( Z );
// Bootstrap + CV              free( eig );
// Bootstrap + CV              matrix_free( X5_boot_cv );
// Bootstrap + CV            } else { /* Just Bootstrapping */
// Bootstrap + CV              matrix_type * X5_boot = enkf_analysis_allocX_boot( enkf_main->analysis_config , 
// Bootstrap + CV                                                                 enkf_main->rng , 
// Bootstrap + CV                                                                 meas_data_resampled , 
// Bootstrap + CV                                                                 obs_data , 
// Bootstrap + CV                                                                 randrot , 
// Bootstrap + CV                                                                 meas_data);
// Bootstrap + CV              msg_update(msg , " matrix multiplication");
// Bootstrap + CV              matrix_inplace_matmul_mt( A_resampled , X5_boot , num_cpu_threads );
// Bootstrap + CV              matrix_free( X5_boot );
// Bootstrap + CV              matrix_inplace_add( A_resampled , work_A );
// Bootstrap + CV              matrix_copy_column( A , A_resampled, ensemble_members_loop, ensemble_members_loop);
// Bootstrap + CV            }
// Bootstrap + CV          }
// Bootstrap + CV          matrix_free( A_resampled );
// Bootstrap + CV          meas_data_free(meas_data_resampled);
// Bootstrap + CV          matrix_free(work_A);
// Bootstrap + CV        }
// Bootstrap + CV        
// Bootstrap + CV        enkf_main_deserialize( update_keys , ikw1 , ikw2 , active_size , 
// Bootstrap + CV                               row_offset , dataset , serialize_info , 
// Bootstrap + CV                               num_cpu_threads , work_pool , msg );
// Bootstrap + CV        
// Bootstrap + CV      }
// Bootstrap + CV      ikw1 = ikw2;
// Bootstrap + CV      if (ikw2 == num_kw)
// Bootstrap + CV        complete = true;
// Bootstrap + CV    } while ( !complete );
// Bootstrap + CV  }
// Bootstrap + CV
// Bootstrap + CV  thread_pool_free( work_pool );
// Bootstrap + CV  serialize_info_free( serialize_info );
// Bootstrap + CV  free( active_size );
// Bootstrap + CV  free( row_offset );
// Bootstrap + CV  
// Bootstrap + CV  msg_free( msg , true );
// Bootstrap + CV  matrix_free( A );
// Bootstrap + CV  matrix_free( randints );
// Bootstrap + CV  matrix_safe_free( randrot );
// Bootstrap + CV}



void enkf_main_module_update( enkf_main_type * enkf_main , 
                              hash_type * use_count,
                              int report_step , 
                              const local_ministep_type * ministep , 
                              const meas_data_type * forecast , 
                              obs_data_type * obs_data) {

  const int cpu_threads       = 4;
  const int matrix_start_size = 25000;
  thread_pool_type * tp       = thread_pool_alloc( cpu_threads , false );
  
  analysis_module_type * module = analysis_config_get_active_module( enkf_main->analysis_config );
  int ens_size          = meas_data_get_ens_size( forecast );
  int active_size       = obs_data_get_active_size( obs_data );
  matrix_type * X       = matrix_alloc( ens_size , ens_size );
  matrix_type * S       = meas_data_allocS( forecast , active_size );
  matrix_type * R       = obs_data_allocR( obs_data , active_size );
  matrix_type * dObs    = obs_data_allocdObs( obs_data , active_size );
  matrix_type * A       = matrix_alloc( matrix_start_size , ens_size );
  matrix_type * E       = NULL;
  matrix_type * D       = NULL;
  matrix_type * randrot = NULL; 
  matrix_type * localA  = NULL;

  if (analysis_module_get_option( module , ANALYSIS_NEED_ED)) {
    E = obs_data_allocE( obs_data , enkf_main->rng , ens_size , active_size );
    D = obs_data_allocD( obs_data , E , S );
  }
  
  if (analysis_module_get_option( module , ANALYSIS_NEED_RANDROT)) 
    randrot = enkf_analysis_alloc_mp_randrot( ens_size , enkf_main->rng );
  
  if (analysis_module_get_option( module , ANALYSIS_USE_A | ANALYSIS_UPDATE_A)) {
    printf("Using A \n");
    localA = A;
  }

  /*****************************************************************/
  
  analysis_module_init_update( module , S , R , dObs , E , D );
  {
    hash_iter_type * dataset_iter = local_ministep_alloc_dataset_iter( ministep );
    serialize_info_type * serialize_info = serialize_info_alloc( enkf_main , report_step , A , cpu_threads);
    while (!hash_iter_is_complete( dataset_iter )) {
      const char * dataset_name = hash_iter_get_next_key( dataset_iter );
      const local_dataset_type * dataset = local_ministep_get_dataset( ministep , dataset_name );
      if (local_dataset_get_size( dataset )) {
        int * active_size = util_malloc( local_dataset_get_size( dataset ) * sizeof * active_size , __func__);
        int * row_offset  = util_malloc( local_dataset_get_size( dataset ) * sizeof * row_offset  , __func__);
        
        enkf_main_serialize_dataset( enkf_main , dataset , report_step ,  use_count , active_size , row_offset , tp , serialize_info);
        
        if (analysis_module_get_option( module , ANALYSIS_UPDATE_A))
          analysis_module_updateA( module , localA , S , R , dObs , E , D , randrot );
        else {
          analysis_module_initX( module , X , localA , S , R , dObs , E , D , randrot );
          matrix_inplace_matmul_mt2( A , X , tp );
        }
          
        enkf_main_deserialize_dataset( dataset , active_size , row_offset , serialize_info , tp);
        
        free( active_size );
        free( row_offset );
      }
    }
    hash_iter_free( dataset_iter );
    serialize_info_free( serialize_info );
      
  }
  analysis_module_complete_update( module );
    

  /*****************************************************************/

  matrix_safe_free( randrot );
  matrix_safe_free( E );
  matrix_safe_free( D );
  matrix_free( S );
  matrix_free( R );
  matrix_free( dObs );
  matrix_free( X );
}




/**
   This is  T H E  EnKF update routine.
**/

void enkf_main_UPDATE(enkf_main_type * enkf_main , const int_vector_type * step_list) {
  /*
     If merge_observations is true all observations in the time
     interval [step1+1,step2] will be used, otherwise only the last
     observation at step2 will be used.
  */
  double alpha       = analysis_config_get_alpha( enkf_main->analysis_config );
  double std_cutoff  = analysis_config_get_std_cutoff( enkf_main->analysis_config );
  const int ens_size = enkf_main_get_ensemble_size(enkf_main);
  
  {
    /*
      Observations and measurements are collected in these temporary
      structures. obs_data is a precursor for the 'd' vector, and
      meas_forecast is a precursor for the 'S' matrix'.

      The reason for going via these temporary structures is to support
      deactivating observations which should not be used in the update
      process.
    */
    obs_data_type               * obs_data      = obs_data_alloc();
    meas_data_type              * meas_forecast = meas_data_alloc( ens_size );
    meas_data_type              * meas_analyzed = meas_data_alloc( ens_size );
    local_config_type           * local_config  = enkf_main->local_config;
    const local_updatestep_type * updatestep    = local_config_iget_updatestep( local_config , int_vector_get_last( step_list ));  /* Only last step considered when forming local update */
    hash_type                   * use_count     = hash_alloc();
    matrix_type                 * randrot       = NULL;
    const char                  * log_path      = analysis_config_get_log_path( enkf_main->analysis_config );
    FILE                        * log_stream;


    if (analysis_config_get_random_rotation( enkf_main->analysis_config ))
      randrot = enkf_analysis_alloc_mp_randrot( ens_size , enkf_main->rng );
    
    {
      char * log_file;
      if (int_vector_size( step_list ) == 1) 
        log_file = util_alloc_sprintf("%s%c%04d" , log_path , UTIL_PATH_SEP_CHAR , int_vector_iget( step_list , 0));
      else 
        log_file = util_alloc_sprintf("%s%c%04d-%04d" , log_path , UTIL_PATH_SEP_CHAR , int_vector_iget( step_list , 0) , int_vector_get_last( step_list ));
      log_stream = util_fopen( log_file , "w" );
      
      free( log_file );
    }
    

    for (int ministep_nr = 0; ministep_nr < local_updatestep_get_num_ministep( updatestep ); ministep_nr++) {   /* Looping over local analysis ministep */
      local_ministep_type * ministep = local_updatestep_iget_ministep( updatestep , ministep_nr );
      local_dataset_type  * dataset = NULL;
      local_obsset_type   * obsset  = local_ministep_get_obsset( ministep );

      obs_data_reset( obs_data );
      meas_data_reset( meas_forecast );
      
      enkf_obs_get_obs_and_measure(enkf_main->obs, 
                                   enkf_main_get_fs(enkf_main), 
                                   step_list , 
                                   FORECAST, 
                                   ens_size,
                                   (const enkf_state_type **) enkf_main->ensemble, 
                                   meas_forecast, 
                                   obs_data , 
                                   obsset );
      
      enkf_analysis_deactivate_outliers( obs_data , meas_forecast  , std_cutoff , alpha);
      
      /* How the fuck does dup() work?? */
      enkf_analysis_fprintf_obs_summary( obs_data , meas_forecast  , step_list , local_ministep_get_name( ministep ) , stdout );
      enkf_analysis_fprintf_obs_summary( obs_data , meas_forecast  , step_list , local_ministep_get_name( ministep ) , log_stream );

      if (obs_data_get_active_size(obs_data) > 0) {
        if (analysis_config_Xbased( enkf_main->analysis_config )) {

          enkf_main_module_update( enkf_main , use_count , int_vector_get_last( step_list ) , ministep , meas_forecast , obs_data );
          
          //Bootstrap + CVif (analysis_config_get_bootstrap( enkf_main->analysis_config )) {
          //Bootstrap + CV  /*
          //Bootstrap + CV    Think there is a memory bug in this update code, when
          //Bootstrap + CV    the allocated A matrix is not large enough to hold all
          //Bootstrap + CV    data.
          //Bootstrap + CV  */
          //Bootstrap + CV  enkf_main_update_mulX_bootstrap(enkf_main , dataset , step_list , use_count , meas_forecast , obs_data, std_cutoff, alpha);
          //Bootstrap + CV} else if (analysis_config_get_do_local_cross_validation( enkf_main->analysis_config )) {
          //Bootstrap + CV  /* Update based on Cross validation AND local analysis. */
          //Bootstrap + CV  enkf_main_update_mulX_prin_comp_cv(enkf_main , 
          //Bootstrap + CV                                     dataset , 
          //Bootstrap + CV                                     int_vector_get_last( step_list ) , 
          //Bootstrap + CV                                     use_count , 
          //Bootstrap + CV                                     meas_forecast , 
          //Bootstrap + CV                                     obs_data);
          //Bootstrap + CV
          //Bootstrap + CV} else {
          //Bootstrap + CV  /* Plain vanilla goes through module update. */
          //Bootstrap + CV  enkf_main_module_update( enkf_main , use_count , int_vector_get_last( step_list ) , ministep , meas_forecast , obs_data );
          //Bootstrap + CV}
        }
      }
    }
    fclose( log_stream );

    matrix_safe_free( randrot );
    obs_data_free( obs_data );
    meas_data_free( meas_forecast );
    meas_data_free( meas_analyzed );
    enkf_main_inflate( enkf_main , int_vector_get_last( step_list ) , use_count);
    hash_free( use_count );
  }
}





/**
   This function will spin while it is periodically checking the
   status of the forward model simulations. Everytime a completed
   simulation is detected a seperate thread is spawned (via a
   thread_pool) to actually load the results.
   
   The function will return when all the results have been loaded, or
   alternatively the jobs have given up completely.
*/



static void enkf_main_run_wait_loop(enkf_main_type * enkf_main ) {
  const int num_load_threads      = 10;
  const int ens_size              = enkf_main_get_ensemble_size(enkf_main);
  job_queue_type * job_queue      = site_config_get_job_queue(enkf_main->site_config);                          
  arg_pack_type ** arg_list       = util_malloc( ens_size * sizeof * arg_list , __func__);
  job_status_type * status_list   = util_malloc( ens_size * sizeof * status_list , __func__);
  thread_pool_type * load_threads = thread_pool_alloc( num_load_threads , true);
  const int usleep_time           = 2500000; 
  const int load_start_usleep     =   10000;
  int jobs_remaining;
  int iens;

  for (iens = 0; iens < ens_size; iens++) {
    arg_list[iens]    = arg_pack_alloc();
    status_list[iens] = JOB_QUEUE_NOT_ACTIVE;
  }

  do {
    job_status_type status;
    jobs_remaining = 0;

    for (iens = 0; iens < ens_size; iens++) {
      enkf_state_type * enkf_state = enkf_main->ensemble[iens];
      status = enkf_state_get_run_status( enkf_state );
      if ((status & JOB_QUEUE_CAN_FINALIZE) == 0)
        jobs_remaining += 1;  /* OK - the job is still running/loading. */

      if ((status == JOB_QUEUE_RUN_OK) || (status == JOB_QUEUE_RUN_FAIL)) {
        if (status_list[iens] != status) {
          arg_pack_clear( arg_list[iens] );
          arg_pack_append_ptr( arg_list[iens] , enkf_state );
          arg_pack_append_int( arg_list[iens] , status );

          /*
             Dispatch a separate thread to load the results from this job. This
             must be done also for the jobs with status JOB_QUEUE_RUN_FAIL -
             because it is the enkf_state_complete_forward_model() function
             which does a resubmit, or alternatively signal complete failure to
             the queue system (should probably be split in two).
          */
          
          thread_pool_add_job( load_threads , enkf_state_complete_forward_model__ , arg_list[iens] );
          /* This will block until the enkf_state_complete_forward_model() has actually started executing. */
          {
            job_status_type new_status;
            do {
              usleep( load_start_usleep );
              new_status = enkf_state_get_run_status( enkf_state );
            } while ( new_status == status );
            status = new_status;
          }
        }
      }

      /*
        The code in the {} above is an attempt to solve this race:
        ----------------------------------------------------------

        In the case of jobs failing with status == JOB_QUEUE_RUN_FAIL this code
        has the following race-condition:

        1. This function detects the JOB_QUEUE_RUN_FAIL state and dispatches a
           thread to run the function enkf_state_complete_forward_model(). This
           again will (possibly) tell the queue system to try the job again.


        2. This function will store status JOB_QUEUE_RUN_FAIL as status for this
           job.


        3. The code dispatching the enkf_state_complete_forward_model() function
           is enclosed in a

             if (status_list[iens] != status) {
                ....
             }

           i.e. it reacts to state *changes*. Now if the job immediately fails
           again with status JOB_QUEUE_RUN_FAIL, without this scope getting the
           chance to temporarily register a different status, there will no
           state *change*, and the second failure will not be registered.

        The same applies to jobs which suceed with JOB_QUEUE_RUN_OK, but then
        subsequently fail to load.
      */

      status_list[iens] = status;
    }
    if (jobs_remaining > 0)
      usleep( usleep_time );
    

    /* A poor man's /proc interface .... */
    {
      if (util_file_exists("MAX_RUNNING")) {
        FILE * stream = util_fopen("MAX_RUNNING" , "r");
        int max_running;
        fscanf( stream , "%d" , &max_running);
        fclose( stream );
        unlink( "MAX_RUNNING" );
        job_queue_set_max_running( job_queue , max_running );
      }
    }
    
  } while (jobs_remaining > 0);
  thread_pool_join( load_threads );
  thread_pool_free( load_threads );

  for (iens = 0; iens < ens_size; iens++)
    arg_pack_free( arg_list[iens] );
  free( arg_list );
  free( status_list );
}



/**
   The function enkf_main_run_step() is quite heavily multithreaded
   involving one designated worker thread (queue_thread) and two
   thread_pools. In the diagram below we have attempted to illustrate
   the multithreaded behaviour of the function enkf_main_run_step():

    o The execution path does not leave a 'box' before the thread / thread_pool
      has been joined.

    o An 'X' is meant to indicate a join.

    o Dotted lines indicate communication; specifically the queue_thread running
      the queue is "the owner" of all the job status information.

    o The thread pool spawns many individual worker threads, these are
      administrated by the thread_pool and not shown in the diagram.


   main_thread: enkf_main_run_step
   -------------------------------
               |
               |
               |------------------------------------->--------------------------+
               |                                                                |
               |                                                                |
               |                                                                |
               |                                                                |
               |                                                                |
               |                                                   _____________|____________________
    ___________|_______ thread pool __________________            /                                  \
   /                                                  \           | queue_thread: job_queue_run_jobs |
   |  submit_threads: enkf_state_start_forward_model  |...........\__________________________________/
   \__________________________________________________/              .     .    |
               |                                                     .     .    |
               |                                                     .     .    |
               |                                                     .     .    |
               |                                                     .     .    |
               |                                                     .     .    |
               +---------+                                           .     .    |
                         |                                           .     .    |
                         |                                           .     .    |
                         |                                           .     .    |
                                                                     .     .    |
              main_thread: enkf_main_wait_loop()......................     .    |
              ----------------------------------                           .    |
                         |                                                 .    |
                         |                                                 .    |
        _________________| thread pool ____________________                .    |
       /                                                   \               .    |
       | load_threads: enkf_state_complete_forward_model() |................    |
       \___________________________________________________/                    |
                         |                                                      |
                         |                                                      |
                         |                                                      |
               +---------+                                                      |
               |                                                                |
               |                                                                |
               X---------------------------------------<------------------------+
               |
               |
              \|/

         Some single threaded clean up.


  In addition to the trivial speed up (on a multi CPU box) the
  multithreading allows for asyncronous treatmeant of the queue,
  loading of results e.t.c. The latter is probably the most important
  argument for using a multithreaded approach.

  If all simulations have completed successfully the function will
  return true, otherwise it will return false.
*/


static bool enkf_main_run_step(enkf_main_type * enkf_main      ,
                               run_mode_type    run_mode       ,
                               const bool * iactive            ,
                               int load_start                  ,      /* For internalizing results, and the first step in the update when merging. */
                               int init_step_parameter         ,
                               state_enum init_state_parameter ,
                               state_enum init_state_dynamic   ,
                               int step1                       ,
                               int step2                       ,      /* Discarded for predictions */
                               bool enkf_update) {

  {
    const ecl_config_type * ecl_config = enkf_main_get_ecl_config( enkf_main );
    if ((step1 > 0) && (!ecl_config_can_restart(ecl_config))) {
      fprintf(stderr,"** Warning - tried to restart case which is not properly set up for restart.\n");
      fprintf(stderr,"** Need <INIT> in datafile and INIT_SECTION keyword in config file.\n");
      util_exit("%s: exiting \n",__func__);
    }
  }
  
  {
    bool     verbose_queue   = true;
    int  max_internal_submit = model_config_get_max_internal_submit(enkf_main->model_config);
    const int ens_size       = enkf_main_get_ensemble_size( enkf_main );
    int   job_size;
    int iens;

    if (run_mode == ENSEMBLE_PREDICTION)
      printf("Starting predictions from step: %d\n",step1);
    else
      printf("Starting forward step: %d -> %d\n",step1 , step2);

    log_add_message(enkf_main->logh , 1 , NULL , "===================================================================", false);
    log_add_fmt_message(enkf_main->logh , 1 , NULL , "Forward model: %d -> %d ",step1,step2);
    job_size = 0;
    for (iens = 0; iens < ens_size; iens++)
      if (iactive[iens]) job_size++;

    {
      pthread_t        queue_thread;
      job_queue_type * job_queue = site_config_get_job_queue(enkf_main->site_config);
      arg_pack_type * queue_args = arg_pack_alloc();    /* This arg_pack will be freed() in the job_que_run_jobs__()
                                                           function after the content has been unpacked. */
      arg_pack_append_ptr(queue_args  , job_queue);
      arg_pack_append_int(queue_args  , job_size);
      arg_pack_append_bool(queue_args , verbose_queue);
      
      pthread_create( &queue_thread , NULL , job_queue_run_jobs__ , queue_args);

      {
        thread_pool_type * submit_threads = thread_pool_alloc( 4 , true );
        for (iens = 0; iens < ens_size; iens++) {
          if (iactive[iens]) {
            int load_start = step1;
            if (step1 > 0)
              load_start++;

            enkf_state_init_run(enkf_main->ensemble[iens] ,
                                run_mode ,
                                iactive[iens] ,
                                max_internal_submit ,
                                init_step_parameter ,
                                init_state_parameter,
                                init_state_dynamic  ,
                                load_start ,
                                step1 ,
                                step2 );
                                
            
            thread_pool_add_job(submit_threads , enkf_state_start_forward_model__ , enkf_main->ensemble[iens]);
          } else
            enkf_state_set_inactive( enkf_main->ensemble[iens] );
        }
        /*
          After this join all directories/files for the simulations
          have been set up correctly, and all the jobs have been added
          to the job_queue manager.
        */
        thread_pool_join(submit_threads);        
        thread_pool_free(submit_threads);        
      }
      job_queue_submit_complete( job_queue );
      log_add_message(enkf_main->logh , 1 , NULL , "All jobs ready for running - waiting for completion" ,  false);

      enkf_main_run_wait_loop( enkf_main );      /* Waiting for all the jobs - and the loading of results - to complete. */
      pthread_join( queue_thread , NULL );       /* Wait for the job_queue_run_jobs() function to complete. */
    }



    {
      bool runOK   = true;  /* The runOK checks both that the external jobs have completed OK, and that the ert layer has loaded all data. */
      
      for (iens = 0; iens < ens_size; iens++) {
        if (! enkf_state_runOK(enkf_main->ensemble[iens])) {
          if ( runOK ) {
            log_add_fmt_message( enkf_main->logh , 1 , stderr , "Some models failed to integrate from DATES %d -> %d:",step1 , step2);
            runOK = false;
          }
          log_add_fmt_message( enkf_main->logh , 1 , stderr , "** Error in: %s " , enkf_state_get_run_path(enkf_main->ensemble[iens]));
        }
      }
      /* 
         If !runOK the tui code used to call util_exit() here, now the code just returns false.
      */

      if (runOK) {
        log_add_fmt_message(enkf_main->logh , 1 , NULL , "All jobs complete and data loaded for step: ->%d" , step2);
        
        if (enkf_update) {
          int_vector_type * step_list = enkf_main_update_alloc_step_list( enkf_main , load_start , step2 );
          enkf_main_UPDATE(enkf_main , step_list );
          int_vector_free( step_list );
        }
      }
      enkf_fs_fsync( enkf_main->dbase );
      
      return runOK;
    }
  }
}


int_vector_type * enkf_main_update_alloc_step_list( const enkf_main_type * enkf_main , int load_start , int step2 ) {
  bool merge_observations = analysis_config_get_merge_observations( enkf_main->analysis_config );
  int_vector_type * step_list = int_vector_alloc( 0 , 0 );
  
  if (merge_observations) {
    for (int step = util_int_max( 1 , load_start ); step <= step2; step++)
      int_vector_append( step_list , step );
  } else
    int_vector_append( step_list , step2 );
  
  return step_list;
}




void * enkf_main_get_enkf_config_node_type(const ensemble_config_type * ensemble_config, const char * key){
  enkf_config_node_type * config_node_type = ensemble_config_get_node(ensemble_config, key);
  return enkf_config_node_get_ref(config_node_type);
}

/**
   This function will initialize the necessary enkf_main structures
   before a run. Currently this means:

     1. Set the enkf_sched instance - either by loading from file or
        by using the default.

     2. Set up the configuration of what should be internalized.

*/


void enkf_main_init_run( enkf_main_type * enkf_main, run_mode_type run_mode) {
  const ext_joblist_type * joblist = site_config_get_installed_jobs( enkf_main->site_config);

  model_config_set_enkf_sched( enkf_main->model_config , joblist , run_mode );
  enkf_main_init_internalization(enkf_main , run_mode);
}


/**
   The main RUN function - will run both enkf assimilations and experiments.
*/
bool enkf_main_run(enkf_main_type * enkf_main            ,
                   run_mode_type    run_mode             ,
                   const bool     * iactive              ,
                   int              init_step_parameters ,
                   int              start_report         ,
                   state_enum       start_state) {

  bool rerun       = analysis_config_get_rerun( enkf_main->analysis_config );
  int  rerun_start = analysis_config_get_rerun_start( enkf_main->analysis_config );

  enkf_main_init_run( enkf_main , run_mode);
  {
    enkf_fs_type * fs = enkf_main_get_fs(enkf_main);
    if (run_mode == ENKF_ASSIMILATION) {
      if (enkf_fs_rw_equal(fs)) {
        bool analyzed_start = false;
        bool prev_enkf_on;
        const enkf_sched_type * enkf_sched = model_config_get_enkf_sched(enkf_main->model_config);
        const int num_nodes                = enkf_sched_get_num_nodes(enkf_sched);
        const int start_inode              = enkf_sched_get_node_index(enkf_sched , start_report);
        int inode;

        if (start_state == ANALYZED)
          analyzed_start = true;
        else if (start_state == FORECAST)
          analyzed_start = false;
        else
          util_abort("%s: internal error - start_state must be analyzed | forecast \n",__func__);

        prev_enkf_on = analyzed_start;
        for (inode = start_inode; inode < num_nodes; inode++) {
          const enkf_sched_node_type * node = enkf_sched_iget_node(enkf_sched , inode);
          state_enum init_state_parameter;
          state_enum init_state_dynamic;
          int      init_step_parameter;
          int      load_start;
          int      report_step1;
          int      report_step2;
          bool enkf_on;

          enkf_sched_node_get_data(node , &report_step1 , &report_step2 , &enkf_on );
          if (inode == start_inode)
            report_step1 = start_report;  /* If we are restarting from somewhere. */

          if (rerun) {
            /* rerun ... */
            load_start           = report_step1;    /* +1 below. Observe that report_step is set to rerun_start below. */
            init_step_parameter  = report_step1;
            init_state_dynamic   = FORECAST;
            init_state_parameter = ANALYZED;
            report_step1         = rerun_start;
          } else {
            if (prev_enkf_on)
              init_state_dynamic = ANALYZED;
            else
              init_state_dynamic = FORECAST;
            /*
               This is not a rerun - and then parameters and dynamic
               data should be initialized from the same report step.
            */
            init_step_parameter  = report_step1;
            init_state_parameter = init_state_dynamic;
            load_start = report_step1;
          }

          if (load_start > 0)
            load_start++;

          {
            bool runOK = enkf_main_run_step(enkf_main , ENKF_ASSIMILATION , iactive , load_start , init_step_parameter , 
                                            init_state_parameter , init_state_dynamic , report_step1 , report_step2 , enkf_on);
            if (!runOK)
              return false;   /* If the run did not succed we just return false. */
          }
          prev_enkf_on = enkf_on;
        }
      } else {
        fprintf(stderr , "\n** Error: when running EnKF read and write cases must be equal.\n\n");
        fprintf(stderr , "Current read case....: %s \n",enkf_fs_get_read_dir( fs ));
        fprintf(stderr , "Current write case...: %s \n",enkf_fs_get_write_dir( fs ));
      }
    } else {
      /* It is an experiment */
      const enkf_sched_type * enkf_sched = model_config_get_enkf_sched(enkf_main->model_config);
      const int last_report              = enkf_sched_get_last_report(enkf_sched);
      int load_start = start_report;
      state_enum init_state_parameter = start_state;
      state_enum init_state_dynamic   = start_state;
      enkf_main_run_step(enkf_main , run_mode , iactive , load_start , init_step_parameters , init_state_parameter , init_state_dynamic , start_report , last_report , false );
    } 
  }
  return true;
}

/*****************************************************************/
/*  Filesystem copy functions                                    */


void enkf_main_copy_ensemble(enkf_main_type * enkf_main        , 
                             const char * source_case          , 
                             int          source_report_step   ,
                             state_enum   source_state         ,
                             const char * target_case          ,  
                             int          target_report_step   ,
                             state_enum   target_state         , 
                             const bool_vector_type * iens_mask,
                             const char * ranking_key ,    /* It is OK to supply NULL - but if != NULL it must exist */
                             const stringlist_type * node_list) {

  /**
     Must start by setting up the enkf_fs instance to read and write
     from the correct cases.
  */

  const int ens_size            = enkf_main_get_ensemble_size( enkf_main );
  
  {
    /* Store current selections */
    char * user_read_dir  = util_alloc_string_copy(enkf_fs_get_read_dir( enkf_main->dbase));
    char * user_write_dir = util_alloc_string_copy(enkf_fs_get_write_dir(enkf_main->dbase));
    
    enkf_fs_select_write_dir(enkf_main->dbase , target_case, true , true);
    enkf_fs_select_read_dir( enkf_main->dbase , source_case       , true);

    {
      int * ranking_permutation;
      int inode , src_iens;
      
      if (ranking_key != NULL) 
        ranking_permutation = (int *) enkf_main_get_ranking_permutation( enkf_main , ranking_key );
      else {
        ranking_permutation = util_malloc( ens_size * sizeof * ranking_permutation , __func__);
        for (src_iens = 0; src_iens < ens_size; src_iens++)
          ranking_permutation[src_iens] = src_iens;
      }
      
      for (inode =0; inode < stringlist_get_size( node_list ); inode++) {
        enkf_config_node_type * config_node = ensemble_config_get_node( enkf_main->ensemble_config , stringlist_iget( node_list , inode ));
        for (src_iens = 0; src_iens < enkf_main_get_ensemble_size( enkf_main ); src_iens++) {
          if (bool_vector_safe_iget(iens_mask , src_iens)) {
            int target_iens = ranking_permutation[src_iens];
            enkf_fs_copy_node( enkf_main->dbase , config_node , 
                               source_report_step , src_iens    , source_state, 
                               target_report_step , target_iens , target_state );
          }
        }
      }

      if (ranking_permutation == NULL) 
        free( ranking_permutation );
    }
    /* Recover initial selections. */
    enkf_fs_select_write_dir(enkf_main->dbase , user_write_dir, false , true);
    enkf_fs_select_read_dir( enkf_main->dbase , user_read_dir         , true);
    free(user_read_dir);
    free(user_write_dir);
  }
}






/**
   This is based on a general copy function, but a couple of variables
   have been set to default values because this is an initialization:

     target_step  = 0
     target_state = analyzed
   
*/

void enkf_main_initialize_from_existing__(enkf_main_type * enkf_main , 
                                          const char * source_case , 
                                          int          source_report_step,
                                          state_enum   source_state,
                                          const bool_vector_type * iens_mask,
                                          const char * ranking_key ,    /* It is OK to supply NULL - but if != NULL it must exist */
                                          const stringlist_type * node_list) {
  
  const int target_report_step  = 0;
  const state_enum target_state = ANALYZED;
  const char * target_case      = enkf_fs_get_write_dir( enkf_main->dbase );

  enkf_main_copy_ensemble(enkf_main , 
                          source_case , source_report_step , source_state , 
                          target_case , target_report_step , target_state , 
                          iens_mask , ranking_key , node_list);
  
}



/**
   This function will select all the parameter variables in the
   ensmeble, and then call enkf_main_initialize_from_existing__() with
   that list.
*/
void enkf_main_initialize_from_existing(enkf_main_type * enkf_main , 
                                        const char * source_case , 
                                        int          source_report_step,
                                        state_enum   source_state,
                                        const bool_vector_type * iens_mask,
                                        const char  * ranking_key) { 
  stringlist_type * param_list = ensemble_config_alloc_keylist_from_var_type( enkf_main->ensemble_config , PARAMETER ); /* Select only paramters - will fail for GEN_DATA of type DYNAMIC_STATE. */
  enkf_main_initialize_from_existing__(enkf_main , source_case , source_report_step , source_state , iens_mask , ranking_key , param_list );
  stringlist_free( param_list );
}



static void * enkf_main_initialize_from_scratch_mt(void * void_arg) {
  arg_pack_type * arg_pack     = arg_pack_safe_cast( void_arg );
  enkf_main_type * enkf_main   = arg_pack_iget_ptr( arg_pack , 0);
  stringlist_type * param_list = arg_pack_iget_ptr( arg_pack , 1 );
  int iens1                    = arg_pack_iget_int( arg_pack , 2 );
  int iens2                    = arg_pack_iget_int( arg_pack , 3 );
  int iens;
  
  for (iens = iens1; iens < iens2; iens++) {
    enkf_state_type * state = enkf_main_iget_state( enkf_main , iens);
    enkf_state_initialize( state , param_list );
  }

  return NULL;
}


void enkf_main_initialize_from_scratch(enkf_main_type * enkf_main , const stringlist_type * param_list , int iens1 , int iens2) {
  int num_cpu               = 4;
  thread_pool_type * tp     = thread_pool_alloc( num_cpu , true );
  int ens_sub_size          = (iens2 - iens1 + 1) / num_cpu;
  arg_pack_type ** arg_list = util_malloc( num_cpu * sizeof * arg_list , __func__ );
  int i;
  
  printf("Initializing .... "); fflush( stdout );
  for (i = 0; i < num_cpu;  i++) {
    arg_list[i] = arg_pack_alloc();
    arg_pack_append_ptr( arg_list[i] , enkf_main );
    arg_pack_append_ptr( arg_list[i] , param_list );
    {
      int start_iens = i * ens_sub_size;
      int end_iens   = start_iens + ens_sub_size;
      
      if (i == (num_cpu - 1))
        end_iens = iens2 + 1;  /* Input is upper limit inclusive. */

      arg_pack_append_int( arg_list[i] , start_iens );
      arg_pack_append_int( arg_list[i] , end_iens );
    }
    thread_pool_add_job( tp , enkf_main_initialize_from_scratch_mt , arg_list[i]);
  }
  thread_pool_join( tp );
  for (i = 0; i < num_cpu; i++)
    arg_pack_free( arg_list[i] ); 
  free( arg_list );
  thread_pool_free( tp );
  printf("\n");
}


/**
   This function creates a local_config file corresponding to the
   default 'ALL_ACTIVE' configuration. We eat our own dogshit around
   here...
*/

void enkf_main_create_all_active_config( const enkf_main_type * enkf_main , 
                                         const char * local_config_file ) {


  bool single_node_update = analysis_config_get_single_node_update( enkf_main->analysis_config );
  bool update_results     = analysis_config_get_update_results( enkf_main->analysis_config );

  const char * update_step_name = "ALL_ACTIVE";
  const char * ministep_name    = "ALL_ACTIVE";
  const char * obsset_name      = "ALL_OBS";
  const char * dataset_name     = "ALL_DATA";   // <- This is is created for possible further use, even if 
                                                //    single_node_update is true. 
  
  FILE * stream = util_fopen( local_config_file , "w");

  fprintf(stream , "%-32s %s\n", local_config_get_cmd_string( CREATE_UPDATESTEP ) , update_step_name);
  fprintf(stream , "%-32s %s \n", local_config_get_cmd_string( CREATE_OBSSET ) , obsset_name);
  fprintf(stream , "%-32s %s %s \n", local_config_get_cmd_string( CREATE_MINISTEP ) , ministep_name , obsset_name);
  fprintf(stream , "%-32s %s %s \n" , local_config_get_cmd_string( ATTACH_MINISTEP ), update_step_name , ministep_name);

  fprintf(stream , "%-32s %s \n", local_config_get_cmd_string( CREATE_DATASET ) , dataset_name);
  if (!single_node_update) 
    fprintf(stream , "%-32s %s %s \n", local_config_get_cmd_string( ATTACH_DATASET ) , ministep_name , dataset_name);
  
  /* Adding all observation keys */
  {
    hash_iter_type * obs_iter = enkf_obs_alloc_iter( enkf_main->obs );
    while ( !hash_iter_is_complete(obs_iter) ) {
      const char * obs_key = hash_iter_get_next_key( obs_iter );
      fprintf(stream , "%-32s %s %s\n",local_config_get_cmd_string( ADD_OBS ) , obsset_name , obs_key);
    }
    hash_iter_free( obs_iter );
  }
  
  /* Adding all node which can be updated. */
  {
    stringlist_type * keylist = ensemble_config_alloc_keylist_from_var_type( enkf_main->ensemble_config , PARAMETER + DYNAMIC_STATE + DYNAMIC_RESULT);
    int i;
    for (i = 0; i < stringlist_get_size( keylist ); i++) {
      const char * key = stringlist_iget( keylist , i);
      const enkf_config_node_type * config_node = ensemble_config_get_node( enkf_main->ensemble_config , key );
      enkf_var_type var_type = enkf_config_node_get_var_type( config_node );
      bool add_node = true;

      if ((var_type == DYNAMIC_RESULT) && (!update_results))
        add_node = false;

      /*
        Make sure the funny GEN_KW instance masquerading as
        SCHEDULE_PREDICTION_FILE is not added to the soup.
      */
      if (util_string_equal(key , "PRED"))
        add_node = false;

      
      if (add_node) {
        if (single_node_update) {
          fprintf(stream , "%-32s %s \n"    , local_config_get_cmd_string( CREATE_DATASET ) , key);
          fprintf(stream , "%-32s %s %s \n" , local_config_get_cmd_string( ATTACH_DATASET ) , ministep_name , key);
          fprintf(stream , "%-32s %s %s\n"  , local_config_get_cmd_string( ADD_DATA ) , key , key);
        } 
        fprintf(stream , "%-32s %s %s\n",local_config_get_cmd_string( ADD_DATA ) , dataset_name , key);
      }
    }
    stringlist_free( keylist);
  }

  /* Install the ALL_ACTIVE step as the default. */
  fprintf(stream , "%-32s ALL_ACTIVE" , local_config_get_cmd_string( INSTALL_DEFAULT_UPDATESTEP ));
  fclose( stream );
}




static config_type * enkf_main_alloc_config( bool site_only , bool strict ) {
  config_type * config = config_alloc();
  config_item_type * item;

  /*****************************************************************/
  /* config_add_item():                                            */
  /*                                                               */
  /*  1. boolean - required?                                       */
  /*  2. boolean - append?                                         */
  /*****************************************************************/
  
  site_config_add_config_items( config , site_only );
  if (site_only)                                                   
    return config;                                                  /* <---------------- return statement here! */


  
  plot_config_add_config_items( config );
  analysis_config_add_config_items( config );
  ensemble_config_add_config_items(config);
  ecl_config_add_config_items( config , strict );
  rng_config_add_config_items( config );

  /*****************************************************************/
  /* Required keywords from the ordinary model_config file */

  item = config_add_item(config , CASE_TABLE_KEY , false , false);
  config_item_set_argc_minmax(item , 1 , 1 , 1, (const config_item_types [1]) {CONFIG_EXISTING_FILE});

  config_add_key_value( config , LOG_LEVEL_KEY , false , CONFIG_INT);
  config_add_key_value( config , LOG_FILE_KEY  , false , CONFIG_STRING); 

  config_add_key_value(config , MAX_RESAMPLE_KEY , false , CONFIG_INT);
  
  
  item = config_add_item(config , NUM_REALIZATIONS_KEY , true , false);
  config_item_set_argc_minmax(item , 1 , 1 , 1, (const config_item_types [1]) {CONFIG_INT});
  config_add_alias(config , NUM_REALIZATIONS_KEY , "SIZE");
  config_add_alias(config , NUM_REALIZATIONS_KEY , "NUM_REALISATIONS");
  config_install_message(config , "SIZE" , "** Warning: \'SIZE\' is depreceated - use \'NUM_REALIZATIONS\' instead.");


  /*****************************************************************/
  /* Optional keywords from the model config file */

  item = config_add_item( config , RUN_TEMPLATE_KEY , false , true );
  config_item_set_argc_minmax(item , 2 , -1 , 2 , (const config_item_types [2]) { CONFIG_EXISTING_FILE , CONFIG_STRING });  /* Force the template to exist at boot time. */

  config_add_key_value(config , RUNPATH_KEY , false , CONFIG_STRING);

  item = config_add_item(config , ENSPATH_KEY , false , false);
  config_item_set_argc_minmax(item , 1 , 1 , 0 , NULL);

  item = config_add_item(config , SELECT_CASE_KEY , false , false);
  config_item_set_argc_minmax(item , 1 , 1 , 0 , NULL);

  item = config_add_item(config , DBASE_TYPE_KEY , false , false);
  config_item_set_argc_minmax(item , 1, 1 , 0 , NULL);
  config_item_set_common_selection_set(item , 3 , (const char *[3]) {"PLAIN" , "SQLITE" , "BLOCK_FS"});

  item = config_add_item(config , FORWARD_MODEL_KEY , strict , true);
  config_item_set_argc_minmax(item , 1 , -1 , 0 , NULL);

  item = config_add_item(config , DATA_KW_KEY , false , true);
  config_item_set_argc_minmax(item , 2 , 2 , 0 , NULL);

  item = config_add_item(config , KEEP_RUNPATH_KEY , false , false);
  config_item_set_argc_minmax(item , 1 , -1 , 0 , NULL);

  config_add_key_value(config , PRE_CLEAR_RUNPATH_KEY , false , CONFIG_BOOLEAN);

  item = config_add_item(config , DELETE_RUNPATH_KEY , false , false);
  config_item_set_argc_minmax(item , 1 , -1 , 0 , NULL);

  item = config_add_item(config , OBS_CONFIG_KEY  , false , false);
  config_item_set_argc_minmax(item , 1 , 1 , 1 , (const config_item_types [1]) { CONFIG_EXISTING_FILE});

  item = config_add_item(config , RFT_CONFIG_KEY , false , false);
  config_item_set_argc_minmax(item , 1 , 1 , 1 , (const config_item_types [1]) { CONFIG_EXISTING_FILE});

  item = config_add_item(config , RFTPATH_KEY , false , false);
  config_item_set_argc_minmax(item , 1 , 1 , 0 , NULL);

  item = config_add_item(config , LOCAL_CONFIG_KEY  , false , true);
  config_item_set_argc_minmax(item , 1 , 1 , 1 , (const config_item_types [1]) { CONFIG_EXISTING_FILE});

  item = config_add_item(config , ENKF_SCHED_FILE_KEY , false , false);
  config_item_set_argc_minmax(item , 1 , 1 , 1 , (const config_item_types [1]) { CONFIG_EXISTING_FILE});

  item = config_add_item(config , HISTORY_SOURCE_KEY , false , false);
  config_item_set_argc_minmax(item , 1 , 1 , 0 , NULL);
  {
    stringlist_type * refcase_dep = stringlist_alloc_argv_ref( (const char *[1]) {"REFCASE"} , 1);

    config_item_set_common_selection_set(item , 3 , (const char *[3]) {"SCHEDULE" , "REFCASE_SIMULATED" , "REFCASE_HISTORY"});
    config_item_set_required_children_on_value(item , "REFCASE_SIMULATED" , refcase_dep);
    config_item_set_required_children_on_value(item , "REFCASE_HISTORY"  , refcase_dep);

    stringlist_free(refcase_dep);
  }
  return config;
}


keep_runpath_type  enkf_main_iget_keep_runpath( const enkf_main_type * enkf_main , int iens ) {
  return enkf_state_get_keep_runpath( enkf_main->ensemble[iens] );
}

void enkf_main_iset_keep_runpath( enkf_main_type * enkf_main , int iens , keep_runpath_type keep_runpath) {
  enkf_state_set_keep_runpath( enkf_main->ensemble[iens] , keep_runpath);
}

/**
   Observe that this function parses and TEMPORARILY stores the keep_runpath
   information ion the enkf_main object. This is subsequently passed on the
   enkf_state members, and the functions enkf_main_iget_keep_runpath() and
   enkf_main_iset_keep_runpath() act on the enkf_state objects, and not on the
   internal keep_runpath field of the enkf_main object (what a fxxxing mess).
*/


void enkf_main_parse_keep_runpath(enkf_main_type * enkf_main , const char * keep_runpath_string , const char * delete_runpath_string , int ens_size ) {

  int i;
  for (i = 0; i < ens_size; i++)
    int_vector_iset( enkf_main->keep_runpath , i , DEFAULT_KEEP);

  {
    int * active_list; 
    int   num_items;

    active_list = util_sscanf_alloc_active_list(keep_runpath_string , &num_items);
    for (i = 0; i < num_items; i++)
      int_vector_iset( enkf_main->keep_runpath , i , EXPLICIT_KEEP);
    
    
    free( active_list );
  }
  
  
  {
    int * active_list; 
    int   num_items;

    active_list = util_sscanf_alloc_active_list(delete_runpath_string , &num_items);
    for (i = 0; i < num_items; i++) 
      int_vector_iset( enkf_main->keep_runpath , i , EXPLICIT_DELETE);
    
    free( active_list );
  }
}



/**
   There is NO tagging anymore - if the user wants tags - the user
   supplies the key __WITH__ tags.
*/
void enkf_main_add_data_kw(enkf_main_type * enkf_main , const char * key , const char * value) {
  subst_list_append_copy( enkf_main->subst_list   , key , value , "Supplied by the user in the configuration file.");
}


void enkf_main_data_kw_fprintf_config( const enkf_main_type * enkf_main , FILE * stream ) {
  for (int i = 0; i < subst_list_get_size( enkf_main->subst_list ); i++) {
    fprintf(stream , CONFIG_KEY_FORMAT , DATA_KW_KEY );
    fprintf(stream , CONFIG_VALUE_FORMAT    , subst_list_iget_key( enkf_main->subst_list , i ));
    fprintf(stream , CONFIG_ENDVALUE_FORMAT , subst_list_iget_value( enkf_main->subst_list , i ));
  }
}


void enkf_main_clear_data_kw( enkf_main_type * enkf_main ) {
  subst_list_clear( enkf_main->subst_list );
}



static void enkf_main_init_subst_list( enkf_main_type * enkf_main ) {
  /* Here we add the functions which should be available for string substitution operations. */
  enkf_main->subst_func_pool = subst_func_pool_alloc( enkf_main->rng );
  subst_func_pool_add_func( enkf_main->subst_func_pool , "EXP"       , "exp"                               , subst_func_exp         , false , 1 , 1 , NULL);
  subst_func_pool_add_func( enkf_main->subst_func_pool , "LOG"       , "log"                               , subst_func_log         , false , 1 , 1 , NULL);
  subst_func_pool_add_func( enkf_main->subst_func_pool , "POW10"     , "Calculates 10^x"                   , subst_func_pow10       , false , 1 , 1 , NULL);
  subst_func_pool_add_func( enkf_main->subst_func_pool , "ADD"       , "Adds arguments"                    , subst_func_add         , true  , 1 , 0 , NULL);
  subst_func_pool_add_func( enkf_main->subst_func_pool , "MUL"       , "Multiplies arguments"              , subst_func_mul         , true  , 1 , 0 , NULL);
  subst_func_pool_add_func( enkf_main->subst_func_pool , "RANDINT"   , "Returns a random integer - 32 bit" , subst_func_randint     , false , 0 , 0 , enkf_main->rng);
  subst_func_pool_add_func( enkf_main->subst_func_pool , "RANDFLOAT" , "Returns a random float 0-1."       , subst_func_randfloat   , false , 0 , 0 , enkf_main->rng);

  /**
     Allocating the parent subst_list instance. This will (should ...)
     be the top level subst instance for all substitions in the ert
     program.

     All the functions available or only installed in this
     subst_list.

     The key->value replacements installed in this instance are
     key,value pairs which are:

      o Common to all ensemble members.

      o Constant in time.
  */

  enkf_main->subst_list = subst_list_alloc( enkf_main->subst_func_pool );
  /* Installing the functions. */
  subst_list_insert_func( enkf_main->subst_list , "EXP"         , "__EXP__");
  subst_list_insert_func( enkf_main->subst_list , "LOG"         , "__LOG__");
  subst_list_insert_func( enkf_main->subst_list , "POW10"       , "__POW10__");
  subst_list_insert_func( enkf_main->subst_list , "ADD"         , "__ADD__");
  subst_list_insert_func( enkf_main->subst_list , "MUL"         , "__MUL__");
  subst_list_insert_func( enkf_main->subst_list , "RANDINT"     , "__RANDINT__");
  subst_list_insert_func( enkf_main->subst_list , "RANDFLOAT"   , "__RANDFLOAT__");
}



static enkf_main_type * enkf_main_alloc_empty( void ) {
  enkf_main_type * enkf_main = util_malloc(sizeof * enkf_main, __func__);
  UTIL_TYPE_ID_INIT(enkf_main , ENKF_MAIN_ID);
  enkf_main->dbase              = NULL;
  enkf_main->ensemble           = NULL;
  enkf_main->user_config_file   = NULL;
  enkf_main->site_config_file   = NULL;
  enkf_main->rft_config_file    = NULL;
  enkf_main->ens_size           = 0;
  enkf_main->keep_runpath       = int_vector_alloc( 0 , DEFAULT_KEEP );
  enkf_main->logh               = log_alloc_existing( NULL , DEFAULT_LOG_LEVEL );
  enkf_main->rng_config         = rng_config_alloc( );
  

  enkf_main->site_config      = site_config_alloc_empty();
  enkf_main->ensemble_config  = ensemble_config_alloc_empty();
  enkf_main->ecl_config       = ecl_config_alloc_empty();
  enkf_main->model_config     = model_config_alloc_empty();
  enkf_main->analysis_config  = analysis_config_alloc_default( enkf_main->rng );   /* This is ready for use. */
  enkf_main->plot_config      = plot_config_alloc_default();       /* This is ready for use. */
  return enkf_main;
}



static void enkf_main_install_data_kw( enkf_main_type * enkf_main , hash_type * config_data_kw) {
  /*
    Installing the DATA_KW keywords supplied by the user - these are
    at the very top level, so they can reuse everything defined later.
  */
  {
    hash_iter_type * iter = hash_iter_alloc(config_data_kw);
    const char * key = hash_iter_get_next_key(iter);
    while (key != NULL) {
      enkf_main_add_data_kw( enkf_main , key , hash_get( config_data_kw , key ));
      key = hash_iter_get_next_key(iter);
    }
    hash_iter_free(iter);
  }



  /*
     Installing the based (key,value) pairs which are common to all
     ensemble members, and independent of time.
  */
  {
    char * cwd             = util_alloc_cwd();
    char * date_string     = util_alloc_date_stamp();

    char * cwd_key         = util_alloc_sprintf( INTERNAL_DATA_KW_TAG_FORMAT , "CWD" );
    char * config_path_key = util_alloc_sprintf( INTERNAL_DATA_KW_TAG_FORMAT , "CONFIG_PATH" );
    char * date_key        = util_alloc_sprintf( INTERNAL_DATA_KW_TAG_FORMAT , "DATE" );
    char * num_cpu_key     = util_alloc_sprintf( INTERNAL_DATA_KW_TAG_FORMAT , "NUM_CPU" );
    char * num_cpu_string  = "1";
    

    subst_list_append_owned_ref( enkf_main->subst_list , cwd_key         , cwd , "The current working directory we are running from - the location of the config file.");
    subst_list_append_ref( enkf_main->subst_list , config_path_key , cwd , "The current working directory we are running from - the location of the config file.");
    subst_list_append_owned_ref( enkf_main->subst_list , date_key        , date_string , "The current date");
    subst_list_append_ref( enkf_main->subst_list , num_cpu_key     , num_cpu_string , "The number of CPU used for one forward model.");
    
    
    free( num_cpu_key );
    free( cwd_key );
    free( config_path_key );
    free( date_key );
  }
}



/**
   This function will resize the enkf_main->ensemble vector,
   allocating or freeing enkf_state instances as needed.
*/


void enkf_main_resize_ensemble( enkf_main_type * enkf_main , int new_ens_size ) {
  int iens;

  /* No change */
  if (new_ens_size == enkf_main->ens_size)
    return ;

  /* Tell the site_config object (i.e. the queue drivers) about the new ensemble size: */
  site_config_set_ens_size( enkf_main->site_config , new_ens_size );
  

  /* The ensemble is shrinking. */
  if (new_ens_size < enkf_main->ens_size) {
    /*1: Free all ensemble members which go out of scope. */
    for (iens = new_ens_size; iens < enkf_main->ens_size; iens++)
      enkf_state_free( enkf_main->ensemble[iens] );
    
    /*2: Shrink the ensemble pointer. */
    enkf_main->ensemble = util_realloc(enkf_main->ensemble , new_ens_size * sizeof * enkf_main->ensemble , __func__);
    enkf_main->ens_size = new_ens_size;
    return;
  }
  
  
  /* The ensemble is expanding */
  if (new_ens_size > enkf_main->ens_size) {
    /*1: Grow the ensemble pointer. */
    enkf_main->ensemble = util_realloc(enkf_main->ensemble , new_ens_size * sizeof * enkf_main->ensemble , __func__);

    /*2: Allocate the new ensemble members. */
    for (iens = enkf_main->ens_size; iens < new_ens_size; iens++) 

      /* Observe that due to the initialization of the rng - this function is currently NOT thread safe. */
      enkf_main->ensemble[iens] = enkf_state_alloc(iens,
                                                   enkf_main->rng , 
                                                   enkf_main->dbase ,
                                                   model_config_iget_casename( enkf_main->model_config , iens ) ,
                                                   enkf_main->pre_clear_runpath                                 ,
                                                   int_vector_safe_iget( enkf_main->keep_runpath , iens)        , 
                                                   enkf_main->model_config                                      ,
                                                   enkf_main->ensemble_config                                   ,
                                                   enkf_main->site_config                                       ,
                                                   enkf_main->ecl_config                                        ,
                                                   enkf_main->logh                                              ,
                                                   enkf_main->templates                                         ,
                                                   enkf_main->subst_list);
    enkf_main->ens_size = new_ens_size;
    return;
  }
  
  util_abort("%s: something is seriously broken - should NOT be here .. \n",__func__);
}





void enkf_main_update_node( enkf_main_type * enkf_main , const char * key ) {
  int iens;
  for (iens = 0; iens < enkf_main->ens_size; iens++) 
    enkf_state_update_node( enkf_main->ensemble[iens] , key );
}





/**
   When the case has changed it is essential to invalidate the meta
   information in the enkf_nodes, otherwise the nodes might reuse old
   data (from a previous case).
*/

static void enkf_main_invalidate_cache( enkf_main_type * enkf_main ) {
  int ens_size = enkf_main_get_ensemble_size( enkf_main );
  int iens;
  for (iens = 0; iens < ens_size; iens++)
    enkf_state_invalidate_cache( enkf_main->ensemble[iens] );
}


void enkf_main_select_case( enkf_main_type * enkf_main , const char * select_case) {
  enkf_fs_select_read_dir( enkf_main->dbase , select_case , true );
  enkf_fs_select_write_dir( enkf_main->dbase , select_case , false , true);
  model_config_set_select_case( enkf_main->model_config , select_case);
}




/**
   This is (probably) not reentrant ...
*/

static void enkf_main_remount_fs( enkf_main_type * enkf_main , const char * select_case ) {
  const model_config_type * model_config = enkf_main->model_config;
  enkf_main->dbase = enkf_fs_mount(model_config_get_enspath(model_config ) , model_config_get_dbase_type( model_config ) , ENKF_MOUNT_MAP , select_case , true , false);
  {
    char * case_key = enkf_util_alloc_tagged_string( "SELECTED_CASE" );
    subst_list_append_ref( enkf_main->subst_list , case_key , enkf_fs_get_read_dir( enkf_main->dbase ) , "The case currently selected.");
    free( case_key );
  }
  
  if (enkf_main->ensemble != NULL)
    enkf_main_invalidate_cache( enkf_main );

  model_config_set_select_case( enkf_main->model_config , enkf_fs_get_read_dir( enkf_main->dbase ));
}


enkf_fs_type * enkf_main_mount_extra_fs( const enkf_main_type * enkf_main , const char * select_case ) {
  const model_config_type * model_config = enkf_main->model_config;
  enkf_fs_type * fs = enkf_fs_mount(model_config_get_enspath(model_config ) , model_config_get_dbase_type( model_config ) , ENKF_MOUNT_MAP , select_case , false , true );
  return fs;
}



/******************************************************************/

/**
   SCHEDULE_PREDICTION_FILE.
   
   The SCHEDULE_PREDICTION_FILE is implemented as a GEN_KW instance,
   with some twists. Observe the following:
   
   1. The SCHEDULE_PREDICTION_FILE is added to the ensemble_config
      as a GEN_KW node with key 'PRED'.
   
   2. The target file is set equal to the initial prediction file
      (i.e. the template in this case), NOT including any path
      components.

*/


void enkf_main_set_schedule_prediction_file__( enkf_main_type * enkf_main , const char * template_file , const char * parameters , const char * min_std , const char * init_file_fmt) {
  const char * key = "PRED";
  /*
    First remove/delete existing PRED node if it is already installed.
  */
  if (ensemble_config_has_key( enkf_main->ensemble_config , key))
    enkf_main_del_node( enkf_main , key );

  if (template_file != NULL) {
    char * target_file;
    enkf_config_node_type * config_node = ensemble_config_add_gen_kw( enkf_main->ensemble_config , key );                                                
    {
      char * base;
      char * ext;
      util_alloc_file_components( template_file , NULL , &base , &ext);
      target_file = util_alloc_filename(NULL , base , ext );
      util_safe_free( base );
      util_safe_free( ext );
    }
    enkf_config_node_update_gen_kw( config_node , target_file , template_file , parameters , min_std , init_file_fmt );
    free( target_file );
    ecl_config_set_schedule_prediction_file( enkf_main->ecl_config , template_file );
  }
}


void enkf_main_set_schedule_prediction_file( enkf_main_type * enkf_main , const char * schedule_prediction_file) {
  enkf_main_set_schedule_prediction_file__(enkf_main , schedule_prediction_file , NULL , NULL , NULL );
}


const char * enkf_main_get_schedule_prediction_file( const enkf_main_type * enkf_main ) {
  return ecl_config_get_schedule_prediction_file( enkf_main->ecl_config );
}


/*
   Adding inverse observation keys to the enkf_nodes; can be called
   several times.
*/


void enkf_main_update_obs_keys( enkf_main_type * enkf_main ) {
  /* First clear all existing observation keys. */
  ensemble_config_clear_obs_keys( enkf_main->ensemble_config );

  /* Add new observation keys. */
  {
    hash_type      * map  = enkf_obs_alloc_data_map(enkf_main->obs);
    hash_iter_type * iter = hash_iter_alloc(map);
    const char * obs_key  = hash_iter_get_next_key(iter);
    while (obs_key  != NULL) {
      const char * state_kw = hash_get(map , obs_key);
      ensemble_config_add_obs_key(enkf_main->ensemble_config , state_kw , obs_key);
      obs_key = hash_iter_get_next_key(iter);
    }
    hash_iter_free(iter);
    hash_free(map);
  }
}

/*****************************************************************/


void enkf_main_set_log_file( enkf_main_type * enkf_main , const char * log_file ) {
  log_reset_filename( enkf_main->logh , log_file);
}


const char * enkf_main_get_log_file( const enkf_main_type * enkf_main ) {
  return log_get_filename( enkf_main->logh );
}


void enkf_main_set_log_level( enkf_main_type * enkf_main , int log_level ) {
  log_set_level( enkf_main->logh , log_level);
}


int enkf_main_get_log_level( const enkf_main_type * enkf_main ) {
  return log_get_level( enkf_main->logh );
}


static void enkf_main_init_log( enkf_main_type * enkf_main , const config_type * config ) {
  if (config_item_set( config , LOG_LEVEL_KEY))
    enkf_main_set_log_level( enkf_main , config_get_value_as_int(config , LOG_LEVEL_KEY));
  
  if (config_item_set( config , LOG_FILE_KEY))
    enkf_main_set_log_file( enkf_main , config_get_value(config , LOG_FILE_KEY));
  else {
    char * log_file = util_alloc_filename(NULL , enkf_main->user_config_file , DEFAULT_LOG_FILE);
    enkf_main_set_log_file( enkf_main , log_file );
    free( log_file );
  }
  
  printf("Activity will be logged to ..............: %s \n",log_get_filename( enkf_main->logh ));
  log_add_message(enkf_main->logh , 1 , NULL , "ert configuration loaded" , false);
}

static void enkf_main_init_data_kw( enkf_main_type * enkf_main , config_type * config ) {
  hash_type      * data_kw   = config_alloc_hash(config , DATA_KW_KEY);
  enkf_main_install_data_kw( enkf_main , data_kw );
  hash_free( data_kw );
}

    

void enkf_main_gen_data_special( enkf_main_type * enkf_main ) {
  stringlist_type * gen_data_keys = ensemble_config_alloc_keylist_from_impl_type( enkf_main->ensemble_config , GEN_DATA);
  for (int i=0; i < stringlist_get_size( gen_data_keys ); i++) {
    enkf_config_node_type * config_node = ensemble_config_get_node( enkf_main->ensemble_config , stringlist_iget( gen_data_keys , i));
    enkf_var_type var_type = enkf_config_node_get_var_type(config_node);
    if ((var_type == DYNAMIC_STATE) || (var_type == DYNAMIC_RESULT)) {
      gen_data_config_type * gen_data_config = enkf_config_node_get_ref( config_node );
      gen_data_config_set_dynamic( gen_data_config , enkf_main->dbase );
      gen_data_config_set_ens_size( gen_data_config , enkf_main->ens_size );
    }
  }
  stringlist_free( gen_data_keys );
}


/*****************************************************************/


void enkf_main_rng_init( enkf_main_type * enkf_main) {
  const char * seed_load  = rng_config_get_seed_load_file( enkf_main->rng_config );
  const char * seed_store = rng_config_get_seed_store_file( enkf_main->rng_config );
  enkf_main->rng = rng_alloc( rng_config_get_type(enkf_main->rng_config) , INIT_DEFAULT);
  
  if (seed_load != NULL) {
    FILE * stream = util_fopen( seed_load , "r");
    rng_fscanf_state( enkf_main->rng , stream );
    fclose( stream );
  } else
    rng_init( enkf_main->rng , INIT_DEV_RANDOM );
  

  if (seed_store != NULL) {
    FILE * stream = util_mkdir_fopen( seed_store , "w");
    rng_fprintf_state( enkf_main->rng , stream );
    fclose( stream );
  }
}



/**
   Observe that the site-config initializations starts with chdir() to
   the location of the site_config_file; this ensures that the
   site_config can contain relative paths to job description files and
   scripts.
*/


static void enkf_main_bootstrap_site(enkf_main_type * enkf_main , const char * site_config_file , bool strict) {
  char * cwd = util_alloc_cwd();
  {

    {
      char * site_config_path;
      util_alloc_file_components( site_config_file , &site_config_path , NULL , NULL );
      if (site_config_path != NULL) {
        if (chdir( site_config_path ) != 0) 
          util_abort("s: holy diver - could not chdir() to directory:%s containing the site configuration. \n",__func__ , site_config_path);
      }
      util_safe_free( site_config_path );
    }
    
    {
      config_type * config = enkf_main_alloc_config( true , strict );
      config_parse(config , site_config_file  , "--" , INCLUDE_KEY , DEFINE_KEY , false , true);
      site_config_init( enkf_main->site_config , config , false);                                /*  <---- site_config : first pass. */  
      config_free( config );
    }

  }
  chdir( cwd );
  free( cwd );
}


/**
   This function boots everything needed for running a EnKF
   application. Very briefly it can be summarized as follows:

    1. A large config object is initalized with all the possible
       keywords we are looking for.

    2. All the config files are parsed in one go.

    3. The various objects are build up by reading from the config
       object.

    4. The resulting enkf_main object contains *EVERYTHING*
       (whoaha...)


  Observe that the function will start with chdir() to the directory
  containing the configuration file, so that all subsequent file
  references are relative to the location of the configuration
  file. This also applies if the command_line argument given is a
  symlink.


  If the parameter @strict is set to false a configuration with some
  missing parameters will validate; this is to support bootstrapping
  from a minimal configuration created by the GUI. The parameters
  which become optional in a non-strict mode are:

    FORWARD_MODEL
    DATA_FILE
    SCHEDULE_FILE
    ECLBASE 

*/



enkf_main_type * enkf_main_bootstrap(const char * _site_config, const char * _model_config, bool strict) {
  const char     * site_config  = getenv("ERT_SITE_CONFIG");
  char           * model_config;
  enkf_main_type * enkf_main;    /* The enkf_main object is allocated when the config parsing is completed. */

  if (site_config == NULL)
    site_config = _site_config;
  
  if (site_config == NULL)
    util_exit("%s: main enkf_config file is not set. Use environment variable \"ERT_SITE_CONFIG\" - or recompile - aborting.\n",__func__);
  
  {

    char * path;
    char * base;
    char * ext;
    if (util_is_link( _model_config )) {   /* The command line argument given is a symlink - we start by changing to */
                                           /* the real location of the configuration file. */
      char  * realpath = util_alloc_link_target( _model_config ); 
      util_alloc_file_components(realpath , &path , &base , &ext);
      free( realpath );
    } else 
      util_alloc_file_components(_model_config , &path , &base , &ext);

    if (path != NULL) {
      if (chdir(path) != 0)
        util_abort("%s: failed to change directory to: %s : %s \n",__func__ , path , strerror(errno));

      printf("Changing to directory ...................: %s \n",path);
      
      if (ext != NULL) 
        model_config = util_alloc_filename( NULL , base , ext );
      else
        model_config = util_alloc_string_copy( base );

    } else
      model_config = util_alloc_string_copy(_model_config);
    
    util_safe_free( path );
    util_safe_free( base );
    util_safe_free( ext );
  }


  if (!util_file_exists(site_config))  util_exit("%s: can not locate site configuration file:%s \n",__func__ , site_config);
  if (!util_file_exists(model_config)) util_exit("%s: can not locate user configuration file:%s \n",__func__ , model_config);
  {
    enkf_main            = enkf_main_alloc_empty( );
    config_type * config;
    /* Parsing the site_config file first */
    enkf_main_bootstrap_site( enkf_main , site_config , strict );
    
    
    config = enkf_main_alloc_config( false , strict );
    site_config_init_user_mode( enkf_main->site_config );
    config_parse(config , model_config , "--" , INCLUDE_KEY , DEFINE_KEY , false , true);
    site_config_init( enkf_main->site_config , config , true );                                   /*  <---- model_config : second pass. */ 

    /*****************************************************************/
    /* OK - now we have parsed everything - and we are ready to start
       populating the enkf_main object.
    */


    enkf_main_set_site_config_file( enkf_main , site_config );
    enkf_main_set_user_config_file( enkf_main , model_config );
    enkf_main_init_log( enkf_main , config );
    /*
      Initializing the various 'large' sub config objects. 
    */
    rng_config_init( enkf_main->rng_config , config );
    enkf_main_rng_init( enkf_main );  /* Must be called before the ensmeble is created. */
    enkf_main_init_subst_list( enkf_main );
    enkf_main_init_data_kw( enkf_main , config );
    
    analysis_config_load_internal_modules( enkf_main->analysis_config , enkf_main->rng );
    analysis_config_init( enkf_main->analysis_config , config , enkf_main->rng);
    ecl_config_init( enkf_main->ecl_config , config );
    plot_config_init( enkf_main->plot_config , config );
    ensemble_config_init( enkf_main->ensemble_config , config , ecl_config_get_grid( enkf_main->ecl_config ) , ecl_config_get_refcase( enkf_main->ecl_config) );
    model_config_init( enkf_main->model_config , 
                       config , 
                       enkf_main_get_ensemble_size( enkf_main ),
                       site_config_get_installed_jobs(enkf_main->site_config) ,
                       ecl_config_get_last_history_restart( enkf_main->ecl_config ),
                       ecl_config_get_sched_file(enkf_main->ecl_config) ,
                       ecl_config_get_refcase( enkf_main->ecl_config ));
    enkf_main_update_num_cpu( enkf_main );
    {
      if (config_item_set( config , SCHEDULE_PREDICTION_FILE_KEY)) {
        stringlist_type * tokens = config_iget_stringlist_ref(config , SCHEDULE_PREDICTION_FILE_KEY , 0);
        const char * template_file = stringlist_iget(tokens , 0);
        {
          hash_type * opt_hash                = hash_alloc_from_options( tokens );
          
          const char * parameters = hash_safe_get( opt_hash , "PARAMETERS" );
          const char * min_std    = hash_safe_get( opt_hash , "MIN_STD"    );
          const char * init_files = hash_safe_get( opt_hash , "INIT_FILES" );  
          
          enkf_main_set_schedule_prediction_file__( enkf_main , template_file , parameters , min_std , init_files );
          hash_free( opt_hash );
        }
      }
    }
    
    
    /*****************************************************************/
    /**
       To keep or not to keep the runpath directories? The problem is
       that the default behavior is different depending on the run_mode:

       enkf_mode: In this case the default behaviour is to delete the
       runpath directories. You can explicitly say that you want to
       keep runpath directories with the KEEP_RUNPATH
       directive.

       experiments: In this case the default is to keep the runpath
       directories around, but you can explicitly say that you
       want to remove the directories by using the DELETE_RUNPATH
       option.

       The final decision is performed in enkf_state().
    */
    {
      {
        char * keep_runpath_string   = NULL;
        char * delete_runpath_string = NULL;
        int    ens_size              = config_get_value_as_int(config , NUM_REALIZATIONS_KEY);
        
        if (config_has_set_item(config , KEEP_RUNPATH_KEY))
          keep_runpath_string = config_alloc_joined_string(config , KEEP_RUNPATH_KEY , "");

        if (config_has_set_item(config , DELETE_RUNPATH_KEY))
          delete_runpath_string = config_alloc_joined_string(config , DELETE_RUNPATH_KEY , "");

        enkf_main_parse_keep_runpath( enkf_main , keep_runpath_string , delete_runpath_string , ens_size );

        util_safe_free( keep_runpath_string   );
        util_safe_free( delete_runpath_string );
      }
      /* This is really in the wrong place ... */
      {
        enkf_main->pre_clear_runpath = DEFAULT_PRE_CLEAR_RUNPATH;
        if (config_has_set_item(config , PRE_CLEAR_RUNPATH_KEY))
          enkf_main->pre_clear_runpath = config_get_value_as_bool( config , PRE_CLEAR_RUNPATH_KEY);
      }




      if (config_has_set_item(config , STATIC_KW_KEY)) {
        for (int i=0; i < config_get_occurences(config , STATIC_KW_KEY); i++) {
          const stringlist_type * static_kw_list = config_iget_stringlist_ref(config , STATIC_KW_KEY , i);
          int k;
          for (k = 0; k < stringlist_get_size(static_kw_list); k++)
            ecl_config_add_static_kw(enkf_main->ecl_config , stringlist_iget( static_kw_list , k));
        }
      }
      
      
      /* Installing templates */
      {
        enkf_main->templates       = ert_templates_alloc( enkf_main->subst_list );
        for (int i=0; i < config_get_occurences( config , RUN_TEMPLATE_KEY); i++) {
          const char * template_file = config_iget( config , RUN_TEMPLATE_KEY , i , 0);
          const char * target_file   = config_iget( config , RUN_TEMPLATE_KEY , i , 1);
          ert_template_type * template = ert_templates_add_template( enkf_main->templates , NULL , template_file , target_file , NULL);
          
          for (int iarg = 2; iarg < config_get_occurence_size( config , RUN_TEMPLATE_KEY , i); iarg++) {
            char * key , *value;
            util_binary_split_string( config_iget( config , RUN_TEMPLATE_KEY , i , iarg ), "=:" , true , &key , &value);
            
            if (value != NULL) 
              ert_template_add_arg( template ,key , value );
            else
              fprintf(stderr,"** Warning - failed to parse argument:%s as key:value - ignored \n",config_iget( config , "RUN_TEMPLATE" , i , iarg ));

            free( key );
            util_safe_free( value );
          }
        }
      }


      {
        const char * obs_config_file;
        if (config_has_set_item(config , OBS_CONFIG_KEY))
          obs_config_file = config_iget(config  , OBS_CONFIG_KEY , 0,0);
        else
          obs_config_file = NULL;

        enkf_main->obs = enkf_obs_alloc( model_config_get_history(enkf_main->model_config), analysis_config_get_std_cutoff(enkf_main->analysis_config) );
        enkf_main_load_obs( enkf_main , obs_config_file );
      }

      enkf_main_update_obs_keys(enkf_main);

      {
        const char * rft_config_file = NULL;
        if (config_has_set_item(config , RFT_CONFIG_KEY))
          rft_config_file = config_iget(config , RFT_CONFIG_KEY , 0,0);

        enkf_main_set_rft_config_file( enkf_main , rft_config_file ); 
      }
      

      /*****************************************************************/
      {
        const char * select_case = NULL;
        if (config_item_set( config , SELECT_CASE_KEY))
          select_case = config_get_value( config , SELECT_CASE_KEY );
        
        enkf_main_remount_fs( enkf_main , select_case );
      }

      /* Adding ensemble members */
      enkf_main_resize_ensemble( enkf_main  , config_iget_as_int(config , NUM_REALIZATIONS_KEY , 0 , 0) );
        
      /*****************************************************************/
      /*
         Installing the local_config object. Observe that the
         ALL_ACTIVE local_config configuration is ALWAYS loaded. But
         if you have created a personal local config that will be
         loaded on top.
      */

      {
        enkf_main->local_config  = local_config_alloc( /*enkf_main->ensemble_config , enkf_main->enkf_obs , */ model_config_get_last_history_restart( enkf_main->model_config ));
        
        /* First create the default ALL_ACTIVE configuration. */
        {
          char * all_active_config_file = util_alloc_tmp_file("/tmp" , "enkf_local_config" , true);
          enkf_main_create_all_active_config( enkf_main , 
                                              all_active_config_file );
          
          /* Install custom local_config - if present.*/
          {
            int i;
            for (i = 0; i < config_get_occurences( config , LOCAL_CONFIG_KEY); i++) {
              const stringlist_type * files = config_iget_stringlist_ref(config , LOCAL_CONFIG_KEY , i);
              for (int j=0; j < stringlist_get_size( files ); j++)
                local_config_add_config_file( enkf_main->local_config , stringlist_iget( files , j) );
            }
          }
          
          /**
             This is where the local configuration files are actually parsed. 
          */
          local_config_reload( enkf_main->local_config , 
                               ecl_config_get_grid( enkf_main->ecl_config ), 
                               enkf_main->ensemble_config , 
                               enkf_main->obs , 
                               all_active_config_file );

          unlink( all_active_config_file );
          free(all_active_config_file);
        }
      }
    }
    config_free(config);
  }
  enkf_main_gen_data_special( enkf_main );
  free( model_config );
  enkf_main->misfit_table = NULL;
  return enkf_main;
}




/**
   This function creates a minimal configuration file, with a few
   parameters (a bit arbitrary) parameters read from (typically) a GUI
   configuration dialog.

   The set of parameters written by this function is _NOT_ a minimum
   set to generate a valid configuration.
*/

void enkf_main_create_new_config( const char * config_file , const char * storage_path , const char * case_name , const char * dbase_type , int num_realizations) {
  
  FILE * stream = util_mkdir_fopen( config_file , "w" );
  
  fprintf(stream , CONFIG_KEY_FORMAT      , ENSPATH_KEY);
  fprintf(stream , CONFIG_ENDVALUE_FORMAT , storage_path );

  fprintf(stream , CONFIG_KEY_FORMAT      , SELECT_CASE_KEY);
  fprintf(stream , CONFIG_ENDVALUE_FORMAT , case_name);

  fprintf(stream , CONFIG_KEY_FORMAT      , DBASE_TYPE_KEY);
  fprintf(stream , CONFIG_ENDVALUE_FORMAT , dbase_type);

  fprintf(stream , CONFIG_KEY_FORMAT      , NUM_REALIZATIONS_KEY);
  fprintf(stream , CONFIG_INT_FORMAT , num_realizations);
  fprintf(stream , "\n");
  
  fclose( stream );

  printf("Have created configuration file: %s \n",config_file );
}


/**
   Sets the misfit_table of the enkf_main object. If a misfit table is
   already installed the currently installed misfit table is freed first.

   The enkf_main object takes ownership of the input misfit table,
   i.e. the calling scope should not free the table.
*/

void enkf_main_set_misfit_table( enkf_main_type * enkf_main , misfit_table_type * misfit) {
  if (enkf_main->misfit_table != NULL)
    misfit_table_free( enkf_main->misfit_table );

  enkf_main->misfit_table = misfit;
}


misfit_table_type * enkf_main_get_misfit_table( const enkf_main_type * enkf_main ) {
  return enkf_main->misfit_table;
}


/**
   o If enkf_main->misfit_table == NULL (i.e. no misfit table has been
     calculated) the ranking key must also be NULL, otherwise it will
     fail hard.

   o ranking_key == NULL the function will return NULL:
   
   o If ranking_key != NULL and NOT an existing ranking key the function will fail hard.

*/

const int * enkf_main_get_ranking_permutation( const enkf_main_type * enkf_main , const char * ranking_key) {
  if (enkf_main->misfit_table == NULL) {
    if (ranking_key != NULL) {
      util_abort("%s: This is a logical error - asking for ranking_key:%s - when no misfit table has been calculated\n",__func__ , ranking_key);
      return NULL;
    } else
      return NULL;
  } else
    return misfit_table_get_ranking_permutation( enkf_main->misfit_table , ranking_key );
}




/**
   First deleting all the nodes - then the configuration.
*/

void enkf_main_del_node(enkf_main_type * enkf_main , const char * key) {
  const int ens_size = enkf_main_get_ensemble_size( enkf_main );
  int iens;
  for (iens = 0; iens < ens_size; iens++)
    enkf_state_del_node(enkf_main->ensemble[iens] , key);
  ensemble_config_del_node(enkf_main->ensemble_config , key);
}



int enkf_main_get_ensemble_size( const enkf_main_type * enkf_main ) {
  return enkf_main->ens_size;
}


enkf_state_type ** enkf_main_get_ensemble( enkf_main_type * enkf_main) {
  return enkf_main->ensemble;
}



/**
   In this function we initialize the variables which control
   which nodes are internalized (i.e. loaded from the forward
   simulation and stored in the enkf_fs 'database'). The system is
   based on two-levels:

   * Should we store the state? This is goverened by the variable
     model_config->internalize_state. If this is true we will
     internalize all nodes which have enkf_var_type = {dynamic_state ,
     static_state}. In the same way the variable
     model_config->internalize_results governs whether the dynamic
     results (i.e. summary variables in ECLIPSE speak) should be
     internalized.

   * In addition we have fine-grained control in the enkf_config_node
     objects where we can explicitly say that, altough we do not want
     to internalize the full state, we want to internalize e.g. the
     pressure field.

   * All decisions on internalization are based on a per report step
     basis.

   The user-space API for manipulating this is (extremely)
   limited. What is implemented here is the following:

     1. We internalize the initial dynamic state.

     2. For all the end-points in the current enkf_sched instance we
        internalize the state.

     3. store_results is set to true for all report steps irrespective
        of run_mode.

     4. We iterate over all the observations, and ensure that the
        observed nodes (i.e. the pressure for an RFT) are internalized
        (irrespective of whether they are of type dynamic_state or
        dynamic_result).

   Observe that this cascade can result in some nodes, i.e. a rate we
   are observing, to be marked for internalization several times -
   that is no problem.

   -----

   For performance reason model_config contains two bool vectors
   __load_state and __load_result; if they are true the state and
   summary are loaded from disk, otherwise no loading is
   performed. This implies that if we do not want to internalize the
   full state but for instance the pressure (i.e. for an RFT) we must
   set the __load_state variable for the actual report step to
   true. For this reason calls enkf_config_node_internalize() must be
   accompanied by calls to model_config_set_load_state|results() -
   this is ensured when using this function to manipulate the
   configuration of internalization.

*/


void enkf_main_init_internalization( enkf_main_type * enkf_main , run_mode_type run_mode ) {
  /* Clearing old internalize flags. */
  model_config_init_internalization( enkf_main->model_config );
  ensemble_config_init_internalization( enkf_main->ensemble_config );

  /* Internalizing the initial state. */
  model_config_set_internalize_state( enkf_main->model_config , 0);

  /* We internalize all the endpoints in the enkf_sched. */
  {
    int inode;
    enkf_sched_type * enkf_sched = model_config_get_enkf_sched(enkf_main->model_config);
    for (inode = 0; inode < enkf_sched_get_num_nodes( enkf_sched ); inode++) {
      const enkf_sched_node_type * node = enkf_sched_iget_node(enkf_sched , inode);
      int report_step2            = enkf_sched_node_get_last_step( node );
      model_config_set_internalize_state( enkf_main->model_config , report_step2);
    }
  }


  /* Make sure we internalize at all observation times.*/
  {
    hash_type      * map  = enkf_obs_alloc_data_map(enkf_main->obs);
    hash_iter_type * iter = hash_iter_alloc(map);
    const char * obs_key  = hash_iter_get_next_key(iter);

    while (obs_key != NULL) {
      obs_vector_type * obs_vector = enkf_obs_get_vector( enkf_main->obs , obs_key );
      enkf_config_node_type * data_node = obs_vector_get_config_node( obs_vector );
      int active_step = -1;
      do {
        active_step = obs_vector_get_next_active_step( obs_vector , active_step );
        if (active_step >= 0) {
          enkf_config_node_set_internalize( data_node , active_step );
          {
            enkf_var_type var_type = enkf_config_node_get_var_type( data_node );
            if (var_type == DYNAMIC_STATE)
              model_config_set_load_state( enkf_main->model_config , active_step);
          }
        }
      } while (active_step >= 0);
      obs_key = hash_iter_get_next_key(iter);
    }
    hash_iter_free(iter);
    hash_free(map);
  }  
}




/*****************************************************************/




/* Used by external application - this is a library ... */
void  enkf_main_list_users(  set_type * users , const char * executable ) {
  DIR * dir = opendir( DEFAULT_VAR_DIR );
  if (dir != NULL) {
    struct dirent * dp;
    do {
      dp = readdir(dir);
      if (dp != NULL) {
        int pid;
        if (util_sscanf_int( dp->d_name , &pid )) {
          char * full_path = util_alloc_filename( DEFAULT_VAR_DIR , dp->d_name , NULL );
          bool add_user    = false;
          int  uid;

          {
            FILE * stream    = util_fopen( full_path , "r");
            char this_executable[512];

            if (fscanf( stream , "%s %d" , this_executable , &uid) == 2) {
              if (executable != NULL) {
                if (util_string_equal( this_executable , executable ))
                  add_user   = true;
              } else
                add_user = true;
            }
            fclose( stream );
          }


          /* Remove the pid files of dead processes. */
          if (!util_proc_alive( pid )) {
            unlink( full_path );
            add_user = false;
          }


          if (add_user) {
            struct passwd *pwd;
            pwd = getpwuid( uid );
            if (pwd != NULL)
              set_add_key( users , pwd->pw_name );
          }


          free( full_path );
        }
      }
    } while (dp != NULL );
    closedir( dir );
  }
}


const ext_joblist_type * enkf_main_get_installed_jobs( const enkf_main_type * enkf_main ) {
  return site_config_get_installed_jobs( enkf_main->site_config );
}

/*****************************************************************/

void enkf_main_get_observations( const enkf_main_type * enkf_main, const char * user_key , int obs_count , time_t * obs_time , double * y , double * std) {
  ensemble_config_get_observations( enkf_main->ensemble_config , enkf_main->obs , user_key , obs_count , obs_time , y , std);
}


int enkf_main_get_observation_count( const enkf_main_type * enkf_main, const char * user_key ) {
  return ensemble_config_get_observations( enkf_main->ensemble_config , enkf_main->obs , user_key , 0 , NULL , NULL , NULL);
}



/**
   This function will go through the filesystem and check that we have
   initial data for all parameters and all realizations. If the second
   argument mask is different from NULL, the function will only
   consider the realizations for which mask is true (if mask == NULL
   all realizations will be checked).
*/

bool enkf_main_is_initialized( const enkf_main_type * enkf_main , bool_vector_type * __mask) {
  stringlist_type  * parameter_keys = ensemble_config_alloc_keylist_from_var_type( enkf_main->ensemble_config , PARAMETER );
  bool_vector_type * mask;
  bool initialized = true;
  int ikey = 0;
  if (__mask != NULL)
    mask = __mask;
  else
    mask = bool_vector_alloc(0 , true );
  
  do {
    const enkf_config_node_type * config_node = ensemble_config_get_node( enkf_main->ensemble_config , stringlist_iget( parameter_keys , ikey) );
    int iens = 0;
    do {
      if (bool_vector_safe_iget( mask , iens)) 
        initialized = enkf_fs_has_node( enkf_main->dbase , config_node , 0 , iens , ANALYZED );
      iens++;
    } while ((iens < enkf_main->ens_size) && (initialized));
    ikey++;
  } while ((ikey < stringlist_get_size( parameter_keys )) && (initialized));
  
  stringlist_free( parameter_keys );
  if (__mask == NULL)
    bool_vector_free( mask );
  return initialized;
}


void enkf_main_log_fprintf_config( const enkf_main_type * enkf_main , FILE * stream ) {
  fprintf( stream , CONFIG_COMMENTLINE_FORMAT );
  fprintf( stream , CONFIG_COMMENT_FORMAT  , "Here comes configuration information about the ERT logging.");
  fprintf( stream , CONFIG_KEY_FORMAT      , LOG_FILE_KEY );
  fprintf( stream , CONFIG_ENDVALUE_FORMAT , enkf_main_get_log_file( enkf_main ));

  if (enkf_main_get_log_level( enkf_main ) != DEFAULT_LOG_LEVEL) {
    fprintf(stream , CONFIG_KEY_FORMAT      , LOG_LEVEL_KEY );
    fprintf(stream , CONFIG_INT_FORMAT , enkf_main_get_log_level( enkf_main ));
    fprintf(stream , "\n");
  }
  
  fprintf(stream , "\n");
  fprintf(stream , "\n");
}


void enkf_main_install_SIGNALS(void) {
  signal(SIGSEGV , util_abort_signal);    /* Segmentation violation, i.e. overwriting memory ... */
  signal(SIGTERM , util_abort_signal);    /* If killing the enkf program with SIGTERM (the default kill signal) you will get a backtrace. 
                                             Killing with SIGKILL (-9) will not give a backtrace.*/
  signal(SIGABRT , util_abort_signal);    /* Signal abort. */ 
}


void enkf_main_init_debug( const char * executable ) {
  char * svn_version      = util_alloc_sprintf("svn version..........: %s \n",SVN_VERSION);
  char * compile_time     = util_alloc_sprintf("Compile time.........: %s \n",COMPILE_TIME_STAMP);

  /* This will be printed if/when util_abort() is called on a later stage. */
  util_abort_append_version_info( svn_version );
  util_abort_append_version_info( compile_time );
  
  free(svn_version);
  free(compile_time);

  if (executable != NULL)
    util_abort_set_executable( executable );
}  


ert_templates_type * enkf_main_get_templates( enkf_main_type * enkf_main ) {
  return enkf_main->templates;
}

void enkf_main_set_case_table( enkf_main_type * enkf_main , const char * case_table_file ) {
  model_config_set_case_table( enkf_main->model_config , enkf_main->ens_size , case_table_file );
}


/*****************************************************************/


void enkf_main_fprintf_runpath_config( const enkf_main_type * enkf_main , FILE * stream ) {
  fprintf(stream , CONFIG_KEY_FORMAT      , PRE_CLEAR_RUNPATH_KEY );
  fprintf(stream , CONFIG_ENDVALUE_FORMAT , CONFIG_BOOL_STRING( enkf_state_get_pre_clear_runpath( enkf_main->ensemble[0] )));
  
  {
    bool keep_comma = false;
    bool del_comma  = false;
    
    
    for (int iens = 0; iens < enkf_main->ens_size; iens++) {
      keep_runpath_type keep_runpath = enkf_main_iget_keep_runpath( enkf_main , iens );
      if (keep_runpath == EXPLICIT_KEEP) {
        if (!keep_comma) {
          fprintf(stream , CONFIG_KEY_FORMAT , KEEP_RUNPATH_KEY );
          fprintf(stream , "%d" , iens);
          keep_comma = true;
        } else 
          fprintf(stream , ",%d" , iens);
      }
    }
    fprintf(stream , "\n");


    for (int iens = 0; iens < enkf_main->ens_size; iens++) {
      keep_runpath_type keep_runpath = enkf_main_iget_keep_runpath( enkf_main , iens );
      if (keep_runpath == EXPLICIT_DELETE) {
        if (!del_comma) {
          fprintf(stream , CONFIG_KEY_FORMAT , DELETE_RUNPATH_KEY );
          fprintf(stream , CONFIG_INT_FORMAT , iens);
          del_comma = true;
        } else {
          fprintf(stream , ",");
          fprintf(stream , CONFIG_INT_FORMAT , iens);
        }
      }
    }
    fprintf(stream , "\n");
  }
}




void enkf_main_fprintf_config( const enkf_main_type * enkf_main ) {
  if (util_file_exists( enkf_main->user_config_file)) {
    /** 
        A version of the config file already exist, and we will take
        backup. 
    */
    char * backup_file = NULL;
    char * prev_backup = NULL;
    int backup_nr      = 1;
    do {
      backup_file = util_realloc_sprintf( backup_file , "%s.%d" , enkf_main->user_config_file , backup_nr);
      if (util_file_exists( backup_file )) {
        prev_backup = util_realloc_string_copy( prev_backup , backup_file );
        backup_nr++;
      }
    } while (util_file_exists( backup_file ));
    
    /**
       When leaving the do { } while loop backup_file will point to
       the first non-existing backup filename; and prev_backup will
       point to the last existing (or be NULL if there was no existing
       backup file).

       1. If prev_backup == NULL there was no previous backup file,
          and we just backup the current file to backup_file and be
          done with it.

       2. If prev_backup != NULL we do the following: The latest
          backup is compared to the current config file, if they are
          equal no new backup is taken; otherwise a new backup is
          stored.

    */
    if (prev_backup == NULL)
      util_copy_file( enkf_main->user_config_file , backup_file );
    else {
      if (!util_files_equal( enkf_main->user_config_file , prev_backup )) 
        util_copy_file( enkf_main->user_config_file , backup_file );
    }
    util_safe_free( prev_backup );
    util_safe_free( backup_file );
  }
  
  /* Start the proper saving */
  {
    FILE * stream = util_fopen( enkf_main->user_config_file , "w");
    
    ecl_config_fprintf_config( enkf_main->ecl_config , stream );
    model_config_fprintf_config( enkf_main->model_config , enkf_main->ens_size , stream );

    enkf_obs_fprintf_config( enkf_main->obs , stream );
    analysis_config_fprintf_config( enkf_main->analysis_config , stream );
    ensemble_config_fprintf_config( enkf_main->ensemble_config , stream );
    local_config_fprintf_config( enkf_main->local_config , stream );
    enkf_main_fprintf_runpath_config( enkf_main , stream );
    ert_templates_fprintf_config( enkf_main->templates , stream );
    enkf_main_log_fprintf_config( enkf_main , stream );
    site_config_fprintf_config( enkf_main->site_config , stream );    
    rng_config_fprintf_config( enkf_main->rng_config , stream );
    fclose( stream );
  }
}
