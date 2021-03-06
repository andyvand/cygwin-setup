/*
 * Copyright (c) 2000, Red Hat, Inc.
 * Copyright (c) 2003, Robert Collins <rbtcollins@hotmail.com>
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     A copy of the GNU General Public License can be found at
 *     http://www.gnu.org/
 *
 * Originally Written by DJ Delorie <dj@cygnus.com>
 *
 */

/* The purpose of this file is to install all the packages selected in
   the install list (in ini.h).  Note that we use a separate thread to
   maintain the progress dialog, so we avoid the complexity of
   handling two tasks in one thread.  We also create or update all the
   files in /etc/setup/\* and create the mount points. */

#if 0
static const char *cvsid = "\n%%% $Id$\n";
#endif

#include "getopt++/BoolOption.h"
#include "csu_util/MD5Sum.h"
#include "LogFile.h"

#include "win32.h"
#include "commctrl.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <process.h>

#include "ini.h"
#include "resource.h"
#include "dialog.h"
#include "geturl.h"
#include "state.h"
#include "diskfull.h"
#include "msg.h"
#include "mount.h"
#include "mount.h"
#include "filemanip.h"
#include "io_stream.h"
#include "compress.h"
#include "compress_gz.h"
#include "archive.h"
#include "archive_tar.h"
#include "script.h"

#include "package_db.h"
#include "package_meta.h"
#include "package_version.h"
#include "package_source.h"

#include "threebar.h"
#include "Exception.h"
#include "processlist.h"

using namespace std;

extern ThreeBarProgressPage Progress;

static long long int total_bytes = 0;
static long long int total_bytes_sofar = 0;
static int package_bytes = 0;

static BoolOption NoReplaceOnReboot (false, 'r', "no-replaceonreboot",
				     "Disable replacing in-use files on next "
				     "reboot.");

struct std_dirs_t {
  const char *name;
  mode_t mode;
};

class Installer
{
  public:
    static std_dirs_t StandardDirs[];
    Installer();
    void initDialog();
    void progress (int bytes);
    void preremoveOne (packagemeta &);
    void uninstallOne (packagemeta &);
    void replaceOnRebootFailed (const std::string& fn);
    void replaceOnRebootSucceeded (const std::string& fn, bool &rebootneeded);
    void installOne (packagemeta &pkg, const packageversion &ver,
                     packagesource &source,
                     const std::string& , const std::string&, HWND );
    int errors;
  private:
    bool extract_replace_on_reboot(archive *, const std::string&,
                                   const std::string&, std::string);

};

Installer::Installer() : errors(0)
{
}

void
Installer::initDialog()
{
  Progress.SetText2 ("");
  Progress.SetText3 ("");
}

void
Installer::progress (int bytes)
{
  if (package_bytes > 0)
      Progress.SetBar1 (bytes, package_bytes);

  if (total_bytes > 0)
      Progress.SetBar2 (total_bytes_sofar + bytes, total_bytes);
}

std_dirs_t
Installer::StandardDirs[] = {
  { "/bin", 0755 },
  { "/dev", 0755 },
  { "/dev/mqueue", 01777 },
  { "/dev/shm", 01777 },
  { "/etc", 0755 },
  { "/etc/fstab.d", 01777 },
  { "/home", 01777 },
  { "/lib", 0755 },
  { "/tmp", 01777 },
  { "/usr", 0755 },
  { "/usr/bin", 0755 },
  { "/usr/lib", 0755 },
  { "/usr/local", 0755 },
  { "/usr/local/bin", 0755 },
  { "/usr/local/etc", 0755 },
  { "/usr/local/lib", 0755 },
  { "/usr/src", 0755 },
  { "/usr/tmp", 01777 },
  { "/var/log", 01777 },
  { "/var/run", 01777 },
  { "/var/tmp", 01777 },
  { NULL, 0 }
};

static int num_installs, num_uninstalls;
static void chksum_one (const packagesource& source);

void
Installer::preremoveOne (packagemeta & pkg)
{
  Progress.SetText1 ("Running preremove script...");
  Progress.SetText2 (pkg.name.c_str());
  Log (LOG_PLAIN) << "Running preremove script for  " << pkg.name << endLog;
  const unsigned numexts = 4;
  const char* exts[numexts] = { ".dash", ".sh", ".bat", ".cmd" };
  for (unsigned i = 0; i < numexts; i++)
    try_run_script ("/etc/preremove/", pkg.name, exts[i]);
}

