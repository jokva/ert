/*
   Copyright (C) 2011  Statoil ASA, Norway. 
    
   The file 'obs_vector.h' is part of ERT - Ensemble based Reservoir Tool. 
    
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

#ifndef __OBS_VECTOR_H__
#define __OBS_VECTOR_H__

#ifdef __cplusplus 
extern "C" {
#endif

#include <enkf_fs.h>
#include <enkf_types.h>
#include <enkf_node.h>
#include <sched_file.h>
#include <ensemble_config.h>
#include <history.h>
#include <time.h>
#include <obs_data.h>
#include <enkf_macros.h>
#include <conf.h>
#include <active_list.h>
#include <ecl_sum.h>
#include <util.h>

typedef void   (obs_free_ftype)                (void *);
typedef void   (obs_get_ftype)                 (const void * , obs_data_type * , int , const active_list_type * );
typedef void   (obs_meas_ftype)                (const void * , const void *, int , int , meas_data_type * , const active_list_type * );
typedef void   (obs_user_get_ftype)            (void * , const char * , double * , double * , bool *); 
typedef double (obs_chi2_ftype)                (const void * , const void *);

typedef enum { GEN_OBS     = 1,
               SUMMARY_OBS = 2,
               FIELD_OBS   = 3} obs_impl_type;


typedef struct obs_vector_struct obs_vector_type;


void                 obs_vector_clear_nodes( obs_vector_type * obs_vector );
void                 obs_vector_del_node(obs_vector_type * obs_vector , int index);
void                 obs_vector_free(obs_vector_type * );
int                  obs_vector_get_num_active(const obs_vector_type * );
time_t               obs_vector_iget_obs_time( const obs_vector_type * vector , int index);
bool                 obs_vector_iget_active(const obs_vector_type * , int );
void                 obs_vector_iget_observations(const obs_vector_type *  , int  , obs_data_type * , const active_list_type * active_list);
void                 obs_vector_measure(const obs_vector_type *  , int  ,int , const enkf_node_type *  ,  meas_data_type * , const active_list_type * active_list);
const char         * obs_vector_get_state_kw(const obs_vector_type * );
obs_impl_type        obs_vector_get_impl_type(const obs_vector_type * );
int                  obs_vector_get_active_report_step(const obs_vector_type * );
void                 obs_vector_user_get(const obs_vector_type * obs_vector , const char * index_key , int report_step , double * value , double * std , bool * valid);
int                  obs_vector_get_next_active_step(const obs_vector_type * , int );
void               * obs_vector_iget_node(const obs_vector_type * , int );
obs_vector_type    * obs_vector_alloc_from_GENERAL_OBSERVATION(const conf_instance_type *  , const sched_file_type * , const history_type * , const ensemble_config_type * , const time_t_vector_type * obs_time);
void                 obs_vector_load_from_SUMMARY_OBSERVATION(obs_vector_type * obs_vector , const conf_instance_type *  , const sched_file_type * , const history_type * , ensemble_config_type * );
  void                 obs_vector_load_from_HISTORY_OBSERVATION(obs_vector_type * obs_vector , const conf_instance_type *  , const sched_file_type * , const history_type * , ensemble_config_type * , double std_cutoff );
obs_vector_type    * obs_vector_alloc_from_BLOCK_OBSERVATION(const conf_instance_type *    , const sched_file_type * , const history_type *   , const ensemble_config_type * , const time_t_vector_type * obs_time);
void                 obs_vector_set_config_node(obs_vector_type *  , const enkf_config_node_type * );
obs_vector_type    * obs_vector_alloc(obs_impl_type obs_type , const char * obs_key , enkf_config_node_type * config_node , const time_t_vector_type * obs_time , int num_reports);

double                  obs_vector_chi2(const obs_vector_type *  , enkf_fs_type *  , int  , int , state_enum);
void                    obs_vector_ensemble_chi2(const obs_vector_type * obs_vector , enkf_fs_type * fs, int step1 , int step2 , int iens1 , int iens2 , state_enum load_state , double ** chi2);
double                  obs_vector_total_chi2(const obs_vector_type * , enkf_fs_type * , int , state_enum  );
void                    obs_vector_ensemble_total_chi2(const obs_vector_type *  , enkf_fs_type *  , int  , state_enum , double * );
enkf_config_node_type * obs_vector_get_config_node(obs_vector_type * );
const char            * obs_vector_get_obs_key( const obs_vector_type * obs_vector);

UTIL_SAFE_CAST_HEADER(obs_vector);
VOID_FREE_HEADER(obs_vector);


#ifdef __cplusplus 
}
#endif
#endif