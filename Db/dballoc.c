/*
* $Id:  $
* $Version: $
*
* Copyright (c) Tanel Tammet 2004,2005,2006,2007,2008,2009
*
* Contact: tanel.tammet@gmail.com                 
*
* This file is part of wgandalf
*
* Wgandalf is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
* 
* Wgandalf is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
* 
* You should have received a copy of the GNU General Public License
* along with Wgandalf.  If not, see <http://www.gnu.org/licenses/>.
*
*/

 /** @file dballoc.c
 *  Database initialisation and common allocation/deallocation procedures: 
 *  areas, subareas, objects, strings etc.
 *
 */

/* ====== Includes =============== */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#ifdef _WIN32
#include "../config-w32.h"
#else
#include "../config.h"
#endif
#include "dballoc.h"
#include "dblock.h"
#include "dbtest.h"
#include "dbindex.h"

/* ====== Private headers and defs ======== */

/* ======= Private protos ================ */

static gint init_db_subarea(void* db, void* area_header, gint index, gint size);
static gint alloc_db_segmentchunk(void* db, gint size); // allocates a next chunk from db memory segment
static gint init_syn_vars(void* db);
static gint init_db_index_area_header(void* db);
static gint init_logging(void* db);
static gint init_hash_subarea(void* db, db_hash_area_header* areah, gint arraylength);

static gint make_subarea_freelist(void* db, void* area_header, gint arrayindex);
static gint init_area_buckets(void* db, void* area_header);
static gint init_subarea_freespace(void* db, void* area_header, gint arrayindex);

static gint extend_fixedlen_area(void* db, void* area_header);

static gint split_free(void* db, void* area_header, gint nr, gint* freebuckets, gint i);
static gint extend_varlen_area(void* db, void* area_header, gint minbytes);

static gint show_dballoc_error_nr(void* db, char* errmsg, gint nr);
static gint show_dballoc_error(void* db, char* errmsg);


/* ====== Functions ============== */


/* -------- segment header initialisation ---------- */

/** starts and completes memsegment initialisation
*
* should be called after new memsegment is allocated
*/

gint wg_init_db_memsegment(void* db, gint key, gint size) {  
  db_memsegment_header* dbh;
  gint tmp;
  gint free;
  gint i;
  
  dbh=(db_memsegment_header*) db;
  // set main global values for db
  dbh->mark=MEMSEGMENT_MAGIC_MARK;
  dbh->version=MEMSEGMENT_VERSION;
  dbh->size=size;
  dbh->initialadr=(gint)db; /* XXX: this assumes pointer size. Currently harmless
                             * because initialadr isn't used much. */
  dbh->key=key;  /* might be 0 if local memory used */
  dbh->parent=0;  /* initially 0, may be overwritten for child databases */
   
#ifdef CHECK
  if(((gint) dbh)%SUBAREA_ALIGNMENT_BYTES)
    show_dballoc_error(dbh,"db base pointer has bad alignment (ignoring)");
#endif

  // set correct alignment for free
  free=sizeof(db_memsegment_header);
  // set correct alignment for free
  i=SUBAREA_ALIGNMENT_BYTES-(free%SUBAREA_ALIGNMENT_BYTES);
  if (i==SUBAREA_ALIGNMENT_BYTES) i=0;  
  dbh->free=free+i;
  
  // allocate and initialise subareas
  
  //datarec
  tmp=init_db_subarea(dbh,&(dbh->datarec_area_header),0,INITIAL_SUBAREA_SIZE);
  if (tmp) {  show_dballoc_error(dbh," cannot create datarec area"); return -1; }
  (dbh->datarec_area_header).fixedlength=0;
  tmp=init_area_buckets(db,&(dbh->datarec_area_header)); // fill buckets with 0-s
  if (tmp) {  show_dballoc_error(dbh," cannot initialize datarec area buckets"); return -1; }
  tmp=init_subarea_freespace(db,&(dbh->datarec_area_header),0); // mark and store free space in subarea 0
  if (tmp) {  show_dballoc_error(dbh," cannot initialize datarec subarea 0"); return -1; }  
  //longstr
  tmp=init_db_subarea(dbh,&(dbh->longstr_area_header),0,INITIAL_SUBAREA_SIZE);
  if (tmp) {  show_dballoc_error(dbh," cannot create longstr area"); return -1; }
  (dbh->longstr_area_header).fixedlength=0;
  tmp=init_area_buckets(db,&(dbh->longstr_area_header)); // fill buckets with 0-s
  if (tmp) {  show_dballoc_error(dbh," cannot initialize longstr area buckets"); return -1; }
  tmp=init_subarea_freespace(db,&(dbh->longstr_area_header),0); // mark and store free space in subarea 0
  if (tmp) {  show_dballoc_error(dbh," cannot initialize datarec subarea 0"); return -1; }
  //listcell
  tmp=init_db_subarea(dbh,&(dbh->listcell_area_header),0,INITIAL_SUBAREA_SIZE);
  if (tmp) {  show_dballoc_error(dbh," cannot create listcell area"); return -1; }
  (dbh->listcell_area_header).fixedlength=1;
  (dbh->listcell_area_header).objlength=sizeof(gcell);
  tmp=make_subarea_freelist(db,&(dbh->listcell_area_header),0); // freelist into subarray 0
  if (tmp) {  show_dballoc_error(dbh," cannot initialize listcell area"); return -1; }
  //shortstr
  tmp=init_db_subarea(dbh,&(dbh->shortstr_area_header),0,INITIAL_SUBAREA_SIZE);
  if (tmp) {  show_dballoc_error(dbh," cannot create short string area"); return -1; }
  (dbh->shortstr_area_header).fixedlength=1;
  (dbh->shortstr_area_header).objlength=SHORTSTR_SIZE;
  tmp=make_subarea_freelist(db,&(dbh->shortstr_area_header),0); // freelist into subarray 0
  if (tmp) {  show_dballoc_error(dbh," cannot initialize shortstr area"); return -1; }
  //word
  tmp=init_db_subarea(dbh,&(dbh->word_area_header),0,INITIAL_SUBAREA_SIZE);
  if (tmp) {  show_dballoc_error(dbh," cannot create word area"); return -1; }
  (dbh->word_area_header).fixedlength=1;
  (dbh->word_area_header).objlength=sizeof(gint);
  tmp=make_subarea_freelist(db,&(dbh->word_area_header),0); // freelist into subarray 0
  if (tmp) {  show_dballoc_error(dbh," cannot initialize word area"); return -1; }
  //doubleword
  tmp=init_db_subarea(dbh,&(dbh->doubleword_area_header),0,INITIAL_SUBAREA_SIZE);
  if (tmp) {  show_dballoc_error(dbh," cannot create doubleword area"); return -1; }
  (dbh->doubleword_area_header).fixedlength=1;
  (dbh->doubleword_area_header).objlength=2*sizeof(gint);
  tmp=make_subarea_freelist(db,&(dbh->doubleword_area_header),0); // freelist into subarray 0
  if (tmp) {  show_dballoc_error(dbh," cannot initialize doubleword area"); return -1; }

  /* index structures also user fixlen object storage:
   *   tnode area - contains index nodes
   *   index header area - contains index headers.
   * index lookup data takes up relatively little space so we allocate
   * the smallest chunk allowed.
   */
  tmp=init_db_subarea(dbh,&(dbh->tnode_area_header),0,INITIAL_SUBAREA_SIZE);
  if (tmp) {  show_dballoc_error(dbh," cannot create tnode area"); return -1; }
  (dbh->tnode_area_header).fixedlength=1;
  (dbh->tnode_area_header).objlength=sizeof(struct wg_tnode);
  tmp=make_subarea_freelist(db,&(dbh->tnode_area_header),0);
  if (tmp) {  show_dballoc_error(dbh," cannot initialize tnode area"); return -1; }

  tmp=init_db_subarea(dbh,&(dbh->indexhdr_area_header),0,MINIMAL_SUBAREA_SIZE);
  if (tmp) {  show_dballoc_error(dbh," cannot create index header area"); return -1; }
  (dbh->indexhdr_area_header).fixedlength=1;
  (dbh->indexhdr_area_header).objlength=sizeof(wg_index_header);
  tmp=make_subarea_freelist(db,&(dbh->indexhdr_area_header),0);
  if (tmp) {  show_dballoc_error(dbh," cannot initialize index header area"); return -1; }
     
  /* initialize other structures */
  
  /* initialize strhash array area */
  tmp=init_hash_subarea(dbh,&(dbh->strhash_area_header),INITIAL_STRHASH_LENGTH);
  if (tmp) {  show_dballoc_error(dbh," cannot create strhash array area"); return -1; }
  
  
  /* initialize synchronization */
  tmp=init_syn_vars(db);
  if (tmp) { show_dballoc_error(dbh," cannot initialize synchronization area"); return -1; }

  /* initialize index structures */
  tmp=init_db_index_area_header(db);
  if (tmp) { show_dballoc_error(dbh," cannot initialize index header area"); return -1; }
  
  /* initialize logging structures */
  
  
  tmp=init_logging(db);
  //printf("hhhhhhhhhhhhhhh %d\n",tmp);
 /* tmp=init_db_subarea(dbh,&(dbh->logging_area_header),0,INITIAL_SUBAREA_SIZE);
  printf("asd %d\n",tmp);
  if (tmp) {  show_dballoc_error(dbh," cannot create logging area"); return -1; }
  (dbh->logging_area_header).fixedlength=0;
  tmp=init_area_buckets(db,&(dbh->logging_area_header)); // fill buckets with 0-s
  if (tmp) {  show_dballoc_error(dbh," cannot initialize logging area buckets"); return -1; }*/

    
  return 0; 
}  