void
Installer::uninstallOne (packagemeta & pkg)
{
  Progress.SetText1 ("Uninstalling...");
  Progress.SetText2 (pkg.name.c_str());
  Log (LOG_PLAIN) << "Uninstalling " << pkg.name << endLog;
  pkg.uninstall ();
  num_uninstalls++;
}

/* log failed scheduling of replace-on-reboot of a given file. */
/* also increment errors. */
void
Installer::replaceOnRebootFailed (const std::string& fn)
{
  Log (LOG_TIMESTAMP) << "Unable to schedule reboot replacement of file "
    << cygpath("/" + fn) << " with " << cygpath("/" + fn + ".new")
    << " (Win32 Error " << GetLastError() << ")" << endLog;
  ++errors;
}

/* log successful scheduling of replace-on-reboot of a given file. */
/* also set rebootneeded. */
void
Installer::replaceOnRebootSucceeded (const std::string& fn, bool &rebootneeded)
{
  Log (LOG_TIMESTAMP) << "Scheduled reboot replacement of file "
    << cygpath("/" + fn) << " with " << cygpath("/" + fn + ".new") << endLog;
  rebootneeded = true;
}

#define MB_RETRYCONTINUE 7
#if !defined(IDCONTINUE)
#define IDCONTINUE IDCANCEL
#endif

static HHOOK hMsgBoxHook;
LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam) {
  HWND hWnd;
  switch (nCode) {
    case HCBT_ACTIVATE:
      hWnd = (HWND)wParam;
      if (GetDlgItem(hWnd, IDCANCEL) != NULL)
         SetDlgItemText(hWnd, IDCANCEL, "Continue");
      UnhookWindowsHookEx(hMsgBoxHook);
  }
  return CallNextHookEx(hMsgBoxHook, nCode, wParam, lParam);
}

int _custom_MessageBox(HWND hWnd, LPCTSTR szText, LPCTSTR szCaption, UINT uType) {
  int retval;
  bool retry_continue = (uType & MB_TYPEMASK) == MB_RETRYCONTINUE;
  if (retry_continue) {
    uType &= ~MB_TYPEMASK; uType |= MB_RETRYCANCEL;
    // Install a window hook, so we can intercept the message-box
    // creation, and customize it
    // Only install for THIS thread!!!
    hMsgBoxHook = SetWindowsHookEx(WH_CBT, CBTProc, NULL, GetCurrentThreadId());
  }
  retval = MessageBox(hWnd, szText, szCaption, uType);
  // Intercept the return value for less confusing results
  if (retry_continue && retval == IDCANCEL)
    return IDCONTINUE;
  return retval;
}

#undef MessageBox
#define MessageBox _custom_MessageBox

typedef struct
{
  const char *msg;
  const char *processlist;
  int iteration;
} FileInuseDlgData;

static INT_PTR CALLBACK
FileInuseDlgProc (HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
    {
    case WM_INITDIALOG:
      {
        FileInuseDlgData *dlg_data = (FileInuseDlgData *)lParam;

        SetDlgItemText (hwndDlg, IDC_FILE_INUSE_MSG, dlg_data->msg);
        SetDlgItemText (hwndDlg, IDC_FILE_INUSE_EDIT, dlg_data->processlist);

        switch (dlg_data->iteration)
          {
          case 0:
            break; // show the dialog the way it is in the resource

          case 1:
            SetDlgItemText (hwndDlg, IDRETRY, "&Kill Processes");
            SetDlgItemText (hwndDlg, IDC_FILE_INUSE_HELP,
                            "Select 'Retry' to retry, "
                            "Select 'Kill' to kill processes and retry, or "
                            "select 'Continue' to go on anyway (the file will be updated after a reboot).");
            break;

          default:
          case 2:
            SetDlgItemText (hwndDlg, IDRETRY, "&Kill Processes");
            SetDlgItemText (hwndDlg, IDC_FILE_INUSE_HELP,
                            "Select 'Retry' to retry, "
                            "select 'Kill' to forcibly kill all processes and retry, or "
                            "select 'Continue' to go on anyway (the file will be updated after a reboot).");
          }
      }
      return TRUE; // automatically set focus, please

    case WM_COMMAND:
      if (HIWORD (wParam) == BN_CLICKED)
        {
          switch (LOWORD (wParam))
            {
            case IDIGNORE:
            case IDRETRY:
            case IDCONTINUE:
              EndDialog (hwndDlg, LOWORD (wParam));
              return TRUE;
            }
        }
    }

  return FALSE;
}

