// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "native.h"
#include <zen/file_access.h>
#include <zen/symlink_target.h>
#include <zen/file_io.h>
#include <zen/stl_tools.h>
#include <zen/resolve_path.h>
#include <zen/recycler.h>
#include <zen/thread.h>
#include <zen/guid.h>
#include <zen/crc.h>
#include "abstract_impl.h"
#include "../base/icon_loader.h"

    #include <sys/vfs.h> //statfs

    #include <sys/stat.h>
    #include <dirent.h>
    #include <fcntl.h> //fallocate, fcntl

using namespace zen;
using namespace fff;
using AFS = AbstractFileSystem;


namespace
{

void initComForThread() //throw FileError
{
}

//====================================================================================================
//====================================================================================================

//persistent + unique (relative to volume) or 0!
inline
AFS::FingerPrint getFileFingerprint(FileIndex fileIndex)
{
    static_assert(sizeof(fileIndex) == sizeof(AFS::FingerPrint));
    return fileIndex; //== 0 if not supported
    /*  File details
        ------------
            st_mtim      (Linux)
            st_mtimespec (macOS): nanosecond-precision for improved uniqueness?
                          => essentially unknown after file copy (e.g. to FAT) without extra directory traversal :(

            macOS st_birthtimespec: "if not supported by file system, holds the ctime instead"
                                    ctime: inode modification time => changed on* rename*! => FU...

        Volume details
        --------------
            st_dev: "st_dev value is not necessarily consistent across reboots or system crashes" https://freefilesync.org/forum/viewtopic.php?t=8054
                    only locally unique and depends on device mount point! => FU...

            f_fsid: "Some operating systems use the device number..." => fuck!
                    "Several OSes restrict giving out the f_fsid field to the superuser only"

            f_bsize  macOS: "fundamental file system block size"
                     Linux: "optimal transfer block size" -> no! for all intents and purposes this *is* the "fundamental file system block size": https://stackoverflow.com/a/54835515
            f_blocks => meh...

            f_type Linux: documented values, nice! https://linux.die.net/man/2/statfs
                    macOS: - not stable between macOS releases: https://developer.apple.com/forums/thread/87745
                            - Apple docs say: "generally not a useful value"
                            - f_fstypename can be used as alternative

            DADiskGetBSDName(): macOS only           */
}


struct NativeFileInfo
{
    FileTimeNative modTime;
    uint64_t fileSize;
    FileIndex fileIndex;
};
NativeFileInfo getFileAttributes(FileBase::FileHandle fh) //throw SysError
{
    struct stat fileInfo = {};
    if (::fstat(fh, &fileInfo) != 0)
        THROW_LAST_SYS_ERROR("fstat");

    return
    {
        fileInfo.st_mtim,
        makeUnsigned(fileInfo.st_size),
        fileInfo.st_ino
    };
}


struct FsItem
{
    Zstring itemName;
};
std::vector<FsItem> getDirContentFlat(const Zstring& dirPath) //throw FileError
{
    //no need to check for endless recursion:
    //1. Linux has a fixed limit on the number of symbolic links in a path
    //2. fails with "too many open files" or "path too long" before reaching stack overflow

    DIR* folder = ::opendir(dirPath.c_str()); //directory must NOT end with path separator, except "/"
    if (!folder)
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot open directory %x."), L"%x", fmtPath(dirPath)), "opendir");
    ZEN_ON_SCOPE_EXIT(::closedir(folder)); //never close nullptr handles! -> crash

    std::vector<FsItem> output;
    for (;;)
    {
        /* Linux: https://man7.org/linux/man-pages/man3/readdir_r.3.html
                "It is recommended that applications use readdir(3) instead of readdir_r"
                "... in modern implementations (including the glibc implementation), concurrent calls to readdir(3) that specify different directory streams are thread-safe"

           macOS: - libc: readdir thread-safe already in code from 2000: https://opensource.apple.com/source/Libc/Libc-166/gen.subproj/readdir.c.auto.html
                  - and in the latest version from 2017:                 https://opensource.apple.com/source/Libc/Libc-1244.30.3/gen/FreeBSD/readdir.c.auto.html                   */
        errno = 0;
        const dirent* dirEntry = ::readdir(folder);
        if (!dirEntry)
        {
            if (errno == 0) //errno left unchanged => no more items
                return output;

            THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read directory %x."), L"%x", fmtPath(dirPath)), "readdir");
            //don't retry but restart dir traversal on error! https://devblogs.microsoft.com/oldnewthing/20140612-00/?p=753
        }

        const char* itemNameRaw = dirEntry->d_name;

        //skip "." and ".."
        if (itemNameRaw[0] == '.' &&
            (itemNameRaw[1] == 0 || (itemNameRaw[1] == '.' && itemNameRaw[2] == 0)))
            continue;

        if (itemNameRaw[0] == 0) //show error instead of endless recursion!!!
            throw FileError(replaceCpy(_("Cannot read directory %x."), L"%x", fmtPath(dirPath)), formatSystemError("readdir", L"", L"Folder contains an item without name."));

        output.push_back({itemNameRaw});

        /* Unicode normalization is file-system-dependent:

               OS                 Accepts   Gives back
               ----------         -------   ----------
               macOS (HFS+)         all        NFD
               Linux                all      <input>
               Windows (NTFS, FAT)  all      <input>

            some file systems return precomposed others decomposed UTF8: https://developer.apple.com/library/archive/qa/qa1173/_index.html
                  - OS X edit controls and text fields may return precomposed UTF as directly received by keyboard or decomposed UTF that was copy & pasted!
                  - Posix APIs require decomposed form: https://freefilesync.org/forum/viewtopic.php?t=2480

            => General recommendation: always preserve input UNCHANGED (both unicode normalization and case sensitivity)
            => normalize only when needed during string comparison

            Create sample files on Linux: touch  decomposed-$'\x6f\xcc\x81'.txt
                                          touch precomposed-$'\xc3\xb3'.txt

            - list file name hex chars in terminal:  ls | od -c -t x1

            - SMB sharing case-sensitive or NFD file names is fundamentally broken on macOS:
                => the macOS SMB manager internally buffers file names as case-insensitive and NFC (= just like NTFS on Windows)
                => test: create SMB share from Linux => *boom* on macOS: "Error Code 2: No such file or directory [lstat]"
                    or WORSE: folders "test" and "Test" *both* incorrectly return the content of one of the two
                => Update 2020-04-24: converting to NFC doesn't help: both NFD/NFC forms fail(ENOENT) lstat in FFS, AS WELL AS IN FINDER => macOS bug!         */
    }
}


struct FsItemDetails
{
    ItemType type;
    time_t   modTime; //number of seconds since Jan. 1st 1970 UTC
    uint64_t fileSize; //unit: bytes!
    AFS::FingerPrint filePrint;
};
FsItemDetails getItemDetails(const Zstring& itemPath) //throw FileError
{
    struct stat itemInfo = {};
    if (::lstat(itemPath.c_str(), &itemInfo) != 0) //lstat() does not resolve symlinks
        THROW_LAST_FILE_ERROR(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(itemPath)), "lstat");