/** initializes a subarea. subarea is used for actual data obs allocation
*
* returns 0 if ok, negative otherwise;
* 
* called - several times - first by wg_init_db_memsegment, then as old subareas 
* get filled up
*/

static gint init_db_subarea(void* db, void* area_header, gint index, gint size) {
  db_area_header* areah;
  gint segmentchunk;
  gint i;
  gint asize;
  
  //printf("init_db_subarea called with size %d \n",size);
  if (size<MINIMAL_SUBAREA_SIZE) return -1; // errcase
  segmentchunk=alloc_db_segmentchunk(db,size);
  if (!segmentchunk) return -2; // errcase      
  areah=(db_area_header*)area_header;  
  ((areah->subarea_array)[index]).size=size;     
  ((areah->subarea_array)[index]).offset=segmentchunk;  
  // set correct alignment for alignedoffset
  i=SUBAREA_ALIGNMENT_BYTES-(segmentchunk%SUBAREA_ALIGNMENT_BYTES);
  if (i==SUBAREA_ALIGNMENT_BYTES) i=0;    
  ((areah->subarea_array)[index]).alignedoffset=segmentchunk+i; 
  // set correct alignment for alignedsize  
  asize=(size-i);
  i=asize-(asize%MIN_VARLENOBJ_SIZE);  
  ((areah->subarea_array)[index]).alignedsize=i;
  // set last index and freelist 
  areah->last_subarea_index=index;  
  areah->freelist=0;
  return 0;
}  

/** allocates a new segment chunk from the segment
*
* returns offset if successful, 0 if no more space available
* if 0 returned, no allocation performed: can try with a smaller value
* used for allocating all subareas 
* 
* Alignment is guaranteed to SUBAREA_ALIGNMENT_BYTES
*/

static gint alloc_db_segmentchunk(void* db, gint size) {
  db_memsegment_header* dbh;
  gint lastfree;
  gint nextfree; 
  gint i;
  
  dbh=(db_memsegment_header*)db;
  lastfree=dbh->free;
  nextfree=lastfree+size;
  // set correct alignment for nextfree 
  i=SUBAREA_ALIGNMENT_BYTES-(nextfree%SUBAREA_ALIGNMENT_BYTES);  
  if (i==SUBAREA_ALIGNMENT_BYTES) i=0;  
  nextfree=nextfree+i;  
  if (nextfree>=(dbh->size)) {
    show_dballoc_error_nr(dbh,"segment does not have enough space for the required chunk of size",size);
    return 0;
  }
  dbh->free=nextfree;
  return lastfree;  
}  

/** initializes sync variable storage
*
* returns 0 if ok, negative otherwise;
*/

static gint init_syn_vars(void* db) {
  
  db_memsegment_header* dbh = (db_memsegment_header *) db;
  gint i;
  
#ifndef QUEUED_LOCKS
  /* calculate aligned pointer */
  i = ((gint) (dbh->locks._storage) + SYN_VAR_PADDING - 1) & -SYN_VAR_PADDING;
  dbh->locks.global_lock = dbaddr(db, (void *) i);
#else
  i = alloc_db_segmentchunk(db, SYN_VAR_PADDING * (MAX_LOCKS+1));
  if(!i) return -1;
  /* re-align (SYN_VAR_PADDING <> SUBAREA_ALIGNMENT_BYTES) */
  i = (i + SYN_VAR_PADDING - 1) & -SYN_VAR_PADDING;
  dbh->locks.storage = i;
  dbh->locks.max_nodes = MAX_LOCKS;
  dbh->locks.freelist = i; /* dummy, wg_init_locks() will overwrite this */
#endif

  /* allocating space was successful, set the initial state */
  return wg_init_locks(db);
}

