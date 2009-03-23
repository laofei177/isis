/* -*- mode: C; mode: fold -*- */

/*  This file is part of ISIS, the Interactive Spectral Interpretation System
    Copyright (C) 1998-2008 Massachusetts Institute of Technology

    Author:  John C. Houck <houck@space.mit.edu>

    This software was developed by the MIT Center for Space Research under
    contract SV1-61010 from the Smithsonian Institution.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*{{{ Includes */

#include "config.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <signal.h>

#ifdef HAVE_STDLIB_H
# include <stdlib.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif

#ifdef HAVE_SYS_STAT_H
#  include <sys/stat.h>
#endif

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include <slang.h>

#include "cfortran.h"
#include "isis.h"

#define FEWER_CFORTRAN_COMPILER_WARNINGS \
        (void)c2fstrv; (void)f2cstrv; (void)kill_trailing; (void)vkill_trailing; \
        (void)num_elem

/*}}}*/

typedef struct _Xspec_Param_t
{
   union {double *d; float *f;} ear;
   union {double *d; float *f;} param;
   union {double *d; float *f;} photar;
   union {double *d; float *f;} photer;
   int ne;
   int ifl;
   char *filename;
}
Xspec_Param_t;

typedef void Xspec_Fun_t (Xspec_Param_t *p);

/*{{{ Eval */

/*
 * NIST 1998 CODATA recommended values of physical constants:
 */

#define PLANCK      ((double) 6.62606876e-27)       /* Planck's constant (erg s) */
#define CLIGHT      ((double) 2.99792458e10)        /* speed of light (cm/s) */
#define ERG_PER_EV  ((double) 1.602176462e-12)
#define KEV_ANGSTROM (((PLANCK * CLIGHT) / (ERG_PER_EV * 1.e3)) * 1.e8)

#define TOL  (10 * FLT_MIN)

static char *Table_Model_Filename = NULL;
static char *Xspec_Model_Names_File = NULL;
static int Xspec_Version;

static volatile sig_atomic_t Signal_In_Progress;
static void sig_segv (int signo) /*{{{*/
{
   static char msg[] =
"\n**** XSPEC is buggy:  Segmentation Fault while in an XSPEC function.\n";

   (void) signo;
   if (Signal_In_Progress)
     return;
   Signal_In_Progress = 1;
   write (STDERR_FILENO, msg, sizeof(msg));
   /* so more SEGVs won't interfere with exit() */
   SLsignal (SIGSEGV, SIG_DFL);
   exit (EXIT_FAILURE);
}

/*}}}*/

static void call_xspec_fun (Xspec_Fun_t *fun, Xspec_Param_t *p) /*{{{*/
{
   SLSig_Fun_Type *sig_func;

   sig_func = SLsignal (SIGSEGV, sig_segv);
   if (SIG_ERR == sig_func)
     fprintf (stderr, "warning:  failed initializing signal handler for SIGSEGV\n");

   (*fun)(p);

   if (SLsignal (SIGSEGV, sig_func) == SIG_ERR)
     fprintf (stderr, "warning:  failed to re-set signal handler\n");
}

/*}}}*/

typedef struct
{
   union {double *d; float *f;} ebins;
   union {double *d; float *f;} photar;
   int *keep;
   int nbins;
}
Xspec_Info_Type;

#define FREE_XIT(s) \
static void free_##s##_xspec_info_type (Xspec_Info_Type *x) \
{ \
   if (x == NULL) \
     return; \
 \
   ISIS_FREE (x->ebins.s); \
   ISIS_FREE (x->photar.s); \
   ISIS_FREE (x->keep); \
   ISIS_FREE (x); \
}
FREE_XIT(f)
FREE_XIT(d)
#if 0
}
#endif

#define NEW_XIT(s,type) \
static Xspec_Info_Type *new_##s##_xspec_info_type (int nbins) \
{ \
   Xspec_Info_Type *x; \
 \
   if (nbins <= 0) \
     return NULL; \
 \
   if (NULL == (x = ISIS_MALLOC (sizeof *x))) \
     return NULL; \
 \
   if (NULL == (x->ebins.s = ISIS_MALLOC ((nbins+1) * sizeof(type))) \
       || NULL == (x->photar.s = ISIS_MALLOC (nbins * sizeof(type))) \
       || NULL == (x->keep = ISIS_MALLOC (nbins * sizeof(int)))) \
     { \
        free_##s##_xspec_info_type (x); \
        return NULL; \
     } \
 \
   memset ((char *) x->photar.s, 0, nbins * sizeof(type)); \
   x->nbins = nbins; \
 \
   return x; \
}
NEW_XIT(f,float)
NEW_XIT(d,double)
#if 0
}
#endif

/*
 *   The ISIS data grid might have holes in it, but the XSPEC
 *   grid cannot.  To work around this, I'll generate a hole-free
 *   grid than spans the full range, then pick out the relevant
 *   bin values later.
 *
 *   Also, the input data grid is in Angstrom, but XSPEC wants keV.
 */