    return {S_ISLNK(itemInfo.st_mode) ? ItemType::symlink : //on Linux there is no distinction between file and directory symlinks!
            /**/ (S_ISDIR(itemInfo.st_mode) ? ItemType::folder : ItemType::file), //a file or named pipe, etc. => dont't check using S_ISREG(): see comment in file_traverser.cpp
            itemInfo.st_mtime,
            makeUnsigned(itemInfo.st_size),
            getFileFingerprint(itemInfo.st_ino)};
}


FsItemDetails getSymlinkTargetDetails(const Zstring& linkPath) //throw FileError
{
    try
    {
        struct stat itemInfo = {};
        if (::stat(linkPath.c_str(), &itemInfo) != 0)
            THROW_LAST_SYS_ERROR("stat");

        const ItemType targetType = S_ISDIR(itemInfo.st_mode) ? ItemType::folder : ItemType::file;

        const AFS::FingerPrint filePrint = targetType == ItemType::folder ? 0 : getFileFingerprint(itemInfo.st_ino);
        return {targetType,
                itemInfo.st_mtime,
                makeUnsigned(itemInfo.st_size),
                filePrint};
    }
    catch (const SysError& e)
    {
        throw FileError(replaceCpy(_("Cannot resolve symbolic link %x."), L"%x", fmtPath(linkPath)), e.toString());
    }
}


