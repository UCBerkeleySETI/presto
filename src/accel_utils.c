#include "accel.h"
#include "accelsearch_cmd.h"

#if defined (__GNUC__)
#  define inline __inline__
#else
#  undef inline
#endif

/*#undef USEMMAP*/

#ifdef USEMMAP
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#define NEAREST_INT(x) (int) (x<0 ? x-0.5 : x+0.5)

/* Return 2**n */
#define index_to_twon(n) (1<<n)

/* Return x such that 2**x = n */
static inline int twon_to_index(int n)
{
  int x=0;

  while(n>1){
    n >>= 1;
    x++;
  }
  return x;
}


static inline int calc_required_z(int numharm, int harmnum, double zfull)
/* Calculate the 'z' you need for subharmonic     */
/* 'harmnum' out of 'numharm' subharmonics if the */
/* 'z' at the fundamental harmonic is 'zfull'.    */
{
  double zz;
  
  zz = ACCEL_RDZ * (zfull / numharm) * harmnum;
  return NEAREST_INT(zz) * ACCEL_DZ;
}


static inline double calc_required_r(int numharm, int harmnum, double rfull)
/* Calculate the 'r' you need for subharmonic     */
/* 'harmnum' out of 'numharm' subharmonics if the */
/* 'r' at the fundamental harmonic is 'rfull'.    */
{
  return (int) ((ACCEL_RDR * (rfull / numharm) * harmnum) + 0.5) * ACCEL_DR;
}


static inline int index_from_r(double r, double lor)
/* Return an index for a Fourier Freq given an array that */
/* has stepsize ACCEL_DR and low freq 'lor'.              */
{
  return (int) ((r - lor) * ACCEL_RDR + DBLCORRECT);
}


static inline int index_from_z(double z, double loz)
/* Return an index for a Fourier Fdot given an array that */
/* has stepsize ACCEL_DZ and low freq 'lor'.              */
{
  return (int) ((z - loz) * ACCEL_RDZ + DBLCORRECT);
}


static void compare_rzw_cands(fourierprops * list, int nlist, char *notes)
{
  int ii, jj, kk;
  char tmp[30];

  for (ii = 0; ii < nlist; ii++) {
    for (jj = 0; jj < nlist; jj++) {
      if (ii == jj)
	continue;
      if (fabs(list[ii].r - list[jj].r) < 15.0 && \
	  fabs(list[ii].z - list[jj].z) > 1.0 && \
	  list[ii].pow > list[jj].pow) {
	if (strncmp(notes+jj*20, "                      ", 20)==0){
	  sprintf(tmp, "SL? of Cand %d", ii+1);
	  strncpy(notes+jj*20, tmp, 20);
	}
	continue;
      }
      for (kk = 1; kk < 61; kk++) {
	if ((fabs(list[ii].r - list[jj].r / kk) < list[jj].rerr * 3) && \
	    (fabs(list[ii].z - list[jj].z / kk) < list[jj].zerr * 2)) {
	  if (strncmp(notes+jj*20, "                      ", 20)==0){
	    sprintf(tmp, "H %d of Cand %d", kk, ii+1);
	    strncpy(notes+jj*20, tmp, 20);
	    break;
	  }
	}
      }
    }
  }
}


static int calc_fftlen(int numharm, int harmnum, int max_zfull)
/* The fft length needed to properly process a subharmonic */
{
  int bins_needed, end_effects;

  bins_needed = (ACCEL_USELEN * harmnum) / numharm + 2;
  end_effects = z_resp_halfwidth(calc_required_z(numharm, harmnum, max_zfull), 
				 LOWACC) * 2 * ACCEL_NUMBETWEEN;
  return next2_to_n(bins_needed + end_effects);
}


static void init_kernel(int z, int fftlen, kernel *kern)
{
  int numkern;
  fcomplex *tempkern;

  kern->z = z;
  kern->fftlen = fftlen;
  kern->numbetween = ACCEL_NUMBETWEEN;
  kern->kern_half_width = z_resp_halfwidth((double) z, LOWACC);
  numkern = 2 * kern->numbetween * kern->kern_half_width;
  kern->numgoodbins = kern->fftlen - numkern;
  kern->data = gen_cvect(kern->fftlen);
  tempkern = gen_z_response(0.0, kern->numbetween, kern->z, numkern);
  place_complex_kernel(tempkern, numkern, kern->data, kern->fftlen);
  free(tempkern);
  COMPLEXFFT(kern->data, kern->fftlen, -1);
}


static void free_kernel(kernel *kern)
{
  free(kern->data);
}


static void init_subharminfo(int numharm, int harmnum, 
			     int zmax, subharminfo *shi)
/* Note:  'zmax' is the overall maximum 'z' in the search */
{
  int ii, fftlen, numz_full;

  shi->numharm = numharm;
  shi->harmnum = harmnum;
  shi->zmax = calc_required_z(numharm, harmnum, zmax);
  numz_full = (zmax / ACCEL_DZ) * 2 + 1;
  shi->rinds_init = 0;
  if (numharm > 1){
    shi->rinds = (unsigned short int **) malloc(numz_full * 
						sizeof(unsigned short int *));
    shi->rinds[0] = (unsigned short int *) malloc(numz_full * ACCEL_USELEN * 
						  sizeof(unsigned short int));
    for (ii=1; ii<numz_full; ii++)
      shi->rinds[ii] = shi->rinds[ii-1] + ACCEL_USELEN;
  }
  fftlen = calc_fftlen(numharm, harmnum, zmax); 
  shi->numkern = (shi->zmax / ACCEL_DZ) * 2 + 1;
  shi->kern = (kernel *)malloc(shi->numkern * sizeof(kernel)); 
  for (ii=0; ii<shi->numkern; ii++)
    init_kernel(-shi->zmax+ii*ACCEL_DZ, fftlen, &shi->kern[ii]);
}


