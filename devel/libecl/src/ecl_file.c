/*
   Copyright (C) 2011  Statoil ASA, Norway. 
    
   The file 'ecl_file.c' is part of ERT - Ensemble based Reservoir Tool. 
    
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

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fortio.h>
#include <ecl_kw.h>
#include <ecl_file.h>
#include <errno.h>
#include <hash.h>
#include <util.h>
#include <time.h>
#include <vector.h>
#include <int_vector.h>
#include <stringlist.h>
#include <ecl_endian_flip.h>
#include <ecl_kw_magic.h>
#include <ecl_file_kw.h>


/**
   This file implements functionality to load an entire ECLIPSE file
   in ecl_kw format. In addition to loading a complete file it can
   also load a section, by stopping when it meets a certain
   keyword. This latter functionality is suitable for loading parts
   (i.e. one report step) of unified files.

   The ecl_file struct is quite simply a vector of ecl_kw instances,
   it has no knowledge of report steps and such, and does not know
   whether it has been built from a complete file, or only from part
   of a unified file.  
*/



#define ECL_FILE_ID 776107


struct ecl_file_map_struct {
  vector_type       * kw_list;      /* This is a vector of ecl_file_kw instances corresponding to the content of the file. */
  hash_type         * kw_index;     /* A hash table with integer vectors of indices - see comment below. */
  stringlist_type   * distinct_kw;  /* A stringlist of the keywords occuring in the file - each string occurs ONLY ONCE. */         
  fortio_type       * fortio;
  bool                owner;
};


struct ecl_file_struct {
  UTIL_TYPE_ID_DECLARATION;
  fortio_type       * fortio;
  ecl_file_map_type * global_map;
  ecl_file_map_type * active_map;
  vector_type       * map_list;
};


/*
  This illustrates the indexing. The ecl_file instance contains in
  total 7 ecl_kw instances, the global index [0...6] is the internal
  way to access the various keywords. The kw_index is a hash table
  with entries 'SEQHDR', 'MINISTEP' and 'PARAMS'. Each entry in the
  hash table is an integer vector which again contains the internal
  index of the various occurences:
  
   ------------------
   SEQHDR            \
   MINISTEP  0        |     
   PARAMS    .....    |
   MINISTEP  1        |
   PARAMS    .....    |
   MINISTEP  2        |
   PARAMS    .....   /
   ------------------

   kw_index    = {"SEQHDR": [0], "MINISTEP": [1,3,5], "PARAMS": [2,4,6]}    <== This is hash table.
   kw_list     = [SEQHDR , MINISTEP , PARAMS , MINISTEP , PARAMS , MINISTEP , PARAMS]
   distinct_kw = [SEQHDR , MINISTEP , PARAMS]
   
*/

/*****************************************************************/

static ecl_file_map_type * ecl_file_map_alloc( fortio_type * fortio , bool owner ) {
  ecl_file_map_type * file_map = util_malloc( sizeof * file_map ,__func__);
  file_map->kw_list            = vector_alloc_new();
  file_map->kw_index           = hash_alloc();
  file_map->distinct_kw        = stringlist_alloc_new();
  file_map->owner              = owner;
  file_map->fortio             = fortio;
  return file_map;
}

  
ecl_kw_type * ecl_file_map_iget_kw( const ecl_file_map_type * file_map , int index) {
  ecl_file_kw_type * file_kw = vector_iget( file_map->kw_list , index);
  return ecl_file_kw_get_kw( file_kw , file_map->fortio );
}


/**
   This function iterates over the kw_list vector and builds the
   internal index fields 'kw_index' and 'distinct_kw'. This function
   must be called every time the content of the kw_list vector is
   modified (otherwise the ecl_file instance will be in an
   inconsistent state).  
*/


static void ecl_file_map_make_index( ecl_file_map_type * file_map ) {
  stringlist_clear( file_map->distinct_kw );
  hash_clear( file_map->kw_index );
  {
    int i;
    for (i=0; i < vector_get_size( file_map->kw_list ); i++) {
      const ecl_file_kw_type * file_kw = vector_iget_const( file_map->kw_list , i);
      const char             * header  = ecl_file_kw_get_header( file_kw );
      if ( !hash_has_key( file_map->kw_index , header )) {
        int_vector_type * index_vector = int_vector_alloc( 0 , -1 );
        hash_insert_hash_owned_ref( file_map->kw_index , header , index_vector , int_vector_free__);
        stringlist_append_copy( file_map->distinct_kw , header);
      }
      
      {
        int_vector_type * index_vector = hash_get( file_map->kw_index , header);
        int_vector_append( index_vector , i);
      }
    }
  }
}