class SingleFolderTraverser
{
public:
    SingleFolderTraverser(const std::vector<std::pair<Zstring, std::shared_ptr<AFS::TraverserCallback>>>& workload /*throw X*/)
    {
        for (const auto& [folderPath, cb] : workload)
            workload_.push_back({folderPath, cb});

        while (!workload_.empty())
        {
            WorkItem wi = std::move(workload_.    back()); //yes, no strong exception guarantee (std::bad_alloc)
            /**/                    workload_.pop_back();  //

            tryReportingDirError([&] //throw X
            {
                traverseWithException(wi.dirPath, *wi.cb); //throw FileError, X
            }, *wi.cb);
        }
    }

private:
    SingleFolderTraverser           (const SingleFolderTraverser&) = delete;
    SingleFolderTraverser& operator=(const SingleFolderTraverser&) = delete;

    void traverseWithException(const Zstring& dirPath, AFS::TraverserCallback& cb) //throw FileError, X
    {
        for (const auto& [itemName] : getDirContentFlat(dirPath)) //throw FileError
        {
            const Zstring itemPath = appendSeparator(dirPath) + itemName;

            FsItemDetails itemDetails = {};
            if (!tryReportingItemError([&] //throw X
        {
            itemDetails = getItemDetails(itemPath); //throw FileError
            }, cb, itemName))
            continue; //ignore error: skip file

            switch (itemDetails.type)
            {
                case ItemType::file:
                    cb.onFile({itemName, itemDetails.fileSize, itemDetails.modTime, itemDetails.filePrint, false /*isFollowedSymlink*/}); //throw X
                    break;

                case ItemType::folder:
                    if (std::shared_ptr<AFS::TraverserCallback> cbSub = cb.onFolder({itemName, false /*isFollowedSymlink*/})) //throw X
                        workload_.push_back({itemPath, std::move(cbSub)});
                    break;

                case ItemType::symlink:
                    switch (cb.onSymlink({itemName, itemDetails.modTime})) //throw X
                    {
                        case AFS::TraverserCallback::HandleLink::follow:
                        {
                            FsItemDetails targetDetails = {};
                            if (!tryReportingItemError([&] //throw X
                        {
                            targetDetails = getSymlinkTargetDetails(itemPath); //throw FileError
                            }, cb, itemName))
                            continue;

                            if (targetDetails.type == ItemType::folder)
                            {
                                if (std::shared_ptr<AFS::TraverserCallback> cbSub = cb.onFolder({itemName, true /*isFollowedSymlink*/})) //throw X
                                    workload_.push_back({itemPath, std::move(cbSub)}); //symlink may link to different volume!
                            }
                            else //a file or named pipe, etc.
                                cb.onFile({itemName, targetDetails.fileSize, targetDetails.modTime, targetDetails.filePrint, true /*isFollowedSymlink*/}); //throw X
                        }
                        break;

                        case AFS::TraverserCallback::HandleLink::skip:
                            break;
                    }
                    break;
            }
        }
    }

    struct WorkItem
    {
        Zstring dirPath;
        std::shared_ptr<AFS::TraverserCallback> cb;
    };
    std::vector<WorkItem> workload_;
};


void traverseFolderRecursiveNative(const std::vector<std::pair<Zstring, std::shared_ptr<AFS::TraverserCallback>>>& workload /*throw X*/, size_t) //throw X
{
    SingleFolderTraverser dummy(workload); //throw X
}
//====================================================================================================
//====================================================================================================