subharminfo **create_subharminfos(int numharmstages, int zmax)
{
  int ii, jj, harmtosum;
  subharminfo **shis;

  shis = (subharminfo **)malloc(numharmstages * sizeof(subharminfo *));
  /* Prep the fundamental (actually, the highest harmonic) */
  shis[0] = (subharminfo *)malloc(2 * sizeof(subharminfo));
  init_subharminfo(1, 1, zmax, &shis[0][0]);
  printf("  Harmonic  1/1  has %3d kernel(s) from z = %4d to %4d,  FFT length = %d\n", 
	 shis[0][0].numkern, -shis[0][0].zmax, shis[0][0].zmax,
	 calc_fftlen(1, 1, shis[0][0].zmax));
  /* Prep the sub-harmonics */
  for (ii=1; ii<numharmstages; ii++){
    harmtosum = index_to_twon(ii);
    shis[ii] = (subharminfo *)malloc(harmtosum * sizeof(subharminfo));
    for (jj=1; jj<harmtosum; jj+=2){
      init_subharminfo(harmtosum, jj, zmax, &shis[ii][jj-1]);
      printf("  Harmonic %2d/%-2d has %3d kernel(s) from z = %4d to %4d,  FFT length = %d\n", 
	     jj, harmtosum, shis[ii][jj-1].numkern, 
	     -shis[ii][jj-1].zmax, shis[ii][jj-1].zmax,
	     calc_fftlen(harmtosum, jj, shis[ii][jj-1].zmax));
    }
  }
  return shis;
}


static void free_subharminfo(subharminfo *shi)
{
  int ii;

  for (ii=0; ii<shi->numkern; ii++)
    free_kernel(&shi->kern[ii]);
  if (shi->numharm > 1){
    free(shi->rinds[0]);
    free(shi->rinds);
  }
  free(shi->kern);
}


void free_subharminfos(int numharmstages, subharminfo **shis)
{
  int ii, jj, harmtosum;

  /* Free the sub-harmonics */
  for (ii=1; ii<numharmstages; ii++){
    harmtosum = index_to_twon(ii);
    for (jj=1; jj<harmtosum; jj+=2){
      free_subharminfo(&shis[ii][jj-1]);
    }
    free(shis[ii]);
  }
  /* Free the fundamental */
  free_subharminfo(&shis[0][0]);
  free(shis[0]);
  /* Free the container */
  free(shis);
}


static accelcand *create_accelcand(float power, float sigma, 
				   int numharm, double r, double z)
{
  accelcand *obj;

  obj = (accelcand *)malloc(sizeof(accelcand));
  obj->power = power;
  obj->sigma = sigma;
  obj->numharm = numharm;
  obj->r = r;
  obj->z = z;
  obj->pows = NULL;
  obj->hirs = NULL;
  obj->hizs = NULL;
  obj->derivs = NULL;
  return obj;
}

void free_accelcand(gpointer data, gpointer user_data)
{
  user_data = NULL;
  if (((accelcand *)data)->pows){
    free(((accelcand *)data)->pows);
    free(((accelcand *)data)->hirs);
    free(((accelcand *)data)->hizs);
    free(((accelcand *)data)->derivs);
  }
  free((accelcand *)data);
}


static int compare_accelcand_sigma(gconstpointer ca, gconstpointer cb)
/* Sorts from high to low sigma (ties are sorted by increasing r) */
{
  int result;
  accelcand *a, *b;

  a = (accelcand *) ca;
  b = (accelcand *) cb;
  result = (a->sigma < b->sigma) - (a->sigma > b->sigma);
  if (result) 
    return result;
  else
    return (a->power < b->power) - (a->power > b->power);
}


GSList *sort_accelcands(GSList *list)
/* Sort the candidate list by decreasing sigma */
{
  return g_slist_sort(list, compare_accelcand_sigma);
}


static GSList *insert_new_accelcand(GSList *list, float power, float sigma, 
				    int numharm, double rr, double zz, 
				    int *added)
/* Checks the current list to see if there is already */
/* a candidate within ACCEL_CLOSEST_R bins.  If not,  */
/* it adds it to the list in increasing freq order.   */
{
  GSList *tmp_list=list, *prev_list=NULL, *new_list;
  double prev_diff_r=ACCEL_CLOSEST_R+1.0, next_diff_r;
  
  *added = 0;
  if (!list){
    new_list = g_slist_alloc();
    new_list->data = (gpointer *)create_accelcand(power, sigma, 
						  numharm, rr, zz);
    *added = 1;
    return new_list;
  }

  /* Find the correct position in the list for the candidate */

  while ((tmp_list->next) && 
	 (((accelcand *)(tmp_list->data))->r < rr)){
    prev_list = tmp_list;
    tmp_list = tmp_list->next;
  }
  next_diff_r = fabs(rr - ((accelcand *)(tmp_list->data))->r);
  if (prev_list)
    prev_diff_r = fabs(rr - ((accelcand *)(prev_list->data))->r);

  /* Similar candidate(s) is(are) present */

  if (prev_diff_r < ACCEL_CLOSEST_R){
    /* Overwrite the prev cand */
    if (((accelcand *)(prev_list->data))->sigma < sigma){
      free_accelcand(prev_list->data, NULL);
      prev_list->data = (gpointer *)create_accelcand(power, sigma, 
						     numharm, rr, zz);
      *added = 1;
    }
    if (next_diff_r < ACCEL_CLOSEST_R){
      if (((accelcand *)(tmp_list->data))->sigma < sigma){
	free_accelcand(tmp_list->data, NULL);
	if (*added){
	  /* Remove the next cand */
	  list = g_slist_remove_link(list, tmp_list);
	  g_slist_free_1(tmp_list);
	} else {
	  /* Overwrite the next cand */
	  tmp_list->data = (gpointer *)create_accelcand(power, sigma, 
							numharm, rr, zz);
	  *added = 1;
	}
      }
    }
  } else if (next_diff_r < ACCEL_CLOSEST_R){
    /* Overwrite the next cand */
    if (((accelcand *)(tmp_list->data))->sigma < sigma){
      free_accelcand(tmp_list->data, NULL);
      tmp_list->data = (gpointer *)create_accelcand(power, sigma, 
						    numharm, rr, zz);
      *added = 1;
    }
  } else {  /* This is a new candidate */
    new_list = g_slist_alloc();
    new_list->data = (gpointer *)create_accelcand(power, sigma, 
						  numharm, rr, zz);
    *added = 1;
    if (!tmp_list->next && 
	(((accelcand *)(tmp_list->data))->r < (rr - ACCEL_CLOSEST_R))){
      tmp_list->next = new_list;
      return list;
    }
    if (prev_list){
      prev_list->next = new_list;
      new_list->next = tmp_list;
    } else {
      new_list->next = list;
      return new_list;
    }
  }
  return list;
}


