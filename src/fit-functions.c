/* -*- mode: C; mode: fold -*- */

/*  This file is part of ISIS, the Interactive Spectral Interpretation System
    Copyright (C) 1998-2008 Massachusetts Institute of Technology

    This software was developed by the MIT Center for Space Research under
    contract SV1-61010 from the Smithsonian Institution.

    Author:  John C. Houck  <houck@space.mit.edu>

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

/* $Id: fit-functions.c,v 1.81 2004/09/07 14:50:03 houck Exp $ */

/*{{{ includes */

#include "config.h"
#include <math.h>
#include <float.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#ifdef HAVE_STDLIB_H
#  include <stdlib.h>
#endif

#include <slang.h>
#include "util.h"
#include "isis.h"
#include "fit.h"
#include "_isis.h"
#include "errors.h"

/*}}}*/

/*{{{ table of fit functions */

static Fit_Fun_t *Fit_Fun;
static Fit_Fun_t *find_fit_fun (unsigned int fun_type, Fit_Fun_t *head);

/*{{{ methods */

static int set_kernel_param_default (Fit_Fun_t *ff, Param_Info_t *p) /*{{{*/
{
   Isis_Kernel_Def_t *def = (Isis_Kernel_Def_t *)ff->client_data;

   if (def == NULL)
     return -1;

   if (def->default_min) p->min = def->default_min[p->fun_par];
   if (def->default_max) p->max = def->default_max[p->fun_par];
   if (def->default_freeze) p->freeze = def->default_freeze[p->fun_par];
   if (def->default_value) p->value = def->default_value[p->fun_par];
   if (def->default_min || def->default_max) p->set_minmax = 1;

   return 0;
}

/*}}}*/

static int set_cfun_param_default (Fit_Fun_t *ff, Param_Info_t *p) /*{{{*/
{
   if (ff == NULL || p == NULL)
     return -1;

   if (ff->s.default_min) p->min = ff->s.default_min[p->fun_par];
   if (ff->s.default_max) p->max = ff->s.default_max[p->fun_par];
   if (ff->s.default_freeze) p->freeze = ff->s.default_freeze[p->fun_par];
   if (ff->s.default_value) p->value = ff->s.default_value[p->fun_par];
   if (ff->s.default_min || ff->s.default_max) p->set_minmax = 1;

   return 0;
}

/*}}}*/

static int set_slangfun_param_default (Fit_Fun_t *ff, Param_Info_t *p) /*{{{*/
{
   int got_min, got_max;

   got_min = got_max = 0;

   /* ok if default aren't provided */
   if (ff->slangfun_param_default == NULL)
     return 0;

   SLang_start_arg_list ();
   SLang_push_integer (p->fun_par);
   isis_push_args (ff->slangfun_param_default_args);
   SLang_end_arg_list ();

   /* returns (value, freeze, min, max) */
   if (-1 == SLexecute_function (ff->slangfun_param_default))
     return -1;

   if (SLANG_NULL_TYPE == SLang_peek_at_stack ())
     SLdo_pop();
   else
     {
        SLang_pop_double (&p->max);
        got_max = 1;
     }

   if (SLANG_NULL_TYPE == SLang_peek_at_stack ())
     SLdo_pop();
   else
     {
        SLang_pop_double (&p->min);
        got_min = 1;
     }

   SLang_pop_uinteger (&p->freeze);
   SLang_pop_double (&p->value);

   if (got_min || got_max)
     p->set_minmax = 1;

   if ((got_min && got_max) && (p->min > p->max))
     {
        double tmp = p->min;
        p->min = p->max;
        p->max = tmp;
     }

   return 0;
}

/*}}}*/

static int c_bin_eval (Fit_Fun_t *ff, Isis_Hist_t *g, double *par) /*{{{*/
{
   SLang_Array_Type *at = NULL;
   int status;

   at = SLang_create_array (SLANG_DOUBLE_TYPE, 0, NULL, &g->n_notice, 1);
   if (NULL == at)
     return -1;

   if ((ff->s.category == ISIS_FUN_OPERATOR)
       && (-1 == Isis_pop_double_array ((double *)at->data, g->n_notice)))
     {
        SLang_free_array (at);
        return -1;
     }

   status = (*ff->fun.c)((double *) at->data, g, par, ff->nparams);
   (void) SLang_push_array (at, 1);
   return status;
}

/*}}}*/