/** initializes main index area
* Currently this function only sets up an empty index table. The rest
* of the index storage is initialized by wg_init_db_memsegment().
* returns 0 if ok
*/
static gint init_db_index_area_header(void* db) {
  db_memsegment_header* dbh = (db_memsegment_header *) db;
  dbh->index_control_area_header.number_of_indexes=0;
  memset(dbh->index_control_area_header.index_table, 0,
    MAX_INDEXED_FIELDNR+1);
  return 0;
}  

/** initializes logging area
*
*/
static gint init_logging(void* db) {
  
  db_memsegment_header* dbh = (db_memsegment_header *) db;
    dbh->logging.firstoffset=alloc_db_segmentchunk(db,INITIAL_SUBAREA_SIZE); //get new area for logging
    dbh->logging.logoffset=dbh->logging.firstoffset;
  //printf("asddddddddddddddddddddddddddd %d\n",dbh->logging.firstoffset);
  dbh->logging.counter=0;
  dbh->logging.writelog=1;
  dbh->logging.fileopen=0;
  return 0;
} 

/** initializes hash area
*
*/
static gint init_hash_subarea(void* db, db_hash_area_header* areah, gint arraylength) {  
  gint segmentchunk;
  gint i;
  gint asize;
  gint j;
  
  //printf("init_hash_subarea called with arraylength %d \n",arraylength);
  asize=((arraylength+1)*sizeof(gint))+(2*SUBAREA_ALIGNMENT_BYTES); // 2* just to be safe
  //printf("asize: %d \n",asize);
  //if (asize<100) return -1; // errcase to filter out stupid requests
  segmentchunk=alloc_db_segmentchunk(db,asize);
  //printf("segmentchunk: %d \n",segmentchunk);
  if (!segmentchunk) return -2; // errcase      
  areah->offset=segmentchunk;
  areah->size=asize;
  areah->arraylength=arraylength;
  // set correct alignment for arraystart
  i=SUBAREA_ALIGNMENT_BYTES-(segmentchunk%SUBAREA_ALIGNMENT_BYTES);
  if (i==SUBAREA_ALIGNMENT_BYTES) i=0;    
  areah->arraystart=segmentchunk+i; 
  i=areah->arraystart;
  for(j=0;j<arraylength;j++) dbstore(db,i+(j*sizeof(gint)),0);  
  //show_strhash(db);
  return 0;
}  



/* -------- freelists creation  ---------- */

/** create freelist for an area
*
* used for initialising (sub)areas used for fixed-size allocation
*
* returns 0 if ok
*
* speed stats:
*
* 10000 * creation of   100000 elems (1 000 000 000 or 1G ops) takes 1.2 sec on penryn 
* 1000 * creation of  1000000 elems (1 000 000 000 or 1G ops) takes 3.4 sec on penryn 
*
*/

static gint make_subarea_freelist(void* db, void* area_header, gint arrayindex) {
  db_area_header* areah;  
  gint freelist;
  gint objlength;
  gint max;
  gint size;
  gint offset;  
  gint i;
  
  // general area info
  areah=(db_area_header*)area_header;
  freelist=areah->freelist;
  objlength=areah->objlength; 
  
  //subarea info  
  size=((areah->subarea_array)[arrayindex]).alignedsize;
  offset=((areah->subarea_array)[arrayindex]).alignedoffset;    
  // create freelist
  max=(offset+size)-(2*objlength);
  for(i=offset;i<=max;i=i+objlength) {
    dbstore(db,i,i+objlength);     
  } 
  dbstore(db,i,0);  
  (areah->freelist)=offset; //
  //printf("(areah->freelist) %d \n",(areah->freelist));
  return 0;
}  




/* -------- buckets creation  ---------- */

/** fill bucket data for an area
*
* used for initialising areas used for variable-size allocation
* 
* returns 0 if ok, not 0 if error
*
*/

gint init_area_buckets(void* db, void* area_header) {
  db_area_header* areah;
  gint* freebuckets;    
  gint i;
  
  // general area info
  areah=(db_area_header*)area_header;
  freebuckets=areah->freebuckets;
  
  // empty all buckets
  for(i=0;i<EXACTBUCKETS_NR+VARBUCKETS_NR+CACHEBUCKETS_NR;i++) {
    freebuckets[i]=0;     
  } 
  return 0;
}  

/** mark up beginning and end for a subarea, set free area as a new victim
*
* used for initialising new subareas used for variable-size allocation
* 
* returns 0 if ok, not 0 if error
*
*/  
    