GSList *eliminate_harmonics(GSList *cands, int *numcands)
/* Eliminate obvious but less-significant harmonically-related candidates */
{
  GSList *currentptr, *otherptr, *toocloseptr;
  accelcand *current_cand, *other_cand;
  int ii, maxharm=16;
  double tooclose=1.5;
  
  currentptr = cands;
  while (currentptr->next){
    current_cand = (accelcand *)(currentptr->data);
    otherptr = currentptr->next;
    do {
      int remove=0;
      other_cand = (accelcand *)(otherptr->data);
      for (ii=1; ii<=maxharm; ii++){
	if (fabs(current_cand->r/ii - other_cand->r) < tooclose){
	  remove = 1;
	  break;
	}
      }
      if (remove==0){
	for (ii=1; ii<=maxharm; ii++){
	  if (fabs(current_cand->r*ii - other_cand->r) < tooclose){
	    remove = 1;
	    break;
	  }
	}
      }
      /* Check a few other common harmonic ratios  */
      /* Hopefully this isn't being overzealous... */
      if (remove==0 && 
	  ((fabs(current_cand->r*3.0/2.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*5.0/2.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*2.0/3.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*4.0/3.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*5.0/3.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*3.0/4.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*5.0/4.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*2.0/5.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*3.0/5.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*4.0/5.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*5.0/6.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*2.0/7.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*3.0/7.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*4.0/7.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*3.0/8.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*5.0/8.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*2.0/9.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*3.0/10.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*2.0/11.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*3.0/11.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*2.0/13.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*3.0/13.0 - other_cand->r) < tooclose) ||
	   (fabs(current_cand->r*2.0/15.0 - other_cand->r) < tooclose))){
	remove = 1;
      }
      /* Remove the "other" cand */
      if (remove){
	toocloseptr = otherptr;
	otherptr = otherptr->next;
	free_accelcand(other_cand, NULL);
	cands = g_slist_remove_link(cands, toocloseptr);
	g_slist_free_1(toocloseptr);
	*numcands = *numcands-1;
      } else {
	otherptr = otherptr->next;
      }
    } while (otherptr);
    if (currentptr->next)
      currentptr = currentptr->next;
  }
  return cands;
}



void optimize_accelcand(accelcand *cand, accelobs *obs)
{
  int ii;
  
  cand->pows = gen_dvect(cand->numharm);
  cand->hirs = gen_dvect(cand->numharm);
  cand->hizs = gen_dvect(cand->numharm);
  cand->derivs = (rderivs *)malloc(sizeof(rderivs)*cand->numharm);
  for (ii=0; ii<cand->numharm; ii++){
    if (obs->mmap_file || obs->dat_input)
      cand->pows[ii] = max_rz_arr(obs->fft,
				  obs->numbins,
				  cand->r * (ii + 1) - obs->lobin,
				  cand->z * (ii + 1), 
				  &(cand->hirs[ii]), 
				  &(cand->hizs[ii]), 
				  &(cand->derivs[ii]));
    else
      cand->pows[ii] = max_rz_file(obs->fftfile, 
				   cand->r * (ii + 1) - obs->lobin,
				   cand->z * (ii + 1), 
				   &(cand->hirs[ii]), 
				   &(cand->hizs[ii]), 
				   &(cand->derivs[ii]));
    cand->hirs[ii] += obs->lobin;
  }
  cand->sigma = candidate_sigma(cand->power, cand->numharm, 
				obs->numindep[twon_to_index(cand->numharm)]);
}


static void center_string(char *outstring, char *instring, int width)
{
  int len;
  char *tmp;

  len = strlen(instring);
  if (width < len){
    printf("\nwidth < len (%d) in center_string(outstring, '%s', width=%d)\n",
	   len, instring, width);
  }
  tmp = memset(outstring, ' ', width);
  outstring[width] = '\0';
  if (len >= width){
    strncpy(outstring, instring, width);
  } else {
    strncpy(outstring+(width-len)/2, instring, len);
  }
}


static void write_val_with_err(FILE *outfile, double val, double err, 
			       int numerr, int width)
{
  int retval;
  char tmpstr[30], ctrstr[30];

  if (numerr==1)
    retval = nice_output_1(tmpstr, val, err, 0);
  else if (numerr==2)
    retval = nice_output_2(tmpstr, val, err, 0);
  else
    printf("\numerr = %d is out-of-range (1-2) in write_val_with_err()\n",
	   numerr);
  center_string(ctrstr, tmpstr, width);
  fprintf(outfile, "%s  ", ctrstr);
}