#define MAKE_XG(s,type) \
static Xspec_Info_Type *make_##s##_xspec_grid (Isis_Hist_t *g) \
{ \
   Xspec_Info_Type *x; \
   int i, n, k, nbins, n_notice; \
   int *notice_list, *keep; \
   type *ebins; \
   double *bin_lo, *bin_hi; \
 \
   if ((NULL == g) || (g->notice_list == NULL)) \
     { \
        fprintf (stderr, "*** internal error:  got NULL ptr in make_xspec_grid\n"); \
        return NULL; \
     } \
 \
   if (g->n_notice < 1) \
     { \
        fprintf (stderr, "*** no noticed bins\n"); \
        return NULL; \
     } \
 \
   bin_lo = g->bin_lo; \
   bin_hi = g->bin_hi; \
   n_notice = g->n_notice; \
   notice_list = g->notice_list; \
   nbins = 1; \
 \
   for (i=1; i < n_notice; i++) \
     { \
        int n1 = notice_list[i  ]; \
        int n0 = notice_list[i-1]; \
        double diff = fabs (bin_lo[n1] - bin_hi[n0]); \
        double avg  = 0.5 * fabs (bin_lo[n1] + bin_hi[n0]); \
        nbins += ((diff < TOL * avg) ? 1 : 2); \
     } \
 \
   if (NULL == (x = new_##s##_xspec_info_type (nbins))) \
     return NULL; \
 \
   n = g->notice_list[0]; \
 \
   k = nbins; \
   x->ebins.s[k] = (type) KEV_ANGSTROM / g->bin_lo[n]; \
 \
   ebins = x->ebins.s; \
   keep = x->keep; \
 \
   for (i=1; i < n_notice; i++) \
     { \
        int n1 = notice_list[i  ]; \
        int n0 = notice_list[i-1]; \
        double diff = fabs (bin_lo[n1] - bin_hi[n0]); \
        double avg  = 0.5 * fabs (bin_lo[n1] + bin_hi[n0]); \
 \
        k--; \
        ebins[k] = (type) KEV_ANGSTROM / bin_hi[n0]; \
        keep[k]  = 1; \
 \
        if (diff > TOL * avg) \
          { \
             k--; \
             ebins[k] = (type) KEV_ANGSTROM / bin_lo[n1]; \
             keep[k]  = 0; \
          } \
     } \
 \
   k--;             /* low edge of first ENERGY bin */ \
 \
   if (k != 0) \
     { \
        fprintf (stderr, "Invalid xspec grid\n"); \
        free_##s##_xspec_info_type (x); \
        return NULL; \
     } \
 \
   n = g->notice_list[g->n_notice-1]; \
   x->ebins.s[k] = (type) KEV_ANGSTROM / g->bin_hi[n]; \
   x->keep[k]  = 1; \
 \
   return x; \
}
MAKE_XG(f,float)
MAKE_XG(d,double)
#if 0
}
#endif

/*    to unpack the xspec result (on energy grid),
 *    reverse array order consistent with the input wavelength grid
 *
 *    And, apply the normalization.
 *    If norm isnt relevant for this function, the caller should
 *    just set it to 1.0
 */
#define EVAL_XF(s,type) \
static int eval_##s##_xspec_fun (Xspec_Fun_t *fun, double *val, Isis_Hist_t *g, \
                                 type *param, type norm, int category) \
{ \
   Xspec_Param_t p; \
   Xspec_Info_Type *x; \
   int i, k; \
   int ret = -1; \
 \
   if (NULL == (x = make_##s##_xspec_grid (g))) \
     goto finish; \
 \
   p.ear.s = x->ebins.s; \
   p.ne = x->nbins; \
   p.param.s = param; \
   p.ifl = 0; \
   p.photar.s = x->photar.s; \
   p.photer.s = ISIS_MALLOC (x->nbins * sizeof(type)); \
   memset ((char *)p.photer.f, 0, x->nbins * sizeof(type)); \
 \
   p.filename = Table_Model_Filename; \
 \
   if (category == ISIS_FUN_OPERATOR) \
     { \
        k = g->n_notice; \
        for (i=0; i < x->nbins; i++) \
          { \
             if (x->keep[i]) \
               x->photar.s[i] = (type) val[--k]; \
          } \
     } \
 \
   call_xspec_fun (fun, &p); \
 \
   ISIS_FREE (p.photer.s); \
   p.photer.s = NULL; \
 \
   k = g->n_notice; \
   for (i=0; i < x->nbins; i++) \
     { \
        if (x->keep[i]) \
          val[--k] = (double) (norm * x->photar.s[i]); \
     } \
 \
   if (k == 0) \
     ret = 0; \
   else \
     fprintf (stderr, "Inconsistent grid while evaluating XSPEC function\n"); \
 \
   finish: \
 \
   free_##s##_xspec_info_type (x); \
   return ret; \
}
EVAL_XF(f,float)
EVAL_XF(d,double)
#if 0
}
#endif
/*}}}*/

typedef void fptr_type (void);
static fptr_type *Generic_Fptr;
static char *Model_Init_String;

typedef int Hook_Type (double *, Isis_Hist_t *, double *, unsigned int);

typedef struct
{
   char *name;
   fptr_type *symbol;
   char *hook_name;
   char *init_string;
}
Xspec_Type;
static int Xspec_Type_Id = -1;

typedef void Fcn_f_Type (float *, int *, float *, int *, float *, float *);
typedef void Fcn_fn_Type (float *, int *, float *, int *, float *);
typedef void Fcn_F_Type (double *, int *, double *, int *, double *, double *);
typedef void Fcn_C_Type (double *, int, double *, int, double *, double *, const char *);

static void f_sub (Xspec_Param_t *p) /*{{{*/
{
   int ne, ifl;
   if (p == NULL || Generic_Fptr == NULL)
     return;
   ne = p->ne;
   ifl = p->ifl;
   (*((Fcn_f_Type *)Generic_Fptr))(p->ear.f, &ne, p->param.f, &ifl, p->photar.f, p->photer.f);
}

/*}}}*/

static void fn_sub (Xspec_Param_t *p) /*{{{*/
{
   int ne, ifl;
   if (p == NULL || Generic_Fptr == NULL)
     return;
   ne = p->ne;
   ifl = p->ifl;
   (*((Fcn_fn_Type *)Generic_Fptr))(p->ear.f, &ne, p->param.f, &ifl, p->photar.f);
}

/*}}}*/

static void F_sub (Xspec_Param_t *p) /*{{{*/
{
   int ne, ifl;
   if (p == NULL || Generic_Fptr == NULL)
     return;
   ne = p->ne;
   ifl = p->ifl;
   (*((Fcn_F_Type *)Generic_Fptr))(p->ear.d, &ne, p->param.d, &ifl, p->photar.d, p->photer.d);
}

/*}}}*/

static void C_sub (Xspec_Param_t *p) /*{{{*/
{
   int ne, ifl;
   if (p == NULL || Generic_Fptr == NULL)
     return;
   ne = p->ne;
   ifl = p->ifl;
   (*((Fcn_C_Type *)Generic_Fptr))(p->ear.d, ne, p->param.d, ifl, p->photar.d, p->photer.d, Model_Init_String);
}

/*}}}*/

static int mul_f (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   float *param = NULL;
   int ret;

   if (npar > 0)
     {
        unsigned int i;

        if (NULL == (param = ISIS_MALLOC (npar * sizeof(float))))
          return -1;

        for (i = 0; i < npar; i++)
          param[i] = (float) par[i];
     }

   ret = eval_f_xspec_fun (f_sub, val, g, param, 1.0, ISIS_FUN_ADDMUL);
   ISIS_FREE (param);

   return ret;
}

/*}}}*/

static int con_f (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   float *param = NULL;
   int ret;

   if (npar > 0)
     {
        unsigned int i;

        if (NULL == (param = ISIS_MALLOC (npar * sizeof(float))))
          return -1;

        for (i = 0; i < npar; i++)
          param[i] = (float) par[i];
     }

   ret = eval_f_xspec_fun (f_sub, val, g, param, 1.0, ISIS_FUN_OPERATOR);
   ISIS_FREE (param);

   return ret;
}

/*}}}*/

static int add_f (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   float tmp[2] = {0.0};
   float *param = tmp;
   unsigned int i;
   int ret, malloced = 0;

   if (npar > 2)
     {
        if (NULL == (param = ISIS_MALLOC (npar * sizeof(float))))
          return -1;
        malloced = 1;
     }

   for (i = 0; i < npar; i++)
     param[i] = (float) par[i];

   ret = eval_f_xspec_fun (f_sub, val, g, &param[1], param[0], ISIS_FUN_ADDMUL);

   if (malloced)
     ISIS_FREE(param);

   return ret;
}

/*}}}*/

static int mul_fn (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   float *param = NULL;
   int ret;

   if (npar > 0)
     {
        unsigned int i;

        if (NULL == (param = ISIS_MALLOC (npar * sizeof(float))))
          return -1;

        for (i = 0; i < npar; i++)
          param[i] = (float) par[i];
     }

   ret = eval_f_xspec_fun (fn_sub, val, g, param, 1.0, ISIS_FUN_ADDMUL);
   ISIS_FREE (param);

   return ret;
}

/*}}}*/

static int con_fn (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   float *param = NULL;
   int ret;

   if (npar > 0)
     {
        unsigned int i;

        if (NULL == (param = ISIS_MALLOC (npar * sizeof(float))))
          return -1;

        for (i = 0; i < npar; i++)
          param[i] = (float) par[i];
     }

   ret = eval_f_xspec_fun (fn_sub, val, g, param, 1.0, ISIS_FUN_OPERATOR);
   ISIS_FREE (param);

   return ret;
}

/*}}}*/

static int add_fn (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   float tmp[2] = {0.0};
   float *param = tmp;
   unsigned int i;
   int ret, malloced = 0;

   if (npar > 2)
     {
        if (NULL == (param = ISIS_MALLOC (npar * sizeof(float))))
          return -1;
        malloced = 1;
     }

   for (i = 0; i < npar; i++)
     param[i] = (float) par[i];

   ret = eval_f_xspec_fun (fn_sub, val, g, &param[1], param[0], ISIS_FUN_ADDMUL);

   if (malloced)
     ISIS_FREE(param);

   return ret;
}

/*}}}*/

static int mul_F (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   int ret;
   (void) npar;
   ret = eval_d_xspec_fun (F_sub, val, g, par, 1.0, ISIS_FUN_ADDMUL);
   return ret;
}

/*}}}*/

static int con_F (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   int ret;
   (void) npar;
   ret = eval_d_xspec_fun (F_sub, val, g, par, 1.0, ISIS_FUN_OPERATOR);
   return ret;
}

/*}}}*/

static int add_F (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   double tmp[2] = {0.0};
   double *param = tmp;
   int ret, malloced = 0;
   unsigned int i;

   if (npar > 2)
     {
        if (NULL == (param = ISIS_MALLOC (npar * sizeof(double))))
          return -1;
        malloced = 1;
     }

   for (i = 0; i < npar; i++)
     param[i] = par[i];

   ret = eval_d_xspec_fun (F_sub, val, g, &param[1], param[0], ISIS_FUN_ADDMUL);

   if (malloced)
     ISIS_FREE(param);

   return ret;
}

/*}}}*/

static int mul_C (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   int ret;
   (void) npar;
   ret = eval_d_xspec_fun (C_sub, val, g, par, 1.0, ISIS_FUN_ADDMUL);
   return ret;
}

/*}}}*/

static int con_C (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   int ret;
   (void) npar;
   ret = eval_d_xspec_fun (C_sub, val, g, par, 1.0, ISIS_FUN_OPERATOR);
   return ret;
}

/*}}}*/

static int add_C (double *val, Isis_Hist_t *g, double *par, unsigned int npar) /*{{{*/
{
   double tmp[2] = {0.0};
   double *param = tmp;
   int ret, malloced = 0;
   unsigned int i;

   if (npar > 2)
     {
        if (NULL == (param = ISIS_MALLOC (npar * sizeof(double))))
          return -1;
        malloced = 1;
     }

   for (i = 0; i < npar; i++)
     param[i] = par[i];

   ret = eval_d_xspec_fun (C_sub, val, g, &param[1], param[0], ISIS_FUN_ADDMUL);

   if (malloced)
     ISIS_FREE(param);

   return ret;
}

/*}}}*/

#if (SLANG_VERSION<20000)
static void SLang_set_error (int err) /*{{{*/
{
   if ((err != 0) && (SLang_Error != 0))
     return;
   SLang_Error = err;
}

/*}}}*/
#endif

static int pop_2_matched_arrays (int type, SLang_Array_Type **ap, SLang_Array_Type **bp) /*{{{*/
{
   SLang_Array_Type *a, *b;

   *ap = *bp = NULL;

   if (-1 == SLang_pop_array_of_type (&b, type))
     return -1;

   if (-1 == SLang_pop_array_of_type (&a, type))
     {
        SLang_free_array (b);
        return -1;
     }

   if (a->num_elements == b->num_elements)
     {
        *ap = a;
        *bp = b;
        return 0;
     }

   fprintf (stderr, "*** inconsistent array sizes\n");
   SLang_set_error (Isis_Error);
   SLang_free_array (a);
   SLang_free_array (b);

   return -1;
}

/*}}}*/

static void _xspec_hook (Xspec_Type *xt, Hook_Type *hook) /*{{{*/
{
   SLang_Array_Type *sl_lo, *sl_hi, *sl_par, *sl_val, *sl_arg;
   double *val, *par;
   Isis_Hist_t g;
   int *notice_list = NULL;
   int i, nbins, npar;
   int ret = -1;

   sl_lo = sl_hi = sl_par = sl_val = sl_arg = NULL;

   /* stack should contain  lo, hi, par [, arg] */

   if (hook == con_f || hook == con_fn || hook == con_F
       || hook == con_C)
     {
        if ((-1 == SLang_pop_array_of_type (&sl_arg, SLANG_DOUBLE_TYPE))
             || sl_arg == NULL)
          goto finish;
     }

   if (-1 == SLang_pop_array_of_type (&sl_par, SLANG_DOUBLE_TYPE))
     goto finish;

   if (-1 == pop_2_matched_arrays (SLANG_DOUBLE_TYPE, &sl_lo, &sl_hi))
     goto finish;

   nbins = sl_lo->num_elements;

   if (NULL == (notice_list = ISIS_MALLOC (nbins * sizeof(int))))
     goto finish;
   for (i = 0; i < nbins; i++)
     notice_list[i] = i;

   sl_val = SLang_create_array (SLANG_DOUBLE_TYPE, 0, NULL, &nbins, 1);
   if (sl_val == NULL)
     goto finish;

   if (sl_arg != NULL)
     {
        if (sl_arg->num_elements != (unsigned int) nbins)
          {
             fprintf (stderr, "*** inconsistent array size for operator arg\n");
             goto finish;
          }
        memcpy (sl_val->data, sl_arg->data, nbins * sizeof(double));
        SLang_free_array (sl_arg);
     }

   g.bin_lo = (double *)sl_lo->data;
   g.bin_hi = (double *)sl_hi->data;
   g.nbins = nbins;
   g.n_notice = nbins;
   g.notice_list = notice_list;

   val = (double *)sl_val->data;
   par = (double *)sl_par->data;
   npar = sl_par->num_elements;

   /* set global function pointer */
   Generic_Fptr = xt->symbol;
   Model_Init_String = xt->init_string;
   ret = (*hook) (val, &g, par, npar);

   finish:
   if (ret) SLang_set_error (Isis_Error);

   ISIS_FREE(notice_list);
   SLang_free_array (sl_lo);
   SLang_free_array (sl_hi);
   SLang_free_array (sl_par);

   SLang_push_array (sl_val, 1);
}

/*}}}*/

#define XS_HOOK(type) \
static void _xspec_##type##_hook (Xspec_Type *xt) \
{ \
   _xspec_hook (xt, type); \
}
XS_HOOK(add_f)
XS_HOOK(mul_f)
XS_HOOK(con_f)
XS_HOOK(add_fn)
XS_HOOK(mul_fn)
XS_HOOK(con_fn)
XS_HOOK(add_F)
XS_HOOK(mul_F)
XS_HOOK(con_F)
XS_HOOK(add_C)
XS_HOOK(mul_C)
XS_HOOK(con_C)
#if 0
}
#endif