gint init_subarea_freespace(void* db, void* area_header, gint arrayindex) {
  db_area_header* areah;
  gint* freebuckets;  
  gint size;
  gint offset;    
  gint dv;
  gint dvindex;
  gint dvsize;
  gint freelist;
  gint endmarkobj;
  gint freeoffset;
  gint freesize;
  //gint i;
  
  // general area info
  areah=(db_area_header*)area_header;
  freebuckets=areah->freebuckets;
  
  //subarea info  
  size=((areah->subarea_array)[arrayindex]).alignedsize;
  offset=((areah->subarea_array)[arrayindex]).alignedoffset;    
  
  // if the previous area exists, store current victim to freelist
  if (arrayindex>0) {
    dv=freebuckets[DVBUCKET];
    dvsize=freebuckets[DVSIZEBUCKET];
    if (dv!=0 && dvsize>=MIN_VARLENOBJ_SIZE) {      
      dbstore(db,dv,makefreeobjectsize(dvsize)); // store new size with freebit to the second half of object
      dbstore(db,dv+dvsize-sizeof(gint),makefreeobjectsize(dvsize));
      dvindex=wg_freebuckets_index(db,dvsize);
      freelist=freebuckets[dvindex];
      if (freelist!=0) dbstore(db,freelist+2*sizeof(gint),dv); // update prev ptr 
      dbstore(db,dv+sizeof(gint),freelist); // store previous freelist 
      dbstore(db,dv+2*sizeof(gint),dbaddr(db,&freebuckets[dvindex])); // store ptr to previous  
      freebuckets[dvindex]=dv; // store offset to correct bucket
      //printf("in init_subarea_freespace: \n PUSHED DV WITH SIZE %d TO FREELIST TO BUCKET %d:\n",
      //        dvsize,dvindex);
      //show_bucket_freeobjects(db,freebuckets[dvindex]); 
    }
  }  
  // create two minimal in-use objects never to be freed: marking beginning 
  // and end of free area via in-use bits in size
  // beginning of free area
  dbstore(db,offset,makespecialusedobjectsize(MIN_VARLENOBJ_SIZE)); // lowest bit 0 means in use
  dbstore(db,offset+sizeof(gint),SPECIALGINT1START); // next ptr
  dbstore(db,offset+2*sizeof(gint),0); // prev ptr
  dbstore(db,offset+MIN_VARLENOBJ_SIZE-sizeof(gint),MIN_VARLENOBJ_SIZE); // len to end as well
  // end of free area
  endmarkobj=offset+size-MIN_VARLENOBJ_SIZE;
  dbstore(db,endmarkobj,makespecialusedobjectsize(MIN_VARLENOBJ_SIZE)); // lowest bit 0 means in use
  dbstore(db,endmarkobj+sizeof(gint),SPECIALGINT1END); // next ptr
  dbstore(db,endmarkobj+2*sizeof(gint),0); // prev ptr
  dbstore(db,endmarkobj+MIN_VARLENOBJ_SIZE-sizeof(gint),MIN_VARLENOBJ_SIZE); // len to end as well
  // calc where real free area starts and what is the size
  freeoffset=offset+MIN_VARLENOBJ_SIZE;
  freesize=size-2*MIN_VARLENOBJ_SIZE;
  // put whole free area into one free object  
  // store the single free object as a designated victim
  dbstore(db,freeoffset,makespecialusedobjectsize(freesize)); // length without free bits: victim not marked free
  dbstore(db,freeoffset+sizeof(gint),SPECIALGINT1DV); // marks that it is a dv kind of special object
  freebuckets[DVBUCKET]=freeoffset;
  freebuckets[DVSIZEBUCKET]=freesize; 
  // alternative: store the single free object to correct bucket  
  /*
  dbstore(db,freeoffset,setcfree(freesize)); // size with free bits stored to beginning of object
  dbstore(db,freeoffset+sizeof(gint),0); // empty ptr to remaining obs stored after size
  i=freebuckets_index(db,freesize);
  if (i<0) {
    show_dballoc_error_nr(db,"initialising free object failed for ob size ",freesize);
    return -1;
  }      
  dbstore(db,freeoffset+2*sizeof(gint),dbaddr(db,&freebuckets[i])); // ptr to previous stored
  freebuckets[i]=freeoffset;  
  */
  return 0;
}  



/* -------- fixed length object allocation and freeing ---------- */


/** allocate a new fixed-len object
*
* return offset if ok, 0 if allocation fails
*/

gint wg_alloc_fixlen_object(void* db, void* area_header) {
  db_area_header* areah;
  gint freelist;
  
  areah=(db_area_header*)area_header;
  freelist=areah->freelist;
  if (!freelist) {
    if(!extend_fixedlen_area(db,areah)) {
      show_dballoc_error_nr(db,"cannot extend fixed length object area for size ",areah->objlength);
      return 0;
    }  
    freelist=areah->freelist;
    if (!freelist) {
      show_dballoc_error_nr(db,"no free fixed length objects available for size ",areah->objlength);
      return 0;       
    } else {
      areah->freelist=dbfetch(db,freelist);
      return freelist;
    }        
  } else {
    areah->freelist=dbfetch(db,freelist);
    return freelist;
  }         
}  

/** create and initialise a new subarea for fixed-len obs area
* 
* returns allocated size if ok, 0 if failure
* used when the area has no more free space
*
*/

static gint extend_fixedlen_area(void* db, void* area_header) {
  gint i;
  gint tmp;
  gint size;
  gint newsize;
  gint nextel;
  db_area_header* areah;

  areah=(db_area_header*)area_header;
  i=areah->last_subarea_index;
  if (i+1>=SUBAREA_ARRAY_SIZE) {
    show_dballoc_error_nr(db,
      " no more subarea array elements available for fixedlen of size: ",areah->objlength); 
    return 0; // no more subarea array elements available 
  }  
  size=((areah->subarea_array)[i]).size; // last allocated subarea size
  // make tmp power-of-two times larger 
  newsize=size*2; 
  nextel=init_db_subarea(db,areah,i+1,newsize); // try to get twice larger area
  if (nextel) {
    //printf("REQUIRED SPACE FAILED, TRYING %d\n",size);
    // required size failed: try last size
    newsize=size;
    nextel=init_db_subarea(db,areah,i+1,newsize); // try same size area as last allocated
    if (nextel) {
      show_dballoc_error_nr(db," cannot extend datarec area with a new subarea of size: ",size); 
      return 0; // cannot allocate enough space
    }  
  }  
  // here we have successfully allocated a new subarea
  tmp=make_subarea_freelist(db,areah,i+1);  // fill with a freelist, store ptrs  
  if (tmp) {  show_dballoc_error(db," cannot initialize new subarea"); return 0; } 
  return newsize;
}  



/** free an existing listcell
*
* the object is added to the freelist
*
*/

void wg_free_listcell(void* db, gint offset) {
  dbstore(db,offset,(((db_memsegment_header*)db)->listcell_area_header).freelist); 
  (((db_memsegment_header*)db)->listcell_area_header).freelist=offset;   
}  


/** free an existing shortstr object
*
* the object is added to the freelist
*
*/

void wg_free_shortstr(void* db, gint offset) {
  dbstore(db,offset,(((db_memsegment_header*)db)->shortstr_area_header).freelist); 
  (((db_memsegment_header*)db)->shortstr_area_header).freelist=offset;   
}  

/** free an existing word-len object
*
* the object is added to the freelist
*
*/

void wg_free_word(void* db, gint offset) {
  dbstore(db,offset,(((db_memsegment_header*)db)->word_area_header).freelist); 
  (((db_memsegment_header*)db)->word_area_header).freelist=offset;   
}  



/** free an existing doubleword object
*
* the object is added to the freelist
*
*/

void wg_free_doubleword(void* db, gint offset) {
  dbstore(db,offset,(((db_memsegment_header*)db)->doubleword_area_header).freelist); //bug fixed here
  (((db_memsegment_header*)db)->doubleword_area_header).freelist=offset;   
}  

/** free an existing tnode object
*
* the object is added to the freelist
*
*/

void wg_free_tnode(void* db, gint offset) {
  dbstore(db,offset,(((db_memsegment_header*)db)->tnode_area_header).freelist); 
  (((db_memsegment_header*)db)->tnode_area_header).freelist=offset;   
}  

/** free generic fixlen object
*
* the object is added to the freelist
*
*/