static int c_diff_eval (Fit_Fun_t *ff, Isis_User_Grid_t *ug, double *par) /*{{{*/
{
   SLang_Array_Type *at = NULL;
   int size = ug->npts;

   if (ff->s.unbinned == NULL)
     return -1;

   if ((NULL == (at = SLang_create_array (SLANG_DOUBLE_TYPE, 0, NULL, &size, 1)))
       || (-1 == (*ff->s.unbinned) ((double *) at->data, ug, par, ff->nparams))
       || (-1 == SLang_push_array (at, 1)))
     {
        SLang_free_array (at);
        return -1;
     }

   return 0;
}

/*}}}*/

static int sl_bin_eval (Fit_Fun_t *ff, Isis_Hist_t *g, double *par) /*{{{*/
{
   SLang_Array_Type *sl_par=NULL, *sl_arg=NULL;

   if (ff->s.category == ISIS_FUN_OPERATOR)
     {
        sl_arg = SLang_create_array (SLANG_DOUBLE_TYPE, 0, NULL, &g->n_notice, 1);
        if ((sl_arg == NULL)
            || (-1 == Isis_pop_double_array ((double *)sl_arg->data, g->n_notice)))
          {
             SLang_free_array (sl_arg);
             return -1;
          }
     }

   sl_par = SLang_create_array (SLANG_DOUBLE_TYPE, 1, NULL, (int *)&ff->nparams, 1);
   if (sl_par != NULL)
     memcpy ((char *)sl_par->data, (char *)par, ff->nparams * sizeof(double));

   SLang_start_arg_list ();
   if ((-1 == Isis_Hist_push_noticed_grid (g))
       || (-1 == SLang_push_array (sl_par, 1)))
     {
        SLang_free_array (sl_par);
        SLang_free_array (sl_arg);
        return -1;
     }
   if ((ff->s.category == ISIS_FUN_OPERATOR)
       && (-1 == SLang_push_array (sl_arg, 1)))
     {
        SLang_free_array (sl_arg);
        return -1;
     }
   SLang_end_arg_list ();

   if (1 == SLexecute_function (ff->fun.sl))
     return 0;

   return -1;
}

/*}}}*/

static int sl_diff_eval (Fit_Fun_t *ff, Isis_User_Grid_t *ug, double *par) /*{{{*/
{
   SLang_Array_Type *sl_x=NULL, *sl_par=NULL;

   if (ff->slangfun_diff_eval == NULL)
     {
        isis_vmesg (INTR, I_ERROR, __FILE__, __LINE__,
                    "%s does not support this evaluation method", ff->name[0]);
        isis_throw_exception (Isis_Error);
        return -1;
     }

   sl_par = SLang_create_array (SLANG_DOUBLE_TYPE, 1, NULL, (int *)&ff->nparams, 1);
   if (sl_par != NULL)
     memcpy ((char *)sl_par->data, (char *)par, ff->nparams * sizeof(double));

   sl_x = SLang_create_array (SLANG_DOUBLE_TYPE, 1, NULL, (int *)&ug->npts, 1);
   if (sl_x != NULL)
     memcpy ((char *)sl_x->data, (char *)ug->x, ug->npts * sizeof(double));

   SLang_start_arg_list ();
   if ((-1 == SLang_push_array (sl_x, 1))
       || (-1 == SLang_push_array (sl_par, 1)))
     {
        SLang_free_array (sl_par);
        SLang_free_array (sl_x);
        return -1;
     }
   SLang_end_arg_list ();

   if (1 == SLexecute_function (ff->slangfun_diff_eval))
     return 0;

   return -1;
}

/*}}}*/

static void default_destroy_fun (Fit_Fun_t *ff) /*{{{*/
{
   (void) ff;
}

/*}}}*/

static void slangfun_destroy_fun (Fit_Fun_t *ff) /*{{{*/
{
   isis_free_args (ff->slangfun_param_default_args);
   SLang_free_function (ff->slangfun_param_default);
   SLang_free_function (ff->fun.sl);
   SLang_free_function (ff->slangfun_diff_eval);
   ISIS_FREE (ff->s.norm_indexes);
}

/*}}}*/

/*}}}*/

static void free_fit_fun (Fit_Fun_t *ff) /*{{{*/
{
   if (ff == NULL) return;

   if (ff->s.function_exit)
     (*ff->s.function_exit)();

   (*ff->destroy_fun)(ff);

   ISIS_FREE (ff->name);
   ISIS_FREE (ff->unit);
   ISIS_FREE (ff);
}

/*}}}*/

