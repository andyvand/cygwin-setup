/*
 * Copyright (c) 2002 Robert Collins.
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     A copy of the GNU General Public License can be found at
 *     http://www.gnu.org/
 *
 * Written by Robert Collins <robertc@hotmail.com>
 *
 */


#ifndef   _PICKCATEGORYLINE_H_
#define   _PICKCATEGORYLINE_H_

class PickView;
#include "PickLine.h"
#include "package_meta.h"

class PickCategoryLine:public PickLine
{
public:
  PickCategoryLine (PickView & aView, Category & _cat, size_t thedepth = 0, bool aBool =
		      true, bool aBool2 =
		      true):PickLine (_cat.key),
    current_default (Default_action), cat (_cat), labellength (0),
    depth (thedepth), theView (aView)
  {
    if (aBool)
      {
	collapsed = true;
	show_label = true;
      }
    else
      {
	collapsed = false;
	show_label = aBool2;
      }
  };
  ~PickCategoryLine ()
  {
    empty ();
  }
  void ShowLabel (bool aBool = true)
  {
    show_label = aBool;
    if (!show_label)
      collapsed = false;
  }
  virtual void paint (HDC hdc, int x, int y, int row, int show_cat);
  virtual int click (int const myrow, int const ClickedRow, int const x);
  virtual int itemcount () const
  {
    if (collapsed)
      return 1;
    int t = show_label ? 1 : 0;
    for (size_t n = 1; n <= bucket.number (); n++)
        t += bucket[n]->itemcount ();
      return t;
  };
  virtual bool IsContainer (void) const
  {
    return true;
  }
  virtual void insert (PickLine & aLine)
  {
    bucket.registerbyobject (aLine);
  }
  void empty ();
private:
  enum _actions
  {
    Default_action,
    Install_action,
    Reinstall_action,
    Uninstall_action
  }
  current_default;
  char const *actiontext ();
  Category & cat;
  bool collapsed;
  bool show_label;
  size_t labellength;
  size_t depth;
  PickCategoryLine (PickCategoryLine const &);
  PickCategoryLine & operator= (PickCategoryLine const &);
  list < PickLine, char const *, strcasecmp > bucket;
  PickView& theView;
};
#endif // _PICKCATEGORYLINE_H_