void wg_free_fixlen_object(void* db, db_area_header *hdr, gint offset) {
  dbstore(db,offset,hdr->freelist); 
  hdr->freelist=offset;   
}  


/* -------- variable length object allocation and freeing ---------- */


/** allocate a new object of given length
*
* returns correct offset if ok, 0 in case of error
*
*/

gint wg_alloc_gints(void* db, void* area_header, gint nr) {
  gint wantedbytes;   // actually wanted size in bytes, stored in object header
  gint usedbytes;     // amount of bytes used: either wantedbytes or bytes+4 (obj must be 8 aligned)
  gint* freebuckets;
  gint res;
  gint nextel;  
  gint i;
  gint j;
  gint tmp;
  gint size;
  db_area_header* areah;
  
  areah=(db_area_header*)area_header; 
  wantedbytes=nr*sizeof(gint); // object sizes are stored in bytes
  if (wantedbytes<0) return 0; // cannot allocate negative or zero sizes
  if (wantedbytes<=MIN_VARLENOBJ_SIZE) usedbytes=MIN_VARLENOBJ_SIZE;
  else if (wantedbytes%8) usedbytes=wantedbytes+4;
  else usedbytes=wantedbytes;
  //printf("wg_alloc_gints called with nr %d and wantedbytes %d and usedbytes %d\n",nr,wantedbytes,usedbytes);  
  // first find if suitable length free object is available  
  freebuckets=areah->freebuckets;
  if (usedbytes<EXACTBUCKETS_NR && freebuckets[usedbytes]!=0) {
    res=freebuckets[usedbytes];  // first freelist element in that bucket
    nextel=dbfetch(db,res+sizeof(gint)); // next element in freelist of that bucket
    freebuckets[usedbytes]=nextel; 
    // change prev ptr of next elem
    if (nextel!=0) dbstore(db,nextel+2*sizeof(gint),dbaddr(db,&freebuckets[usedbytes]));
    // prev elem cannot be free (no consecutive free elems)  
    dbstore(db,res,makeusedobjectsizeprevused(wantedbytes)); // store wanted size to the returned object
    return res;
  }
  // next try to find first free object in a few nearest exact-length buckets (shorter first)
  for(j=0,i=usedbytes+1;i<EXACTBUCKETS_NR && j<3;i++,j++) {
    if (freebuckets[i]!=0 && 
        getfreeobjectsize(dbfetch(db,freebuckets[i]))>=usedbytes+MIN_VARLENOBJ_SIZE) {
      // found one somewhat larger: now split and store the rest
      res=freebuckets[i];
      tmp=split_free(db,areah,usedbytes,freebuckets,i);
      if (tmp<0) return 0; // error case
      // prev elem cannot be free (no consecutive free elems)     
      dbstore(db,res,makeusedobjectsizeprevused(wantedbytes)); // store wanted size to the returned object    
      return res;
    }          
  }  
  // next try to use the cached designated victim for creating objects off beginning
  // designated victim is not marked free by header and is not present in any freelist
  size=freebuckets[DVSIZEBUCKET];
  if (usedbytes<=size && freebuckets[DVBUCKET]!=0) {
    res=freebuckets[DVBUCKET];    
    if (usedbytes==size) { 
      // found a designated victim of exactly right size, dv is used up and disappears                  
      freebuckets[DVBUCKET]=0;
      freebuckets[DVSIZEBUCKET]=0; 
      // prev elem of dv cannot be free
      dbstore(db,res,makeusedobjectsizeprevused(wantedbytes)); // store wanted size to the returned object      
      return res;
    } else if (usedbytes+MIN_VARLENOBJ_SIZE<=size) {
      // found a designated victim somewhat larger: take the first part and keep the rest as dv
      dbstore(db,res+usedbytes,makespecialusedobjectsize(size-usedbytes)); // store smaller size to victim, turn off free bits     
      dbstore(db,res+usedbytes+sizeof(gint),SPECIALGINT1DV); // marks that it is a dv kind of special object
      freebuckets[DVBUCKET]=res+usedbytes; // point to rest of victim
      freebuckets[DVSIZEBUCKET]=size-usedbytes; // rest of victim becomes shorter      
      // prev elem of dv cannot be free        
      dbstore(db,res,makeusedobjectsizeprevused(wantedbytes)); // store wanted size to the returned object          
      return res;
    }      
  }
  // next try to find first free object in exact-length buckets (shorter first)
  for(i=usedbytes+1;i<EXACTBUCKETS_NR;i++) {
    if (freebuckets[i]!=0 && 
        getfreeobjectsize(dbfetch(db,freebuckets[i]))>=usedbytes+MIN_VARLENOBJ_SIZE) {
      // found one somewhat larger: now split and store the rest
      res=freebuckets[i];
      tmp=split_free(db,areah,usedbytes,freebuckets,i);
      if (tmp<0) return 0; // error case
      // prev elem cannot be free (no consecutive free elems)      
      dbstore(db,res,makeusedobjectsizeprevused(wantedbytes)); // store wanted size to the returned object    
      return res;
    }          
  }   
  // next try to find first free object in var-length buckets (shorter first)
  for(i=wg_freebuckets_index(db,usedbytes);i<EXACTBUCKETS_NR+VARBUCKETS_NR;i++) {
    if (freebuckets[i]!=0) {
      size=getfreeobjectsize(dbfetch(db,freebuckets[i]));
      if (size==usedbytes) { 
        // found one of exactly right size
        res=freebuckets[i];  // first freelist element in that bucket
        nextel=dbfetch(db,res+sizeof(gint)); // next element in freelist of that bucket
        freebuckets[i]=nextel; 
        // change prev ptr of next elem
        if (nextel!=0) dbstore(db,nextel+2*sizeof(gint),dbaddr(db,&freebuckets[i]));
        // prev elem cannot be free (no consecutive free elems)   
        dbstore(db,res,makeusedobjectsizeprevused(wantedbytes)); // store wanted size to the returned object
        return res;
      } else if (size>=usedbytes+MIN_VARLENOBJ_SIZE) {
        // found one somewhat larger: now split and store the rest
        res=freebuckets[i];
        //printf("db %d,nr %d,freebuckets %d,i %d\n",db,(int)nr,(int)freebuckets,(int)i);
        tmp=split_free(db,areah,usedbytes,freebuckets,i);
        if (tmp<0) return 0; // error case
        // prev elem cannot be free (no consecutive free elems)   
        dbstore(db,res,makeusedobjectsizeprevused(wantedbytes)); // store wanted size to the returned object
        return res;      
      }  
    }          
  }  
  // down here we have found no suitable dv or free object to use for allocation
  // try to get a new memory area
  //printf("ABOUT TO CREATE A NEW SUBAREA\n");
  tmp=extend_varlen_area(db,areah,usedbytes);
  if (!tmp) {  show_dballoc_error(db," cannot initialize new varlen subarea"); return 0; }
  // here we have successfully allocated a new subarea
  // call self recursively: this call will use the new free area
  tmp=wg_alloc_gints(db,areah,nr);
  //show_db_memsegment_header(db);
  return tmp;
}  