class RecycleSessionNative : public AFS::RecycleSession
{
public:
    explicit RecycleSessionNative(const Zstring& baseFolderPath) : baseFolderPath_(baseFolderPath) {}

    void recycleItemIfExists(const AbstractPath& itemPath, const Zstring& logicalRelPath) override; //throw FileError
    void tryCleanup(const std::function<void (const std::wstring& displayPath)>& notifyDeletionStatus /*throw X*/) override; //throw FileError, X

private:
    const Zstring baseFolderPath_; //ends with path separator
};

//===========================================================================================================================

struct InputStreamNative : public AFS::InputStream
{
    InputStreamNative(const Zstring& filePath, const IoCallback& notifyUnbufferedIO /*throw X*/) : fi_(filePath, notifyUnbufferedIO) {} //throw FileError, ErrorFileLocked

    size_t read(void* buffer, size_t bytesToRead) override { return fi_.read(buffer, bytesToRead); } //throw FileError, ErrorFileLocked, X; return "bytesToRead" bytes unless end of stream!
    size_t getBlockSize() const override { return fi_.getBlockSize(); } //non-zero block size is AFS contract!
    std::optional<AFS::StreamAttributes> getAttributesBuffered() override //throw FileError
    {
        try
        {
            const NativeFileInfo& fileInfo = getFileAttributes(fi_.getHandle()); //throw SysError

            return AFS::StreamAttributes({nativeFileTimeToTimeT(fileInfo.modTime),
                                          fileInfo.fileSize,
                                          getFileFingerprint(fileInfo.fileIndex)});
        }
        catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(fi_.getFilePath())), e.toString()); }
    }

private:
    FileInput fi_;
};

//===========================================================================================================================

struct OutputStreamNative : public AFS::OutputStreamImpl
{
    OutputStreamNative(const Zstring& filePath,
                       std::optional<uint64_t> streamSize,
                       std::optional<time_t> modTime,
                       const IoCallback& notifyUnbufferedIO /*throw X*/) :
        fo_(filePath, notifyUnbufferedIO), //throw FileError, ErrorTargetExisting
        modTime_(modTime)
    {
        if (streamSize) //preallocate disk space + reduce fragmentation
            fo_.reserveSpace(*streamSize); //throw FileError
    }

    void write(const void* buffer, size_t bytesToWrite) override { fo_.write(buffer, bytesToWrite); } //throw FileError, X

    AFS::FinalizeResult finalize() override //throw FileError, X
    {
        AFS::FinalizeResult result;
        if (modTime_)
            try
            {
                const FileIndex fileIndex = getFileAttributes(fo_.getHandle()).fileIndex; //throw SysError
                result.filePrint = getFileFingerprint(fileIndex);
            }
            catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(fo_.getFilePath())), e.toString()); }

        fo_.finalize(); //throw FileError, X

        try
        {
            if (modTime_)
                setFileTime(fo_.getFilePath(), *modTime_, ProcSymlink::follow); //throw FileError
            /* is setting modtime after closing the file handle a pessimization?
               no, needed for functional correctness, see file_access.cpp */
        }
        catch (const FileError& e) { result.errorModTime = FileError(e.toString()); /*avoid slicing*/ }

        return result;
    }

private:
    FileOutput fo_;
    const std::optional<time_t> modTime_;
};

//===========================================================================================================================

class NativeFileSystem : public AbstractFileSystem
{
public:
    explicit NativeFileSystem(const Zstring& rootPath) : rootPath_(rootPath) {}

    Zstring getNativePath(const AfsPath& afsPath) const { return isNullFileSystem() ? Zstring() : nativeAppendPaths(rootPath_, afsPath.value); }

private:
    Zstring getInitPathPhrase(const AfsPath& afsPath) const override
    {
        Zstring initPathPhrase = getNativePath(afsPath);
        if (endsWith(initPathPhrase, Zstr(' '))) //path prase concept must survive trimming!
            initPathPhrase += FILE_NAME_SEPARATOR;
        return initPathPhrase;
    }

