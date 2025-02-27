// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "file_access.h"
#include <map>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include "file_traverser.h"
#include "scope_guard.h"
#include "symlink_target.h"
#include "file_io.h"
#include "crc.h"
#include "guid.h"

    #include <sys/vfs.h> //statfs
    //#include <sys/time.h> //lutimes
    #ifdef HAVE_SELINUX
        #include <selinux/selinux.h>
    #endif


    #include <fcntl.h> //open, close, AT_SYMLINK_NOFOLLOW, UTIME_OMIT
    #include <sys/stat.h>

using namespace zen;


namespace
{
}


ItemType zen::getItemType(const Zstring& itemPath) //throw FileError
{
    struct stat itemInfo = {};
    if (::lstat(itemPath.c_str(), &itemInfo) != 0)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(itemPath)), "lstat");

    if (S_ISLNK(itemInfo.st_mode))
        return ItemType::symlink;
    if (S_ISDIR(itemInfo.st_mode))
        return ItemType::folder;
    return ItemType::file; //S_ISREG || S_ISCHR || S_ISBLK || S_ISFIFO || S_ISSOCK
}


std::optional<ItemType> zen::itemStillExists(const Zstring& itemPath) //throw FileError
{
    try
    {
        return getItemType(itemPath); //throw FileError
    }
    catch (const FileError& e) //not existing or access error
    {
        const std::optional<Zstring> parentPath = getParentFolderPath(itemPath);
        if (!parentPath) //device root
            throw;
        //else: let's dig deeper... don't bother checking Win32 codes; e.g. not existing item may have the codes:
        //  ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_INVALID_NAME, ERROR_INVALID_DRIVE,
        //  ERROR_NOT_READY, ERROR_INVALID_PARAMETER, ERROR_BAD_PATHNAME, ERROR_BAD_NETPATH => not reliable

        const Zstring itemName = afterLast(itemPath, FILE_NAME_SEPARATOR, IfNotFoundReturn::all);
        assert(!itemName.empty());

        const std::optional<ItemType> parentType = itemStillExists(*parentPath); //throw FileError

        if (parentType && *parentType != ItemType::file /*obscure, but possible (and not an error)*/)
            try
            {
                traverseFolder(*parentPath,
                [&](const    FileInfo& fi) { if (fi.itemName == itemName) throw ItemType::file;    },
                [&](const  FolderInfo& fi) { if (fi.itemName == itemName) throw ItemType::folder;  },
                [&](const SymlinkInfo& si) { if (si.itemName == itemName) throw ItemType::symlink; },
                [](const std::wstring& errorMsg) { throw FileError(errorMsg); });
            }
            catch (const ItemType&) //finding the item after getItemType() previously failed is exceptional
            {
                throw FileError(_("Temporary access error:") + L' ' + e.toString());
            }
        return {};
    }
}


bool zen::fileAvailable(const Zstring& filePath) //noexcept
{
    //symbolic links (broken or not) are also treated as existing files!
    struct stat fileInfo = {};
    if (::stat(filePath.c_str(), &fileInfo) == 0) //follow symlinks!
        return S_ISREG(fileInfo.st_mode);
    return false;
}


bool zen::dirAvailable(const Zstring& dirPath) //noexcept
{
    //symbolic links (broken or not) are also treated as existing directories!
    struct stat dirInfo = {};
    if (::stat(dirPath.c_str(), &dirInfo) == 0) //follow symlinks!
        return S_ISDIR(dirInfo.st_mode);
    return false;
}


namespace
{
}


int64_t zen::getFreeDiskSpace(const Zstring& path) //throw FileError, returns < 0 if not available
{
    struct statfs info = {};
    if (::statfs(path.c_str(), &info) != 0) //follows symlinks!
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot determine free disk space for %x."), L"%x", fmtPath(path)), "statfs");
    //Linux: "Fields that are undefined for a particular file system are set to 0."
    //macOS: "Fields that are undefined for a particular file system are set to -1." - mkay :>
    if (makeSigned(info.f_bsize)  <= 0 ||
        makeSigned(info.f_bavail) <= 0)
        return -1;

    return static_cast<int64_t>(info.f_bsize) * info.f_bavail;
}


uint64_t zen::getFileSize(const Zstring& filePath) //throw FileError
{
    struct stat fileInfo = {};
    if (::stat(filePath.c_str(), &fileInfo) != 0)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(filePath)), "stat");

    return fileInfo.st_size;
}