static void _xspec_model_init_string (Xspec_Type *xt, char *init)
{
   if (xt == NULL)
     return;
   ISIS_FREE(xt->init_string);
   if ((init != NULL)
       && (NULL == (xt->init_string = isis_make_string (init))))
     {
        SLang_set_error (Isis_Error);
     }
}

static void handle_link_error (char *path, char *name) /*{{{*/
{
   const char *error;

   if (-2 != SLang_is_defined ("_xspec_module_verbose_link_errors"))
     return;

   if (NULL != (error = dlerror ()))
     {
        fprintf (stderr, "Link error:  %s\n", error);
     }
   else
     {
        fprintf (stderr, "Link error:  failed loading %s from %s\n",
                 name, path);
     }
}
/*}}}*/

static fptr_type *load_function (char *path, char *name) /*{{{*/
{
   fptr_type *f = NULL;
   void *handle = NULL;

   if (path == NULL || name == NULL)
     return NULL;

#ifndef RTLD_GLOBAL
# define RTLD_GLOBAL 0
#endif
#ifdef RTLD_NOW
# define DLOPEN_FLAG  (RTLD_NOW | RTLD_GLOBAL)
#else
# define DLOPEN_FLAG  (RTLD_LAZY | RTLD_GLOBAL)
#endif

   handle = dlopen (path, DLOPEN_FLAG);
   if (handle == NULL)
     {
        handle_link_error (path, name);
        return NULL;
     }

   f = (fptr_type *) dlsym (handle, name);
   if (f == NULL)
     {
        handle_link_error (path, name);
        /* dlclose (handle); */
        return NULL;
     }

   return f;
}
/*}}}*/