void output_fundamentals(fourierprops *props, GSList *list, 
			 accelobs *obs, infodata *idata)
{
  double accel=0.0, accelerr=0.0, coherent_pow;
  int ii, jj, numcols=12, numcands, *width, *error;
  int widths[12]={4, 5, 6, 8, 4, 16, 15, 15, 15, 11, 15, 20};
  int errors[12]={0, 0, 0, 0, 0,  1,  1,  2,  1,  2,  2,  0};
  char tmpstr[30], ctrstr[30], *notes;
  accelcand *cand;
  GSList *listptr;
  rzwerrs errs;
  static char **title;
  static char *titles1[]={"", "", "Summed", "Coherent", "Num", "Period", 
			  "Frequency", "FFT 'r'", "Freq Deriv", "FFT 'z'", 
			  "Accel", ""};
  static char *titles2[]={"Cand", "Sigma", "Power", "Power", "Harm", "(ms)",
			  "(Hz)", "(bin)", "(Hz/s)", "(bins)", 
			  "(m/s)", "Notes"};

  numcands = g_slist_length(list);
  listptr = list;

  /* Close the old work file and open the cand file */
  
  fclose(obs->workfile);
  obs->workfile = chkfopen(obs->accelnm, "w");
  
  /* Set our candidate notes to all spaces */

  notes = (char *)malloc(numcands * widths[numcols-1]);
  memset(notes, ' ', numcands * widths[numcols-1]);
  
  /* Compare the candidates with the pulsar database */

  if (dms2rad(idata->ra_h, idata->ra_m, idata->ra_s) != 0.0 && 
      hms2rad(idata->dec_d, idata->dec_m, idata->dec_s) != 0.0){
    for (ii=0; ii<numcands; ii++){
      comp_psr_to_cand(props + ii, idata, notes + ii * 20, 0);
    }
  }

  /* Compare the candidates with themselves */

  compare_rzw_cands(props, numcands, notes);

  /* Print the header */
  
  width = widths; title = titles1;
  for (ii=0; ii<numcols-1; ii++){
    center_string(ctrstr, *title++, *width++);
    fprintf(obs->workfile, "%s  ", ctrstr);
  }
  center_string(ctrstr, *title++, *width++);
  fprintf(obs->workfile, "%s\n", ctrstr);

  width = widths; title = titles2;
  for (ii=0; ii<numcols-1; ii++){
    center_string(ctrstr, *title++, *width++);
    fprintf(obs->workfile, "%s  ", ctrstr);
  }
  center_string(ctrstr, *title++, *width++);
  fprintf(obs->workfile, "%s\n", ctrstr);

  width = widths;
  for (ii=0; ii<numcols-1; ii++){
    memset(tmpstr, '-', *width);
    tmpstr[*width++] = '\0';
    fprintf(obs->workfile, "%s--", tmpstr);
  }
  memset(tmpstr, '-', *width++);
  tmpstr[widths[ii]] = '\0';
  fprintf(obs->workfile, "%s\n", tmpstr);
  
  /* Print the fundamentals */
  
  for (ii=0; ii<numcands; ii++){
    width = widths;  error = errors;
    cand = (accelcand *)(listptr->data);
    calc_rzwerrs(props+ii, obs->T, &errs);
    
    { /* Calculate the coherently summed power */
      double coherent_r=0.0, coherent_i=0.0;
      double phs0, phscorr, amp;
      rderivs harm;

      phs0 = cand->derivs[0].phs;
      for (jj=0; jj<cand->numharm; jj++){
	harm = cand->derivs[jj];
	if (obs->nph > 0.0)
	  amp = sqrt(harm.pow / obs->nph);
	else
	  amp = sqrt(harm.pow / harm.locpow);
	phscorr = phs0 - fmod((jj+1.0)*phs0, TWOPI);
	coherent_r += amp * cos(harm.phs+phscorr);
	coherent_i += amp * sin(harm.phs+phscorr);
      }
      coherent_pow = coherent_r * coherent_r + coherent_i * coherent_i;
    }

    sprintf(tmpstr, "%-4d", ii+1);
    center_string(ctrstr, tmpstr, *width++); error++;
    fprintf(obs->workfile, "%s  ", ctrstr);

    sprintf(tmpstr, "%.2f", cand->sigma);
    center_string(ctrstr, tmpstr, *width++); error++;
    fprintf(obs->workfile, "%s  ", ctrstr);

    sprintf(tmpstr, "%.2f", cand->power);
    center_string(ctrstr, tmpstr, *width++); error++;
    fprintf(obs->workfile, "%s  ", ctrstr);

    sprintf(tmpstr, "%.2f", coherent_pow);
    center_string(ctrstr, tmpstr, *width++); error++;
    fprintf(obs->workfile, "%s  ", ctrstr);

    sprintf(tmpstr, "%d", cand->numharm);
    center_string(ctrstr, tmpstr, *width++); error++;
    fprintf(obs->workfile, "%s  ", ctrstr);

    write_val_with_err(obs->workfile, errs.p*1000.0, errs.perr*1000.0, 
		       *error++, *width++);
    write_val_with_err(obs->workfile, errs.f, errs.ferr, 
		       *error++, *width++);
    write_val_with_err(obs->workfile, props[ii].r, props[ii].rerr, 
		       *error++, *width++);
    write_val_with_err(obs->workfile, errs.fd, errs.fderr, 
		       *error++, *width++);
    write_val_with_err(obs->workfile, props[ii].z, props[ii].zerr, 
		       *error++, *width++);
    accel = props[ii].z * SOL / (obs->T * obs->T * errs.f);
    accelerr = props[ii].zerr * SOL / (obs->T * obs->T * errs.f);
    write_val_with_err(obs->workfile, accel, accelerr, 
		       *error++, *width++);
    fprintf(obs->workfile, "  %.20s\n", notes + ii * 20);
    fflush(obs->workfile);
    listptr = listptr->next;
  }
  fprintf(obs->workfile, "\n\n");
  free(notes);
}