/* Helper function to create the registry value "AllowProtectedRenames",
   which is required to make MOVEFILE_DELAY_UNTIL_REBOOT to work on WFP
   protected files.  By default, the entire system drive is WFP protected,
   so a Cygwin installation on this drive sufferes from the WFP problem.
   Even though this value is only required since Windows Server 2003 SP1,
   we just set it here unconditionally since it doesn't hurt at all on
   older systems. */
void
create_allow_protected_renames ()
{
  HKEY key;
  DWORD val = 1;

  if (RegOpenKeyEx (HKEY_LOCAL_MACHINE,
		    "System\\CurrentControlSet\\Control\\Session Manager",
		    0, KEY_ALL_ACCESS | SETUP_KEY_WOW64, &key) == ERROR_SUCCESS)
    RegSetValueEx (key, "AllowProtectedRenames", 0, REG_DWORD,
		   (BYTE *) &val, sizeof (DWORD));
  RegCloseKey (key);
}

bool
Installer::extract_replace_on_reboot (archive *tarstream, const std::string& prefixURL,
                                      const std::string& prefixPath, std::string fn)
{
  /* Extract a copy of the file with extension .new appended and
     indicate it should be replaced on the next reboot.  */
  if (archive::extract_file (tarstream, prefixURL, prefixPath,
                             ".new") != 0)
    {
      Log (LOG_PLAIN) << "Unable to install file " << prefixURL
                      << prefixPath << fn << ".new" << endLog;
      ++errors;
      return true;
    }
  else
    {
      std::string s = cygpath ("/" + fn + ".new"),
        d = cygpath ("/" + fn);

      WCHAR sname[s.size () + 7];
      WCHAR dname[d.size () + 7];
      /* Windows 2000 has a bug:  Prepending \\?\ does not
       * work in conjunction with MOVEFILE_DELAY_UNTIL_REBOOT.
       * So in case of Windows 2000 we just convert the path
       * to wide char and hope for the best. */
      if (OSMajorVersion () == 5 && OSMinorVersion () == 0)
	{
	  mbstowcs (sname, s.c_str (), s.size () + 7);
	  mbstowcs (dname, d.c_str (), d.size () + 7);
	}
      else
	{
	  mklongpath (sname, s.c_str (), s.size () + 7);
	  mklongpath (dname, d.c_str (), d.size () + 7);
	}
      if (!MoveFileExW (sname, dname,
                        MOVEFILE_DELAY_UNTIL_REBOOT |
                        MOVEFILE_REPLACE_EXISTING))
        replaceOnRebootFailed (fn);
      else
        {
          create_allow_protected_renames ();
          replaceOnRebootSucceeded (fn, rebootneeded);
        }
    }
  return false;
}

static char all_null[512];