Zstring zen::getTempFolderPath() //throw FileError
{
    if (const char* tempPath = ::getenv("TMPDIR")) //no extended error reporting
        return tempPath;
    //TMPDIR not set on CentOS 7, WTF!
    return P_tmpdir; //usually resolves to "/tmp"
}



void zen::removeFilePlain(const Zstring& filePath) //throw FileError
{
    const char* functionName = "unlink";
    if (::unlink(filePath.c_str()) != 0)
    {
        ErrorCode ec = getLastError(); //copy before directly/indirectly making other system calls!
        //begin of "regular" error reporting
        std::wstring errorDescr = formatSystemError(functionName, ec);

        throw FileError(replaceCpy(_("Cannot delete file %x."), L"%x", fmtPath(filePath)), errorDescr);
    }
}


void zen::removeSymlinkPlain(const Zstring& linkPath) //throw FileError
{
    removeFilePlain(linkPath); //throw FileError
}


void zen::removeDirectoryPlain(const Zstring& dirPath) //throw FileError
{
    const char* functionName = "rmdir";
    if (::rmdir(dirPath.c_str()) != 0)
    {
        ErrorCode ec = getLastError(); //copy before making other system calls!
        bool symlinkExists = false;
        try { symlinkExists = getItemType(dirPath) == ItemType::symlink; } /*throw FileError*/ catch (FileError&) {} //previous exception is more relevant

        if (symlinkExists)
        {
            if (::unlink(dirPath.c_str()) != 0)
                THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot delete directory %x."), L"%x", fmtPath(dirPath)), "unlink");
            return;
        }
        throw FileError(replaceCpy(_("Cannot delete directory %x."), L"%x", fmtPath(dirPath)), formatSystemError(functionName, ec));
    }
    /*  Windows: may spuriously fail with ERROR_DIR_NOT_EMPTY(145) even though all child items have
        successfully been *marked* for deletion, but some application still has a handle open!
        e.g. Open "C:\Test\Dir1\Dir2" (filled with lots of files) in Explorer, then delete "C:\Test\Dir1" via ::RemoveDirectory() => Error 145
        Sample code: http://us.generation-nt.com/answer/createfile-directory-handles-removing-parent-help-29126332.html
        Alternatives: 1. move file/empty folder to some other location, then DeleteFile()/RemoveDirectory()
                      2. use CreateFile/FILE_FLAG_DELETE_ON_CLOSE *without* FILE_SHARE_DELETE instead of DeleteFile() => early failure            */
}


namespace
{
void removeDirectoryImpl(const Zstring& folderPath) //throw FileError
{
    std::vector<Zstring> filePaths;
    std::vector<Zstring> symlinkPaths;
    std::vector<Zstring> folderPaths;

    //get all files and directories from current directory (WITHOUT subdirectories!)
    traverseFolder(folderPath,
    [&](const    FileInfo& fi) {    filePaths.push_back(fi.fullPath); },
    [&](const  FolderInfo& fi) {  folderPaths.push_back(fi.fullPath); }, //defer recursion => save stack space and allow deletion of extremely deep hierarchies!
    [&](const SymlinkInfo& si) { symlinkPaths.push_back(si.fullPath); },
    [](const std::wstring& errorMsg) { throw FileError(errorMsg); });

    for (const Zstring& filePath : filePaths)
        removeFilePlain(filePath); //throw FileError

    for (const Zstring& symlinkPath : symlinkPaths)
        removeSymlinkPlain(symlinkPath); //throw FileError

    //delete directories recursively
    for (const Zstring& subFolderPath : folderPaths)
        removeDirectoryImpl(subFolderPath); //throw FileError; call recursively to correctly handle symbolic links

    removeDirectoryPlain(folderPath); //throw FileError
}
}


void zen::removeDirectoryPlainRecursion(const Zstring& dirPath) //throw FileError
{
    if (getItemType(dirPath) == ItemType::symlink) //throw FileError
        removeSymlinkPlain(dirPath); //throw FileError
    else
        removeDirectoryImpl(dirPath); //throw FileError
}