static Fit_Fun_t *new_fit_fun (unsigned int num_args) /*{{{*/
{
   unsigned int i;
   Fit_Fun_t *ff;

   if (NULL == (ff = ISIS_MALLOC (sizeof(Fit_Fun_t))))
     return NULL;
   memset ((char *) ff, 0, sizeof (*ff));

   /* most common default */
   ff->s.category = ISIS_FUN_ADDMUL;

   if ((NULL == (ff->name = ISIS_MALLOC ((num_args + 1) * sizeof(Fit_Fun_Name_t))))
       ||(NULL == (ff->unit = ISIS_MALLOC (num_args * sizeof(Fit_Fun_Name_t)))))
     {
        free_fit_fun (ff);
        return NULL;
     }

   for (i = 0; i < num_args; i++)
     {
        ff->name[i][0] = 0;
        ff->unit[i][0] = 0;
     }
   ff->name[num_args][0] = 0;

   ff->fun_type = 0;
   ff->fun_version = 1;
   ff->next = NULL;

   ff->set_param_default = &set_cfun_param_default;
   ff->slangfun_param_default = NULL;
   ff->slangfun_param_default_args = NULL;

   ff->bin_eval_method = &c_bin_eval;
   ff->diff_eval_method = &c_diff_eval;
   ff->destroy_fun = &default_destroy_fun;

   return ff;
}

/*}}}*/

static void free_all_fit_functions (Fit_Fun_t *head) /*{{{*/
{
   Fit_Fun_t *next;

   while (head)
     {
        next = head->next;
        free_fit_fun (head);
        head = next;
     }
}

/*}}}*/

int Fit_get_fun_par (Fit_Fun_t *ff, char *par_name) /*{{{*/
{
   unsigned int i;

   if (ff == NULL || par_name == NULL)
     return -1;

   for (i = 1; i <= ff->nparams; i++)
     {
        if (0 == strcmp (par_name, ff->name[i]))
          return i-1;
     }

   return -1;
}

/*}}}*/

int Fit_get_fun_type (char *name) /*{{{*/
{
   Fit_Fun_t *ff;

   for (ff = Fit_Fun; ff != NULL; ff = ff->next)
     {
        if (0 == strcmp (name, ff->name[0]))
          return ff->fun_type;
     }

   return -1;
}

/*}}}*/

static Fit_Fun_t *find_fit_fun (unsigned int fun_type, Fit_Fun_t *head) /*{{{*/
{
   Fit_Fun_t *ff;

   for (ff = head; ff != NULL; ff = ff->next)
     {
        if (ff->fun_type == fun_type)
          break;
     }

   return ff;
}

/*}}}*/

static int append_fit_fun (Fit_Fun_t *pf, Fit_Fun_t *head) /*{{{*/
{
   Fit_Fun_t *ff;

   if ((pf == NULL) || (head == NULL))
     return -1;

   for (ff = head; ff->next != NULL; ff = ff->next)
     {
        if (0 == strcmp (ff->next->name[0], pf->name[0]))
          {
             /* Re-use fun_type index when re-defining fit-functions.
              * Test fun_version to detect such changes.
              */
             pf->fun_type = ff->next->fun_type;
             pf->fun_version = ff->next->fun_version + 1;
             pf->next = ff->next->next;
             free_fit_fun (ff->next);
             ff->next = pf;
             return 0;
          }
     }

   pf->fun_type = ff->fun_type + 1;
   ff->next = pf;

   return 0;
}

/*}}}*/

static int init_fun_table (void) /*{{{*/
{
   if (Fit_Fun)
     return 0;

   if (NULL == (Fit_Fun = new_fit_fun (0)))
     return -1;
   Fit_Fun->fun_type = UINT_MAX;

   return 0;
}

/*}}}*/

/*}}}*/

/*{{{ maintain fit function table */

int Fit_set_fun_param_default (Param_Info_t *p) /*{{{*/
{
   Fit_Fun_t *ff = Fit_get_fit_fun (p->fun_type);

   if (ff == NULL)
     return -1;

   return (*ff->set_param_default) (ff, p);
}

/*}}}*/

Fit_Fun_t *Fit_get_fit_fun (int fun_type) /*{{{*/
{
   return find_fit_fun (fun_type, Fit_Fun);
}

/*}}}*/

void set_function_category (char *fun_name, unsigned int *category) /*{{{*/
{
   Fit_Fun_t *ff;

   if (fun_name == NULL)
     return;

   for (ff = Fit_Fun; ff != NULL; ff = ff->next)
     {
        if (0 == strcmp (ff->name[0], fun_name))
          {
             ff->s.category = *category;
             return;
          }
     }
}

/*}}}*/