    std::wstring getDisplayPath(const AfsPath& afsPath) const override { return utfTo<std::wstring>(getNativePath(afsPath)); }

    bool isNullFileSystem() const override { return rootPath_.empty(); }

    std::weak_ordering compareDeviceSameAfsType(const AbstractFileSystem& afsRhs) const override
    {
        return compareNativePath(rootPath_, static_cast<const NativeFileSystem&>(afsRhs).rootPath_);
    }

    //----------------------------------------------------------------------------------------------------------------
    ItemType getItemType(const AfsPath& afsPath) const override //throw FileError
    {
        initComForThread(); //throw FileError
        switch (zen::getItemType(getNativePath(afsPath))) //throw FileError
        {
            case zen::ItemType::file:
                return AFS::ItemType::file;
            case zen::ItemType::folder:
                return AFS::ItemType::folder;
            case zen::ItemType::symlink:
                return AFS::ItemType::symlink;
        }
        assert(false);
        return AFS::ItemType::file;
    }

    std::optional<ItemType> itemStillExists(const AfsPath& afsPath) const override //throw FileError
    {
        //default implementation: folder traversal
        return AFS::itemStillExists(afsPath); //throw FileError
    }
    //----------------------------------------------------------------------------------------------------------------

    //already existing: fail
    void createFolderPlain(const AfsPath& afsPath) const override //throw FileError
    {
        initComForThread(); //throw FileError
        createDirectory(getNativePath(afsPath)); //throw FileError, ErrorTargetExisting
    }

    void removeFilePlain(const AfsPath& afsPath) const override //throw FileError
    {
        initComForThread(); //throw FileError
        zen::removeFilePlain(getNativePath(afsPath)); //throw FileError
    }

    void removeSymlinkPlain(const AfsPath& afsPath) const override //throw FileError
    {
        initComForThread(); //throw FileError
        zen::removeSymlinkPlain(getNativePath(afsPath)); //throw FileError
    }

    void removeFolderPlain(const AfsPath& afsPath) const override //throw FileError
    {
        initComForThread(); //throw FileError
        zen::removeDirectoryPlain(getNativePath(afsPath)); //throw FileError
    }

    void removeFolderIfExistsRecursion(const AfsPath& afsPath, //throw FileError
                                       const std::function<void (const std::wstring& displayPath)>& onBeforeFileDeletion /*throw X*/, //optional
                                       const std::function<void (const std::wstring& displayPath)>& onBeforeFolderDeletion) const override //one call for each object!
    {
        //default implementation: folder traversal
        AFS::removeFolderIfExistsRecursion(afsPath, onBeforeFileDeletion, onBeforeFolderDeletion); //throw FileError, X
    }

    //----------------------------------------------------------------------------------------------------------------
    AbstractPath getSymlinkResolvedPath(const AfsPath& afsPath) const override //throw FileError
    {
        initComForThread(); //throw FileError
        const Zstring nativePath = getNativePath(afsPath);

        const Zstring resolvedPath = zen::getSymlinkResolvedPath(nativePath); //throw FileError
        const std::optional<zen::PathComponents> comp = parsePathComponents(resolvedPath);
        if (!comp)
            throw FileError(replaceCpy(_("Cannot determine final path for %x."), L"%x", fmtPath(nativePath)),
                            replaceCpy<std::wstring>(L"Invalid path %x.", L"%x", fmtPath(resolvedPath)));

        return AbstractPath(makeSharedRef<NativeFileSystem>(comp->rootPath), AfsPath(comp->relPath));
    }

    bool equalSymlinkContentForSameAfsType(const AfsPath& afsLhs, const AbstractPath& apRhs) const override //throw FileError
    {
        initComForThread(); //throw FileError

        const NativeFileSystem& nativeFsR = static_cast<const NativeFileSystem&>(apRhs.afsDevice.ref());

        const SymlinkRawContent linkContentL = getSymlinkRawContent(getNativePath(afsLhs)); //throw FileError
        const SymlinkRawContent linkContentR = getSymlinkRawContent(nativeFsR.getNativePath(apRhs.afsPath)); //throw FileError

        if (linkContentL.targetPath != linkContentR.targetPath)
            return false;

        return true;
    }
    //----------------------------------------------------------------------------------------------------------------