static int load_xspec_fun (SLang_Ref_Type *ref, char *file, char *fun_name) /*{{{*/
{
   SLang_MMT_Type *mmt;
   fptr_type *fptr;
   Xspec_Type *xt;

   if (-1 == SLang_assign_to_ref (ref, SLANG_NULL_TYPE, NULL))
     return -1;

   if (NULL == (fptr = load_function (file, fun_name)))
     return -1;

   if (NULL == (xt = ISIS_MALLOC (sizeof(Xspec_Type))))
     return -1;

   xt->symbol = (fptr_type *)fptr;
   xt->init_string = NULL;

   if (NULL == (mmt = SLang_create_mmt (Xspec_Type_Id, (void *) xt)))
     {
        ISIS_FREE (xt);
        return -1;
     }

   if (-1 == SLang_assign_to_ref (ref, Xspec_Type_Id, &mmt))
     {
        ISIS_FREE (xt);
        SLang_free_mmt (mmt);
        return -1;
     }

   return 0;
}

/*}}}*/

#if defined(HAVE_XSPEC_11)
# include "_model_externs_xspec11.inc"
# define XSPEC_MODEL_NAMES_FILE "_names_xspec11.dat"
# define XSPEC_VERSION 11
#elif defined(HAVE_XSPEC_12)
# include "_model_externs_xspec12.inc"
# define XSPEC_MODEL_NAMES_FILE "_names_xspec12.dat"
# define XSPEC_VERSION 12
#endif

static Xspec_Type Static_Fun_Table[] =
{
#if defined(HAVE_XSPEC_11)
#  include "_model_table_xspec11.inc"
#elif defined(HAVE_XSPEC_12)
#  include "_model_table_xspec12.inc"
#endif
     {NULL, NULL, NULL, NULL}
};

static void find_xspec_fun (SLang_Ref_Type *ref, char *fun_name) /*{{{*/
{
   SLang_MMT_Type *mmt;
   Xspec_Type *xt;

   /* silence compiler warnings about unused symbols */
   (void) mul_C;  (void) con_C;  (void) add_C;
   (void) mul_F;  (void) con_F;  (void) add_F;

   if (-1 == SLang_assign_to_ref (ref, SLANG_NULL_TYPE, NULL))
     return;

   for (xt = Static_Fun_Table; xt->name != NULL; xt++)
     {
        if (0 == strcmp (fun_name, xt->name))
          break;
     }

   if (xt->name != NULL)
     {
        if (NULL == (mmt = SLang_create_mmt (Xspec_Type_Id, (void *) xt)))
          return;

        if (-1 == SLang_assign_to_ref (ref, Xspec_Type_Id, &mmt))
          {
             SLang_free_mmt (mmt);
             return;
          }
     }

   SLang_push_string (xt->hook_name);
}

/*}}}*/

/* Intrinsics */

/* DUMMY_MODEL_TYPE is a temporary hack that will be modified to the true
  * id once the interpreter provides it when the class is registered.  See below
  * for details.  The reason for this is simple: for a module, the type-id
  * must be assigned dynamically.
  */
#define DUMMY_MODEL_TYPE   255
#define MT DUMMY_MODEL_TYPE
#define I SLANG_INT_TYPE
#define R SLANG_REF_TYPE
#define S SLANG_STRING_TYPE
#define V SLANG_VOID_TYPE

static SLang_Intrin_Fun_Type Intrinsics [] =
{
   MAKE_INTRINSIC_3("load_xspec_fun", load_xspec_fun, I, R, S, S),
   MAKE_INTRINSIC_2("find_xspec_fun", find_xspec_fun, V, R, S),
   MAKE_INTRINSIC_1("_xspec_mul_fn_hook", _xspec_mul_fn_hook, V, MT),
   MAKE_INTRINSIC_1("_xspec_add_fn_hook", _xspec_add_fn_hook, V, MT),
   MAKE_INTRINSIC_1("_xspec_con_fn_hook", _xspec_con_fn_hook, V, MT),
   MAKE_INTRINSIC_1("_xspec_mul_f_hook", _xspec_mul_f_hook, V, MT),
   MAKE_INTRINSIC_1("_xspec_add_f_hook", _xspec_add_f_hook, V, MT),
   MAKE_INTRINSIC_1("_xspec_con_f_hook", _xspec_con_f_hook, V, MT),
   MAKE_INTRINSIC_1("_xspec_mul_F_hook", _xspec_mul_F_hook, V, MT),
   MAKE_INTRINSIC_1("_xspec_add_F_hook", _xspec_add_F_hook, V, MT),
   MAKE_INTRINSIC_1("_xspec_con_F_hook", _xspec_con_F_hook, V, MT),
   MAKE_INTRINSIC_1("_xspec_mul_C_hook", _xspec_mul_C_hook, V, MT),
   MAKE_INTRINSIC_1("_xspec_add_C_hook", _xspec_add_C_hook, V, MT),
   MAKE_INTRINSIC_1("_xspec_con_C_hook", _xspec_con_C_hook, V, MT),
   MAKE_INTRINSIC_2("_xspec_model_init_string", _xspec_model_init_string, V, MT, S),
   SLANG_END_INTRIN_FUN_TABLE
};

