/*
 * Copyright (c) 2000, Red Hat, Inc.
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     A copy of the GNU General Public License can be found at
 *     http://www.gnu.org/
 *
 * Written by DJ Delorie <dj@cygnus.com>
 *
 */

/* The purpose of this file is to coordinate the various access
   methods known to setup.  To add a new method, create a pair of
   nio-*.[ch] files and add the logic to NetIO::open here */

static char *cvsid = "\n%%% $Id$\n";

#include "win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resource.h"
#include "state.h"
#include "msg.h"
#include "netio.h"
#include "nio-file.h"
#include "nio-ie5.h"
#include "dialog.h"

#include "port.h"

NetIO::NetIO (char *Purl)
{
  char *bp, *ep, c;

  file_size = 0;
  url = _strdup (Purl);
  proto = 0;
  host = 0;
  port = 0;
  path = 0;

  bp = url;
  ep = strstr (bp, "://");
  if (!ep)
    {
      path = url;
      return;
    }

  *ep = 0;
  proto = _strdup (bp);
  *ep = ':';
  bp = ep+3;

  ep += strcspn (bp, ":/");
  c = *ep;
  *ep = 0;
  host = _strdup (bp);
  *ep = c;

  if (*ep == ':')
    {
      port = atoi (ep+1);
      ep = strchr (ep, '/');
    }

  if (*ep)
    path = _strdup (ep);
}

NetIO::~NetIO ()
{
  if (url)
    free (url);
  if (proto)
    free (proto);
  if (host)
    free (host);
  if (path)
    free (path);
}

int
NetIO::ok ()
{
  return 0;
}

int
NetIO::read (char *buf, int nbytes)
{
  return 0;
}

NetIO *
NetIO::open (char *url)
{
  NetIO *rv = 0;
  enum {http, ftp, file} proto;
  if (strncmp (url, "http://", 7) == 0)
    proto = http;
  else if (strncmp (url, "ftp://", 6) == 0)
    proto = ftp;
  else
    proto = file;

  if (proto == file)
    rv = new NetIO_File (url);
  else if (net_method == IDC_NET_IE5)
    rv = new NetIO_IE5 (url);
#if 0
  else if (net_method == IDC_NET_DIRECT)
    rv = new NetIO_Direct (url);
  else if (net_method == IDC_NET_PROXY)
    rv = new NetIO_Proxy (url);
#endif

  if (!rv->ok ())
    {
      delete rv;
      return 0;
    }

  return rv;
}


static char **user, **passwd;

static void
check_if_enable_ok (HWND h)
{
  int e = 0;
  msg ("u = %p p = %p", *user, *passwd);
  if (*user && *passwd)
    e = 1;
  EnableWindow (GetDlgItem (h, IDOK), e);
}

static void
load_dialog (HWND h)
{
  eset (h, IDC_NET_USER, *user);
  eset (h, IDC_NET_PASSWD, *passwd);
  check_if_enable_ok (h);
}

static void
save_dialog (HWND h)
{
  *user = eget (h, IDC_NET_USER, *user);
  *passwd = eget (h, IDC_NET_PASSWD, *passwd);
}

static BOOL
auth_cmd (HWND h, int id, HWND hwndctl, UINT code)
{
  switch (id)
    {

    case IDC_NET_USER:
    case IDC_NET_PASSWD:
      save_dialog (h);
      check_if_enable_ok (h);
      break;

    case IDOK:
      save_dialog (h);
      EndDialog (h, 0);
      break;

    case IDCANCEL:
      EndDialog (h, 1);
      break;
    }
}

static BOOL CALLBACK
auth_proc (HWND h, UINT message, WPARAM wParam, LPARAM lParam)
{
  switch (message)
    {
    case WM_INITDIALOG:
      load_dialog (h);
      return FALSE;
    case WM_COMMAND:
      return HANDLE_WM_COMMAND (h, wParam, lParam, auth_cmd);
    }
  return FALSE;
}

static int
auth_common (HINSTANCE h, int id)
{
  return DialogBox (h, MAKEINTRESOURCE (id), 0, auth_proc);
}

int
NetIO::get_auth ()
{
  user = &net_user;
  passwd = &net_passwd;
  return auth_common (hinstance, IDD_NET_AUTH);
}

int
NetIO::get_proxy_auth ()
{
  user = &net_proxy_user;
  passwd = &net_proxy_passwd;
  return auth_common (hinstance, IDD_PROXY_AUTH);
}