/** create and initialise a new subarea for var-len obs area
* 
* returns allocated size if ok, 0 if failure
* used when the area has no more free space
*
* bytes indicates the minimal required amount:
* could be extended much more, but not less than bytes
*
*/

static gint extend_varlen_area(void* db, void* area_header, gint minbytes) {  
  gint i;
  gint tmp;
  gint size;
  gint newsize;
  gint nextel;
  db_area_header* areah;

  areah=(db_area_header*)area_header;
  i=areah->last_subarea_index;
  if (i+1>=SUBAREA_ARRAY_SIZE) {
    show_dballoc_error_nr(db," no more subarea array elements available for datarec: ",i); 
    return 0; // no more subarea array elements available 
  }  
  size=((areah->subarea_array)[i]).size; // last allocated subarea size
  // make newsize power-of-two times larger so that it would be enough for required bytes
  for(newsize=size*2; 
      newsize>=0 && newsize<(minbytes+SUBAREA_ALIGNMENT_BYTES+2*(MIN_VARLENOBJ_SIZE)); 
      newsize=newsize*2) {};
  //printf("OLD SUBAREA SIZE WAS %d NEW SUBAREA SIZE SHOULD BE %d\n",size,newsize);
  if (newsize<MINIMAL_SUBAREA_SIZE) nextel=-1; // wrong size asked for    
  else nextel=init_db_subarea(db,areah,i+1,newsize); // try one or more power-of-two larger area
  if (nextel) {
    //printf("REQUIRED SPACE FAILED, TRYING %d\n",size);
    // required size failed: try last size, if enough for required bytes
    if (size<(minbytes+SUBAREA_ALIGNMENT_BYTES+2*(MIN_VARLENOBJ_SIZE))) {
      show_dballoc_error_nr(db," cannot extend datarec area for a large request of bytes: ",minbytes); 
      return 0; // too many bytes wanted
    }  
    nextel=init_db_subarea(db,areah,i+1,size); // try same size area as last allocated
    if (nextel) {
      show_dballoc_error_nr(db," cannot extend datarec area with a new subarea of size: ",size); 
      return 0; // cannot allocate enough space
    }  
    newsize=size;
  }  
  // here we have successfully allocated a new subarea
  tmp=init_subarea_freespace(db,areah,i+1); // mark beg and end, store new victim
  if (tmp) {  show_dballoc_error(db," cannot initialize new subarea"); return 0; } 
  return newsize;
}  



/** splits a free object into a smaller new object and the remainder, stores remainder to right list
*
* returns 0 if ok, negative nr in case of error
* we assume we always split the first elem in a bucket freelist
* we also assume the remainder is >=MIN_VARLENOBJ_SIZE
*
*/ 

static gint split_free(void* db, void* area_header, gint nr, gint* freebuckets, gint i) {
  gint object;
  gint oldsize;
  gint oldnextptr;
  gint splitsize;
  gint splitobject;
  gint splitindex; 
  gint freelist;
  gint dv;
  gint dvsize;
  gint dvindex;
  db_area_header* areah;
  
  areah=(db_area_header*)area_header; 
  object=freebuckets[i]; // object offset
  oldsize=dbfetch(db,object); // first gint at offset 
  if (!isfreeobject(oldsize)) return -1; // not really a free object!  
  oldsize=getfreeobjectsize(oldsize); // remove free bits, get real size
  // observe object is first obj in freelist, hence no free obj at prevptr
  oldnextptr=dbfetch(db,object+sizeof(gint)); // second gint at offset
  // store new size at offset (beginning of object) and mark as used with used prev
  // observe that a free object cannot follow another free object, hence we know prev is used
  dbstore(db,object,makeusedobjectsizeprevused(nr)); 
  freebuckets[i]=oldnextptr; // store ptr to next elem into bucket ptr
  splitsize=oldsize-nr; // remaining size
  splitobject=object+nr;  // offset of the part left
  // we may store the splitobject as a designated victim instead of a suitable freelist
  // but currently this is disallowed and the underlying code is not really finished:
  // marking of next used object prev-free/prev-used is missing
  // instead of this code we rely on using a newly freed object as dv is larger than dv
  dvsize=freebuckets[DVSIZEBUCKET];
  if (0) { // (splitsize>dvsize) {
    // store splitobj as a new designated victim, but first store current victim to freelist, if possible        
    dv=freebuckets[DVBUCKET];
    if (dv!=0) { 
      if (dvsize<MIN_VARLENOBJ_SIZE) {
        show_dballoc_error(db,"split_free notices corruption: too small designated victim");
        return -1; // error case 
      }         
      dbstore(db,dv,makefreeobjectsize(dvsize)); // store new size with freebits to dv
      dbstore(db,dv+dvsize-sizeof(gint),makefreeobjectsize(dvsize));
      dvindex=wg_freebuckets_index(db,dvsize);
      freelist=freebuckets[dvindex];
      if (freelist!=0) dbstore(db,freelist+2*sizeof(gint),dv); // update prev ptr 
      dbstore(db,dv+sizeof(gint),freelist); // store previous freelist 
      dbstore(db,dv+2*sizeof(gint),dbaddr(db,&freebuckets[dvindex])); // store ptr to previous  
      freebuckets[dvindex]=dv; // store offset to correct bucket
      //printf("PUSHED DV WITH SIZE %d TO FREELIST TO BUCKET %d:\n",dvsize,dvindex);
      //show_bucket_freeobjects(db,freebuckets[dvindex]);      
    }  
    // store splitobj as a new victim
    //printf("REPLACING DV WITH OBJ AT %d AND SIZE %d\n",splitobject,splitsize);
    dbstore(db,splitobject,makespecialusedobjectsize(splitsize)); // length with special used object mark
    dbstore(db,splitobject+sizeof(gint),SPECIALGINT1DV); // marks that it is a dv kind of special object   
    freebuckets[DVBUCKET]=splitobject;
    freebuckets[DVSIZEBUCKET]=splitsize;    
    return 0;
  } else {  
    // store splitobj in a freelist, no changes to designated victim
    dbstore(db,splitobject,makefreeobjectsize(splitsize)); // store new size with freebit to the second half of object
    dbstore(db,splitobject+splitsize-sizeof(gint),makefreeobjectsize(splitsize));
    splitindex=wg_freebuckets_index(db,splitsize); // bucket to store the split remainder 
    if (splitindex<0) return splitindex; // error case
    freelist=freebuckets[splitindex];
    if (freelist!=0) dbstore(db,freelist+2*sizeof(gint),splitobject); // update prev ptr 
    dbstore(db,splitobject+sizeof(gint),freelist); // store previous freelist 
    dbstore(db,splitobject+2*sizeof(gint),dbaddr(db,&freebuckets[splitindex])); // store ptr to previous  
    freebuckets[splitindex]=splitobject; // store remainder offset to correct bucket
    return 0;
  }  
}  