void output_harmonics(GSList *list, accelobs *obs, infodata *idata)
{
  int ii, jj, numcols=13, numcands;
  int widths[13]={5, 4, 5, 15, 11, 18, 13, 12, 9, 12, 10, 10, 20};
  int errors[13]={0, 0, 0,  2,  0,  2,  0,  2, 0,  2,  2,  2, 0};
  char tmpstr[30], ctrstr[30], command[200], notes[21];
  accelcand *cand;
  GSList *listptr;
  fourierprops props;
  rzwerrs errs;
  static char *titles1[]={"", "", "", "Power /", "Raw", 
			  "FFT 'r'", "Pred 'r'", "FFT 'z'", "Pred 'z'", 
			  "Phase", "Centroid", "Purity", ""};
  static char *titles2[]={"Cand", "Harm", "Sigma", "Loc Pow", "Power", 
			  "(bin)", "(bin)", "(bins)", "(bins)", 
			  "(rad)", "(0-1)", "<p> = 1", "Notes"};

  numcands = g_slist_length(list);
  listptr = list;

  /* Print the header */
  
  for (ii=0; ii<numcols-1; ii++){
    center_string(ctrstr, titles1[ii], widths[ii]);
    fprintf(obs->workfile, "%s  ", ctrstr);
  }
  center_string(ctrstr, titles1[ii], widths[ii]);
  fprintf(obs->workfile, "%s\n", ctrstr);
  for (ii=0; ii<numcols-1; ii++){
    if (obs->nph > 0.0 && ii==3) /*  HAAACK!!! */
      center_string(ctrstr, "NumPhot", widths[ii]);
    else 
      center_string(ctrstr, titles2[ii], widths[ii]);
    fprintf(obs->workfile, "%s  ", ctrstr);
  }
  center_string(ctrstr, titles2[ii], widths[ii]);
  fprintf(obs->workfile, "%s\n", ctrstr);
  for (ii=0; ii<numcols-1; ii++){
    memset(tmpstr, '-', widths[ii]);
    tmpstr[widths[ii]] = '\0';
    fprintf(obs->workfile, "%s--", tmpstr);
  }
  memset(tmpstr, '-', widths[ii]);
  tmpstr[widths[ii]] = '\0';
  fprintf(obs->workfile, "%s\n", tmpstr);
  
  /* Print the fundamentals */
  
  for (ii=0; ii<numcands; ii++){
    cand = (accelcand *)(listptr->data);
    for (jj=0; jj<cand->numharm; jj++){
      if (obs->nph > 0.0){
	double tmp_locpow;

	tmp_locpow = cand->derivs[jj].locpow;
	cand->derivs[jj].locpow = obs->nph;
	calc_props(cand->derivs[jj], cand->hirs[jj], 
		   cand->hizs[jj], 0.0, &props);
	cand->derivs[jj].locpow = tmp_locpow;
      } else {
	calc_props(cand->derivs[jj], cand->hirs[jj], 
		   cand->hizs[jj], 0.0, &props);
      }
      calc_rzwerrs(&props, obs->T, &errs);
      comp_psr_to_cand(&props, idata, notes, 0);
      if (jj==0) sprintf(tmpstr, " %-4d", ii+1);
      else sprintf(tmpstr, "     ");
      center_string(ctrstr, tmpstr, widths[0]);
      fprintf(obs->workfile, "%s  ", ctrstr);
      sprintf(tmpstr, "%-4d", jj+1);
      center_string(ctrstr, tmpstr, widths[1]);
      fprintf(obs->workfile, "%s  ", ctrstr);
      sprintf(tmpstr, "%.2f", candidate_sigma(props.pow, 1, 1));
      center_string(ctrstr, tmpstr, widths[2]);
      fprintf(obs->workfile, "%s  ", ctrstr);
      write_val_with_err(obs->workfile, props.pow, props.powerr, 
			 errors[3], widths[3]);
      sprintf(tmpstr, "%.3g", props.rawpow);
      center_string(ctrstr, tmpstr, widths[4]);
      fprintf(obs->workfile, "%s  ", ctrstr);
      write_val_with_err(obs->workfile, props.r, props.rerr, 
			 errors[5], widths[5]);
      sprintf(tmpstr, "%.2f", cand->r * (jj+1));
      center_string(ctrstr, tmpstr, widths[6]);
      fprintf(obs->workfile, "%s  ", ctrstr);
      write_val_with_err(obs->workfile, props.z, props.zerr, 
			 errors[7], widths[7]);
      sprintf(tmpstr, "%.2f", cand->z * (jj+1));
      center_string(ctrstr, tmpstr, widths[8]);
      fprintf(obs->workfile, "%s  ", ctrstr);
      write_val_with_err(obs->workfile, props.phs, props.phserr, 
			 errors[9], widths[9]);
      write_val_with_err(obs->workfile, props.cen, props.cenerr, 
			 errors[10], widths[10]);
      write_val_with_err(obs->workfile, props.pur, props.purerr, 
			 errors[11], widths[11]);
      fprintf(obs->workfile, "  %.20s\n", notes);
      fflush(obs->workfile);
    }     
    listptr = listptr->next;
  }
  fprintf(obs->workfile, "\n\n");
  fclose(obs->workfile);
  sprintf(command, "cat %s.inf >> %s", obs->rootfilenm, obs->accelnm);
  system(command);
}


