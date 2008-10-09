#include <util.h>
#include <stdlib.h>
#include <string.h>
#include <lsf_request.h>
#include <ext_job.h>
#include <ext_joblist.h>
#include <set.h>
#include <stringlist.h>
#include <job_queue.h>

/**

   This file implements a simple functionality to parse lsf_request
   strings; and combine them into one common resource request. The
   documentation of the -R option on bsub leaves a lot to be desired -
   to say the least. We therefor have quite limited ambitions:

    * We *ONLY* handle the rusage[] and select[] strings. 

    * When using bsub a string without section heading is defaulted to
      the select[] section - this is not supported.

    * When two rusage[] strings are combined the resulting rusage
      section is just the "sum", combined with ",".

    * In select strings we handle logical or (||). Different select
      sections are combined with logical and.

*/





struct lsf_request_struct {
  char                   * request;        /* The current string representation of the complete request. */
  const ext_joblist_type * joblist;        /* Reference to the current list of installed jobs. The lsf_request instance does *NOT* own this. */
  const stringlist_type  * forward_model;  /* Reference to the current forward model - NOT owned by lsf-request. */
  set_type               * select_set;
  set_type               * rusage;
  char                   * manual_init;
};


lsf_request_type * lsf_request_alloc(const ext_joblist_type * joblist, const char * manual_init) {
  lsf_request_type * lsf_request = util_malloc(sizeof * lsf_request , __func__);
  lsf_request->request = NULL;
  lsf_request->joblist = joblist;
  lsf_request->forward_model = NULL;

  lsf_request->rusage      = NULL;
  lsf_request->select_set  = NULL;
  lsf_request->manual_init = util_alloc_string_copy( manual_init );
  return lsf_request;
}


void lsf_request_reset(lsf_request_type * lsf_request) {
  lsf_request->request = util_safe_free( lsf_request->request );
  if (lsf_request->rusage != NULL)      set_free(lsf_request->rusage);
  if (lsf_request->select_set != NULL)  set_free(lsf_request->select_set);
}



void lsf_request_free(lsf_request_type * lsf_request) {
  lsf_request_reset(lsf_request);
  util_safe_free(lsf_request->manual_init); 
  free( lsf_request );
}


void lsf_request_set_manual_request(lsf_request_type * lsf_request , const char * resource_request) {
  lsf_request_reset(lsf_request);
  lsf_request->request = util_alloc_string_copy(resource_request);
}


/*static*/ void lsf_request_set_request_string(lsf_request_type * lsf_request) {
  char * select_string = NULL;
  char * rusage_string = NULL;

  if ( lsf_request->select_set != NULL) {
    char ** select_keys = set_alloc_keylist(lsf_request->select_set);
    int     keys        = set_get_size(lsf_request->select_set);
    if (keys == 0) 
      util_abort("%s: the selection set is the empty set - \n",__func__);
    {
      char * tmp = util_alloc_joined_string((const char **) select_keys , keys , "||");
      select_string = util_alloc_sprintf("select[%s]" , tmp);
      free(tmp);
    }
    util_free_stringlist(select_keys , keys);
  }
  
  if (lsf_request->rusage != NULL) {
    char ** rusage_keys = set_alloc_keylist(lsf_request->rusage);
    int     keys        = set_get_size(lsf_request->rusage);
    if (keys > 0) {
      char * tmp = util_alloc_joined_string((const char **) rusage_keys , keys , ",");
      select_string = util_alloc_sprintf("rusage[%s]" , tmp);
      free(tmp);
    }
    util_free_stringlist(rusage_keys , keys);
  }
  
  {
    char * resource_request = util_alloc_joined_string((const char *[2]) { rusage_string , select_string} , 2 , " ");
    lsf_request_set_manual_request( lsf_request , resource_request);
    util_safe_free(resource_request);
  }
  util_safe_free(select_string);
  util_safe_free(rusage_string);
}