    //return value always bound:
    std::unique_ptr<InputStream> getInputStream(const AfsPath& afsPath, const IoCallback& notifyUnbufferedIO /*throw X*/) const override //throw FileError, ErrorFileLocked
    {
        initComForThread(); //throw FileError
        return std::make_unique<InputStreamNative>(getNativePath(afsPath), notifyUnbufferedIO); //throw FileError, ErrorFileLocked
    }

    //already existing: undefined behavior! (e.g. fail/overwrite/auto-rename)
    //=> actual behavior: fail with clear error message
    std::unique_ptr<OutputStreamImpl> getOutputStream(const AfsPath& afsPath, //throw FileError
                                                      std::optional<uint64_t> streamSize,
                                                      std::optional<time_t> modTime,
                                                      const IoCallback& notifyUnbufferedIO /*throw X*/) const override
    {
        initComForThread(); //throw FileError
        return std::make_unique<OutputStreamNative>(getNativePath(afsPath), streamSize, modTime, notifyUnbufferedIO); //throw FileError
    }

    //----------------------------------------------------------------------------------------------------------------
    void traverseFolderRecursive(const TraverserWorkload& workload /*throw X*/, size_t parallelOps) const override
    {
        //initComForThread() -> done on traverser worker threads

        std::vector<std::pair<Zstring, std::shared_ptr<TraverserCallback>>> initialWorkItems;
        for (const auto& [folderPath, cb] : workload)
            initialWorkItems.emplace_back(getNativePath(folderPath), cb);

        traverseFolderRecursiveNative(initialWorkItems, parallelOps); //throw X
    }
    //----------------------------------------------------------------------------------------------------------------

    //symlink handling: follow
    //already existing: undefined behavior! (e.g. fail/overwrite/auto-rename)
    //=> actual behavior: fail with clear error message
    FileCopyResult copyFileForSameAfsType(const AfsPath& afsSource, const StreamAttributes& attrSource, //throw FileError, ErrorFileLocked, X
                                          const AbstractPath& apTarget, bool copyFilePermissions, const IoCallback& notifyUnbufferedIO /*throw X*/) const override
    {
        const Zstring nativePathTarget = static_cast<const NativeFileSystem&>(apTarget.afsDevice.ref()).getNativePath(apTarget.afsPath);

        initComForThread(); //throw FileError

        const zen::FileCopyResult nativeResult = copyNewFile(getNativePath(afsSource), nativePathTarget, notifyUnbufferedIO); //throw FileError, ErrorTargetExisting, ErrorFileLocked, X

        //at this point we know we created a new file, so it's fine to delete it for cleanup!
        ZEN_ON_SCOPE_FAIL(try { zen::removeFilePlain(nativePathTarget); }
        catch (FileError&) {});

        if (copyFilePermissions)
            copyItemPermissions(getNativePath(afsSource), nativePathTarget, ProcSymlink::follow); //throw FileError

        FileCopyResult result;
        result.fileSize = nativeResult.fileSize;
        //caveat: modTime will be incorrect for file systems with imprecise file times, e.g. see FAT_FILE_TIME_PRECISION_SEC
        result.modTime = nativeFileTimeToTimeT(nativeResult.sourceModTime);
        result.sourceFilePrint = getFileFingerprint(nativeResult.sourceFileIdx);
        result.targetFilePrint = getFileFingerprint(nativeResult.targetFileIdx);
        result.errorModTime = nativeResult.errorModTime;
        return result;
    }

