/*
   Copyright (C) 2011  Statoil ASA, Norway. 
    
   The file 'local_config.h' is part of ERT - Ensemble based Reservoir Tool. 
    
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

#ifndef __LOCAL_CONFIG_H__
#define __LOCAL_CONFIG_H__

#include <local_updatestep.h>
#include <local_ministep.h>
#include <stringlist.h>
#include <ensemble_config.h>
#include <enkf_obs.h>
#include <ecl_grid.h>



#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  INVALID_CMD                     = 0,  /* MArks EOF */
  CREATE_UPDATESTEP               = 1,  /* UPDATESTEP_NAME                   ->     local_config_alloc_updatestep(); */
  CREATE_MINISTEP                 = 2,  /* MINISTEP_NAME  OBSSET_NAME        ->     local_config_alloc_ministep();   */
  ATTACH_MINISTEP                 = 3,  /* UPDATESTEP_NAME , MINISTEP_NAME   ->     local_updatestep_add_ministep(); */
  CREATE_DATASET                  = 4,  /* NAME */
  ATTACH_DATASET                  = 5,  /* DATASET_NAME MINISETP_NAME */
  CREATE_OBSSET                   = 6,  /* NAME */ 
  ADD_DATA                        = 7,  /* DATA_KEY                          ->     local_ministep_add_node();       */
  ADD_OBS                         = 8,  /* OBS_KEY                           ->     local_ministep_add_obs();        */
  ACTIVE_LIST_ADD_OBS_INDEX       = 9,  /* OBS_KEY , ACTIVE_INDEX            */
  ACTIVE_LIST_ADD_DATA_INDEX      = 10, /* DATA_KEY , ACTIVE_INDEX           */
  ACTIVE_LIST_ADD_MANY_OBS_INDEX  = 11,  /* OBS_KEY , NUM_INDEX , INDEX1, INDEX2, INDEX3,... */
  ACTIVE_LIST_ADD_MANY_DATA_INDEX = 12, /* DATA_KEY , NUM_INDEX , INDEX1 , INDEX2 , INDEX3 ,... */  
  INSTALL_UPDATESTEP              = 13, /* UPDATESTEP_NAME , STEP1 , STEP2          local_config_set_updatestep() */
  INSTALL_DEFAULT_UPDATESTEP      = 14, /* UPDATETSTEP_NAME                         local_config_set_default_updatestep() */
  DEL_DATA                        = 16, /* MINISTEP KEY*/
  DEL_OBS                         = 17, /* MINISTEP OBS_KEY */
  DEL_ALL_DATA                    = 18, /* No arguments */
  DEL_ALL_OBS                     = 19, /* No arguments */
  ADD_FIELD                       = 20,  /* MINISTEP  FIELD_NAME  REGION_NAME */
  /*****************************************************************/
  CREATE_REGION                   = 21, /* Name of region  TRUE|FALSE*/
  LOAD_FILE                       = 22, /* Key, filename      */  
  REGION_SELECT_ALL               = 23, /* Region  TRUE|FALSE */
  REGION_SELECT_VALUE_EQUAL       = 24, /* Region FILE_key:kw(:nr) VALUE   TRUE|FALSE */
  REGION_SELECT_VALUE_LESS        = 25, /* Region FILE_key:kw(:nr) VALUE   TRUE|FALSE */  
  REGION_SELECT_VALUE_MORE        = 26, /* Region FILE_key:kw(:nr) VALUE   TRUE|FALSE */  
  REGION_SELECT_BOX               = 27, /* Region i1 i2 j1 j2 k1 k2 TRUE|FALSE */
  REGION_SELECT_SLICE             = 28  /* Region dir n1 n2    TRUE|FALSE  */
} local_config_instruction_type; 