#undef I
#undef R
#undef S
#undef V
#undef MT

static void patchup_intrinsic_table (void) /*{{{*/
{
   SLang_Intrin_Fun_Type *f;

   f = Intrinsics;
   while (f->name != NULL)
     {
        unsigned int i, nargs;
        SLtype *args;

        nargs = f->num_args;
        args = f->arg_types;
        for (i = 0; i < nargs; i++)
          {
             if (args[i] == DUMMY_MODEL_TYPE)
               args[i] = Xspec_Type_Id;
          }

        /* For completeness */
        if (f->return_type == DUMMY_MODEL_TYPE)
          f->return_type = Xspec_Type_Id;

        f++;
     }
}

/*}}}*/

static void free_xspec_fun_type (SLtype type, void *f) /*{{{*/
{
   Xspec_Type *xt = (Xspec_Type *)f;
   (void) type;
   (void) xt;
   if (xt == NULL)
     return;
   ISIS_FREE(xt->init_string);
}

/*}}}*/

#include "xspec-compat.inc"

/* In the xspec source code,
 * fpdatd is defined in xspec/src/functions/fpunc.f
 * fgdatd is defined in xspec/src/functions/fgunc.f
 */
PROTOCCALLSFSUB2(FPDATD,fpdatd,STRING,PINT)
#define FPDATD(cvalue,ierr) CCALLSFSUB2(FPDATD,fpdatd,STRING,PINT,cvalue,ierr)
PROTOCCALLSFFUN0(STRING,FGDATD,fgdatd)
#define FGDATD() CCALLSFFUN0(FGDATD,fgdatd)

static int xs_set_datadir (char *name) /*{{{*/
{
   int ierr = -1;
   if (name == NULL)
     return -1;
   /* FPDATD modifies ierr on return */
   FPDATD(name, ierr);
   return ierr ? -1 : 0;
}

/*}}}*/

static void xs_get_datadir (void) /*{{{*/
{
   SLang_push_string (FGDATD());
}

/*}}}*/

#if 0
/* In the xspec source code,
 * fpslfn is defined in xspec/src/functions/fpunc.f
 * fgslfn is defined in xspec/src/functions/fgunc.f
 *
 * >>>  These routines aren't available in xspec12.
 */
PROTOCCALLSFSUB2(FPSLFN,fpslfn,STRING,PINT)
#define FPSLFN(cvalue,ierr) CCALLSFSUB2(FPSLFN,fpslfn,STRING,PINT,cvalue,ierr)
PROTOCCALLSFFUN0(STRING,FGSLFN,fgslfn)
#define FGSLFN() CCALLSFFUN0(FGSLFN,fgslfn)

static int xs_set_abundance_table_filename (char *name) /*{{{*/
{
   int ierr = -1;
   if (name == NULL)
     return -1;
   /* FPSLFN modifies ierr on return */
   FPSLFN(name, ierr);
   return ierr ? -1 : 0;
}

/*}}}*/

static void xs_get_abundance_table_filename (void) /*{{{*/
{
   SLang_push_string (FGSLFN());
}

/*}}}*/
#endif

/* In the xspec source code,
 * fpsolr is defined in xspec/src/functions/fpunc.f
 * fgsolr is defined in xspec/src/functions/fgunc.f
 */
PROTOCCALLSFSUB2(FPSOLR,fpsolr,STRING,PINT)
#define FPSOLR(cvalue,ierr) CCALLSFSUB2(FPSOLR,fpsolr,STRING,PINT,cvalue,ierr)
PROTOCCALLSFFUN0(STRING,FGSOLR,fgsolr)
#define FGSOLR() CCALLSFFUN0(FGSOLR,fgsolr)

static int xs_set_abundance_table (char *name) /*{{{*/
{
   int ierr = -1;
   if (name == NULL)
     return -1;
   /* FPSOLR modifies ierr on return */
   FPSOLR(name, ierr);
   return ierr ? -1 : 0;
}

/*}}}*/

static void xs_get_abundance_table (void) /*{{{*/
{
   SLang_push_string (FGSOLR());
}

/*}}}*/

/* In the xspec source code,
 * fpxsct is defined in xspec/src/functions/fpunc.f
 * fgxsct is defined in xspec/src/functions/fgunc.f
 */
PROTOCCALLSFSUB2(FPXSCT,fpxsct,STRING,PINT)
#define FPXSCT(cvalue,ierr) CCALLSFSUB2(FPXSCT,fpxsct,STRING,PINT,cvalue,ierr)
PROTOCCALLSFFUN0(STRING,FGXSCT,fgxsct)
#define FGXSCT() CCALLSFFUN0(FGXSCT,fgxsct)

static int xs_set_xsection_table (char *name) /*{{{*/
{
   int ierr = -1;
   if (name == NULL)
     return -1;
   /* FPXSCT modifies ierr on return */
   FPXSCT(name, ierr);
   return ierr ? -1 : 0;
}

/*}}}*/

static void xs_get_xsection_table (void) /*{{{*/
{
   SLang_push_string (FGXSCT());
}

/*}}}*/

PROTOCCALLSFSUB2(FPMSTR,fpmstr,STRING,STRING)
#define FPMSTR(p,v) CCALLSFSUB2(FPMSTR,fpmstr,STRING,STRING,p,v)

static void xs_fpmstr (char *p, char *v) /*{{{*/
{
   if (p == NULL || v == NULL)
     return;
   FPMSTR(p, v);
}

/*}}}*/

PROTOCCALLSFSUB1(CSMPH0,csmph0,FLOAT)
PROTOCCALLSFSUB1(CSMPQ0,csmpq0,FLOAT)
PROTOCCALLSFSUB1(CSMPL0,csmpl0,FLOAT)
#define CSMPH0(h) CCALLSFSUB1(CSMPH0,csmph0,FLOAT,h)
#define CSMPQ0(q) CCALLSFSUB1(CSMPQ0,csmpq0,FLOAT,q)
#define CSMPL0(l) CCALLSFSUB1(CSMPL0,csmpl0,FLOAT,l)