    //symlink handling: follow
    //already existing: fail
    void copyNewFolderForSameAfsType(const AfsPath& afsSource, const AbstractPath& apTarget, bool copyFilePermissions) const override //throw FileError
    {
        initComForThread(); //throw FileError

        const Zstring& sourcePath = getNativePath(afsSource);
        const Zstring& targetPath = static_cast<const NativeFileSystem&>(apTarget.afsDevice.ref()).getNativePath(apTarget.afsPath);

        zen::createDirectory(targetPath); //throw FileError, ErrorTargetExisting

        ZEN_ON_SCOPE_FAIL(try { removeDirectoryPlain(targetPath); }
        catch (FileError&) {});

        //do NOT copy attributes for volume root paths which return as: FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY
        //https://freefilesync.org/forum/viewtopic.php?t=5550
        if (getParentPath(afsSource)) //=> not a root path
            tryCopyDirectoryAttributes(sourcePath, targetPath); //throw FileError

        if (copyFilePermissions)
            copyItemPermissions(sourcePath, targetPath, ProcSymlink::follow); //throw FileError
    }

    //already existing: fail
    void copySymlinkForSameAfsType(const AfsPath& afsSource, const AbstractPath& apTarget, bool copyFilePermissions) const override //throw FileError
    {
        const Zstring nativePathTarget = static_cast<const NativeFileSystem&>(apTarget.afsDevice.ref()).getNativePath(apTarget.afsPath);

        initComForThread(); //throw FileError
        zen::copySymlink(getNativePath(afsSource), nativePathTarget); //throw FileError

        ZEN_ON_SCOPE_FAIL(try { zen::removeSymlinkPlain(nativePathTarget); /*throw FileError*/ }
        catch (FileError&) {});

        if (copyFilePermissions)
            copyItemPermissions(getNativePath(afsSource), nativePathTarget, ProcSymlink::direct); //throw FileError
    }

    //already existing: undefined behavior! (e.g. fail/overwrite)
    //=> actual behavior: fail with clear error message
    void moveAndRenameItemForSameAfsType(const AfsPath& pathFrom, const AbstractPath& pathTo) const override //throw FileError, ErrorMoveUnsupported
    {
        //perf test: detecting different volumes by path is ~30 times faster than having ::MoveFileEx() fail with ERROR_NOT_SAME_DEVICE (6µs vs 190µs)
        //=> maybe we can even save some actual I/O in some cases?
        if (compareDeviceSameAfsType(pathTo.afsDevice.ref()) != std::weak_ordering::equivalent)
            throw ErrorMoveUnsupported(replaceCpy(replaceCpy(_("Cannot move file %x to %y."),
                                                             L"%x", L'\n' + fmtPath(getDisplayPath(pathFrom))),
                                                  L"%y", L'\n' + fmtPath(AFS::getDisplayPath(pathTo))),
                                       _("Operation not supported between different devices."));
        initComForThread(); //throw FileError
        const Zstring nativePathTarget = static_cast<const NativeFileSystem&>(pathTo.afsDevice.ref()).getNativePath(pathTo.afsPath);
        zen::moveAndRenameItem(getNativePath(pathFrom), nativePathTarget, false /*replaceExisting*/); //throw FileError, ErrorTargetExisting, ErrorMoveUnsupported
    }

    bool supportsPermissions(const AfsPath& afsPath) const override //throw FileError
    {
        initComForThread(); //throw FileError
        return zen::supportsPermissions(getNativePath(afsPath));
    }

    //----------------------------------------------------------------------------------------------------------------
    FileIconHolder getFileIcon(const AfsPath& afsPath, int pixelSize) const override //throw SysError; (optional return value)
    {
        try
        {
            initComForThread(); //throw FileError
        }
        catch (const FileError& e) { throw SysError(e.toString()); }

        return fff::getFileIcon(getNativePath(afsPath), pixelSize); //throw SysError
    }

    ImageHolder getThumbnailImage(const AfsPath& afsPath, int pixelSize) const override //throw SysError; (optional return value)
    {
        try
        {
            initComForThread(); //throw FileError
        }
        catch (const FileError& e) { throw SysError(e.toString()); }

        return fff::getThumbnailImage(getNativePath(afsPath), pixelSize); //throw SysError
    }