namespace
{

/* Usage overview: (avoid circular pattern!)

  moveAndRenameItem() --> moveAndRenameFileSub()
      |                            /|\
     \|/                            |
             Fix8Dot3NameClash()                */

//wrapper for file system rename function:
void moveAndRenameFileSub(const Zstring& pathFrom, const Zstring& pathTo, bool replaceExisting) //throw FileError, ErrorMoveUnsupported, ErrorTargetExisting
{
    auto throwException = [&](int ec)
    {
        const std::wstring errorMsg = replaceCpy(replaceCpy(_("Cannot move file %x to %y."), L"%x", L'\n' + fmtPath(pathFrom)), L"%y", L'\n' + fmtPath(pathTo));
        const std::wstring errorDescr = formatSystemError("rename", ec);

        if (ec == EXDEV)
            throw ErrorMoveUnsupported(errorMsg, errorDescr);

        assert(!replaceExisting || ec != EEXIST);
        if (!replaceExisting && ec == EEXIST)
            throw ErrorTargetExisting(errorMsg, errorDescr);

        throw FileError(errorMsg, errorDescr);
    };

    //rename() will never fail with EEXIST, but always (atomically) overwrite!
    //=> equivalent to SetFileInformationByHandle() + FILE_RENAME_INFO::ReplaceIfExists or ::MoveFileEx() + MOVEFILE_REPLACE_EXISTING
    //Linux: renameat2() with RENAME_NOREPLACE -> still new, probably buggy
    //macOS: no solution https://developer.apple.com/legacy/library/documentation/Darwin/Reference/ManPages/man2/rename.2.html
    if (!replaceExisting)
    {
        struct stat sourceInfo = {};
        if (::lstat(pathFrom.c_str(), &sourceInfo) != 0)
            THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(pathFrom)), "stat");

        struct stat targetInfo = {};
        if (::lstat(pathTo.c_str(), &targetInfo) == 0)
        {
            if (sourceInfo.st_dev != targetInfo.st_dev ||
                sourceInfo.st_ino != targetInfo.st_ino)
                throwException(EEXIST); //that's what we're really here for
            //else: continue with a rename in case
            //caveat: if we have a hardlink referenced by two different paths, the source one will be unlinked => fine, but not exactly a "rename"...
        }
        //else: not existing or access error (hopefully ::rename will also fail!)
    }

    if (::rename(pathFrom.c_str(), pathTo.c_str()) != 0)
        throwException(errno);
}


}


//rename file: no copying!!!
void zen::moveAndRenameItem(const Zstring& pathFrom, const Zstring& pathTo, bool replaceExisting) //throw FileError, ErrorMoveUnsupported, ErrorTargetExisting
{
    try
    {
        moveAndRenameFileSub(pathFrom, pathTo, replaceExisting); //throw FileError, ErrorMoveUnsupported, ErrorTargetExisting
    }
    catch (ErrorTargetExisting&)
    {
        throw;
    }
}


namespace
{
void setWriteTimeNative(const Zstring& itemPath, const timespec& modTime, ProcSymlink procSl) //throw FileError
{
    /* [2013-05-01] sigh, we can't use utimensat() on NTFS volumes on Ubuntu: silent failure!!! what morons are programming this shit???
        => fallback to "retarded-idiot version"! -- DarkByte

        [2015-03-09]
         - cannot reproduce issues with NTFS and utimensat() on Ubuntu
         - utimensat() is supposed to obsolete utime/utimes and is also used by "cp" and "touch"
            => let's give utimensat another chance:
            using open()/futimens() for regular files and utimensat(AT_SYMLINK_NOFOLLOW) for symlinks is consistent with "cp" and "touch"!
        cp:    https://github.com/coreutils/coreutils/blob/master/src/cp.c
            => utimens: https://github.com/coreutils/gnulib/blob/master/lib/utimens.c
        touch: https://github.com/coreutils/coreutils/blob/master/src/touch.c
            => fdutimensat: https://github.com/coreutils/gnulib/blob/master/lib/fdutimensat.c                  */
    timespec newTimes[2] = {};
    newTimes[0].tv_sec = ::time(nullptr); //access time; don't use UTIME_NOW/UTIME_OMIT: more bugs! https://freefilesync.org/forum/viewtopic.php?t=1701
    newTimes[1] = modTime; //modification time
    //test: even modTime == 0 is correctly applied (no NOOP!) test2: same behavior for "utime()"

    //hell knows why files on gvfs-mounted Samba shares fail to open(O_WRONLY) returning EOPNOTSUPP:
    //https://freefilesync.org/forum/viewtopic.php?t=2803 => utimensat() works (but not for gvfs SFTP)
    if (::utimensat(AT_FDCWD, itemPath.c_str(), newTimes, procSl == ProcSymlink::direct ? AT_SYMLINK_NOFOLLOW : 0) == 0)
        return;
    try
    {
        if (procSl == ProcSymlink::direct)
            try
            {
                if (getItemType(itemPath) == ItemType::symlink) //throw FileError
                    THROW_LAST_SYS_ERROR("utimensat(AT_SYMLINK_NOFOLLOW)"); //use lutimes()? just a wrapper around utimensat()!
                //else: fall back
            }
            catch (const FileError& e) { throw SysError(e.toString()); }

        //in other cases utimensat() returns EINVAL for CIFS/NTFS drives, but open+futimens works: https://freefilesync.org/forum/viewtopic.php?t=387
        //2017-07-04: O_WRONLY | O_APPEND seems to avoid EOPNOTSUPP on gvfs SFTP!
        const int fdFile = ::open(itemPath.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fdFile == -1)
            THROW_LAST_SYS_ERROR("open");
        ZEN_ON_SCOPE_EXIT(::close(fdFile));

        if (::futimens(fdFile, newTimes) != 0)
            THROW_LAST_SYS_ERROR("futimens");

        //need  more fallbacks? e.g. futimes()? careful, bugs! futimes() rounds instead of truncates when falling back on utime()!
    }
    catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot write modification time of %x."), L"%x", fmtPath(itemPath)), e.toString()); }
}


}