#define XSPEC_DEFAULT_H0 70.0
#define XSPEC_DEFAULT_Q0 0.0
#define XSPEC_DEFAULT_L0 0.73

static void xs_set_cosmo_hubble (float *h0)
{
   float h = h0 ? *h0 : XSPEC_DEFAULT_H0;
   CSMPH0(h);
}
static void xs_set_cosmo_decel (float *q0)
{
   float q = q0 ? *q0 : XSPEC_DEFAULT_Q0;
   CSMPQ0(q);
}
static void xs_set_cosmo_lambda (float *l0)
{
   float l = l0 ? *l0 : XSPEC_DEFAULT_L0;
   CSMPL0(l);
}

PROTOCCALLSFFUN0(FLOAT,CSMGH0,csmgh0)
PROTOCCALLSFFUN0(FLOAT,CSMGQ0,csmgq0)
PROTOCCALLSFFUN0(FLOAT,CSMGL0,csmgl0)
#define CSMGH0()  CCALLSFFUN0(CSMGH0,csmgh0)
#define CSMGQ0()  CCALLSFFUN0(CSMGQ0,csmgq0)
#define CSMGL0()  CCALLSFFUN0(CSMGL0,csmgl0)

static double xs_get_cosmo_hubble (void)
{
   return (double) CSMGH0();
}
static double xs_get_cosmo_decel (void)
{
   return (double) CSMGQ0();
}
static double xs_get_cosmo_lambda (void)
{
   return (double) CSMGL0();
}

PROTOCCALLSFFUN1(FLOAT,FGABND,fgabnd,STRING)
#define FGABND(element) CCALLSFFUN1(FGABND,fgabnd,STRING,element)

static double xs_get_element_solar_abundance (char *element) /*{{{*/
{
   double x = FGABND(element);
   return x;
}

/*}}}*/

PROTOCCALLSFFUN5(FLOAT,PHOTO,photo,FLOAT,FLOAT,INT,INT,LONG)
#define       PHOTO(kev1,kev2,Z,versn,status) \
  CCALLSFFUN5(PHOTO,photo,FLOAT,FLOAT,INT,INT,LONG,kev1,kev2,Z,versn,status)

static double xs_photo (float *kev1, float *kev2, int *Z, int *versn) /*{{{*/
{
   long status = 0;
   double xsect = (double) PHOTO(*kev1,*kev2,*Z,*versn,&status);
   if (status)
     {
        SLang_set_error(Isis_Error);
        xsect = 0.0;
     }
   return xsect;
}

/*}}}*/

PROTOCCALLSFFUN4(FLOAT,GPHOTO,gphoto,FLOAT,FLOAT,INT,LONG)
#define       GPHOTO(kev1,kev2,Z,status) \
  CCALLSFFUN4(GPHOTO,gphoto,FLOAT,FLOAT,INT,LONG,kev1,kev2,Z,status)

static double xs_gphoto (float *kev1, float *kev2, int *Z) /*{{{*/
{
   long status = 0;
   double xsect = (double) GPHOTO(*kev1,*kev2,*Z,&status);
   if (status)
     {
        SLang_set_error(Isis_Error);
        xsect = 0.0;
     }
   return xsect;
}

/*}}}*/

PROTOCCALLSFSUB5(PHFIT2,phfit2,INT,INT,INT,FLOAT,PFLOAT)
#define       PHFIT2(Nz,Ne,Is,E,S) \
  CCALLSFSUB5(PHFIT2,phfit2,INT,INT,INT,FLOAT,PFLOAT,Nz,Ne,Is,E,S)

static double xs_phfit2 (int *nz, int *ne, int *is, float *e) /*{{{*/
{
   float s;
   PHFIT2(*nz, *ne, *is, *e, s);
   return (double)s;
}

/*}}}*/

PROTOCCALLSFSUB2(INITNEI,initnei,PINT,PINT)
#define INITNEI(nionp,nzmax) CCALLSFSUB2(INITNEI,initnei,PINT,PINT,nionp,nzmax)

static void xs_initnei (int *nionp,int *nzmax) /*{{{*/
{
   static int nei_is_initialized = 0;
   static int ni, nz;
   if (nei_is_initialized == 0)
     {
        INITNEI(ni,nz);
        nei_is_initialized = 1;
     }
   *nionp = ni;
   *nzmax = nz;
}

/*}}}*/

PROTOCCALLSFSUB8(IONSNEQR,ionsneqr,FLOATV,FLOATV,INT,INT,INT,FLOATV,INTV,INTV)
#define IONSNEQR(tmp,tau,n,nzmax,nionp,fout,ionel,ionstage) \
   CCALLSFSUB8(IONSNEQR,ionsneqr,FLOATV,FLOATV,INT,INT,INT,FLOATV,INTV,INTV,\
                 tmp,tau,n,nzmax,nionp,fout,ionel,ionstage)

static void xs_ionsneqr(void) /*{{{*/
{
   SLang_Array_Type *sl_tmp=NULL, *sl_tau=NULL, *sl_fout=NULL;
   SLang_Array_Type *sl_ionel=NULL, *sl_ionstage=NULL;
   float *tmp, *tau, *fout;
   int *ionel, *ionstage;
   int n, nzmax, nionp;

   xs_initnei(&nionp, &nzmax);

   if (-1 == pop_2_matched_arrays (SLANG_FLOAT_TYPE, &sl_tmp, &sl_tau))
     return;

   n = sl_tmp->num_elements;

   sl_fout = SLang_create_array (SLANG_FLOAT_TYPE, 0, NULL, &nionp, 1);
   sl_ionel= SLang_create_array (SLANG_INT_TYPE, 0, NULL, &nionp, 1);
   sl_ionstage= SLang_create_array (SLANG_INT_TYPE, 0, NULL, &nionp, 1);
   if ((sl_fout == NULL) || (sl_ionel == NULL) || (sl_ionstage == NULL))
     {
        SLang_set_error (Isis_Error);
        goto push_results;
     }

   tmp = (float *)sl_tmp->data;
   tau = (float *)sl_tau->data;
   fout = (float *)sl_fout->data;
   ionel = (int *)sl_ionel->data;
   ionstage = (int *)sl_ionstage->data;

   IONSNEQR(tmp, tau, n, nzmax, nionp, fout, ionel, ionstage);

   push_results:
   SLang_free_array (sl_tmp);
   SLang_free_array (sl_tau);
   SLang_push_array (sl_fout,1);
   SLang_push_array (sl_ionel,1);
   SLang_push_array (sl_ionstage,1);
}

/*}}}*/

#ifdef HAVE_XSPEC_12

PROTOCCALLSFFUN0(INT,FGCHAT,fgchat)
PROTOCCALLSFSUB1(FPCHAT,fpchat,INT)
#define FGCHAT()  CCALLSFFUN0(FGCHAT,fgchat)
#define FPCHAT(lev)  CCALLSFSUB1(FPCHAT,fpchat,INT,lev)
static int xs_gchat (void)
{
   return FGCHAT();
}
static void xs_pchat (int *lev)
{
   FPCHAT(*lev);
}
#endif