void del_function (char *fun_name) /*{{{*/
{
   Fit_Fun_t *ff;

   if (fun_name == NULL)
     return;

   for (ff = Fit_Fun; (ff != NULL) && (ff->next != NULL); ff = ff->next)
     {
        if (0 == strcmp (ff->next->name[0], fun_name))
          {
             Fit_Fun_t *t = ff->next->next;
             free_fit_fun (ff->next);
             ff->next = t;
             return;
          }
     }
}

/*}}}*/

void function_list (void) /*{{{*/
{
   Fit_Fun_t *ff = Fit_Fun;
   SLang_Array_Type *sl_names = NULL;
   int n;

   if (ff == NULL)
     return;

   n = 0;
   for (ff = ff->next; ff != NULL; ff = ff->next)
     n++;

   if (n == 0)
     return;

   sl_names = SLang_create_array (SLANG_STRING_TYPE, 1, NULL, &n, 1);
   if(sl_names == NULL)
     {
        isis_throw_exception (Isis_Error);
        return;
     }

   n = 0;
   ff = Fit_Fun;

   for (ff = ff->next; ff != NULL; ff = ff->next)
     {
        if (SLang_set_array_element (sl_names, &n, &ff->name))
          {
             isis_throw_exception (Isis_Error);
             break;
          }
        n++;
     }

   SLang_start_arg_list ();
   SLang_push_array (sl_names, 1);
   SLang_end_arg_list ();
}

/*}}}*/

/*}}}*/

/*{{{ support user-defined functions */

/* UDF = user-defined function */
typedef struct UDF_Info_Type UDF_Info_Type;
struct UDF_Info_Type
{
   SLang_Array_Type *sl_pnames;      /* parameter names */
   SLang_Array_Type *sl_units;       /* parameter unit names */
   SLang_Array_Type *sl_norm_ids;    /* ids of parameters considered norms */
   char **pnames;                    /* copy of parameter names */
   char **units;                     /* copy of unit names */
   char *fun_name;                   /* new function name */
   char *lib_name;                   /* .so name for C functions */
   void *fdata[2];                   /* tmp storage */
   Isis_User_Source_Init_Fun_t *static_initfun;     /* initfun for static functions */
   unsigned int num;                 /* number of parameters */
   Fit_Fun_t *(*make_fitfun_of_type)(UDF_Info_Type *);
   void *client_data;
};

#define NULL_UDF_INFO_TYPE {0,0,0,0,0,0,0,{0,0},0,0,NULL,NULL}

static int check_name_string (char *s, int size) /*{{{*/
{
   if (NULL == s)
     return -1;

   /* empty string is ok */
   if (*s == 0) return 0;

   /* allow for terminating null char */
   if (--size < 1)
     return -1;

   for ( ;*s != '\0' && size-- > 0; s++)
     {
        if (isspace((int)*s) || iscntrl((int)*s))
          return -1;
     }

   return ((size < 0) ? -1 : 0);
}

/*}}}*/

static int check_new_function (char *fun_name, int nparams, /*{{{*/
                               char **param_name, char **param_unit)
{
   int j;

   if ((NULL == fun_name) || (nparams < 0))
     return -1;

   if (-1 == check_name_string (fun_name, MAX_NAME_SIZE))
     goto bad_name_string;

   if (nparams == 0 || param_name == NULL)
     return 0;

   for (j = 0; j < nparams; j++)
     {
        if (-1 == check_name_string (param_name[j], MAX_NAME_SIZE))
          goto bad_name_string;

        if ((param_unit != NULL)
            && (param_unit[j] != NULL)
            && (strlen (param_unit[j]) >= MAX_NAME_SIZE))
          {
             isis_vmesg (FAIL, I_ERROR, __FILE__, __LINE__, "%s definition:  units string length exceeds %d characters",
                         fun_name,
                         MAX_NAME_SIZE-1);
             return -1;
          }
     }

   return 0;

   bad_name_string:
   isis_vmesg (FAIL, I_ERROR, __FILE__, __LINE__, "invalid string:  names must have 1-%d non-whitespace characters",
               MAX_NAME_SIZE-1);
   return -1;
}

/*}}}*/

static int set_function_name_fields (Fit_Fun_t *pf, char *fun_name, /*{{{*/
                                     unsigned int num, char **pnames, char **units)
{
   unsigned int j;

   if (-1 == check_new_function (fun_name, num, pnames, units))
     return -1;

   isis_strcpy (pf->name[0], fun_name, MAX_NAME_SIZE);

   if ((pnames == NULL)
       || ((num == 1) && (0 == *pnames[0])))
     {
        pf->nparams = 0;
        return 0;
     }

   pf->nparams = num;
   for (j=0; j < pf->nparams; j++)
     {
        if (NULL == pnames[j])
          return -1;
        isis_strcpy (pf->name[j+1], pnames[j], MAX_NAME_SIZE);
        if (units && units[j])
          isis_strcpy (pf->unit[j], units[j], MAX_NAME_SIZE);
     }

   return 0;
}

