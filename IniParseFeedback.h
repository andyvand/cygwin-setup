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

#ifndef _INIPARSEFEEDBACK_H_
#define _INIPARSEFEEDBACK_H_


class String;
/* Strategy for feedback from IniParsing.
 * Used by the builder or parsing classes to send feedback that users need
 * but that should not interrupt parsing.
 * Fatal errors are thrown as exceptions.
 */
class IniParseFeedback
{
public:
  virtual void babble(String const &) const;
  virtual void warning (String const &) const;
  virtual void error(String const &) const;
  virtual ~ IniParseFeedback ();
};

#endif // _INIPARSEFEEDBACK_H_