PROTOCCALLSFSUB0(FNINIT,fninit)
#define FNINIT() CCALLSFSUB0(FNINIT,fninit)
static int xs_init (void)
{
   FEWER_CFORTRAN_COMPILER_WARNINGS;
   FNINIT();
   CSMPH0(XSPEC_DEFAULT_H0);
   CSMPQ0(XSPEC_DEFAULT_Q0);
   CSMPL0(XSPEC_DEFAULT_L0);
   return 0;
}

/* XSPEC table models */
#ifdef HAVE_XSPEC_TABLE_MODELS /*{{{*/

static void set_table_model_filename (char *filename) /*{{{*/
{
   char *t;

   if (filename == NULL)
     {
        fputs ("Filename not set", stderr);
        return;
     }

   t = ISIS_MALLOC (1 + strlen(filename));
   if (t == NULL)
     {
        fputs ("Filename not set", stderr);
        return;
     }

   strcpy (t, filename);
   ISIS_FREE (Table_Model_Filename);
   Table_Model_Filename = t;
}

/*}}}*/

static int evaluate_table_model (Xspec_Fun_t *fun) /*{{{*/
{
   Isis_Hist_t g;
   SLang_Array_Type *sl_lo, *sl_hi, *sl_val, *sl_par;
   double *val = NULL;
   float *param = NULL;
   int *notice_list = NULL;
   int *notice = NULL;
   int i, npar, nbins, ret = -1;

   sl_lo = sl_hi = sl_val = sl_par = NULL;

   if (Table_Model_Filename == NULL)
     {
        fprintf (stderr, "Internal error in xspec module - table model filename not set\n");
        return -1;
     }

   if (-1 == SLang_pop_array_of_type (&sl_par, SLANG_FLOAT_TYPE)
       ||-1 == SLang_pop_array_of_type (&sl_hi, SLANG_DOUBLE_TYPE)
       ||-1 == SLang_pop_array_of_type (&sl_lo, SLANG_DOUBLE_TYPE)
       || (sl_par == NULL) || (sl_hi == NULL) || (sl_lo == NULL))
     goto finish;

   npar = sl_par->num_elements;
   nbins = sl_lo->num_elements;
   if (nbins != (int) sl_hi->num_elements)
     goto finish;

   notice_list = ISIS_MALLOC (nbins * sizeof (int));
   notice = ISIS_MALLOC (nbins * sizeof (int));
   val = ISIS_MALLOC (nbins * sizeof (double));
   if ((notice == NULL) || (notice_list == NULL) || (val == NULL))
     goto finish;

   for (i = 0; i < nbins; i++)
     {
        notice_list[i] = i;
        notice[i] = 1;
     }

   g.val = val;
   g.bin_lo = (double *)sl_lo->data;
   g.bin_hi = (double *)sl_hi->data;
   g.nbins = nbins;
   g.n_notice = nbins;
   g.notice = notice;
   g.notice_list = notice_list;

   param = (float *)sl_par->data;

   ret = eval_f_xspec_fun (fun, val, &g, param, 1.0, ISIS_FUN_ADDMUL);

   finish:
   SLang_free_array (sl_par);
   SLang_free_array (sl_hi);
   SLang_free_array (sl_lo);
   ISIS_FREE (notice_list);
   ISIS_FREE (notice);

   sl_val = SLang_create_array (SLANG_DOUBLE_TYPE, 0, val, &nbins, 1);
   SLang_push_array (sl_val, 1);

   return ret;
}

/*}}}*/

#define XSPEC10_TABLE_FUN(name,xsname,XSNAME)                       \
   PROTOCCALLSFSUB6(XSNAME,xsname,FLOATV,INT,FLOATV,PSTRING,INT,FLOATV) \
   static void name (Xspec_Param_t *p)                                  \
   {                                                                    \
      CCALLSFSUB6(XSNAME,xsname,FLOATV,INT,FLOATV,PSTRING,INT,FLOATV,   \
                     p->ear.f,p->ne,p->param.f,p->filename,p->ifl,p->photar.f);   \
   }

#define XSPEC11_TABLE_FUN(name,xsname,XSNAME)                              \
   PROTOCCALLSFSUB7(XSNAME,xsname,FLOATV,INT,FLOATV,PSTRING,INT,FLOATV,FLOATV) \
   static void name (Xspec_Param_t *p)                                         \
   {                                                                           \
      CCALLSFSUB7(XSNAME,xsname,FLOATV,INT,FLOATV,PSTRING,INT,FLOATV,FLOATV,   \
                     p->ear.f,p->ne,p->param.f,p->filename,p->ifl,p->photar.f,p->photer.f);   \
   }

/*}}}*/

XSPEC11_TABLE_FUN(xs_atbl,xsatbl,XSATBL)
XSPEC11_TABLE_FUN(xs_mtbl,xsmtbl,XSMTBL)
XSPEC11_TABLE_FUN(xs_etbl,xsetbl,XSETBL)

static int atbl (void) /*{{{*/
{
   return evaluate_table_model (xs_atbl);
}

/*}}}*/

static int mtbl (void) /*{{{*/
{
   return evaluate_table_model (xs_mtbl);
}

/*}}}*/

static int etbl (void) /*{{{*/
{
   return evaluate_table_model (xs_etbl);
}
/*}}}*/

static SLang_Intrin_Fun_Type Table_Model_Intrinsics [] =
{
   MAKE_INTRINSIC_S("_set_table_model_filename", set_table_model_filename, SLANG_VOID_TYPE),
   MAKE_INTRINSIC("_atbl", atbl, SLANG_VOID_TYPE, 0),
   MAKE_INTRINSIC("_mtbl", mtbl, SLANG_VOID_TYPE, 0),
   MAKE_INTRINSIC("_etbl", etbl, SLANG_VOID_TYPE, 0),
   SLANG_END_INTRIN_FUN_TABLE
};

#endif  /* HAVE_XSPEC_TABLE_MODELS */

#ifndef HEADAS
  #define HEADAS "xxx"
#endif
static char *Compiled_Headas_Path = HEADAS ;

#define V SLANG_VOID_TYPE
#define I SLANG_INT_TYPE
#define S SLANG_STRING_TYPE
#define F SLANG_FLOAT_TYPE
#define D SLANG_DOUBLE_TYPE