/** returns a correct freebuckets index for a given size of object
*
* returns -1 in case of error, 0,...,EXACBUCKETS_NR+VARBUCKETS_NR-1 otherwise
*
* sizes 0,1,2,...,255 in exactbuckets (say, EXACBUCKETS_NR=256)
* longer sizes in varbuckets:
* sizes 256-511 in bucket 256,
*       512-1023 in bucket 257 etc
* 256*2=512, 512*2=1024, etc
*/

gint wg_freebuckets_index(void* db, gint size) {
  gint i;
  gint cursize;
  
  if (size<EXACTBUCKETS_NR) return size;
  cursize=EXACTBUCKETS_NR*2;
  for(i=0; i<VARBUCKETS_NR; i++) {
    if (size<cursize) return EXACTBUCKETS_NR+i;
    cursize=cursize*2;
  }
  return -1; // too large size, not enough buckets 
}  

/** frees previously alloc_bytes obtained var-length object at offset
*
* returns 0 if ok, negative value if error (likely reason: wrong object ptr)
* merges the freed object with free neighbours, if available, to get larger free objects
* 
*/

gint wg_free_object(void* db, void* area_header, gint object) {
  gint size;
  gint i;
  gint* freebuckets;
  
  gint objecthead;
  gint prevobject;
  gint prevobjectsize;
  gint prevobjecthead; 
  gint previndex;
  gint nextobject;
  gint nextobjecthead; 
  gint nextindex;
  gint freelist;
  gint prevnextptr;
  gint prevprevptr;
  gint nextnextptr;
  gint nextprevptr;
  gint bucketfreelist;
  db_area_header* areah;
  
  gint dv;
  gint dvsize;
  gint tmp;
  
  areah=(db_area_header*)area_header;
  if (!dbcheck(db)) {
    show_dballoc_error(db,"wg_free_object first arg is not a db address");
    return -1;
  }  
  //printf("db %u object %u \n",db,object);
  //printf("freeing object %d with size %d and end %d\n",
  //        object,getusedobjectsize(dbfetch(db,object)),object+getusedobjectsize(dbfetch(db,object)));
  objecthead=dbfetch(db,object);
  if (isfreeobject(objecthead)) {
    show_dballoc_error(db,"wg_free_object second arg is already a free object");
    return -2; // attempting to free an already free object
  }  
  size=getusedobjectsize(objecthead); // size stored at first gint of object
  if (size<MIN_VARLENOBJ_SIZE) { 
    show_dballoc_error(db,"wg_free_object second arg has a too small size");
    return -3; // error: wrong size info (too small)
  }  
  freebuckets=areah->freebuckets;
  
  // first try to merge with the previous free object, if so marked
  if (isnormalusedobjectprevfree(objecthead)) {  
    //printf("**** about to merge object %d on free with prev %d !\n",object,prevobject);    
    // use the size of the previous (free) object stored at the end of the previous object
    prevobjectsize=getfreeobjectsize(dbfetch(db,(object-sizeof(gint))));
    prevobject=object-prevobjectsize;
    prevobjecthead=dbfetch(db,prevobject);
    if (!isfreeobject(prevobjecthead) || !getfreeobjectsize(prevobject)==prevobjectsize) {
      show_dballoc_error(db,"wg_free_object notices corruption: previous object is not ok free object");
      return -4; // corruption noticed
    }      
    // remove prev object from its freelist    
    // first, get necessary information
    prevnextptr=dbfetch(db,prevobject+sizeof(gint));
    prevprevptr=dbfetch(db,prevobject+2*sizeof(gint));    
    previndex=wg_freebuckets_index(db,prevobjectsize);
    freelist=freebuckets[previndex];
    // second, really remove prev object from freelist
    if (freelist==prevobject) {
      // prev object pointed to directly from bucket
      freebuckets[previndex]=prevnextptr;  // modify prev prev
      if (prevnextptr!=0) dbstore(db,prevnextptr+2*sizeof(gint),prevprevptr); // modify prev next      
    } else {
      // prev object pointed to from another object, not directly bucket
      // next of prev of prev will point to next of next
      dbstore(db,prevprevptr+sizeof(gint),prevnextptr); 
      // prev of next of prev will prev-point to prev of prev
      if (prevnextptr!=0) dbstore(db,prevnextptr+2*sizeof(gint),prevprevptr);       
    }    
    // now treat the prev object as the current object to be freed!    
    object=prevobject;
    size=size+prevobjectsize;
  } else if ((freebuckets[DVBUCKET]+freebuckets[DVSIZEBUCKET])==object) {
    // should merge with a previous dv
    object=freebuckets[DVBUCKET];
    size=size+freebuckets[DVSIZEBUCKET]; // increase size to cover dv as well
    // modify dv size information in area header: dv will extend to freed object  
    freebuckets[DVSIZEBUCKET]=size;  
    // store dv size and marker to dv head
    dbstore(db,object,makespecialusedobjectsize(size));
    dbstore(db,object+sizeof(gint),SPECIALGINT1DV);
    return 0;    // do not store anything to freebuckets!!  
  }
  
  // next, try to merge with the next object: either free object or dv
  // also, if next object is normally used instead, mark it as following the free object
  nextobject=object+size;
  nextobjecthead=dbfetch(db,nextobject);
  if (isfreeobject(nextobjecthead)) {
    // should merge with a following free object
    //printf("**** about to merge object %d on free with next %d !\n",object,nextobject);
    size=size+getfreeobjectsize(nextobjecthead); // increase size to cover next object as well
    // remove next object from its freelist    
    // first, get necessary information
    nextnextptr=dbfetch(db,nextobject+sizeof(gint));
    nextprevptr=dbfetch(db,nextobject+2*sizeof(gint));    
    nextindex=wg_freebuckets_index(db,getfreeobjectsize(nextobjecthead));
    freelist=freebuckets[nextindex];
    // second, really remove next object from freelist
    if (freelist==nextobject) {
      // next object pointed to directly from bucket
      freebuckets[nextindex]=nextnextptr;  // modify next prev
      if (nextnextptr!=0) dbstore(db,nextnextptr+2*sizeof(gint),nextprevptr); // modify next next      
    } else {
      // next object pointed to from another object, not directly bucket
      // prev of next will point to next of next
      dbstore(db,nextprevptr+sizeof(gint),nextnextptr); 
      // next of next will prev-point to prev of next
      if (nextnextptr!=0) dbstore(db,nextnextptr+2*sizeof(gint),nextprevptr); 
    }        
  } else if (isspecialusedobject(nextobjecthead) && nextobject==freebuckets[DVBUCKET]) {
    // should merge with a following dv
    size=size+freebuckets[DVSIZEBUCKET]; // increase size to cover next object as well
    // modify dv information in area header
    freebuckets[DVBUCKET]=object;
    freebuckets[DVSIZEBUCKET]=size;  
    // store dv size and marker to dv head
    dbstore(db,object,makespecialusedobjectsize(size));
    dbstore(db,object+sizeof(gint),SPECIALGINT1DV);
    return 0;    // do not store anything to freebuckets!!  
  }  else if (isnormalusedobject(nextobjecthead)) {
    // mark the next used object as following a free object
    dbstore(db,nextobject,makeusedobjectsizeprevfree(dbfetch(db,nextobject)));  
  }  // we do no special actions in case next object is end marker
  
  // maybe the newly freed object is larger than the designated victim? 
  // if yes, use the newly freed object as a new designated victim
  // and afterwards put the old dv to freelist
  if (size>freebuckets[DVSIZEBUCKET]) {
    dv=freebuckets[DVBUCKET];
    dvsize=freebuckets[DVSIZEBUCKET];
    freebuckets[DVBUCKET]=object;
    freebuckets[DVSIZEBUCKET]=size; 
    dbstore(db,object,makespecialusedobjectsize(size));
    dbstore(db,object+sizeof(gint),SPECIALGINT1DV);    
    // set the next used object mark to prev-used!
    nextobject=object+size;
    tmp=dbfetch(db,nextobject);
    if (isnormalusedobject(tmp)) dbstore(db,nextobject,makeusedobjectsizeprevused(tmp));
    // dv handling        
    if (dv==0) return 0; // if no dv actually, then nothing to put to freelist
    // set the object point to dv to make it put into freelist after
    // but first mark the next object after dv as following free
    nextobject=dv+dvsize;
    tmp=dbfetch(db,nextobject);
    if (isnormalusedobject(tmp)) dbstore(db,nextobject,makeusedobjectsizeprevfree(tmp));
    // let the old dv be handled as object to be put to freelist after
    object=dv;
    size=dvsize;    
  }
  // store freed (or freed and merged) object to the correct bucket, 
  // except for dv-merge cases above (returns earlier)    
  i=wg_freebuckets_index(db,size);
  bucketfreelist=freebuckets[i];
  if (bucketfreelist!=0) dbstore(db,bucketfreelist+2*sizeof(gint),object); // update prev ptr
  dbstore(db,object,makefreeobjectsize(size)); // store size and freebit
  dbstore(db,object+size-sizeof(gint),makefreeobjectsize(size)); // store size and freebit
  dbstore(db,object+sizeof(gint),bucketfreelist); // store previous freelist
  dbstore(db,object+2*sizeof(gint),dbaddr(db,&freebuckets[i])); // store prev ptr   
  freebuckets[i]=object;
  return 0;    
}  