/* install one source at a given prefix. */
void
Installer::installOne (packagemeta &pkgm, const packageversion &ver,
                       packagesource &source,
                       const std::string& prefixURL,
                       const std::string& prefixPath,
                       HWND owner)
{
  if (!source.Canonical())
    return;
  Progress.SetText1 ("Installing");
  Progress.SetText2 (source.Base () ? source.Base () : "(unknown)");

  io_stream *pkgfile = NULL;

  if (!source.Cached() || !io_stream::exists (source.Cached ())
      || !(pkgfile = io_stream::open (source.Cached (), "rb", 0)))
    {
      note (NULL, IDS_ERR_OPEN_READ, source.Cached (), "No such file");
      ++errors;
      return;
    }


  /* At this point pkgfile is an opened io_stream to either a .tar.bz2 file,
     a .tar.gz file, a .tar.lzma file, or just a .tar file.  Try it first as
     a compressed file and if that fails try opening it as a tar directly.
     If both fail, abort.

     Note on io_stream pointer management:

     Both the archive and decompress classes take ownership of the io_stream
     pointer they were opened with, meaning they delete it in their dtor.  So
     deleting tarstream should also result in deleting the underlying
     try_decompress and pkgfile io_streams as well.  */

  archive *tarstream = NULL;
  io_stream *try_decompress = NULL;

  if ((try_decompress = compress::decompress (pkgfile)) != NULL)
    {
      if ((tarstream = archive::extract (try_decompress)) == NULL)
        {
          /* Decompression succeeded but we couldn't grok it as a valid tar
             archive.  */
          char c[512];
	  ssize_t len;
          if ((len = try_decompress->peek (c, 512)) < 0
	      || !memcmp (c, all_null, len))
            /* In some cases, maintainers have uploaded bzipped
               0-byte files as dummy packages instead of empty tar files.
               This is common enough that we should not treat this as an
               error condition.
	       Same goes for tar archives consisting of a big block of
	       all zero bytes (the famous 46 bytes tar archives). */
	    {
	      if (ver.Type () == package_binary)
		pkgm.installed = ver;
	    }
          else
            {
              note (NULL, IDS_ERR_OPEN_READ, source.Cached (),
                    "Invalid or unsupported tar format");
              ++errors;
            }
          delete try_decompress;
          return;
        }
    }
  else if ((tarstream = archive::extract (pkgfile)) == NULL)
    {
      /* Not a compressed tarball, not a plain tarball, give up.  */
      delete pkgfile;
      note (NULL, IDS_ERR_OPEN_READ, source.Cached (),
            "Unrecognisable file format");
      ++errors;
      return;
    }

  /* For binary packages, create a manifest in /etc/setup/ that lists the
     filename of each file that was unpacked.  */

  io_stream *lst = NULL;
  if (ver.Type () == package_binary)
    {
      std::string lstfn = "cygfile:///etc/setup/" + pkgm.name + ".lst.gz";

      io_stream *tmp;
      if ((tmp = io_stream::open (lstfn, "wb", 0644)) == NULL)
        Log (LOG_PLAIN) << "Warning: Unable to create lst file " + lstfn +
          " - uninstall of this package will leave orphaned files." << endLog;
      else
        {
          lst = new compress_gz (tmp, "w9");
          if (lst->error ())
            {
              delete lst;
              lst = NULL;
              Log (LOG_PLAIN) << "Warning: gzip unable to write to lst file " +
                lstfn + " - uninstall of this package will leave orphaned files."
                << endLog;
            }
        }
    }

  bool error_in_this_package = false;
  bool ignoreInUseErrors = false;
  bool ignoreExtractErrors = unattended_mode;

  package_bytes = source.size;
  Log (LOG_PLAIN) << "Extracting from " << source.Cached () << endLog;

  std::string fn;
  while ((fn = tarstream->next_file_name ()).size ())
    {
      std::string canonicalfn = prefixPath + fn;
      Progress.SetText3 (canonicalfn.c_str ());
      Log (LOG_BABBLE) << "Installing file " << prefixURL << prefixPath
          << fn << endLog;
      if (lst)
        {
          std::string tmp = fn + "\n";
          lst->write (tmp.c_str(), tmp.size());
        }
      if (Script::isAScript (fn))
        pkgm.desired.addScript (Script (canonicalfn));

      int iteration = 0;
      archive::extract_results extres;
      while ((extres = archive::extract_file (tarstream, prefixURL, prefixPath)) != archive::extract_ok)
        {
          switch (extres)
            {
	    case archive::extract_inuse: // in use
              {
                if (!ignoreInUseErrors)
                  {
                    // convert the file name to long UNC form
                    std::string s = backslash (cygpath ("/" + fn));
                    WCHAR sname[s.size () + 7];
                    mklongpath (sname, s.c_str (), s.size () + 7);

                    // find any process which has that file loaded into it
                    // (note that this doesn't find when the file is un-writeable because the process has
                    // that file opened exclusively)
                    ProcessList processes = Process::listProcessesWithModuleLoaded (sname);

                    std::string plm;
                    for (ProcessList::iterator i = processes.begin (); i != processes.end (); i++)
                      {
                        if (i != processes.begin ()) plm += "\r\n";

                        std::string processName = i->getName ();
                        Log (LOG_BABBLE) << processName << endLog;
                        plm += processName;
                      }

                    INT_PTR rc = (iteration < 3) ? IDRETRY : IDCONTINUE;
                    if (unattended_mode == attended)
                      {
                        if (!processes.empty())
                          {
                            // Use the IDD_FILE_INUSE dialog to ask the user if we should try to kill the
                            // listed processes, or just ignore the problem and schedule the file to be
                            // replaced after a reboot
                            FileInuseDlgData dlg_data;
                            dlg_data.msg = ("Unable to extract /" + fn).c_str ();
                            dlg_data.processlist = plm.c_str ();
                            dlg_data.iteration = iteration;

                            rc = DialogBoxParam(hinstance, MAKEINTRESOURCE (IDD_FILE_INUSE), owner, FileInuseDlgProc, (LPARAM)&dlg_data);
                          }
                        else
                          {
                            // We couldn't enumerate any processes which have this file loaded into it
                            // either the cause of the error is something else, or something (e.g security
                            // policy) prevents us from listing those processes.
                            // All we can offer the user is a generic "retry or ignore" choice and a chance
                            // to fix the problem themselves
                            char msg[fn.size() + 300];
                            sprintf (msg,
                                     "Unable to extract /%s\r\n\r\n"
                                     "The file is in use or some other error occurred.\r\n\r\n"
                                     "Please stop all Cygwin processes and select \"Retry\", or "
                                     "select \"Continue\" to go on anyway (the file will be updated after a reboot).\r\n",
                                     fn.c_str());

                            rc = MessageBox (owner, msg, "Error writing file",
                                             MB_RETRYCONTINUE | MB_ICONWARNING | MB_TASKMODAL);
                          }
                      }

                    switch (rc)
                      {
                      case IDIGNORE:
                        // manual intervention may have fixed the problem, retry
                        continue;
                      case IDRETRY:
                        if (!processes.empty())
                          {
                            // try to stop all the processes
                            for (ProcessList::iterator i = processes.begin (); i != processes.end (); i++)
                              {
                                i->kill (iteration);
                              }

                            // wait up to 15 seconds for processes to stop
                            for (unsigned int i = 0; i < 15; i++)
                              {
                                processes = Process::listProcessesWithModuleLoaded (sname);
                                if (processes.size () == 0)
                                  break;

                                Sleep (1000);
                              }
                          }
                        // else, manual intervention may have fixed the problem

                        // retry
                        iteration++;
                        continue;
                      case IDCONTINUE:
                        // ignore this in-use error, and any subsequent in-use errors for other files in the same package
                        ignoreInUseErrors = true;
                        break;
                      default:
                        break;
                      }
                    // fall through to previous functionality
                  }

                if (NoReplaceOnReboot)
                  {
                    ++errors;
                    error_in_this_package = true;
                    Log (LOG_PLAIN) << "Not replacing in-use file " << prefixURL
                                    << prefixPath << fn << endLog;
                  }
                else
                  {
                    error_in_this_package = extract_replace_on_reboot(tarstream, prefixURL, prefixPath, fn);
                  }
              }
              break;
	    case archive::extract_other: // extract failed
              {
                if (!ignoreExtractErrors)
                  {
                    char msg[fn.size() + 300];
                    sprintf (msg,
                             "Unable to extract /%s -- corrupt package?\r\n",
                             fn.c_str());

                    // XXX: We should offer the option to retry,
                    // continue without extracting this particular archive,
                    // or ignore all extraction errors.
                    // Unfortunately, we don't currently know how to rewind
                    // the archive, so we can't retry at present,
                    // and ignore all errors is mis-implemented at present
                    // to only apply to errors arising from a single archive,
                    // so we degenerate to the continue option.
                    MessageBox (owner, msg, "File extraction error",
                                MB_OK | MB_ICONWARNING | MB_TASKMODAL);
                  }

                // don't mark this package as successfully installed
                error_in_this_package = true;
              }
              break;
	    case archive::extract_ok:
	      break;
            }

          // We're done with this file
          break;
        }
      progress (pkgfile->tell ());
      num_installs++;
    }

  if (lst)
    delete lst;
  delete tarstream;

  total_bytes_sofar += package_bytes;
  progress (0);

  int df = diskfull (get_root_dir ().c_str ());
  Progress.SetBar3 (df);

  if (ver.Type () == package_binary && !error_in_this_package)
    pkgm.installed = ver;
}