/*}}}*/

static int set_slangfun_norms (Isis_User_Source_t *s, UDF_Info_Type *u) /*{{{*/
{
   SLang_Array_Type *u_sl_norm_ids = u->sl_norm_ids;
   unsigned int u_num = u->num;

   s->num_norms = 0;
   s->norm_indexes = NULL;

   /* Did the user specify which parameters are the norms?
    * [First norm index==num_parameters (which is out of range)
    * means that no norm indexes were provided]
    */
   if ((u_sl_norm_ids != NULL)
       && (u_sl_norm_ids->num_elements > 0)
       && (!(*((unsigned int *)u_sl_norm_ids->data) == u_num)))
     {
        unsigned int num = u_sl_norm_ids->num_elements;
        unsigned int size = num * sizeof(unsigned int);
        void *data = u_sl_norm_ids->data;

        if (NULL == (s->norm_indexes = ISIS_MALLOC (num * sizeof(unsigned int))))
          return -1;

        memcpy ((char *)s->norm_indexes, (char *)data, size);
        s->num_norms = num;

        return 0;
     }

   /* If not, check for a parameter called 'norm' or 'Norm' or ...*/
   while (u_num-- > 0)
     {
        if (0 == isis_strcasecmp (u->pnames[u_num], "norm"))
          {
             /* yuk */
             if (NULL == (s->norm_indexes = ISIS_MALLOC (sizeof(unsigned int))))
               return -1;

             *s->norm_indexes = u_num;
             s->num_norms = 1;

             break;
          }
     }

   /* Conclude there is no norm (==> multiplicative model) */
   return 0;
}

/*}}}*/

static Fit_Fun_t *make_slangfun (UDF_Info_Type *u) /*{{{*/
{
   Fit_Fun_t *pf;

   if (u->fdata[0] == NULL || u->fun_name == NULL)
     return NULL;

   if (NULL == (pf = new_fit_fun (u->num)))
     return NULL;

   pf->bin_eval_method = &sl_bin_eval;
   pf->diff_eval_method = &sl_diff_eval;
   pf->set_param_default = &set_slangfun_param_default;
   pf->destroy_fun = &slangfun_destroy_fun;

   if (-1 == set_function_name_fields (pf, u->fun_name, u->num, u->pnames, u->units))
     {
        free_fit_fun (pf);
        return NULL;
     }

   pf->fun.sl = (SLang_Name_Type *)u->fdata[0];
   pf->slangfun_diff_eval = (SLang_Name_Type *)u->fdata[1];

   memset ((char *)&pf->s, 0, sizeof(pf->s));

   if (-1 == set_slangfun_norms (&pf->s, u))
     {
        free_fit_fun (pf);
        return NULL;
     }

   return pf;
}

/*}}}*/

static Fit_Fun_t *make_kernel_fun (UDF_Info_Type *u) /*{{{*/
{
   Fit_Fun_t *ff;

   if (NULL == (ff = make_slangfun (u)))
     return NULL;

   ff->set_param_default = &set_kernel_param_default;
   ff->client_data = u->client_data;

   return ff;
}

/*}}}*/

static Fit_Fun_t *do_user_source_init (Isis_User_Source_Init_Fun_t *us_init_fun, /*{{{*/
                                       char *fun_name, char *options)
{
   Isis_User_Source_t s;
   Fit_Fun_t *pf;

   /* ptrs default to NULL */
   memset ((char *)&s, 0, sizeof(s));

   if ((-1 == (*us_init_fun)(&s, options))
       || (s.binned == NULL))
     return NULL;

   /* allow null terminated parameter name list */
   if (s.num_parameters == 0)
     {
        char **p = s.parameter_names;
        while (*p) p++;
        s.num_parameters = p - s.parameter_names;
     }

   if (NULL == (pf = new_fit_fun (s.num_parameters)))
     return NULL;

   pf->fun.c = s.binned;
   /* struct copy */
   pf->s = s;

   if (-1 == set_function_name_fields (pf, fun_name, s.num_parameters, s.parameter_names, s.parameter_units))
     {
        free_fit_fun (pf);
        return NULL;
     }

   return pf;
}

/*}}}*/