    void authenticateAccess(bool allowUserInteraction) const override //throw FileError
    {
    }

    int getAccessTimeout() const override { return 0; } //returns "0" if no timeout in force

    bool hasNativeTransactionalCopy() const override { return false; }
    //----------------------------------------------------------------------------------------------------------------

    int64_t getFreeDiskSpace(const AfsPath& afsPath) const override //throw FileError, returns < 0 if not available
    {
        initComForThread(); //throw FileError
        return zen::getFreeDiskSpace(getNativePath(afsPath)); //throw FileError
    }

    bool supportsRecycleBin(const AfsPath& afsPath) const override //throw FileError
    {
        return true; //truth be told: no idea!!!
    }

    std::unique_ptr<RecycleSession> createRecyclerSession(const AfsPath& afsPath) const override //throw FileError, return value must be bound!
    {
        initComForThread(); //throw FileError
        assert(supportsRecycleBin(afsPath));
        return std::make_unique<RecycleSessionNative>(getNativePath(afsPath));
    }

    void recycleItemIfExists(const AfsPath& afsPath) const override //throw FileError
    {
        initComForThread(); //throw FileError
        zen::recycleOrDeleteIfExists(getNativePath(afsPath)); //throw FileError
    }

    const Zstring rootPath_;
};

//===========================================================================================================================



//- return true if item existed
//- multi-threaded access: internally synchronized!
void RecycleSessionNative::recycleItemIfExists(const AbstractPath& itemPath, const Zstring& logicalRelPath) //throw FileError
{
    assert(!startsWith(logicalRelPath, FILE_NAME_SEPARATOR));

    const Zstring& itemPathNative = getNativeItemPath(itemPath);
    if (itemPathNative.empty())
        throw std::logic_error("Contract violation! " + std::string(__FILE__) + ':' + numberTo<std::string>(__LINE__));

    recycleOrDeleteIfExists(itemPathNative); //throw FileError
}


void RecycleSessionNative::tryCleanup(const std::function<void (const std::wstring& displayPath)>& notifyDeletionStatus /*throw X*/) //throw FileError, X
{
}
}


//coordinate changes with getResolvedFilePath()!
bool fff::acceptsItemPathPhraseNative(const Zstring& itemPathPhrase) //noexcept
{
    Zstring path = expandMacros(itemPathPhrase); //expand before trimming!
    trim(path);


    if (path.empty()) //eat up empty paths before other AFS implementations get a chance!
        return true;

    if (startsWith(path, Zstr('['))) //drive letter by volume name syntax
        return true;

    //don't accept relative paths!!! indistinguishable from MTP paths as shown in Explorer's address bar!
    //don't accept empty paths (see drag & drop validation!)
    return static_cast<bool>(parsePathComponents(path));
}


AbstractPath fff::createItemPathNative(const Zstring& itemPathPhrase) //noexcept
{
    warn_static("reevaluate")
    //TODO: get volume by name hangs for idle HDD! => run createItemPathNative during getFolderStatusNonBlocking() but getResolvedFilePath currently not thread-safe!
    const Zstring& itemPath = getResolvedFilePath(itemPathPhrase);
    return createItemPathNativeNoFormatting(itemPath);
}


AbstractPath fff::createItemPathNativeNoFormatting(const Zstring& nativePath) //noexcept
{
    if (const std::optional<PathComponents> comp = parsePathComponents(nativePath))
        return AbstractPath(makeSharedRef<NativeFileSystem>(comp->rootPath), AfsPath(comp->relPath));
    else //broken path syntax
        return AbstractPath(makeSharedRef<NativeFileSystem>(nativePath), AfsPath());
}


Zstring fff::getNativeItemPath(const AbstractPath& ap)
{
    if (const auto nativeDevice = dynamic_cast<const NativeFileSystem*>(&ap.afsDevice.ref()))
        return nativeDevice->getNativePath(ap.afsPath);
    return {};
}