static void
check_for_old_cygwin (HWND owner)
{
  /* Paths within system dir expected to be always < MAX_PATH. */
  char buf[MAX_PATH + sizeof ("\\cygwin1.dll")];
  if (!GetSystemDirectory (buf, sizeof (buf)))
    return;
  strcat (buf, "\\cygwin1.dll");
  if (_access (buf, 0) != 0)
    return;

  char msg[sizeof (buf) + 132];
  sprintf (msg,
	   "An old version of cygwin1.dll was found here:\r\n%s\r\nDelete?",
	   buf);
  switch (MessageBox
	  (owner, msg, "What's that doing there?",
	   MB_YESNO | MB_ICONQUESTION | MB_TASKMODAL))
    {
    case IDYES:
      if (!DeleteFile (buf))
	{
	  sprintf (msg, "Couldn't delete file %s.\r\n"
		   "Is the DLL in use by another application?\r\n"
		   "You should delete the old version of cygwin1.dll\r\n"
		   "at your earliest convenience.", buf);
	  MessageBox (owner, buf, "Couldn't delete file",
		      MB_OK | MB_ICONEXCLAMATION | MB_TASKMODAL);
	}
      break;
    default:
      break;
    }

  return;
}

static void
do_install_thread (HINSTANCE h, HWND owner)
{
  int i;

  num_installs = 0, num_uninstalls = 0;
  rebootneeded = false;

  io_stream::mkpath_p (PATH_TO_DIR,
                       std::string("file://") + std::string(get_root_dir()),
		       0755);

  for (i = 0; Installer::StandardDirs[i].name; i++)
  {
    std::string p = cygpath (Installer::StandardDirs[i].name);
    if (p.size())
      io_stream::mkpath_p (PATH_TO_DIR, "file://" + p,
			   Installer::StandardDirs[i].mode);
  }

  /* Create /var/run/utmp */
  io_stream *utmp = io_stream::open ("cygfile:///var/run/utmp", "wb", 0666);
  delete utmp;

  Installer myInstaller;
  myInstaller.initDialog();

  total_bytes = 0;
  total_bytes_sofar = 0;

  int df = diskfull (get_root_dir ().c_str());
  Progress.SetBar3 (df);

  /* Writes Cygwin/setup/rootdir registry value */
  create_install_root ();

  vector <packagemeta *> install_q, uninstall_q, sourceinstall_q;

  packagedb db;

  /* Calculate the amount of data to md5sum */
  Progress.SetText1("Calculating...");
  long long int md5sum_total_bytes = 0;
  for (packagedb::packagecollection::iterator i = db.packages.begin ();
       i != db.packages.end (); ++i)
  {
    packagemeta & pkg = *(i->second);

    if (pkg.desired.picked())
    {
      md5sum_total_bytes += pkg.desired.source()->size;
    }

    if (pkg.desired.sourcePackage ().picked())
    {
      md5sum_total_bytes += pkg.desired.sourcePackage ().source()->size;
    }
  }

  /* md5sum the packages, build lists of packages to install and uninstall
     and calculate the total amount of data to install */
  long long int md5sum_total_bytes_sofar = 0;
  for (packagedb::packagecollection::iterator i = db.packages.begin ();
       i != db.packages.end (); ++i)
  {
    packagemeta & pkg = *(i->second);

    if (pkg.desired.picked())
    {
      try
      {
        chksum_one (*pkg.desired.source ());
      }
      catch (Exception *e)
      {
        if (yesno (owner, IDS_SKIP_PACKAGE, e->what()) == IDYES)
          pkg.desired.pick (false, &pkg);
      }
      if (pkg.desired.picked())
      {
        md5sum_total_bytes_sofar += pkg.desired.source()->size;
        total_bytes += pkg.desired.source()->size;
        install_q.push_back (&pkg);
      }
    }

    if (pkg.desired.sourcePackage ().picked())
    {
      try
      {
        chksum_one (*pkg.desired.sourcePackage ().source ());
      }
      catch (Exception *e)
      {
        if (yesno (owner, IDS_SKIP_PACKAGE, e->what()) == IDYES)
          pkg.desired.sourcePackage ().pick (false, &pkg);
      }
      if (pkg.desired.sourcePackage().picked())
      {
        md5sum_total_bytes_sofar += pkg.desired.sourcePackage ().source()->size;
        total_bytes += pkg.desired.sourcePackage ().source()->size;
        sourceinstall_q.push_back (&pkg);
      }
    }

    if ((pkg.installed && pkg.desired != pkg.installed)
        || pkg.installed.picked ())
    {
      uninstall_q.push_back (&pkg);
    }

    if (md5sum_total_bytes > 0)
      Progress.SetBar2 (md5sum_total_bytes_sofar, md5sum_total_bytes);
  }

  /* start with uninstalls - remove files that new packages may replace */
  for (vector <packagemeta *>::iterator i = uninstall_q.begin ();
       i != uninstall_q.end (); ++i)
  {
    myInstaller.preremoveOne (**i);
  }
  for (vector <packagemeta *>::iterator i = uninstall_q.begin ();
       i != uninstall_q.end (); ++i)
  {
    myInstaller.uninstallOne (**i);
  }

  for (vector <packagemeta *>::iterator i = install_q.begin ();
       i != install_q.end (); ++i)
  {
    packagemeta & pkg = **i;
    try {
      myInstaller.installOne (pkg, pkg.desired, *pkg.desired.source(),
                              "cygfile://", "/", owner);
    }
    catch (exception *e)
    {
      if (yesno (owner, IDS_INSTALL_ERROR, e->what()) != IDYES)
      {
        Log (LOG_TIMESTAMP)
          << "User cancelled setup after install error" << endLog;
        Logger ().exit (1);
        return;
      }
    }
  }

  for (vector <packagemeta *>::iterator i = sourceinstall_q.begin ();
       i != sourceinstall_q.end (); ++i)
  {
    packagemeta & pkg = **i;
    myInstaller.installOne (pkg, pkg.desired.sourcePackage(),
                            *pkg.desired.sourcePackage().source(),
                            "cygfile://", "/usr/src/", owner);
  }

  if (rebootneeded)
    note (owner, IDS_REBOOT_REQUIRED);

  int temperr;
  if ((temperr = db.flush ()))
    {
      const char *err = strerror (temperr);
      if (!err)
	err = "(unknown error)";
      fatal (owner, IDS_ERR_OPEN_WRITE, "Package Database",
	  err);
    }

  if (!myInstaller.errors)
    check_for_old_cygwin (owner);
  if (num_installs == 0 && num_uninstalls == 0)
    {
      if (!unattended_mode)
	Logger ().setExitMsg (IDS_NOTHING_INSTALLED);
      return;
    }
  if (num_installs == 0)
    {
      if (!unattended_mode)
	Logger ().setExitMsg (IDS_UNINSTALL_COMPLETE);
      return;
    }

  if (myInstaller.errors)
    Logger ().setExitMsg (IDS_INSTALL_INCOMPLETE);
  else if (!unattended_mode)
    Logger ().setExitMsg (IDS_INSTALL_COMPLETE);

  if (rebootneeded)
    Logger ().setExitMsg (IDS_REBOOT_REQUIRED);
}