void print_accelcand(gpointer data, gpointer user_data)
{
  accelcand *obj=(accelcand *)data;

  user_data = NULL;
  printf("sigma: %-7.4f  pow: %-7.2f  harm: %-2d  r: %-14.4f  z: %-10.4f\n", 
	 obj->sigma, obj->power, obj->numharm, obj->r, obj->z); 
}


fcomplex *get_fourier_amplitudes(int lobin, int numbins, accelobs *obs)
{
  if (obs->mmap_file || obs->dat_input)
    return (fcomplex *)obs->fft+(lobin-obs->lobin);
  else
    return read_fcomplex_file(obs->fftfile, lobin-obs->lobin, numbins);
}


ffdotpows *subharm_ffdot_plane(int numharm, int harmnum,
			       double fullrlo, double fullrhi, 
			       subharminfo *shi, accelobs *obs)
{
  int ii, lobin, hibin, numdata, nrs, fftlen, binoffset;
  static int numrs_full=0, numzs_full=0;
  float powargr, powargi;
  double drlo, drhi, norm;
  ffdotpows *ffdot;
  fcomplex *data, **result;
  presto_datainf datainf;

  if (numrs_full==0){
    if (numharm==1 && harmnum==1){
      numrs_full = ACCEL_USELEN;
      numzs_full = shi->numkern;
    } else {
      printf("You must call subharm_ffdot_plane() with numharm=1 and\n");
      printf("harnum=1 before you use other values!  Exiting.\n\n");
      exit(0);
    }
  }
  ffdot = (ffdotpows *)malloc(sizeof(ffdotpows));

  /* Calculate and get the required amplitudes */

  drlo = calc_required_r(numharm, harmnum, fullrlo);
  drhi = calc_required_r(numharm, harmnum, fullrhi);
  ffdot->rlo = (int) floor(drlo);
  ffdot->zlo = calc_required_z(numharm, harmnum, obs->zlo);
  if (shi->rinds_init==0 && numharm > 1){
    int jj;
    double rr, subr;
    for (ii=0; ii<numzs_full; ii++){
      for (jj=0; jj<numrs_full; jj++){
	rr = fullrlo + jj * ACCEL_DR; 
	subr = calc_required_r(numharm, harmnum, rr);
	shi->rinds[ii][jj] = index_from_r(subr, ffdot->rlo);
      }
    }
    shi->rinds_init = 1;
  }
  ffdot->rinds = shi->rinds;
  ffdot->numrs = (int) ((ceil(drhi) - floor(drlo)) 
			* ACCEL_RDR + DBLCORRECT) + 1;
  if (numharm==1 && harmnum==1){
    ffdot->numrs = ACCEL_USELEN;
  } else {
    if (ffdot->numrs % ACCEL_RDR){
      ffdot->numrs = (ffdot->numrs / ACCEL_RDR + 1) * ACCEL_RDR;
    }
  }
  ffdot->numzs = shi->numkern;
  binoffset = shi->kern[0].kern_half_width;
  fftlen = shi->kern[0].fftlen;
  lobin = ffdot->rlo - binoffset;
  hibin = (int) ceil(drhi) + binoffset;
  numdata = hibin - lobin + 1;
  data = get_fourier_amplitudes(lobin-obs->lobin, numdata, obs);

  /* Determine the mean local power level (via median) */

  if (obs->nph > 0.0){ /* Unless freq 0 normalization is requested */
    norm = 1.0 / obs->nph;
  } else {
    float *powers;
    
    powers = gen_fvect(numdata);
    for (ii=0; ii<numdata; ii++) 
      powers[ii] = POWER(data[ii].r, data[ii].i);
    norm = 1.0 / (1.442695 * median(powers, numdata));
    free(powers);
  }

  /* Perform the correlations */

  result = gen_cmatrix(ffdot->numzs, ffdot->numrs);
  datainf = RAW;
  for (ii=0; ii<ffdot->numzs; ii++){
    nrs = corr_complex(data, numdata, datainf, \
		       shi->kern[ii].data, fftlen, FFT, \
		       result[ii], ffdot->numrs, binoffset, \
		       ACCEL_NUMBETWEEN, binoffset, CORR);
    datainf = SAME;
  }
  if (!obs->mmap_file && !obs->dat_input)
    free(data);

  /* Convert the amplitudes to normalized powers */

  ffdot->powers = gen_fmatrix(ffdot->numzs, ffdot->numrs);
  for (ii=0; ii<(ffdot->numzs*ffdot->numrs); ii++)
    ffdot->powers[0][ii] = POWER(result[0][ii].r, result[0][ii].i) * norm;
  free(result[0]);
  free(result);
  return ffdot;
}


ffdotpows *copy_ffdotpows(ffdotpows *orig)
{
  int ii;
  ffdotpows *copy;

  copy = (ffdotpows *)malloc(sizeof(ffdotpows));
  copy->numrs = orig->numrs;
  copy->numzs = orig->numzs;
  copy->rlo = orig->rlo;
  copy->zlo = orig->zlo;
  copy->powers = gen_fmatrix(orig->numzs, orig->numrs);
  for (ii=0; ii<(orig->numzs * orig->numrs); ii++)
    copy->powers[0][ii] = orig->powers[0][ii];
  return copy;
}

void free_ffdotpows(ffdotpows *ffd)
{
  free(ffd->powers[0]);
  free(ffd->powers);
  free(ffd);
}