static int ecl_file_map_find_kw_value( const ecl_file_map_type * file_map , const char * kw , const void * value) {
  int global_index = -1;
  if ( ecl_file_map_has_kw( file_map , kw)) {
    const int_vector_type * index_list = hash_get( file_map->kw_index , kw );
    int index = 0;
    while (index < int_vector_size( index_list )) {
      const ecl_kw_type * ecl_kw = ecl_file_map_iget_kw( file_map , int_vector_iget( index_list , index ));
      if (ecl_kw_data_equal( ecl_kw , value )) {
        global_index = int_vector_iget( index_list , index );
        break;
      }
      index++;
    }
  }
  return global_index;
}

const char * ecl_file_map_iget_distinct_kw( const ecl_file_map_type * file_map , int index) {
  return stringlist_iget( file_map->distinct_kw , index);
}

int ecl_file_map_get_num_distinct_kw( const ecl_file_map_type * file_map ) {
  return stringlist_get_size( file_map->distinct_kw );
}

bool ecl_file_map_has_kw( const ecl_file_map_type * file_map, const char * kw) {
  return hash_has_key( file_map->kw_index , kw );
} 

int ecl_file_map_get_size( const ecl_file_map_type * file_map ) {
  return vector_get_size( file_map->kw_list );
}


static void ecl_file_map_add_kw( ecl_file_map_type * file_map , ecl_file_kw_type * file_kw) {
  if (file_map->owner)
    vector_append_owned_ref( file_map->kw_list , file_kw , ecl_file_kw_free__ ); 
  else
    vector_append_ref( file_map->kw_list , file_kw); 
}

static void ecl_file_map_free( ecl_file_map_type * file_map ) {
  hash_free( file_map->kw_index );
  stringlist_free( file_map->distinct_kw );
  vector_free( file_map->kw_list );
  free( file_map );
}

static void ecl_file_map_free__( void * arg ) {
  ecl_file_map_type * file_map = ( ecl_file_map_type * ) arg;
  ecl_file_map_free( file_map );
}

static int ecl_file_map_get_global_index( const ecl_file_map_type * file_map , const char * kw , int ith) {
  const int_vector_type * index_vector = hash_get(file_map->kw_index , kw);
  int global_index = int_vector_iget( index_vector , ith);
  return global_index;
}

ecl_kw_type * ecl_file_map_iget_named_kw( const ecl_file_map_type * file_map , const char * kw, int ith) {
  int global_index = ecl_file_map_get_global_index( file_map , kw , ith);
  ecl_file_kw_type * file_kw = vector_iget( file_map->kw_list , global_index );
  return ecl_file_kw_get_kw( file_kw , file_map->fortio );
}


int ecl_file_map_get_num_named_kw(const ecl_file_map_type * file_map , const char * kw) {
  if (hash_has_key(file_map->kw_index , kw)) {
    const int_vector_type * index_vector = hash_get(file_map->kw_index , kw);
    return int_vector_size( index_vector );
  } else
    return 0;
}

void ecl_file_map_fwrite( const ecl_file_map_type * file_map , fortio_type * target , int offset) {
  int index;
  for (index = offset; index < vector_get_size( file_map->kw_list ); index++)
    ecl_file_kw_fwrite( vector_iget( file_map->kw_list , index ) , file_map->fortio , target);
}


static int ecl_file_map_iget_occurence( const ecl_file_map_type * file_map , int index) {
  const ecl_file_kw_type * file_kw = vector_iget_const( file_map->kw_list , index);
  const char * header              = ecl_file_kw_get_header( file_kw );
  const int_vector_type * index_vector = hash_get( file_map->kw_index , header );
  const int * index_data = int_vector_get_const_ptr( index_vector );
  
  int occurence = -1;
  {
    /* Manual reverse lookup. */
    for (int i=0; i < int_vector_size( index_vector ); i++)
      if (index_data[i] == index)
        occurence = i;
  }
  if (occurence < 0)
    util_abort("%s: internal error ... \n" , __func__);

  return occurence;
}