static DWORD WINAPI
do_install_reflector (void *p)
{
  HANDLE *context;
  context = (HANDLE *) p;

  try
  {
    do_install_thread ((HINSTANCE) context[0], (HWND) context[1]);

    // Tell the progress page that we're done downloading
    Progress.PostMessageNow (WM_APP_INSTALL_THREAD_COMPLETE);
  }
  TOPLEVEL_CATCH("install");

  ExitThread (0);
}

static HANDLE context[2];

void
do_install (HINSTANCE h, HWND owner)
{
  context[0] = h;
  context[1] = owner;

  DWORD threadID;
  CreateThread (NULL, 0, do_install_reflector, context, 0, &threadID);
}

static char *
sha512_str (const unsigned char *in, char *buf)
{
  char *bp = buf;
  for (int i = 0; i < SHA512_DIGEST_LENGTH; ++i)
    bp += sprintf (bp, "%02x", in[i]);
  *bp = '\0';
  return buf;
}

static void
sha512_one (const packagesource& pkgsource)
{
  std::string fullname (pkgsource.Cached ());

  io_stream *thefile = io_stream::open (fullname, "rb", 0);
  if (!thefile)
    throw new Exception (TOSTRING (__LINE__) " " __FILE__,
			 std::string ("IO Error opening ") + fullname,
			 APPERR_IO_ERROR);
  SHA2_CTX ctx;
  unsigned char sha512result[SHA512_DIGEST_LENGTH];
  char ini_sum[SHA512_DIGEST_STRING_LENGTH],
       disk_sum[SHA512_DIGEST_STRING_LENGTH];

  SHA512Init (&ctx);

  Log (LOG_BABBLE) << "Checking SHA512 for " << fullname << endLog;

  Progress.SetText1 ((std::string ("Checking SHA512 for ")
		      + pkgsource.Base ()).c_str ());
  Progress.SetText4 ("Progress:");
  Progress.SetBar1 (0);

  unsigned char buffer[64 * 1024];
  ssize_t count;
  while ((count = thefile->read (buffer, sizeof (buffer))) > 0)
  {
    SHA512Update (&ctx, buffer, count);
    Progress.SetBar1 (thefile->tell (), thefile->get_size ());
  }
  delete thefile;
  if (count < 0)
    throw new Exception (TOSTRING(__LINE__) " " __FILE__,
			 "IO Error reading " + fullname,
			 APPERR_IO_ERROR);

  SHA512Final (sha512result, &ctx);

  if (memcmp (pkgsource.sha512sum, sha512result, sizeof sha512result))
    {
      Log (LOG_BABBLE) << "INVALID PACKAGE: " << fullname
		       << " - SHA512 mismatch: Ini-file: "
		       << sha512_str (pkgsource.sha512sum, ini_sum)
		       << " != On-disk: "
		       << sha512_str (sha512result, disk_sum)
		       << endLog;
      throw new Exception (TOSTRING(__LINE__) " " __FILE__,
			   "SHA512 failure for " + fullname,
			   APPERR_CORRUPT_PACKAGE);
    }

  Log (LOG_BABBLE) << "SHA512 verified OK: " << fullname << " "
    <<  sha512_str (pkgsource.sha512sum, ini_sum) << endLog;
}