void zen::setFileTime(const Zstring& filePath, time_t modTime, ProcSymlink procSl) //throw FileError
{
    setWriteTimeNative(filePath, timetToNativeFileTime(modTime),
                       procSl); //throw FileError
}


bool zen::supportsPermissions(const Zstring& dirPath) //throw FileError
{
    return true;
}


namespace
{
#ifdef HAVE_SELINUX
//copy SELinux security context
void copySecurityContext(const Zstring& source, const Zstring& target, ProcSymlink procSl) //throw FileError
{
    security_context_t contextSource = nullptr;
    const int rv = procSl == ProcSymlink::follow ?
                   ::getfilecon (source.c_str(), &contextSource) :
                   ::lgetfilecon(source.c_str(), &contextSource);
    if (rv < 0)
    {
        if (errno == ENODATA ||  //no security context (allegedly) is not an error condition on SELinux
            errno == EOPNOTSUPP) //extended attributes are not supported by the filesystem
            return;

        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read security context of %x."), L"%x", fmtPath(source)), "getfilecon");
    }
    ZEN_ON_SCOPE_EXIT(::freecon(contextSource));

    {
        security_context_t contextTarget = nullptr;
        const int rv2 = procSl == ProcSymlink::follow ?
                        ::getfilecon(target.c_str(), &contextTarget) :
                        ::lgetfilecon(target.c_str(), &contextTarget);
        if (rv2 < 0)
        {
            if (errno == EOPNOTSUPP)
                return;
            //else: still try to set security context
        }
        else
        {
            ZEN_ON_SCOPE_EXIT(::freecon(contextTarget));

            if (::strcmp(contextSource, contextTarget) == 0) //nothing to do
                return;
        }
    }

    const int rv3 = procSl == ProcSymlink::follow ?
                    ::setfilecon(target.c_str(), contextSource) :
                    ::lsetfilecon(target.c_str(), contextSource);
    if (rv3 < 0)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot write security context of %x."), L"%x", fmtPath(target)), "setfilecon");
}
#endif
}