void add_ffdotpows(ffdotpows *fundamental,
		   ffdotpows *subharmonic, 
		   int numharm, int harmnum)
{
  int ii, jj, zz, rind, zind, subz, lastrind=-1;
  float lastpow=0;
  
  for (ii=0; ii<fundamental->numzs; ii++){
    zz = fundamental->zlo + ii * ACCEL_DZ; 
    subz = calc_required_z(numharm, harmnum, zz);
    zind = index_from_z(subz, subharmonic->zlo);
    for (jj=0; jj<fundamental->numrs; jj++){
      rind = subharmonic->rinds[ii][jj];
      if (rind!=lastrind)
	lastpow = subharmonic->powers[zind][rind];
      fundamental->powers[ii][jj] += lastpow;
      lastrind = rind;
    }
  }
}


GSList *search_ffdotpows(ffdotpows *ffdot, int numharm, 
			 accelobs *obs, GSList *cands)
{
  int ii, jj;
  float powcut;
  long long numindep;
  
  powcut = obs->powcut[twon_to_index(numharm)];
  numindep = obs->numindep[twon_to_index(numharm)];

  for (ii=0; ii<ffdot->numzs; ii++){
    for (jj=0; jj<ffdot->numrs; jj++){
      if (ffdot->powers[ii][jj] > powcut){
	float pow, sig;
	double rr, zz;
	int added=0;

	pow = ffdot->powers[ii][jj];
	sig = candidate_sigma(pow, numharm, numindep);
	rr = (ffdot->rlo + jj * ACCEL_DR) / numharm;
	zz = (ffdot->zlo + ii * ACCEL_DZ) / numharm;
	cands = insert_new_accelcand(cands, pow, sig, numharm, 
				     rr, zz, &added);
	if (added)
	  fprintf(obs->workfile,
		  "%-7.2f  %-7.4f  %-2d  %-14.4f  %-14.9f  %-10.4f\n", 
		  pow, sig, numharm, rr, rr / obs->T, zz);
      }
    }
  }
  return cands;
}