void ecl_file_map_fprintf_kw_list(const ecl_file_map_type * file_map , FILE * stream) {
  int i;
  for (i=0; i < vector_get_size( file_map->kw_list ); i++) {
    const ecl_file_kw_type * file_kw = vector_iget_const( file_map->kw_list , i );
    fprintf(stream , "%-8s %7d:%s\n",
            ecl_file_kw_get_header( file_kw ) , 
            ecl_file_kw_get_size( file_kw ) , 
            ecl_util_get_type_name( ecl_file_kw_get_type( file_kw )));
  }
}

/**
   Will return NULL if the block which is asked for is not present.
*/
static ecl_file_map_type * ecl_file_map_alloc_blockmap(const ecl_file_map_type * file_map , const char * header, int occurence) {
  if (ecl_file_map_get_num_named_kw( file_map , header ) > occurence) {
    ecl_file_map_type * block_map = ecl_file_map_alloc( file_map->fortio , false );
    if (ecl_file_map_has_kw( file_map , header )) {
      int kw_index = ecl_file_map_get_global_index( file_map , header , occurence );
      ecl_file_kw_type * file_kw = vector_iget( file_map->kw_list , kw_index );
      
      while (true) {
        ecl_file_map_add_kw( block_map , file_kw );
        
        kw_index++;
        if (kw_index == vector_get_size( file_map->kw_list ))
          break;              
        else {
          file_kw = vector_iget(file_map->kw_list , kw_index);
          if (strcmp( header , ecl_file_kw_get_header( file_kw )) == 0)
            break;
        }
      }
    } 
    ecl_file_map_make_index( block_map );
    return block_map;
  } else
    return NULL;
}


/*****************************************************************/

UTIL_SAFE_CAST_FUNCTION( ecl_file , ECL_FILE_ID)
UTIL_IS_INSTANCE_FUNCTION( ecl_file , ECL_FILE_ID)



ecl_file_type * ecl_file_alloc_empty( ) {
  ecl_file_type * ecl_file = util_malloc( sizeof * ecl_file , __func__);
  UTIL_TYPE_ID_INIT(ecl_file , ECL_FILE_ID);
  ecl_file->map_list = vector_alloc_new();
  return ecl_file;
}


/*
  The input @file must be either an INIT file or a restart file. Will
  fail hard if an INTEHEAD kw can not be found - or if the INTEHEAD
  keyword is not sufficiently large.

  The eclipse files can distinguish between ECLIPSE300 ( value == 300)
  and ECLIPSE300-Thermal option (value == 500). This function will
  return ECLIPSE300 in both those cases.  
*/

ecl_version_enum ecl_file_get_ecl_version( const ecl_file_type * file ) {
  ecl_kw_type * intehead_kw = ecl_file_iget_named_kw( file , INTEHEAD_KW , 0 );
  int int_value = ecl_kw_iget_int( intehead_kw , INTEHEAD_VERSION_INDEX );

  if (int_value == 100)
    return ECLIPSE100;
  else if ((int_value == 300) || (int_value == 500))
    return ECLIPSE300;
  else {
    util_abort("%s: ECLIPSE version value:%d not recognized \n",__func__ , int_value );
    return -1;
  }
}

/*
  1: Oil
  2: Water
  3: Oil + water
  4: Gas
  5: Gas + Oil
  6: Gas + water
  7: Gas + Water + Oil

  Do not know if this differs between init files and restart files?
*/
int ecl_file_get_phases( const ecl_file_type * init_file ) {
  ecl_kw_type * intehead_kw = ecl_file_iget_named_kw( init_file , INTEHEAD_KW , 0 );
  int phases = ecl_kw_iget_int( intehead_kw , INTEHEAD_PHASE_INDEX );
  return phases;
}








/*****************************************************************/
/* fwrite functions */

void ecl_file_fwrite_fortio(const ecl_file_type * ecl_file , fortio_type * target, int offset) {
  ecl_file_map_fwrite( ecl_file->active_map , target , offset );
}



/* 
   Observe : if the filename is a standard filename which can be used
   to infer formatted/unformatted automagically the fmt_file variable
   is NOT consulted.
*/