//copy permissions for files, directories or symbolic links: requires admin rights
void zen::copyItemPermissions(const Zstring& sourcePath, const Zstring& targetPath, ProcSymlink procSl) //throw FileError
{

#ifdef HAVE_SELINUX  //copy SELinux security context
    copySecurityContext(sourcePath, targetPath, procSl); //throw FileError
#endif

    struct stat fileInfo = {};
    if (procSl == ProcSymlink::follow)
    {
        if (::stat(sourcePath.c_str(), &fileInfo) != 0)
            THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read permissions of %x."), L"%x", fmtPath(sourcePath)), "stat");

        if (::chown(targetPath.c_str(), fileInfo.st_uid, fileInfo.st_gid) != 0) // may require admin rights!
            THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot write permissions of %x."), L"%x", fmtPath(targetPath)), "chown");

        if (::chmod(targetPath.c_str(), fileInfo.st_mode) != 0)
            THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot write permissions of %x."), L"%x", fmtPath(targetPath)), "chmod");
    }
    else
    {
        if (::lstat(sourcePath.c_str(), &fileInfo) != 0)
            THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read permissions of %x."), L"%x", fmtPath(sourcePath)), "lstat");

        if (::lchown(targetPath.c_str(), fileInfo.st_uid, fileInfo.st_gid) != 0) // may require admin rights!
            THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot write permissions of %x."), L"%x", fmtPath(targetPath)), "lchown");

        const bool isSymlinkTarget = getItemType(targetPath) == ItemType::symlink; //throw FileError
        if (!isSymlinkTarget && //setting access permissions doesn't make sense for symlinks on Linux: there is no lchmod()
            ::chmod(targetPath.c_str(), fileInfo.st_mode) != 0)
            THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot write permissions of %x."), L"%x", fmtPath(targetPath)), "chmod");
    }

}


void zen::createDirectory(const Zstring& dirPath) //throw FileError, ErrorTargetExisting
{
    auto getErrorMsg = [&] { return replaceCpy(_("Cannot create directory %x."), L"%x", fmtPath(dirPath)); };

    //don't allow creating irregular folders!
    const Zstring dirName = afterLast(dirPath, FILE_NAME_SEPARATOR, IfNotFoundReturn::all);

    //e.g. "...." https://social.technet.microsoft.com/Forums/windows/en-US/ffee2322-bb6b-4fdf-86f9-8f93cf1fa6cb/
    if (std::all_of(dirName.begin(), dirName.end(), [](Zchar c) { return c == Zstr('.'); }))
    /**/throw FileError(getErrorMsg(), replaceCpy<std::wstring>(L"Invalid folder name %x.", L"%x", fmtPath(dirName)));

#if 0 //not appreciated: https://freefilesync.org/forum/viewtopic.php?t=7509
    if (startsWith(dirName, Zstr(' ')) || //Windows can access these just fine once created!
        endsWith  (dirName, Zstr(' ')))   //
        throw FileError(getErrorMsg(), replaceCpy<std::wstring>(L"Invalid folder name. %x starts/ends with space character.", L"%x", fmtPath(dirName)));
#endif


    const mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO; //0777 => consider umask!

    if (::mkdir(dirPath.c_str(), mode) != 0)
    {
        const int lastError = errno; //copy before directly or indirectly making other system calls!
        const std::wstring errorDescr = formatSystemError("mkdir", lastError);

        if (lastError == EEXIST)
            throw ErrorTargetExisting(getErrorMsg(), errorDescr);
        //else if (lastError == ENOENT)
        //    throw ErrorTargetPathMissing(errorMsg, errorDescr);
        throw FileError(getErrorMsg(), errorDescr);
    }
}


bool zen::createDirectoryIfMissingRecursion(const Zstring& dirPath) //throw FileError
{
    const std::optional<Zstring> parentPath = getParentFolderPath(dirPath);
    if (!parentPath) //device root
        return false;

    try //generally we expect that path already exists (see: ffs_paths.cpp) => check first
    {
        if (getItemType(dirPath) != ItemType::file) //throw FileError
            return false;
    }
    catch (FileError&) {} //not yet existing or access error? let's find out...

    createDirectoryIfMissingRecursion(*parentPath); //throw FileError

    try
    {
        createDirectory(dirPath); //throw FileError, ErrorTargetExisting
        return true;
    }
    catch (FileError&)
    {
        try
        {
            if (getItemType(dirPath) != ItemType::file) //throw FileError
                return true; //already existing => possible, if createDirectoryIfMissingRecursion() is run in parallel
        }
        catch (FileError&) {} //not yet existing or access error

        throw;
    }
}


void zen::tryCopyDirectoryAttributes(const Zstring& sourcePath, const Zstring& targetPath) //throw FileError
{
}