/*
Tanel Tammet
http://www.epl.ee/?i=112121212
Kuiv tn 9, Tallinn, Estonia
+3725524876

len |  refcount |   xsd:type |  namespace |  contents .... |

header: 4*4=16 bytes

128 bytes 

*/

/***************** Child database functions ******************/

#if 0
/** Creates child database in parent base
 * returns (db_memsegment_header *) pointer if initialization is successful
 * returns NULL on error.
 * XXX: child database created this way is not freeable
 */
void *wg_create_child_db(void* db, gint size) {  
  db_memsegment_header *dbh, *parent_dbh;
  gint i;
  
  /* Is the requested size reasonable? */
  if(size < sizeof(db_memsegment_header)) {
    show_dballoc_error(parent_dbh, "Requested size too small");
    return NULL;
  }

  /* 1st, examine the parent database to see if
   * the child database fits in the free memory area.
   */
  parent_dbh = (db_memsegment_header*) db;
  if(parent_dbh->size - parent_dbh->free - SUBAREA_ALIGNMENT_BYTES < size) {
    show_dballoc_error(parent_dbh, "Requested size too large");
    return NULL;
  }

  /* Initialize child database */
  dbh = offsettoptr(parent_dbh, parent_dbh->free); /* assume it's aligned */
  if(wg_init_db_memsegment(dbh, parent_dbh->key, size)) {
    show_dballoc_error(parent_dbh, "Initialization of child segment failed");
    return NULL;
  }

  /* Calculate new free pointer for parent database */
  parent_dbh->free = ptrtooffset(parent_dbh, ((char *) dbh)+size);
  i = SUBAREA_ALIGNMENT_BYTES - (parent_dbh->free%SUBAREA_ALIGNMENT_BYTES);
  if(i != SUBAREA_ALIGNMENT_BYTES)
    parent_dbh->free += i;

  /* Set parent offset */
  dbh->parent = ptrtooffset(dbh, parent_dbh);

  return dbh;
}  
#endif

/* Set parent database offset
 *
 * Marks that pointer type data with offsets outside the
 * database belongs to the database with the header at *parent.
 * XXX: there isn't much error checking, so the user must
 * consistently call this function *and* use the same offset
 * when encoding external references.
 */
void wg_set_parent_db(void *db, void *parent) {
  ((db_memsegment_header *) db)->parent = ptrtooffset(db, parent);
}

/* --------------- error handling ------------------------------*/

/** called with err msg when an allocation error occurs
*
*  may print or log an error
*  does not do any jumps etc
*/

static gint show_dballoc_error(void* db, char* errmsg) {
  printf("db memory allocation error: %s\n",errmsg);
  return -1;
} 

/** called with err msg and err nr when an allocation error occurs
*
*  may print or log an error
*  does not do any jumps etc
*/

static gint show_dballoc_error_nr(void* db, char* errmsg, gint nr) {
  printf("db memory allocation error: %s %d\n", errmsg, (int) nr);
  return -1;
}  

/** called with err msg and err nr when an allocation error occurs
*
*  may print or log an error
*  does not do any jumps etc
*/