void ecl_file_fwrite(const ecl_file_type * ecl_file , const char * filename, bool fmt_file) {
  bool __fmt_file;
  ecl_file_enum file_type;
  
  file_type = ecl_util_get_file_type( filename , &__fmt_file , NULL);
  if (file_type == ECL_OTHER_FILE)
    __fmt_file = fmt_file;
  
  {
    fortio_type * target = fortio_open_writer( filename , ECL_ENDIAN_FLIP , __fmt_file);
    ecl_file_fwrite_fortio( ecl_file , target , 0);
    fortio_fclose( target );
  }
}






/*****************************************************************/
/**
   Here comes several functions for querying the ecl_file instance, and
   getting pointers to the ecl_kw content of the ecl_file. For getting
   ecl_kw instances there are two principally different access methods:

   * ecl_file_iget_named_kw(): This function will take a keyword
   (char *) and an integer as input. The integer corresponds to the
   ith occurence of the keyword in the file.

   * ecl_file_iget_kw(): This function just takes an integer index as
   input, and returns the corresponding ecl_kw instance - without
   considering which keyword it is.

   -------

   In addition the functions ecl_file_get_num_distinct_kw() and
   ecl_file_iget_distinct_kw() will return the number of distinct
   keywords, and distinct keyword keyword nr i (as a const char *).


   Possible usage pattern:

   ....
   for (ikw = 0; ikw < ecl_file_get_num_distinct_kw(ecl_file); ikw++) {
   const char * kw = ecl_file_iget_distinct_kw(ecl_file , ikw);
     
   printf("The file contains: %d occurences of \'%s\' \n",ecl_file_get_num_named_kw( ecl_file , kw) , kw);
   }
   ....

   For the summary file showed in the top this code will produce:

   The file contains 1 occurences of 'SEQHDR'
   The file contains 3 occurences of 'MINISTEP'
   The file contains 3 occurences of 'PARAMS'
  
*/







/**
   This will just return ecl_kw nr i - without looking at the names.
*/
ecl_kw_type * ecl_file_iget_kw( const ecl_file_type * ecl_file , int index) {
  return ecl_file_map_iget_kw( ecl_file->active_map , index );
}



/**
   This will return a copy ecl_kw nr i - without looking at the names.
*/
ecl_kw_type * ecl_file_icopy_kw( const ecl_file_type * ecl_file , int index) {
  return ecl_kw_alloc_copy( ecl_file_iget_kw( ecl_file , index ));
}


/**
   This function will iterate through the ecl_file instance and search
   for the ecl_kw instance @old_kw - the search is based on pointer
   equality, i.e. the actual ecl_kw instance, and not on content
   equality.

   When @old_kw is found that keyword instance will be discarded with
   ecl_kw_free() and the new keyword @new_kw will be inserted. If
   @old_kw can not be found the function will fail hard - to verify
   that @new_kw is indeed in the ecl_file instance you should use
   ecl_file_has_kw_ptr() first.
   
   The ecl_file function typically gives our references to the
   internal ecl_kw instances via the ecl_file_iget_kw() function. Use
   of ecl_file_replace_kw() might lead to invalidating ecl_kw
   instances held by the calling scope:


   ....
   ecl_file_type * restart_file   = ecl_file_fread_alloc( "ECLIPSE.UNRST" );
   ecl_kw_type * initial_pressure = ecl_file_iget_named_kw( ecl_file , "PRESSURE" , 0);
   ecl_kw_type * faked_pressure   = ecl_kw_alloc_copy( initial_pressure );
   
   ecl_kw_scale( faked_pressure , 1.25 );
   ecl_file_replace_kw( restart_file , initial_pressure , faked_pressure , false );  <--- This call will invalidate the inital_pressure reference
   ....
   ....
   // This will fail horribly: 
   printf("The initial pressure in cell(0) was:%g \n",ecl_kw_iget_double( initial_pressure , 0 ));
   /|\
   |
   +---------> Using initial_pressure => Crash and burn! 
                                                           
   The ecl_file structure takes ownership of all the keywords, and
   will also take ownership of the newly instered @new_kw instance; if
   the boolean @insert_copy is set to true the function will insert a
   copy of @new_kw, leaving the original reference untouched. 
*/                