void zen::copySymlink(const Zstring& sourcePath, const Zstring& targetPath) //throw FileError
{
    const SymlinkRawContent linkContent = getSymlinkRawContent(sourcePath); //throw FileError; accept broken symlinks

    try //harmonize with NativeFileSystem::equalSymlinkContentForSameAfsType()
    {
        if (::symlink(linkContent.targetPath.c_str(), targetPath.c_str()) != 0)
            THROW_LAST_SYS_ERROR("symlink");
    }
    catch (const SysError& e)
    {
        throw FileError(replaceCpy(replaceCpy(_("Cannot copy symbolic link %x to %y."), L"%x", L'\n' + fmtPath(sourcePath)), L"%y", L'\n' + fmtPath(targetPath)), e.toString());
    }

    //allow only consistent objects to be created -> don't place before ::symlink(); targetPath may already exist!
    ZEN_ON_SCOPE_FAIL(try { removeSymlinkPlain(targetPath); /*throw FileError*/ }
    catch (FileError&) {});

    //file times: essential for syncing a symlink: enforce this! (don't just try!)
    struct stat sourceInfo = {};
    if (::lstat(sourcePath.c_str(), &sourceInfo) != 0)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(sourcePath)), "lstat");

    setWriteTimeNative(targetPath, sourceInfo.st_mtim, ProcSymlink::direct); //throw FileError
}


FileCopyResult zen::copyNewFile(const Zstring& sourceFile, const Zstring& targetFile, //throw FileError, ErrorTargetExisting, (ErrorFileLocked), X
                                const IoCallback& notifyUnbufferedIO /*throw X*/)
{
    int64_t totalUnbufferedIO = 0;

    FileInput fileIn(sourceFile, IOCallbackDivider(notifyUnbufferedIO, totalUnbufferedIO)); //throw FileError, (ErrorFileLocked -> Windows-only)

    struct stat sourceInfo = {};
    if (::fstat(fileIn.getHandle(), &sourceInfo) != 0)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(sourceFile)), "fstat");

    const mode_t mode = sourceInfo.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO); //analog to "cp" which copies "mode" (considering umask) by default
    //it seems we don't need S_IWUSR, not even for the setFileTime() below! (tested with source file having different user/group!)

    //=> need copyItemPermissions() only for "chown" and umask-agnostic permissions
    const int fdTarget = ::open(targetFile.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, mode);
    if (fdTarget == -1)
    {
        const int ec = errno; //copy before making other system calls!
        const std::wstring errorMsg = replaceCpy(_("Cannot write file %x."), L"%x", fmtPath(targetFile));
        const std::wstring errorDescr = formatSystemError("open", ec);

        if (ec == EEXIST)
            throw ErrorTargetExisting(errorMsg, errorDescr);

        throw FileError(errorMsg, errorDescr);
    }
    FileOutput fileOut(fdTarget, targetFile, IOCallbackDivider(notifyUnbufferedIO, totalUnbufferedIO)); //pass ownership

    //preallocate disk space + reduce fragmentation (perf: no real benefit)
    fileOut.reserveSpace(sourceInfo.st_size); //throw FileError

    bufferedStreamCopy(fileIn, fileOut); //throw FileError, (ErrorFileLocked), X

    //flush intermediate buffers before fiddling with the raw file handle
    fileOut.flushBuffers(); //throw FileError, X

    struct stat targetInfo = {};
    if (::fstat(fileOut.getHandle(), &targetInfo) != 0)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(targetFile)), "fstat");

    //close output file handle before setting file time; also good place to catch errors when closing stream!
    fileOut.finalize(); //throw FileError, (X)  essentially a close() since  buffers were already flushed

    //==========================================================================================================
    //take fileOut ownership => from this point on, WE are responsible for calling removeFilePlain() on failure!!
    //===========================================================================================================

    std::optional<FileError> errorModTime;
    try
    {
        /*  we cannot set the target file times (::futimes) while the file descriptor is still open after a write operation:
            this triggers bugs on Samba shares where the modification time is set to current time instead.
            Linux: https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=340236
                   http://comments.gmane.org/gmane.linux.file-systems.cifs/2854
            macOS: https://freefilesync.org/forum/viewtopic.php?t=356             */
        setWriteTimeNative(targetFile, sourceInfo.st_mtim, ProcSymlink::follow); //throw FileError
    }
    catch (const FileError& e)
    {
        errorModTime = FileError(e.toString()); //avoid slicing
    }

    FileCopyResult result;
    result.fileSize = sourceInfo.st_size;
    result.sourceModTime = sourceInfo.st_mtim;
    result.sourceFileIdx = sourceInfo.st_ino;
    result.targetFileIdx = targetInfo.st_ino;
    result.errorModTime = errorModTime;
    return result;
}