#define CREATE_UPDATESTEP_STRING                "CREATE_UPDATESTEP"
#define CREATE_MINISTEP_STRING                  "CREATE_MINISTEP"
#define ATTACH_MINISTEP_STRING                  "ATTACH_MINISTEP"
#define CREATE_DATASET_STRING                   "CREATE_DATASET"
#define ATTACH_DATASET_STRING                   "ATTACH_DATASET"
#define CREATE_OBSSET_STRING                    "CREATE_OBSSET"
#define ADD_DATA_STRING                         "ADD_DATA"
#define ADD_OBS_STRING                          "ADD_OBS"      
#define ACTIVE_LIST_ADD_OBS_INDEX_STRING        "ACTIVE_LIST_ADD_OBS_INDEX"
#define ACTIVE_LIST_ADD_DATA_INDEX_STRING       "ACTIVE_LIST_ADD_DATA_INDEX"
#define ACTIVE_LIST_ADD_MANY_OBS_INDEX_STRING   "ACTIVE_LIST_ADD_MANY_OBS_INDEX"
#define ACTIVE_LIST_ADD_MANY_DATA_INDEX_STRING  "ACTIVE_LIST_ADD_MANY_DATA_INDEX"
#define INSTALL_UPDATESTEP_STRING               "INSTALL_UPDATESTEP"
#define INSTALL_DEFAULT_UPDATESTEP_STRING       "INSTALL_DEFAULT_UPDATESTEP"
#define DEL_DATA_STRING                         "DEL_DATA"
#define DEL_OBS_STRING                          "DEL_OBS"
#define ADD_FIELD_STRING                        "ADD_FIELD"
#define DEL_ALL_DATA_STRING                     "DEL_ALL_DATA"
#define DEL_ALL_OBS_STRING                      "DEL_ALL_OBS"
#define CREATE_REGION_STRING                    "CREATE_REGION"
#define LOAD_FILE_STRING                        "LOAD_FILE"
#define REGION_SELECT_ALL_STRING                "REGION_SELECT_ALL"   
#define REGION_SELECT_VALUE_EQUAL_STRING        "REGION_SELECT_VALUE_EQUAL"
#define REGION_SELECT_VALUE_LESS_STRING         "REGION_SELECT_VALUE_LESS"
#define REGION_SELECT_VALUE_MORE_STRING         "REGION_SELECT_VALUE_MORE"
#define REGION_SELECT_BOX_STRING                "REGION_SELECT_BOX" 
#define REGION_SELECT_SLICE_STRING              "REGION_SELECT_SLICE" 





typedef struct local_config_struct local_config_type;

local_config_type           * local_config_alloc( int history_length );
void                          local_config_free( local_config_type * local_config );
local_updatestep_type       * local_config_alloc_updatestep( local_config_type * local_config , const char * key );
local_ministep_type         * local_config_alloc_ministep( local_config_type * local_config , const char * key , const char * obsset_name);
local_ministep_type         * local_config_alloc_ministep_copy( local_config_type * local_config , const char * src_key , const char * new_key);
void                          local_config_set_default_updatestep( local_config_type * local_config , const char * default_key);
const local_updatestep_type * local_config_iget_updatestep( const local_config_type * local_config , int index);
local_updatestep_type       * local_config_get_updatestep( const local_config_type * local_config , const char * key);
local_ministep_type         * local_config_get_ministep( const local_config_type * local_config , const char * key);
void                          local_config_set_updatestep(local_config_type * local_config, int step1 , int step2 , const char * key);
void                          local_config_reload( local_config_type * local_config , const ecl_grid_type * ecl_grid , const ensemble_config_type * ensemble_config , const enkf_obs_type * enkf_obs , 
                                                   const char * all_active_config_file);
const char                  * local_config_get_cmd_string( local_config_instruction_type cmd );

stringlist_type             * local_config_get_config_files( const local_config_type * local_config );
void                          local_config_clear_config_files( local_config_type * local_config );
void                          local_config_add_config_file( local_config_type * local_config , const char * config_file );
void                          local_config_fprintf( const local_config_type * local_config , const char * config_file);
void                          local_config_fprintf_config( const local_config_type * local_config , FILE * stream);

#ifdef __cplusplus
}
#endif
#endif