static SLang_Intrin_Fun_Type Private_Intrinsics [] =
{
   MAKE_INTRINSIC_S("_xs_set_datadir", xs_set_datadir, I),
   MAKE_INTRINSIC_0("_xs_get_datadir", xs_get_datadir, V),
#if 0
   MAKE_INTRINSIC_S("_xs_set_abundance_filename", xs_set_abundance_table_filename, I),
   MAKE_INTRINSIC_0("_xs_get_abundance_filename", xs_get_abundance_table_filename, V),
#endif
   MAKE_INTRINSIC_S("_xs_set_abundances", xs_set_abundance_table, I),
   MAKE_INTRINSIC_0("_xs_get_abundances", xs_get_abundance_table, V),
   MAKE_INTRINSIC_S("_xs_set_xsections", xs_set_xsection_table, I),
   MAKE_INTRINSIC_0("_xs_get_xsections", xs_get_xsection_table, V),
   MAKE_INTRINSIC_4("_xs_photo", xs_photo, D, F,F,I,I),
   MAKE_INTRINSIC_3("_xs_gphoto", xs_gphoto, D, F,F,I),
   MAKE_INTRINSIC_4("_xs_phfit2", xs_phfit2, D, I,I,I,F),
   MAKE_INTRINSIC_0("_xs_ionsneqr", xs_ionsneqr, V),
   MAKE_INTRINSIC_S("_xs_get_element_solar_abundance", xs_get_element_solar_abundance, D),
   MAKE_INTRINSIC_2("_xs_fpmstr", xs_fpmstr, V, S, S),
   MAKE_INTRINSIC_1("_xs_set_cosmo_hubble", xs_set_cosmo_hubble, V, F),
   MAKE_INTRINSIC_1("_xs_set_cosmo_decel", xs_set_cosmo_decel, V, F),
   MAKE_INTRINSIC_1("_xs_set_cosmo_lambda", xs_set_cosmo_lambda, V, F),
   MAKE_INTRINSIC_0("_xs_get_cosmo_hubble", xs_get_cosmo_hubble, D),
   MAKE_INTRINSIC_0("_xs_get_cosmo_decel", xs_get_cosmo_decel, D),
   MAKE_INTRINSIC_0("_xs_get_cosmo_lambda", xs_get_cosmo_lambda, D),
#ifdef HAVE_XSPEC_12
   MAKE_INTRINSIC_1("_xs_pchat", xs_pchat, V, I),
   MAKE_INTRINSIC_0("_xs_gchat", xs_gchat, I),
#endif
   SLANG_END_INTRIN_FUN_TABLE
};

static SLang_Intrin_Var_Type Private_Vars [] =
{
   MAKE_VARIABLE("Xspec_Compiled_Headas_Path", &Compiled_Headas_Path, S, 1),
   MAKE_VARIABLE("Xspec_Model_Names_File", &Xspec_Model_Names_File, S, 1),
   MAKE_VARIABLE("Xspec_Version", &Xspec_Version, I, 1),
   SLANG_END_INTRIN_VAR_TABLE
};

#undef V
#undef I
#undef S
#undef F
#undef D

/*}}}*/

/*}}}*/

/* init */

static char *Xanadu_Setenv = NULL;
static char *Headas_Setenv = NULL;

static void free_env (void)
{
   ISIS_FREE(Xanadu_Setenv);
   ISIS_FREE(Headas_Setenv);
}

static char *copy_and_set_env (char *env_name, char *env_builtin_value) /*{{{*/
{
   struct stat st;
   char *env, *env_set;

   env = getenv (env_name);

   if (env != NULL)
     {
        if (-1 == stat (env, &st))
          {
             fprintf (stderr, "*** %s environment variable provides an invalid path.\n", env_name);
             fprintf (stderr, "    Falling back to compiled-in path %s=%s\n",
                     env_name, env_builtin_value);
             env = env_builtin_value;
          }
     }
   else env = env_builtin_value;

   if ((env == env_builtin_value)
       && (-1 == stat (env, &st)))
     {
        fprintf (stderr, "*** Invalid path: %s=%s\n", env_name, env);
        return NULL;
     }

   if (NULL == (env_set = isis_mkstrcat (env_name, "=", env, NULL)))
     return NULL;

   if (-1 == putenv (env_set))
     {
        fprintf (stderr, "Failed setting %s environment variable: %s\n", env_name, env_set);
        ISIS_FREE(env_set);
        return NULL;
     }

   return env_set;
}

/*}}}*/

extern void deinit_xspec_module (void);
void deinit_xspec_module (void) /*{{{*/
{
   ISIS_FREE (Table_Model_Filename);
   free_env();
}

/*}}}*/

SLANG_MODULE(xspec);
int init_xspec_module_ns (char *ns_name) /*{{{*/
{
   SLang_Class_Type *cl;
   SLang_NameSpace_Type *ns;

   FEWER_CFORTRAN_COMPILER_WARNINGS;

   if (NULL == (ns = SLns_create_namespace (ns_name)))
     return -1;

   if (Xspec_Type_Id == -1)
     {
        if (NULL == (cl = SLclass_allocate_class ("Xspec_Type")))
          return -1;

        (void) SLclass_set_destroy_function (cl, free_xspec_fun_type);

        /* By registering as SLANG_VOID_TYPE, slang will dynamically allocate a
         * type.
         */
        if (-1 == SLclass_register_class (cl, SLANG_VOID_TYPE, sizeof (Xspec_Type),
                                          SLANG_CLASS_TYPE_MMT))
          return -1;

        Xspec_Type_Id = SLclass_get_class_id (cl);
        patchup_intrinsic_table ();
     }

   if (-1 == SLns_add_intrin_fun_table (NULL, Intrinsics, NULL))
     return -1;

#if defined(HAVE_XSPEC_11)
   if (NULL == (Xanadu_Setenv = copy_and_set_env ("XANADU", HEADAS "/..")))
     goto return_error;
#endif

   if (NULL == (Headas_Setenv = copy_and_set_env ("HEADAS", HEADAS)))
     goto return_error;

#ifdef HAVE_XSPEC_TABLE_MODELS
   if (-1 == SLns_add_intrin_fun_table (NULL, Table_Model_Intrinsics, "__HAVE_XSPEC_TABLE_MODELS__"))
     {
        fprintf (stderr, "Failed initializing XSPEC table-model intrinsics\n");
        goto return_error;
     }
#endif

   Xspec_Model_Names_File = XSPEC_MODEL_NAMES_FILE;
   Xspec_Version = XSPEC_VERSION;

   if (-1 == SLns_add_intrin_fun_table (ns, Private_Intrinsics, NULL)
      || -1 == SLns_add_intrin_var_table (ns, Private_Vars, NULL))
     {
        fprintf (stderr, "Failed initializing XSPEC intrinsics\n");
        goto return_error;
     }

   if (-1 == xs_init())
     {
        fprintf (stderr, "Failed initializing XSPEC module\n");
        goto return_error;
     }

   (void) SLdefine_for_ifdef ("__XSPEC__");

#ifdef HAVE_XSPEC_12
   (void) SLdefine_for_ifdef ("__HAVE_XSPEC_12__");
#endif

   return 0;

   return_error:
   deinit_xspec_module();
   return -1;
}

/*}}}*/