static Isis_User_Source_Init_Fun_t *get_us_init_fun (char *libfile, char *fun_name) /*{{{*/
{
   Isis_User_Source_Init_Fun_t *f;

   if (NULL == libfile)
     return NULL;

   f = (Isis_User_Source_Init_Fun_t *)isis_load_function (libfile, fun_name, "function");

   return f;
}

/*}}}*/

static Fit_Fun_t *make_cfun (UDF_Info_Type *u) /*{{{*/
{
   Isis_User_Source_Init_Fun_t *us_init_fun = NULL;
   char *options;

   if (u->fun_name == NULL)
     return NULL;

   if (u->fdata[0])
     us_init_fun = get_us_init_fun ((char *)u->fdata[0], u->fun_name);
   else
     us_init_fun = u->static_initfun;

   if (us_init_fun == NULL)
     return NULL;

   options = u->pnames ? u->pnames[0] : NULL;

   return do_user_source_init (us_init_fun, u->fun_name, options);
}

/*}}}*/

static Fit_Fun_t *add_function_of_type (UDF_Info_Type *u) /*{{{*/
{
   Fit_Fun_t *pf = NULL;

   if (u == NULL || u->make_fitfun_of_type == NULL)
     return NULL;

   if (NULL == (pf = (*u->make_fitfun_of_type)(u)))
     {
        isis_vmesg (FAIL, I_FAILED, __FILE__, __LINE__, "adding fit-function");
        return NULL;
     }

   if (0 != append_fit_fun (pf, Fit_Fun))
     {
        free_fit_fun (pf);
        return NULL;
     }

   return pf;
}

/*}}}*/

#define FUNCTION_WRAPPER_FORMAT_STRING \
   "define %s () {\
      variable id, num_args = _NARGS; \
      if (num_args == 0) \
        id = 1;\
      else {_stk_roll(-num_args); id = (); num_args--;}\
      return _isis->_mode_switch (id, %d, num_args); \
   }"

static int wrap_mode_switch (Fit_Fun_t *ff) /*{{{*/
{
   char buf[2*BUFSIZE];

   if (ff == NULL)
     return -1;

   /* note that fit-functions go into the Global namespace */
   (void) sprintf (buf, FUNCTION_WRAPPER_FORMAT_STRING,
                   ff->name[0], ff->fun_type);

   return SLang_load_string (buf);
}

/*}}}*/

static int do_add_function (UDF_Info_Type *u) /*{{{*/
{
   Fit_Fun_t *ff = NULL;

   if (NULL == (ff = add_function_of_type (u)))
     {
        isis_vmesg (FAIL, I_FAILED, __FILE__, __LINE__, "adding fit-function");
        return -1;
     }

   if (-1 == wrap_mode_switch (ff))
     {
        isis_vmesg (FAIL, I_FAILED, __FILE__, __LINE__, "initializing fit-function");
        del_function (u->fun_name);
        return -1;
     }

   return 0;
}

/*}}}*/

static void free_udf_info (UDF_Info_Type *u) /*{{{*/
{
   if (u == NULL)
     return;
   ISIS_FREE (u->pnames);
   ISIS_FREE (u->units);
   SLfree (u->fun_name);
   SLfree (u->lib_name);
   SLang_free_array (u->sl_pnames);
   SLang_free_array (u->sl_units);
   SLang_free_array (u->sl_norm_ids);
}

/*}}}*/

static int copy_slstring_array (unsigned int n, SLang_Array_Type *sl, char **s) /*{{{*/
{
   unsigned int i;

   for (i = 0; i < n; i++)
     {
        int si = i;
        if (-1 == SLang_get_array_element (sl, &si, (VOID_STAR) &s[i]))
          return -1;
     }

   return 0;
}

/*}}}*/

static int pop_udf_info (UDF_Info_Type *u) /*{{{*/
{
   /* For S-Lang functions, sl_pnames holds function parameter names.
    * For compiled functions, sl_pnames holds an option string.
    * In either case, it might be a single empty string but should
    * always be non-NULL.
    */

   if ((-1 == SLang_pop_array_of_type (&u->sl_norm_ids, SLANG_UINT_TYPE))
       || (-1 == SLang_pop_array_of_type (&u->sl_units, SLANG_STRING_TYPE))
       || -1 == SLang_pop_array_of_type (&u->sl_pnames, SLANG_STRING_TYPE)
       || u->sl_pnames == NULL
       || -1 == SLpop_string (&u->fun_name))
     {
        free_udf_info (u);
        return -1;
     }

   u->num = (int) u->sl_pnames->num_elements;

   if ((NULL == (u->pnames = ISIS_MALLOC (u->num * sizeof(char *))))
       || (NULL == (u->units = ISIS_MALLOC (u->num * sizeof(char *)))))
     {
        free_udf_info (u);
        return -1;
     }
   memset ((char *)u->units, 0, u->num * sizeof(char *));

   if ((-1 == copy_slstring_array (u->num, u->sl_pnames, u->pnames))
       || ((u->sl_units != NULL)
           && (-1 == copy_slstring_array (u->num, u->sl_units, u->units))))
     {
        free_udf_info (u);
        return -1;
     }

   return 0;
}