void lsf_request_update__(lsf_request_type * lsf_request , const char * __resource_request) {
  if (__resource_request != NULL) {
    char * resource_request = util_alloc_string_copy( __resource_request );
    char * select_ptr = strstr(resource_request , "select");
    char * rusage_ptr = strstr(resource_request , "rusage");
    char * end_ptr;
    char * start_ptr;
    if (rusage_ptr != NULL) {
      start_ptr = &rusage_ptr[6];
      if (start_ptr[0] != '[') 
	util_abort("%s: expect to find \"[\" *immediately* after \'rusage\' when parsing:%s - aborting \n",__func__ , resource_request);
      start_ptr++;
      end_ptr = strchr(start_ptr , ']');
      if (end_ptr == NULL)
	util_abort("%s: could not find ending \"]\" when parsing rusage in: %s \n",__func__ , resource_request);
      
      {
	char * rusage_string = util_alloc_substring_copy( start_ptr , end_ptr - start_ptr);
	char ** token_list;
	int     tokens;

	if (lsf_request->rusage == NULL)
	  lsf_request->rusage = set_alloc_empty();
	
	util_split_string( rusage_string , " ," , &tokens , &token_list);
	for (int i = 0; i < tokens; i++)
	  set_add_key(lsf_request->rusage , token_list[i]);
	
	free(token_list);
	free(rusage_string);
      }


      if (select_ptr != NULL) {
	/* 
	   Explicitly clearing what we have used/read from the string in
	   an attempt of improving the chance of detecting errors.
	*/
	{
	  char * ptr = rusage_ptr;
	  while (ptr != end_ptr) {
	    *ptr = '-';
	    ptr++;
	  }
	}
	if (strstr(resource_request , "select") != select_ptr)
	  util_abort("%s: select string has vanished - internal parsing error when parsing: %s \n",__func__ , __resource_request);
	
	start_ptr = &select_ptr[6];
	if (start_ptr[0] != '[') 
	  util_abort("%s: expect to find \"[\" *immediately* after \'select\' when parsing:%s - aborting \n",__func__ , resource_request);
	start_ptr++;
	end_ptr = strchr(start_ptr , ']');
	if (end_ptr == NULL)
	  util_abort("%s: could not find ending \"]\" when parsing select in: %s \n",__func__ , resource_request);

	{
	  char * select_string = util_alloc_substring_copy( start_ptr , end_ptr - start_ptr);
	  char ** token_list;
	  int     tokens , i;
	  set_type * select_set = set_alloc_empty();
  
	  /* 
	     ONLY Understands "||" - this is higly fucked. No free
	     beers to those who can break this ... 
	  */
	  util_split_string( select_string , "|" , &tokens , &token_list);  
	  for (i = 0; i < tokens; i++) {
	    set_add_key(select_set , token_list[i]);
	    free(token_list[i]);
	  }

	  if (lsf_request->select_set == NULL)
	    lsf_request->select_set = select_set;
	  else {
	    printf("Intersecting: ");
	    set_fprintf(lsf_request->select_set , stdout);
	    printf(" and ");
	    set_fprintf(select_set , stdout);
	    set_intersect(lsf_request->select_set , select_set);   /* Combining existing set with new set with logical AND */
	    printf(" => ");
	    set_fprintf(lsf_request->select_set , stdout);
	    set_free(select_set);
	    printf("\n");
	  }
	  
	  free(token_list);
	  free(select_string);
	}
      }
    }
  }
}


/**
   This function updates the internal lsf_request status, and also
   calls the job_queue with the new request.

*/

void  lsf_request_update(lsf_request_type * lsf_request , const stringlist_type * forward_model , job_queue_type * job_queue) {
  if (forward_model != lsf_request->forward_model) {
    lsf_request->forward_model = forward_model;
    lsf_request->request = util_safe_free( lsf_request->request );
    /*
      Iterate forward model ....
    */
    lsf_request_update__(lsf_request , lsf_request->manual_init);
    for (int i = 0; i < stringlist_get_size(forward_model); i++) {
      const ext_job_type * ext_job = ext_joblist_get_job( lsf_request->joblist , stringlist_iget(forward_model, i));
      lsf_request_update__(lsf_request , ext_job_get_lsf_resources( ext_job ));
    }
    
    lsf_request_set_request_string(lsf_request);
    job_queue_set_resource_request(job_queue , lsf_request->request);
  }
}


const char * lsf_request_get(const lsf_request_type * lsf_request) {
  return lsf_request->request;
}