void create_accelobs(accelobs *obs, infodata *idata, 
		     Cmdline *cmd, int usemmap)
{
  int ii, rootlen, input_shorts=0;

  {
    int hassuffix=0;
    char *suffix;
    
    hassuffix = split_root_suffix(cmd->argv[0], 
				  &(obs->rootfilenm), &suffix);
    if (hassuffix){
      if (strcmp(suffix, "fft")!=0 &&
	  strcmp(suffix, "dat")!=0 &&
	  strcmp(suffix, "sdat")!=0){
	printf("\nInput file ('%s') must be an '.fft' or '.[s]dat' file!\n\n",
	       cmd->argv[0]);
	free(suffix);
	exit(0);
      }
      /* If the input file is a time series */
      if (strcmp(suffix, "dat")==0 ||
	  strcmp(suffix, "sdat")==0){
	obs->dat_input = 1;
	obs->mmap_file = 0;
	if (strcmp(suffix, "sdat")==0) input_shorts = 1;
      } else {
	obs->dat_input = 0;
      }
      free(suffix);
    } else {
      printf("\nInput file ('%s') must be an '.fft' or '.[s]dat' file!\n\n",
	     cmd->argv[0]);
      exit(0);
    }
  }
  
  /* Read the info file */

  readinf(idata, obs->rootfilenm);
  if (idata->object) {
    printf("Analyzing %s data from '%s'.\n\n", 
	   remove_whitespace(idata->object), cmd->argv[0]);
  } else {
    printf("Analyzing data from '%s'.\n\n", cmd->argv[0]);
  }

  /* Prepare the input time series if required */

  if (obs->dat_input){
    FILE *datfile;
    long long filelen;
    float *ftmp;

    printf("Reading and FFTing the time series...");
    fflush(NULL);
    datfile = chkfopen(cmd->argv[0], "rb");

    /* Check the length of the file to see if we can handle it */
    filelen = chkfilelen(datfile, sizeof(float));
    if (input_shorts) filelen *= 2;
    if (filelen > 67108864){ /* Small since we need memory for the templates */
      printf("\nThe input time series is too large.  Use 'realfft' first.\n\n");
      exit(0);
    }

    /* Read the time series into a temporary buffer */
    if (input_shorts){
      short *stmp = gen_svect(filelen);
      ftmp = gen_fvect(filelen);
      chkfread(stmp, sizeof(short), filelen, datfile);
      for (ii=0; ii<filelen; ii++)
	ftmp[ii] = (float)stmp[ii];
      free(stmp);
    } else {
      ftmp = read_float_file(datfile, 0, filelen);
    }
    fclose(datfile);

    /* FFT it */
    realfft(ftmp, filelen, -1);
    obs->fftfile = NULL;
    obs->fft = (fcomplex *)ftmp;
    obs->numbins = filelen/2;
    printf("done.\n\n");
  }

  /* Determine the output filenames */

  rootlen = strlen(obs->rootfilenm)+25;
  obs->candnm = (char *)calloc(rootlen, 1);
  obs->accelnm = (char *)calloc(rootlen, 1);
  obs->workfilenm = (char *)calloc(rootlen, 1);
  sprintf(obs->candnm, "%s_ACCEL_%d.cand", 
	  obs->rootfilenm, cmd->zmax);
  sprintf(obs->accelnm, "%s_ACCEL_%d", 
	  obs->rootfilenm, cmd->zmax);
  sprintf(obs->workfilenm, "%s_ACCEL_%d.txtcand", 
	  obs->rootfilenm, cmd->zmax);

  /* Open the FFT file if it exists appropriately */
  if (!obs->dat_input){
    obs->fftfile = chkfopen(cmd->argv[0], "rb");
    obs->numbins = chkfilelen(obs->fftfile, sizeof(fcomplex));
    if (usemmap){
      fclose(obs->fftfile);
      obs->fftfile = NULL;
      printf("Memory mapping the input FFT.  This may take a while...\n");
      obs->mmap_file = open(cmd->argv[0], O_RDONLY);
      if (obs->mmap_file == -1){
	perror("\nError in open() in accel_utils.c");
	printf("\n");
	exit(-1);
      }
      obs->fft = (fcomplex *)mmap(0, sizeof(fcomplex)*obs->numbins, PROT_READ, 
				  MAP_SHARED, obs->mmap_file, 0);
    } else {
      obs->mmap_file = 0;
    }
  }

  /* Determine the other parameters */

  if (cmd->zmax % ACCEL_DZ)
    cmd->zmax = (cmd->zmax / ACCEL_DZ + 1) * ACCEL_DZ;
  obs->workfile = chkfopen(obs->workfilenm, "w");
  obs->N = (long long) idata->N;
  if (cmd->photonP){
    if (obs->mmap_file || obs->dat_input){
      obs->nph = obs->fft[0].r;
    } else {
      obs->nph = get_numphotons(obs->fftfile);
    }
    printf("Normalizing powers using %.0f photons.\n\n", obs->nph);
  } else 
    obs->nph = 0.0;
  obs->lobin = cmd->lobin;
  if (obs->lobin > 0){
    obs->nph = 0.0;
    if (cmd->lobin > obs->numbins - 1) {
      printf("\n'lobin' is greater than the total number of\n");
      printf("   frequencies in the data set.  Exiting.\n\n");
      exit(1);
    }
  }
  if (cmd->numharm != 1 &&
      cmd->numharm != 2 &&
      cmd->numharm != 4 &&
      cmd->numharm != 8 &&
      cmd->numharm != 16){
      printf("\n'numharm' = %d must be a power-of-two!  Exiting\n\n", 
	     cmd->numharm);
      exit(1);
  }
  obs->numharmstages = twon_to_index(cmd->numharm)+1;
  obs->dz = ACCEL_DZ;
  obs->numz = cmd->zmax * 2 + 1;
  obs->numbetween = ACCEL_NUMBETWEEN;
  obs->dt = idata->dt;
  obs->T = idata->dt * idata->N;
  if (cmd->floP){
    obs->rlo = floor(cmd->flo * obs->T);
    if (obs->rlo < obs->lobin) 
      obs->rlo = obs->lobin;
    if (obs->rlo > obs->numbins - 1) {
      printf("\nLow frequency to search 'flo' is greater than\n");
      printf("   the highest available frequency.  Exiting.\n\n");
      exit(1);
    }
  } else {
    if (cmd->rloP)
      obs->rlo = cmd->rlo;
    else
      obs->rlo = 1.0;
    if (obs->rlo < obs->lobin) 
      obs->rlo = obs->lobin;
    if (obs->rlo > obs->numbins - 1) {
      printf("\nLow frequency to search 'rlo' is greater than\n");
      printf("   the available number of points.  Exiting.\n\n");
      exit(1);
    }
  }
  obs->highestbin = obs->numbins - 1;
  if (cmd->fhiP){
    obs->highestbin = ceil(cmd->fhi * obs->T);
    if (obs->highestbin > obs->numbins - 1) 
      obs->highestbin = obs->numbins - 1;
    obs->rhi = obs->highestbin;
    if (obs->highestbin < obs->rlo){
      printf("\nHigh frequency to search 'fhi' is less than\n");
      printf("   the lowest frequency to search 'flo'.  Exiting.\n\n");
      exit(1);
    }
  } else if (cmd->rhiP){
    obs->highestbin = cmd->rhi;
    if (obs->highestbin > obs->numbins - 1) 
      obs->highestbin = obs->numbins - 1;
    obs->rhi = obs->highestbin;
    if (obs->highestbin < obs->rlo){
      printf("\nHigh frequency to search 'rhi' is less than\n");
      printf("   the lowest frequency to search 'rlo'.  Exiting.\n\n");
      exit(1);
    }
  }
  obs->dr = ACCEL_DR;
  obs->zhi = cmd->zmax;
  obs->zlo = -cmd->zmax;
  obs->sigma = cmd->sigma;
  obs->powcut = (float *)malloc(obs->numharmstages * sizeof(float));
  obs->numindep = (long long *)malloc(obs->numharmstages * sizeof(long long));
  for (ii=0; ii<obs->numharmstages; ii++){
    if (obs->numz==1)
      obs->numindep[ii] = (obs->rhi - obs->rlo) / index_to_twon(ii);
    else
      /* The numz+1 takes care of the small amount of  */
      /* search we get above zmax and below zmin.      */
      obs->numindep[ii] = (obs->rhi - obs->rlo) * (obs->numz + 1) * 
	(obs->dz / 6.95) / index_to_twon(ii);
    obs->powcut[ii] = power_for_sigma(obs->sigma, 
				      index_to_twon(ii), 
				      obs->numindep[ii]);
  }
  obs->numzap = 0;
  /*
  if (zapfile!=NULL)
    obs->numzap = get_birdies(cmd->zapfile, obs->T, obs->baryv, 
			      &(obs->lobins), &(obs->hibins));
  else
    obs->numzap = 0;
  */
}


void free_accelobs(accelobs *obs)
{
  if (obs->mmap_file)
    close(obs->mmap_file);
  else if (obs->dat_input)
    free(obs->fft);
  else
    fclose(obs->fftfile);
  free(obs->powcut);
  free(obs->numindep);
  free(obs->rootfilenm);
  free(obs->candnm);
  free(obs->accelnm);
  free(obs->workfilenm);
  if (obs->numzap){
    free(obs->lobins);
    free(obs->hibins);
  }
}