void ecl_file_replace_kw( ecl_file_type * ecl_file , ecl_kw_type * old_kw , ecl_kw_type * new_kw , bool insert_copy) {
  //file_map_replace_kw( ecl_file , old_kw , new_kw , insert_copy );
  
  //int index = 0;
  //while (index < vector_get_size( ecl_file->kw_list )) {
  //  ecl_file_kw_type * ikw = vector_iget( ecl_file->kw_list , index );
  //  if (ecl_file_kw_ptr_eq( ikw , old_kw)) {
  //    /* 
  //       Found it; observe that the vector_iset() function will
  //       automatically invoke the destructor on the old_kw. 
  //    */
  //    
  //    ecl_file_kw_replace_kw( ikw , ecl_file->fortio , new_kw );
  //    
  //    ecl_file_make_index( ecl_file );
  //    return;
  //  }
  //  index++;
  //}
  //util_abort("%s: could not find ecl_kw ptr: %p \n",__func__ , old_kw);
}



//bool ecl_file_has_kw_ptr(const ecl_file_type * ecl_file , const ecl_kw_type * ecl_kw) {
//  int index;
//
//  for (index = 0; index < vector_get_size( ecl_file->kw_list ); index++) {
//    const ecl_file_kw_type * file_kw = vector_iget_const( ecl_file->kw_list , index );
//    if (ecl_file_kw_ptr_eq( file_kw , ecl_kw ))
//      return true;
//  } 
//  return false;
//}


/*
  Will search through the whole file given by @filename and look for
  an ecl_kw instance which compares equal with the input keyword @ecl_kw.
*/
  
//bool ecl_file_contains_kw( const char * filename , const ecl_kw_type * ecl_kw) {
//  bool has_kw = false;
//  bool          fmt_file = ecl_util_fmt_file( filename );
//  fortio_type * fortio   = fortio_open_reader( filename , ECL_ENDIAN_FLIP , fmt_file);
//  {
//    ecl_kw_type * file_kw = ecl_kw_alloc_empty();
//    while (true) {
//      if (ecl_kw_fseek_kw(ecl_kw_get_header( ecl_kw ) , false , false , fortio)) {
//        ecl_kw_fread_realloc( file_kw , fortio );
//        if (ecl_kw_equal( file_kw , ecl_kw )) {
//          has_kw = true;
//          break;
//        }
//      } else 
//        break;  /* Keyword not found. */
//    }
//    ecl_kw_free( file_kw );
//  }
//  fortio_fclose( fortio );
//  return has_kw;
//}




/* 
   This function will return the ith occurence of 'kw' in
   ecl_file. Will abort hard if the request can not be satisifed - use
   query functions if you can not take that.  
*/
   

ecl_kw_type * ecl_file_iget_named_kw( const ecl_file_type * ecl_file , const char * kw, int ith) {
  return ecl_file_map_iget_named_kw( ecl_file->active_map , kw , ith );
}


ecl_kw_type * ecl_file_icopy_named_kw( const ecl_file_type * ecl_file , const char * kw, int ith) {
  return ecl_kw_alloc_copy( ecl_file_iget_named_kw( ecl_file , kw , ith ));
}

  
/*
  Will return the number of times a particular keyword occurs in a
  ecl_file instance. Will return 0 if the keyword can not be found.
*/

int ecl_file_get_num_named_kw(const ecl_file_type * ecl_file , const char * kw) {
  return ecl_file_map_get_num_named_kw( ecl_file->active_map , kw);
}




/**
   This function does the following:

   1. Takes an input index which goes in to the global kw_list vector.
   2. Looks up the corresponding keyword.
   3. Return the number of this particular keyword instance, among
   the other instance with the same header.

   With the example above we get:

   ecl_file_iget_occurence(ecl_file , 2) -> 0; Global index 2 will
   look up the first occurence of PARAMS.
  
   ecl_file_iget_occurence(ecl_file , 5) -> 2; Global index 5 will
   look up th third occurence of MINISTEP.

   The enkf layer uses this funny functionality.
*/


int ecl_file_iget_occurence( const ecl_file_type * ecl_file , int index) {
  return ecl_file_map_iget_occurence( ecl_file->active_map , index );
}