static void
md5_one (const packagesource& pkgsource)
{
  std::string fullname (pkgsource.Cached ());

  io_stream *thefile = io_stream::open (fullname, "rb", 0);
  if (!thefile)
    throw new Exception (TOSTRING (__LINE__) " " __FILE__,
			 std::string ("IO Error opening ") + fullname,
			 APPERR_IO_ERROR);
  MD5Sum tempMD5;
  tempMD5.begin ();

  Log (LOG_BABBLE) << "Checking MD5 for " << fullname << endLog;

  Progress.SetText1 ((std::string ("Checking MD5 for ")
		      + pkgsource.Base ()).c_str ());
  Progress.SetText4 ("Progress:");
  Progress.SetBar1 (0);

  unsigned char buffer[16384];
  ssize_t count;
  while ((count = thefile->read (buffer, sizeof (buffer))) > 0)
    {
      tempMD5.append (buffer, count);
      Progress.SetBar1 (thefile->tell (), thefile->get_size ());
    }
  delete thefile;
  if (count < 0)
    throw new Exception (TOSTRING(__LINE__) " " __FILE__,
			 "IO Error reading " + fullname,
			 APPERR_IO_ERROR);

  tempMD5.finish ();

  if (pkgsource.md5 != tempMD5)
    {
      Log (LOG_BABBLE) << "INVALID PACKAGE: " << fullname
	<< " - MD5 mismatch: Ini-file: " << pkgsource.md5.str()
	<< " != On-disk: " << tempMD5.str() << endLog;
      throw new Exception (TOSTRING(__LINE__) " " __FILE__,
			   "MD5 failure for " + fullname,
			   APPERR_CORRUPT_PACKAGE);
    }

  Log (LOG_BABBLE) << "MD5 verified OK: " << fullname << " "
    << pkgsource.md5.str() << endLog;
}

static void
chksum_one (const packagesource& pkgsource)
{
  if (!pkgsource.Cached ())
    return;
  if (pkgsource.sha512sum[0])
    sha512_one (pkgsource);
  else if (pkgsource.md5.isSet())
    md5_one (pkgsource);
}