/*}}}*/

static int add_function_intrin (UDF_Info_Type *u, void *client_data, /*{{{*/
                                int (*customize)(UDF_Info_Type *, void *))
{
   if (u == NULL)
     return -1;

   memset ((char *)u, 0, sizeof (*u));

   if (-1 == pop_udf_info (u))
     return -1;

   if (-1 == (*customize)(u, client_data))
     return -1;

   return do_add_function (u);
}

/*}}}*/

static int slangfun_customize (UDF_Info_Type *u, void *client_data) /*{{{*/
{
   (void) client_data;

   u->make_fitfun_of_type = &make_slangfun;
   if (SLANG_NULL_TYPE == SLang_peek_at_stack ())
     {
        SLdo_pop();
     }
   else u->fdata[1] = SLang_pop_function();

   u->fdata[0] = SLang_pop_function();

   return 0;
}

/*}}}*/

void add_slangfun_intrin (void) /*{{{*/
{
   UDF_Info_Type u;
   if (-1 == add_function_intrin (&u, NULL, &slangfun_customize))
     {
        isis_vmesg (INTR, I_ERROR, __FILE__, __LINE__, "function not defined");
     }
   free_udf_info (&u);
}

/*}}}*/

static int cfun_customize (UDF_Info_Type *u, void *client_data) /*{{{*/
{
   (void) client_data;

   u->make_fitfun_of_type = &make_cfun;
   if (-1 == SLpop_string (&u->lib_name))
     u->fdata[0] = NULL;
   else u->fdata[0] = u->lib_name;

   return 0;
}

/*}}}*/

void add_cfun_intrin (void) /*{{{*/
{
   UDF_Info_Type u;
   if (-1 == add_function_intrin (&u, NULL, &cfun_customize))
     {
        isis_vmesg (INTR, I_ERROR, __FILE__, __LINE__, "function not defined");
     }
   free_udf_info (&u);
}

/*}}}*/

int Fit_add_kernel_function (Isis_Kernel_Def_t *def) /*{{{*/
{
   UDF_Info_Type u;
   char buf[MAX_NAME_SIZE +5];
   char fun[256];
   int fun_type;

   sprintf (fun, "define %s_fit(l,h,p){return 1.0;}", def->kernel_name);
   if (-1 == SLang_load_string (fun))
     return -1;
   sprintf (buf, "%s_fit", def->kernel_name);

   memset ((char *)&u, 0, sizeof (u));

   u.make_fitfun_of_type = &make_kernel_fun;
   u.num = def->num_kernel_parms;
   u.pnames = def->kernel_parm_names;
   u.units = def->kernel_parm_units;
   u.fun_name = def->kernel_name;
   u.fdata[0] = SLang_get_function (buf);
   if (u.fdata[0] == NULL)
     return -1;

   u.client_data = def;

   if (-1 == do_add_function (&u))
     return -1;

   if (-1 == (fun_type = Fit_get_fun_type (def->kernel_name)))
     return -1;

   def->fun_type = fun_type;

   return 0;
}

/*}}}*/

/*}}}*/

int Isis_Add_Static_Fun (Isis_User_Source_Init_Fun_t *us_init, char *us_name) /*{{{*/
{
   UDF_Info_Type u = NULL_UDF_INFO_TYPE;

   u.static_initfun = us_init;
   u.fun_name = us_name;
   u.make_fitfun_of_type = &make_cfun;

   return do_add_function (&u);
}
/*}}}*/

static Fit_Fun_t *find_function (Fit_Fun_t *head, char *name) /*{{{*/
{
   Fit_Fun_t *ff;

   if (name == NULL)
     return NULL;

   for (ff = head; ff != NULL; ff = ff->next)
     {
        if (0 == strcmp (ff->name[0], name))
          return ff;
     }

   return NULL;
}

/*}}}*/

