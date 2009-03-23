% -*- mode: SLang; mode: fold -*-
%
%    This file is part of ISIS, the Interactive Spectral Interpretation System
%    Copyright (C) 1998-2008 Massachusetts Institute of Technology
%
%    This software was developed by the MIT Center for Space Research under
%    contract SV1-61010 from the Smithsonian Institution.
%
%    Author:  John C. Houck  <houck@space.mit.edu>
%
%    This program is free software; you can redistribute it and/or modify
%    it under the terms of the GNU General Public License as published by
%    the Free Software Foundation; either version 2 of the License, or
%    (at your option) any later version.
%
%    This program is distributed in the hope that it will be useful,
%    but WITHOUT ANY WARRANTY; without even the implied warranty of
%    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
%    GNU General Public License for more details.
%
%    You should have received a copy of the GNU General Public License
%    along with this program; if not, write to the Free Software
%    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
%

#ifnexists require
() = evalfile ("require", _isis->Isis_Public_Namespace_Name);
#endif

#ifnexists quit
define quit () {_isis->_quit (0);}
#endif
define isis_quit () {_isis->_quit (0);}

#ifnexists exit
define exit ()
{
   if (_NARGS > 0) _isis->_quit ();
   isis_quit();
}
#endif
define isis_exit ()
{
   if (_NARGS > 0) _isis->_quit ();
   isis_quit ();
}

private define _set_path (nargs, msg, setfun) %{{{
{
   if (nargs != 1)
     usage (msg);

   variable path = ();
   if (path == NULL)
     path = "";

   (@setfun)(path);
}

%}}}

define _prepend (dir, getfun, setfun) %{{{
{
   if (orelse
       {dir == NULL}
       {strlen(dir) == 0})
     return;

   variable path = (@getfun)();
   if (orelse
       {path == NULL}
       {strlen(path) == 0})
     (@setfun)(dir);
   else
     (@setfun)(sprintf ("%s:%s", dir, path));
}

%}}}

define _append (dir, getfun, setfun) %{{{
{
   if (orelse
       {dir == NULL}
       {strlen(dir) == 0})
     return;

   variable path = (@getfun)();
   if (orelse
       {path == NULL}
       {strlen(path) == 0})
     (@setfun)(dir);
   else
     (@setfun)(sprintf ("%s:%s", path, dir));
}

%}}}

define get_isis_load_path () %{{{
{
   return get_slang_load_path ();
}
%}}}

define set_isis_load_path () %{{{
{
   variable msg = "set_isis_load_path (path)";
   _set_path (_NARGS, msg, &set_slang_load_path);
}

%}}}

define prepend_to_isis_load_path (dir) %{{{
{
   _prepend (dir, &get_isis_load_path, &set_isis_load_path);
}

%}}}

define append_to_isis_load_path (dir) %{{{
{
   _append (dir, &get_isis_load_path, &set_isis_load_path);
}

%}}}

define get_isis_module_path () %{{{
{
   return get_import_module_path ();
}
%}}}

define set_isis_module_path () %{{{
{
   variable msg = "set_isis_module_path (path)";
   _set_path (_NARGS, msg, &set_import_module_path);
}

%}}}

define prepend_to_isis_module_path (dir) %{{{
{
   _prepend (dir, &get_isis_module_path, &set_isis_module_path);
}

%}}}

define append_to_isis_module_path (dir) %{{{
{
   _append (dir, &get_isis_module_path, &set_isis_module_path);
}

%}}}

% deprecated: keep for back-compatibility
define add_to_isis_load_path (dir)
{
   prepend_to_isis_load_path (dir);
}

% deprecated: keep for back-compatibility
define add_to_isis_module_path (dir)
{
   prepend_to_isis_module_path (dir);
}

private define setup_slang_paths (dir) %{{{
{
   append_to_isis_load_path (dir + "/share/slsh");
   append_to_isis_load_path (dir + "/share/slsh/local-packages");
   if (_slang_version < 20000)
     append_to_isis_module_path (dir + "/lib/slang/modules");
   else
     append_to_isis_module_path (dir + "/lib/slang/v2/modules");
}

%}}}

private define init_slang_paths () %{{{
{
   variable p;

#ifnexists _slang_install_prefix
   variable _slang_install_prefix = "/usr/local";
#endif

   % slang compiled locally?
   p = _slang_install_prefix;
   if (NULL != stat_file (path_concat (p, "share/slsh")))
     {
        setup_slang_paths (p);
        return;
     }

   % isis built locally?
   p = _isis->_isis_config_slang_libdir;
   if (NULL != stat_file (path_concat (p, "../share/slsh")))
     {
        (p,) = strreplace (p, "/lib", "", 1);
        setup_slang_paths (p);
        return;
     }

   % isis binary distribution?
   p = getenv ("ISIS_SRCDIR");
   if (NULL != p)
     {
        p = path_concat (p, "opt");
        if (NULL != stat_file (p))
          {
             setup_slang_paths (p);
             return;
          }
     }

   % some other kind of binary installation?
   variable env;
   foreach env (["SLSH_PATH", "CIAO_SLSH_PATH"])
     {
        p = getenv (env);
        if (NULL != p)
          {
             (p,) = strreplace (p, "/share/slsh", "", 1);
             setup_slang_paths (p);
             return;
          }
     }

   vmessage ("Warning:  trouble setting up slang search paths");
}

%}}}

init_slang_paths();