/** 
    Returns the total number of ecl_kw instances in the ecl_file
    instance.
*/
int ecl_file_get_size( const ecl_file_type * ecl_file ){
  return ecl_file_map_get_size( ecl_file->active_map );
}


/**
   Returns true if the ecl_file instance has at-least one occurence of
   ecl_kw 'kw'.
*/
bool ecl_file_has_kw( const ecl_file_type * ecl_file , const char * kw) {
  return ecl_file_map_has_kw( ecl_file->active_map , kw );
}


int ecl_file_get_num_distinct_kw(const ecl_file_type * ecl_file) {
  return ecl_file_map_get_num_distinct_kw( ecl_file->active_map );
}


const char * ecl_file_iget_distinct_kw(const ecl_file_type * ecl_file, int index) {
  return ecl_file_map_iget_distinct_kw( ecl_file->active_map , index );
}


const char * ecl_file_get_src_file( const ecl_file_type * ecl_file ) {
  return fortio_filename_ref( ecl_file->fortio );
}


void ecl_file_fprintf_kw_list( const ecl_file_type * ecl_file , FILE * stream ) {
  ecl_file_map_fprintf_kw_list( ecl_file->active_map , stream );
}

const char * ecl_file_enum_iget( int index , int * value) {
  return util_enum_iget( index , ECL_FILE_ENUM_SIZE , (const util_enum_element_type []) { ECL_FILE_ENUM_DEFS } , value);
}






/****************************************************************************/
#include "ecl_rstfile.c"


static void ecl_file_scan( ecl_file_type * ecl_file ) {
  fortio_fseek( ecl_file->fortio , 0 , SEEK_SET );
  {
    ecl_kw_type   * work_kw = ecl_kw_alloc_new("WORK-KW" , 0 , ECL_INT_TYPE , NULL);
    long current_offset;
    while (true) {
      current_offset = fortio_ftell( ecl_file->fortio );
      if (ecl_kw_fread_header( work_kw , ecl_file->fortio )) {
        ecl_file_kw_type * file_kw = ecl_file_kw_alloc( work_kw , current_offset);
        ecl_file_map_add_kw( ecl_file->global_map , file_kw );
        ecl_file_kw_fskip_data( file_kw , ecl_file->fortio );
      } else 
        break;
    }
    ecl_kw_free( work_kw );
  }
}


void ecl_file_close(ecl_file_type * ecl_file) {
  fortio_fclose( ecl_file->fortio );
  vector_free( ecl_file->map_list );
  free( ecl_file );
}


void ecl_file_free__(void * arg) {
  ecl_file_close( ecl_file_safe_cast( arg ) );
}

static void ecl_file_add_map( ecl_file_type * ecl_file , ecl_file_map_type * file_map) {
  vector_append_owned_ref(ecl_file->map_list , file_map , ecl_file_map_free__ );
}


ecl_file_map_type * ecl_file_get_blockmap( ecl_file_type * ecl_file , const char * kw , int occurence) {
  ecl_file_map_type * blockmap = ecl_file_map_alloc_blockmap( ecl_file->active_map , kw , occurence );
  if (blockmap != NULL)
    ecl_file_add_map( ecl_file , blockmap );
  return blockmap;
}


ecl_file_map_type * ecl_file_get_unsmrymap( ecl_file_type * ecl_file , int seqhdr_nr) {
  return ecl_file_get_blockmap( ecl_file , SEQHDR_KW , seqhdr_nr );
}


ecl_file_map_type * ecl_file_get_global_map( const ecl_file_type * ecl_file ) {
  return ecl_file->active_map;
}



ecl_file_type * ecl_file_open( const char * filename ) {
  bool          fmt_file   = ecl_util_fmt_file( filename );
  ecl_file_type * ecl_file = ecl_file_alloc_empty( );

  ecl_file->fortio = fortio_open_reader( filename , ECL_ENDIAN_FLIP , fmt_file );
  ecl_file->global_map = ecl_file_map_alloc( ecl_file->fortio , true );
  ecl_file_add_map( ecl_file , ecl_file->global_map );
  ecl_file_scan( ecl_file );
  ecl_file_map_make_index( ecl_file->global_map );
  ecl_file->active_map = ecl_file->global_map;

  return ecl_file;
}