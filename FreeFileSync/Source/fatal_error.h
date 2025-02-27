// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef ERROR_LOG_H_89734181783491324134
#define ERROR_LOG_H_89734181783491324134

#include <cassert>
#include <zen/file_io.h>
#include <zen/time.h>
#include "ffs_paths.h"


namespace fff
{
//write error message to a file (even with corrupted stack)- call in desperate situations when no other means of error handling is available
void logFatalError(const std::wstring& msg); //noexcept









//##################### implementation ############################
inline
void logFatalError(const std::wstring& msg) //noexcept
{
    warn_static("new semantics: logErrorWhileBusy or logErrorShowLater + show upon next FFS start!?")
        warn_static("this really should append!")
        //create time-stamped file path + show if newer than last FFS run? (save in GlobalSettings.xml)
        //replace calls to  ::MessageBox() and std::cerr ?
        //save std::time()

    using namespace zen;

    assert(false); //this is stuff we like to debug
    const std::string logEntry = '[' + utfTo<std::string>(formatTime(formatDateTimeTag)) + "] " + utfTo<std::string>(msg);
    try
    {
        setFileContent(getConfigDirPathPf() + Zstr("LastError.log"), logEntry, nullptr /*notifyUnbufferedIO*/); //throw FileError
    }
    catch (FileError&) { assert(false); }
}
}

#endif //ERROR_LOG_H_89734181783491324134