typedef struct
{
   SLang_Array_Type *name;
   SLang_Array_Type *value;
   SLang_Array_Type *min;
   SLang_Array_Type *max;
   SLang_Array_Type *freeze;
   SLang_Array_Type *unit;
}
Fit_Fun_Info_Type;

static SLang_CStruct_Field_Type Fit_Fun_Info_Type_Layout [] =
{
   MAKE_CSTRUCT_FIELD (Fit_Fun_Info_Type, name, "name", SLANG_ARRAY_TYPE, 0),
   MAKE_CSTRUCT_FIELD (Fit_Fun_Info_Type, unit, "unit", SLANG_ARRAY_TYPE, 0),
   MAKE_CSTRUCT_FIELD (Fit_Fun_Info_Type, value, "value", SLANG_ARRAY_TYPE, 0),
   MAKE_CSTRUCT_FIELD (Fit_Fun_Info_Type, min, "min", SLANG_ARRAY_TYPE, 0),
   MAKE_CSTRUCT_FIELD (Fit_Fun_Info_Type, max, "max", SLANG_ARRAY_TYPE, 0),
   MAKE_CSTRUCT_FIELD (Fit_Fun_Info_Type, freeze, "freeze", SLANG_ARRAY_TYPE, 0),
   SLANG_END_CSTRUCT_TABLE
};

void Fit_get_fun_info (char *name) /*{{{*/
{
   Param_Info_t p;
   Fit_Fun_Info_Type fi;
   Fit_Fun_t *ff;
   int i, num_pars;

   memset ((char *)&fi, 0, sizeof fi);

   if (NULL == (ff = find_function (Fit_Fun, name)))
     goto push_struct;

   num_pars = ff->nparams;

   if ((NULL == (fi.name = SLang_create_array (SLANG_STRING_TYPE, 0, NULL, &num_pars, 1)))
       || (NULL == (fi.unit = SLang_create_array (SLANG_STRING_TYPE, 0, NULL, &num_pars, 1)))
       || (NULL == (fi.value = SLang_create_array (SLANG_DOUBLE_TYPE, 0, NULL, &num_pars, 1)))
       || (NULL == (fi.min = SLang_create_array (SLANG_DOUBLE_TYPE, 0, NULL, &num_pars, 1)))
       || (NULL == (fi.max = SLang_create_array (SLANG_DOUBLE_TYPE, 0, NULL, &num_pars, 1)))
       || (NULL == (fi.freeze = SLang_create_array (SLANG_UINT_TYPE, 0, NULL, &num_pars, 1)))
      )
     {
        isis_throw_exception (Isis_Error);
        goto push_struct;
     }

   p.fun_type = ff->fun_type;
   p.fun_version = ff->fun_version;
   p.fun_id = -1;

   for (i = 0; i < num_pars; i++)
     {
        char *unit;

        p.fun_par = i;
        p.param_name = ff->name[i+1];
        p.value = 0.0;
        p.min = 0.0;
        p.max = 0.0;
        p.freeze = 0;

        unit = ff->unit[i];

        if (-1 == (*ff->set_param_default)(ff, &p))
          {
             isis_throw_exception (Isis_Error);
             goto push_struct;
          }

        if ((0 != SLang_set_array_element (fi.name, &i, &p.param_name))
            || (0 != SLang_set_array_element (fi.unit, &i, &unit))
            || (0 != SLang_set_array_element (fi.value, &i, &p.value))
            || (0 != SLang_set_array_element (fi.min, &i, &p.min))
            || (0 != SLang_set_array_element (fi.max, &i, &p.max))
            || (0 != SLang_set_array_element (fi.freeze, &i, &p.freeze))
           )
          {
             isis_throw_exception (Isis_Error);
             goto push_struct;
          }
     }

push_struct:
   (void) SLang_push_cstruct ((VOID_STAR)&fi, Fit_Fun_Info_Type_Layout);
   SLang_free_cstruct ((VOID_STAR)&fi, Fit_Fun_Info_Type_Layout);
}

/*}}}*/

void deinit_fit_functions (void) /*{{{*/
{
   free_all_fit_functions (Fit_Fun);
   Fit_Fun = NULL;
}

/*}}}*/

int init_fit_functions (void) /*{{{*/
{
   if (-1 == init_fun_table ())
     return -1;

   if (-1 == Fit_Append_Builtin_Functions ())
     {
        isis_vmesg (FAIL, I_INTERNAL, __FILE__, __LINE__, "failed initializing built-in fit-functions");
        return -1;
     }

   if (-1 == SLang_run_hooks ("init_static_slang_functions", 0))
     return -1;

   return 0;
}

/*}}}*/
