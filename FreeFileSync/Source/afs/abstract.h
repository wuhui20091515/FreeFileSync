// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef ABSTRACT_H_873450978453042524534234
#define ABSTRACT_H_873450978453042524534234

#include <functional>
#include <chrono>
#include <zen/file_error.h>
#include <zen/zstring.h>
#include <zen/serialize.h> //InputStream/OutputStream support buffered stream concept
#include <wx+/image_holder.h> //NOT a wxWidgets dependency!


namespace fff
{
struct AfsPath;
bool isValidRelPath(const Zstring& relPath);
AfsPath sanitizeDeviceRelativePath(Zstring relPath);

struct AbstractFileSystem;

//==============================================================================================================
using AfsDevice = zen::SharedRef<const AbstractFileSystem>;

struct AfsPath //= path relative to the file system root folder (no leading/traling separator)
{
    AfsPath() {}
    explicit AfsPath(const Zstring& p) : value(p) { assert(isValidRelPath(value)); }
    Zstring value;

    std::strong_ordering operator<=>(const AfsPath&) const = default;
};

struct AbstractPath //THREAD-SAFETY: like an int!
{
    AbstractPath(const AfsDevice& afsIn, const AfsPath& afsPathIn) : afsDevice(afsIn), afsPath(afsPathIn) {}

    //template <class T1, class T2> -> don't use forwarding constructor: it circumvents AfsPath's explicit constructor!
    //AbstractPath(T1&& afsIn, T2&& afsPathIn) : afsDevice(std::forward<T1>(afsIn)), afsPath(std::forward<T2>(afsPathIn)) {}

    AfsDevice afsDevice; //"const AbstractFileSystem" => all accesses expected to be thread-safe!!!
    AfsPath afsPath; //relative to device root
};
//==============================================================================================================

struct AbstractFileSystem //THREAD-SAFETY: "const" member functions must model thread-safe access!
{
    //=============== convenience =================
    static Zstring getItemName(const AbstractPath& ap) { assert(getParentPath(ap)); return getItemName(ap.afsPath); }
    static Zstring getItemName(const AfsPath& afsPath) { using namespace zen; return afterLast(afsPath.value, FILE_NAME_SEPARATOR, IfNotFoundReturn::all); }

    static bool isNullPath(const AbstractPath& ap) { return isNullDevice(ap.afsDevice) /*&& ap.afsPath.value.empty()*/; }

    static AbstractPath appendRelPath(const AbstractPath& ap, const Zstring& relPath);

    static std::optional<AbstractPath> getParentPath(const AbstractPath& ap);
    static std::optional<AfsPath>      getParentPath(const AfsPath& afsPath);
    //=============================================

    static std::weak_ordering compareDevice(const AbstractFileSystem& lhs, const AbstractFileSystem& rhs);

    static bool isNullDevice(const AfsDevice& afsDevice) { return afsDevice.ref().isNullFileSystem(); }

    static std::wstring getDisplayPath(const AbstractPath& ap) { return ap.afsDevice.ref().getDisplayPath(ap.afsPath); }

    static Zstring getInitPathPhrase(const AbstractPath& ap) { return ap.afsDevice.ref().getInitPathPhrase(ap.afsPath); }

    //----------------------------------------------------------------------------------------------------------------
    static void authenticateAccess(const AfsDevice& afsDevice, bool allowUserInteraction) //throw FileError
    { return afsDevice.ref().authenticateAccess(allowUserInteraction); }

    static int getAccessTimeout(const AbstractPath& ap) { return ap.afsDevice.ref().getAccessTimeout(); } //returns "0" if no timeout in force

    static bool supportPermissionCopy(const AbstractPath& apSource, const AbstractPath& apTarget); //throw FileError

    static bool hasNativeTransactionalCopy(const AbstractPath& ap) { return ap.afsDevice.ref().hasNativeTransactionalCopy(); }
    //----------------------------------------------------------------------------------------------------------------

    using FingerPrint = uint64_t; //AfsDevice-dependent persistent unique ID

    enum class ItemType : unsigned char
    {
        file,
        folder,
        symlink,
    };
    //(hopefully) fast: does not distinguish between error/not existing
    //root path? => do access test
    static ItemType getItemType(const AbstractPath& ap) { return ap.afsDevice.ref().getItemType(ap.afsPath); } //throw FileError

    //assumes: - base path still exists
    //         - all child item path parts must correspond to folder traversal
    //    => we can conclude whether an item is *not* existing anymore by doing a *case-sensitive* name search => potentially SLOW!
    //      root path? => do access test
    static std::optional<ItemType> itemStillExists(const AbstractPath& ap) { return ap.afsDevice.ref().itemStillExists(ap.afsPath); } //throw FileError
    //----------------------------------------------------------------------------------------------------------------

    //already existing: fail
    //does NOT create parent directories recursively if not existing
    static void createFolderPlain(const AbstractPath& ap) { ap.afsDevice.ref().createFolderPlain(ap.afsPath); } //throw FileError

    //creates directories recursively if not existing
    //returns false if folder already exists
    static bool createFolderIfMissingRecursion(const AbstractPath& ap); //throw FileError

    static void removeFolderIfExistsRecursion(const AbstractPath& ap, //throw FileError
                                              const std::function<void (const std::wstring& displayPath)>& onBeforeFileDeletion /*throw X*/, //optional
                                              const std::function<void (const std::wstring& displayPath)>& onBeforeFolderDeletion)           //one call for each object!
    { return ap.afsDevice.ref().removeFolderIfExistsRecursion(ap.afsPath, onBeforeFileDeletion, onBeforeFolderDeletion); }

    static void removeFileIfExists       (const AbstractPath& ap); //
    static void removeSymlinkIfExists    (const AbstractPath& ap); //throw FileError
    static void removeEmptyFolderIfExists(const AbstractPath& ap); //

    static void removeFilePlain   (const AbstractPath& ap) { ap.afsDevice.ref().removeFilePlain   (ap.afsPath); } //
    static void removeSymlinkPlain(const AbstractPath& ap) { ap.afsDevice.ref().removeSymlinkPlain(ap.afsPath); } //throw FileError
    static void removeFolderPlain (const AbstractPath& ap) { ap.afsDevice.ref().removeFolderPlain (ap.afsPath); } //
    //----------------------------------------------------------------------------------------------------------------
    //static void setModTime(const AbstractPath& ap, time_t modTime) { ap.afsDevice.ref().setModTime(ap.afsPath, modTime); } //throw FileError, follows symlinks

    static AbstractPath getSymlinkResolvedPath(const AbstractPath& ap) { return ap.afsDevice.ref().getSymlinkResolvedPath (ap.afsPath); } //throw FileError
    static bool equalSymlinkContent(const AbstractPath& apLhs, const AbstractPath& apRhs); //throw FileError
    //----------------------------------------------------------------------------------------------------------------
    static zen::FileIconHolder getFileIcon      (const AbstractPath& ap, int pixelSize) { return ap.afsDevice.ref().getFileIcon      (ap.afsPath, pixelSize); } //throw SysError; optional return value
    static zen::ImageHolder    getThumbnailImage(const AbstractPath& ap, int pixelSize) { return ap.afsDevice.ref().getThumbnailImage(ap.afsPath, pixelSize); } //throw SysError; optional return value
    //----------------------------------------------------------------------------------------------------------------

    struct StreamAttributes
    {
        time_t modTime; //number of seconds since Jan. 1st 1970 UTC
        uint64_t fileSize;
        FingerPrint filePrint; //optional
    };

    //----------------------------------------------------------------------------------------------------------------
    struct InputStream
    {
        virtual ~InputStream() {}
        virtual size_t read(void* buffer, size_t bytesToRead) = 0; //throw FileError, ErrorFileLocked, X; return "bytesToRead" bytes unless end of stream!
        virtual size_t getBlockSize() const = 0; //non-zero block size is AFS contract! it's implementer's job to always give a reasonable buffer size!

        //only returns attributes if they are already buffered within stream handle and determination would be otherwise expensive (e.g. FTP/SFTP):
        virtual std::optional<StreamAttributes> getAttributesBuffered() = 0; //throw FileError
    };
    //return value always bound:
    static std::unique_ptr<InputStream> getInputStream(const AbstractPath& ap, const zen::IoCallback& notifyUnbufferedIO /*throw X*/) //throw FileError, ErrorFileLocked
    { return ap.afsDevice.ref().getInputStream(ap.afsPath, notifyUnbufferedIO); }


    struct FinalizeResult
    {
        FingerPrint filePrint = 0; //optional
        std::optional<zen::FileError> errorModTime;
    };

    struct OutputStreamImpl
    {
        virtual ~OutputStreamImpl() {}
        virtual void write(const void* buffer, size_t bytesToWrite) = 0; //throw FileError, X
        virtual FinalizeResult finalize() = 0;                           //throw FileError, X
    };

    struct OutputStream //call finalize when done!
    {
        OutputStream(std::unique_ptr<OutputStreamImpl>&& outStream, const AbstractPath& filePath, std::optional<uint64_t> streamSize);
        ~OutputStream();
        void write(const void* buffer, size_t bytesToWrite); //throw FileError, X
        FinalizeResult finalize();                           //throw FileError, X

    private:
        std::unique_ptr<OutputStreamImpl> outStream_; //bound!
        const AbstractPath filePath_;
        bool finalizeSucceeded_ = false;
        const std::optional<uint64_t> bytesExpected_;
        uint64_t bytesWrittenTotal_ = 0;
    };
    //already existing: undefined behavior! (e.g. fail/overwrite/auto-rename)
    static std::unique_ptr<OutputStream> getOutputStream(const AbstractPath& ap, //throw FileError
                                                         std::optional<uint64_t> streamSize,
                                                         std::optional<time_t> modTime,
                                                         const zen::IoCallback& notifyUnbufferedIO /*throw X*/)
    { return std::make_unique<OutputStream>(ap.afsDevice.ref().getOutputStream(ap.afsPath, streamSize, modTime, notifyUnbufferedIO), ap, streamSize); }
    //----------------------------------------------------------------------------------------------------------------

    struct SymlinkInfo
    {
        Zstring itemName;
        time_t modTime;
    };

    struct FileInfo
    {
        Zstring itemName;
        uint64_t fileSize; //unit: bytes!
        time_t modTime; //number of seconds since Jan. 1st 1970 UTC
        FingerPrint filePrint; //optional; persistent + unique (relative to device) or 0!
        bool isFollowedSymlink;
    };

    struct FolderInfo
    {
        Zstring itemName;
        bool isFollowedSymlink;
    };

    struct TraverserCallback
    {
        virtual ~TraverserCallback() {}

        enum class HandleLink
        {
            follow, //follows link, then calls "onFolder()" or "onFile()"
            skip
        };

        enum class HandleError
        {
            retry,
            ignore
        };

        virtual void                               onFile   (const FileInfo&    fi) = 0; //
        virtual HandleLink                         onSymlink(const SymlinkInfo& si) = 0; //throw X
        virtual std::shared_ptr<TraverserCallback> onFolder (const FolderInfo&  fi) = 0; //
        //nullptr: ignore directory, non-nullptr: traverse into, using the (new) callback

        struct ErrorInfo
        {
            std::wstring msg;
            std::chrono::steady_clock::time_point failTime;
            size_t retryNumber = 0;
        };

        virtual HandleError reportDirError (const ErrorInfo& errorInfo)                          = 0; //failed directory traversal -> consider directory data at current level as incomplete!
        virtual HandleError reportItemError(const ErrorInfo& errorInfo, const Zstring& itemName) = 0; //failed to get data for single file/dir/symlink only!
    };

    using TraverserWorkload = std::vector<std::pair<AfsPath, std::shared_ptr<TraverserCallback> /*throw X*/>>;

    //- client needs to handle duplicate file reports! (FilePlusTraverser fallback, retrying to read directory contents, ...)
    static void traverseFolderRecursive(const AfsDevice& afsDevice, const TraverserWorkload& workload /*throw X*/, size_t parallelOps) { afsDevice.ref().traverseFolderRecursive(workload, parallelOps); }

    static void traverseFolderFlat(const AbstractPath& ap, //throw FileError
                                   const std::function<void (const FileInfo&    fi)>& onFile,    //
                                   const std::function<void (const FolderInfo&  fi)>& onFolder,  //optional
                                   const std::function<void (const SymlinkInfo& si)>& onSymlink) //
    { ap.afsDevice.ref().traverseFolderFlat(ap.afsPath, onFile, onFolder, onSymlink); }
    //----------------------------------------------------------------------------------------------------------------

    //already existing: undefined behavior! (e.g. fail/overwrite)
    static void moveAndRenameItem(const AbstractPath& pathFrom, const AbstractPath& pathTo); //throw FileError, ErrorMoveUnsupported

    //Note: it MAY happen that copyFileTransactional() leaves temp files behind, e.g. temporary network drop.
    // => clean them up at an appropriate time (automatically set sync directions to delete them). They have the following ending:
    static inline const Zchar* const TEMP_FILE_ENDING = Zstr(".ffs_tmp"); //don't use Zstring as global constant: avoid static initialization order problem in global namespace!
    // caveat: ending is hard-coded by RealTimeSync

    struct FileCopyResult
    {
        uint64_t fileSize = 0;
        time_t modTime = 0; //number of seconds since Jan. 1st 1970 UTC
        FingerPrint sourceFilePrint = 0; //optional
        FingerPrint targetFilePrint = 0; //
        std::optional<zen::FileError> errorModTime; //failure to set modification time
    };

    //symlink handling: follow
    //already existing + no onDeleteTargetFile: undefined behavior! (e.g. fail/overwrite/auto-rename)
    //returns current attributes at the time of copy
    static FileCopyResult copyFileTransactional(const AbstractPath& apSource, const StreamAttributes& attrSource, //throw FileError, ErrorFileLocked, X
                                                const AbstractPath& apTarget,
                                                bool copyFilePermissions,
                                                bool transactionalCopy,
                                                //if target is existing user *must* implement deletion to avoid undefined behavior
                                                //if transactionalCopy == true, full read access on source had been proven at this point, so it's safe to delete it.
                                                const std::function<void()>& onDeleteTargetFile /*throw X*/,
                                                //accummulated delta != file size! consider ADS, sparse, compressed files
                                                const zen::IoCallback& notifyUnbufferedIO /*throw X*/);

    //already existing: fail
    //symlink handling: follow
    static void copyNewFolder(const AbstractPath& apSource, const AbstractPath& apTarget, bool copyFilePermissions); //throw FileError

    //already existing: fail
    static void copySymlink(const AbstractPath& apSource, const AbstractPath& apTarget, bool copyFilePermissions); //throw FileError

    //----------------------------------------------------------------------------------------------------------------

    static int64_t getFreeDiskSpace(const AbstractPath& ap) { return ap.afsDevice.ref().getFreeDiskSpace(ap.afsPath); } //throw FileError, returns < 0 if not available

    static bool supportsRecycleBin(const AbstractPath& ap) { return ap.afsDevice.ref().supportsRecycleBin(ap.afsPath); } //throw FileError

    struct RecycleSession
    {
        virtual ~RecycleSession() {}
        //- multi-threaded access: internally synchronized!
        virtual void recycleItemIfExists(const AbstractPath& itemPath, const Zstring& logicalRelPath) = 0; //throw FileError

        virtual void tryCleanup(const std::function<void (const std::wstring& displayPath)>& notifyDeletionStatus /*throw X*; displayPath may be empty*/) = 0; //throw FileError, X
    };

    //precondition: supportsRecycleBin() must return true!
    static std::unique_ptr<RecycleSession> createRecyclerSession(const AbstractPath& ap) { return ap.afsDevice.ref().createRecyclerSession(ap.afsPath); } //throw FileError, return value must be bound!

    static void recycleItemIfExists(const AbstractPath& ap) { ap.afsDevice.ref().recycleItemIfExists(ap.afsPath); } //throw FileError

    //================================================================================================================

    //no need to protect access:
    virtual ~AbstractFileSystem() {}


protected:
    //default implementation: folder traversal
    virtual std::optional<ItemType> itemStillExists(const AfsPath& afsPath) const = 0; //throw FileError

    //default implementation: folder traversal
    virtual void removeFolderIfExistsRecursion(const AfsPath& afsPath, //throw FileError
                                               const std::function<void (const std::wstring& displayPath)>& onBeforeFileDeletion,              //optional
                                               const std::function<void (const std::wstring& displayPath)>& onBeforeFolderDeletion) const = 0; //one call for each object!

    void traverseFolderFlat(const AfsPath& afsPath, //throw FileError
                            const std::function<void (const FileInfo&    fi)>& onFile,           //
                            const std::function<void (const FolderInfo&  fi)>& onFolder,         //optional
                            const std::function<void (const SymlinkInfo& si)>& onSymlink) const; //

    //already existing: undefined behavior! (e.g. fail/overwrite/auto-rename)
    FileCopyResult copyFileAsStream(const AfsPath& afsSource, const StreamAttributes& attrSource, //throw FileError, ErrorFileLocked, X
                                    const AbstractPath& apTarget, const zen::IoCallback& notifyUnbufferedIO /*throw X*/) const;

private:
    virtual std::optional<Zstring> getNativeItemPath(const AfsPath& afsPath) const { return {}; };

    virtual Zstring getInitPathPhrase(const AfsPath& afsPath) const = 0;

    virtual std::wstring getDisplayPath(const AfsPath& afsPath) const = 0;

    virtual bool isNullFileSystem() const = 0;

    virtual std::weak_ordering compareDeviceSameAfsType(const AbstractFileSystem& afsRhs) const = 0;

    //----------------------------------------------------------------------------------------------------------------
    virtual ItemType getItemType(const AfsPath& afsPath) const = 0; //throw FileError
    //----------------------------------------------------------------------------------------------------------------

    //already existing: fail
    virtual void createFolderPlain(const AfsPath& afsPath) const = 0; //throw FileError

    //non-recursive folder deletion:
    virtual void removeFilePlain   (const AfsPath& afsPath) const = 0; //throw FileError
    virtual void removeSymlinkPlain(const AfsPath& afsPath) const = 0; //throw FileError
    virtual void removeFolderPlain (const AfsPath& afsPath) const = 0; //throw FileError

    //----------------------------------------------------------------------------------------------------------------
    //virtual void setModTime(const AfsPath& afsPath, time_t modTime) const = 0; //throw FileError, follows symlinks

    virtual AbstractPath getSymlinkResolvedPath(const AfsPath& afsPath) const = 0; //throw FileError
    virtual bool equalSymlinkContentForSameAfsType(const AfsPath& afsLhs, const AbstractPath& apRhs) const = 0; //throw FileError

    //----------------------------------------------------------------------------------------------------------------
    virtual std::unique_ptr<InputStream> getInputStream(const AfsPath& afsPath, const zen::IoCallback& notifyUnbufferedIO /*throw X*/) const = 0; //throw FileError, ErrorFileLocked

    //already existing: undefined behavior! (e.g. fail/overwrite/auto-rename)
    virtual std::unique_ptr<OutputStreamImpl> getOutputStream(const AfsPath& afsPath, //throw FileError
                                                              std::optional<uint64_t> streamSize,
                                                              std::optional<time_t> modTime,
                                                              const zen::IoCallback& notifyUnbufferedIO /*throw X*/) const = 0;
    //----------------------------------------------------------------------------------------------------------------
    virtual void traverseFolderRecursive(const TraverserWorkload& workload /*throw X*/, size_t parallelOps) const = 0;
    //----------------------------------------------------------------------------------------------------------------
    virtual bool supportsPermissions(const AfsPath& afsPath) const = 0; //throw FileError

    //already existing: undefined behavior! (e.g. fail/overwrite)
    virtual void moveAndRenameItemForSameAfsType(const AfsPath& pathFrom, const AbstractPath& pathTo) const = 0; //throw FileError, ErrorMoveUnsupported

    //symlink handling: follow
    //already existing: undefined behavior! (e.g. fail/overwrite/auto-rename)
    virtual FileCopyResult copyFileForSameAfsType(const AfsPath& afsSource, const StreamAttributes& attrSource, //throw FileError, ErrorFileLocked, X
                                                  const AbstractPath& apTarget, bool copyFilePermissions,
                                                  //accummulated delta != file size! consider ADS, sparse, compressed files
                                                  const zen::IoCallback& notifyUnbufferedIO /*throw X*/) const = 0;


    //symlink handling: follow
    //already existing: fail
    virtual void copyNewFolderForSameAfsType(const AfsPath& afsSource, const AbstractPath& apTarget, bool copyFilePermissions) const = 0; //throw FileError

    //already existing: fail
    virtual void copySymlinkForSameAfsType(const AfsPath& afsSource, const AbstractPath& apTarget, bool copyFilePermissions) const = 0; //throw FileError

    //----------------------------------------------------------------------------------------------------------------
    virtual zen::FileIconHolder getFileIcon      (const AfsPath& afsPath, int pixelSize) const = 0; //throw SysError; optional return value
    virtual zen::ImageHolder    getThumbnailImage(const AfsPath& afsPath, int pixelSize) const = 0; //throw SysError; optional return value

    virtual void authenticateAccess(bool allowUserInteraction) const = 0; //throw FileError

    virtual int getAccessTimeout() const = 0; //returns "0" if no timeout in force

    virtual bool hasNativeTransactionalCopy() const = 0;
    //----------------------------------------------------------------------------------------------------------------

    virtual int64_t getFreeDiskSpace(const AfsPath& afsPath) const = 0; //throw FileError, returns < 0 if not available
    virtual bool supportsRecycleBin(const AfsPath& afsPath) const  = 0; //throw FileError
    virtual std::unique_ptr<RecycleSession> createRecyclerSession(const AfsPath& afsPath) const = 0; //throw FileError, return value must be bound!
    virtual void recycleItemIfExists(const AfsPath& afsPath) const = 0; //throw FileError
};


inline std::weak_ordering operator<=>(const AfsDevice& lhs, const AfsDevice& rhs) { return AbstractFileSystem::compareDevice(lhs.ref(), rhs.ref()); }
inline bool               operator== (const AfsDevice& lhs, const AfsDevice& rhs) { return (lhs <=> rhs) == std::weak_ordering::equivalent; }

inline
std::weak_ordering operator<=>(const AbstractPath& lhs, const AbstractPath& rhs)
{
    return std::tie(lhs.afsDevice, lhs.afsPath) <=>
           std::tie(rhs.afsDevice, rhs.afsPath);
}

inline
bool operator==(const AbstractPath& lhs, const AbstractPath& rhs) { return lhs.afsPath == rhs.afsPath && lhs.afsDevice == rhs.afsDevice; }








//------------------------------------ implementation -----------------------------------------
inline
AbstractPath AbstractFileSystem::appendRelPath(const AbstractPath& ap, const Zstring& relPath)
{
    assert(isValidRelPath(relPath));
    return AbstractPath(ap.afsDevice, AfsPath(nativeAppendPaths(ap.afsPath.value, relPath)));
}

//---------------------------------------------------------------------------------------------

inline
AbstractFileSystem::OutputStream::OutputStream(std::unique_ptr<OutputStreamImpl>&& outStream, const AbstractPath& filePath, std::optional<uint64_t> streamSize) :
    outStream_(std::move(outStream)),
    filePath_(filePath),
    bytesExpected_(streamSize) {}


inline
AbstractFileSystem::OutputStream::~OutputStream()
{
    //we delete the file on errors: => file should not have existed prior to creating OutputStream instance!!
    outStream_.reset(); //close file handle *before* remove!

    if (!finalizeSucceeded_) //transactional output stream! => clean up!
        //- needed for Google Drive: e.g. user might cancel during OutputStreamImpl::finalize(), just after file was written transactionally
        //- also for Native: setFileTime() may fail *after* FileOutput::finalize()
        try { AbstractFileSystem::removeFilePlain(filePath_); /*throw FileError*/ }
        catch (zen::FileError&) {}
}


inline
void AbstractFileSystem::OutputStream::write(const void* data, size_t len) //throw FileError, X
{
    outStream_->write(data, len); //throw FileError, X
    bytesWrittenTotal_ += len;
}


inline
AbstractFileSystem::FinalizeResult AbstractFileSystem::OutputStream::finalize() //throw FileError, X
{
    using namespace zen;

    //important check: catches corrupt SFTP download with libssh2!
    if (bytesExpected_ && *bytesExpected_ != bytesWrittenTotal_)
        throw FileError(replaceCpy(_("Cannot write file %x."), L"%x", fmtPath(getDisplayPath(filePath_))), //instead we should report the source file, but don't have it here...
                        replaceCpy(replaceCpy(_("Unexpected size of data stream.\nExpected: %x bytes\nActual: %y bytes"),
                                              L"%x", formatNumber(*bytesExpected_)),
                                   L"%y", formatNumber(bytesWrittenTotal_)));

    const FinalizeResult result = outStream_->finalize(); //throw FileError, X
    finalizeSucceeded_ = true;
    return result;
}

//--------------------------------------------------------------------------

inline
bool AbstractFileSystem::supportPermissionCopy(const AbstractPath& apSource, const AbstractPath& apTarget) //throw FileError
{
    if (typeid(apSource.afsDevice.ref()) != typeid(apTarget.afsDevice.ref()))
        return false;

    return apSource.afsDevice.ref().supportsPermissions(apSource.afsPath) && //throw FileError
           apTarget.afsDevice.ref().supportsPermissions(apTarget.afsPath);
}


inline
bool AbstractFileSystem::equalSymlinkContent(const AbstractPath& apLhs, const AbstractPath& apRhs) //throw FileError
{
    if (typeid(apLhs.afsDevice.ref()) != typeid(apRhs.afsDevice.ref()))
        return false;

    return apLhs.afsDevice.ref().equalSymlinkContentForSameAfsType(apLhs.afsPath, apRhs); //throw FileError
}


inline
void AbstractFileSystem::moveAndRenameItem(const AbstractPath& pathFrom, const AbstractPath& pathTo) //throw FileError, ErrorMoveUnsupported
{
    using namespace zen;

    if (typeid(pathFrom.afsDevice.ref()) != typeid(pathTo.afsDevice.ref()))
        throw ErrorMoveUnsupported(replaceCpy(replaceCpy(_("Cannot move file %x to %y."),
                                                         L"%x", L'\n' + fmtPath(getDisplayPath(pathFrom))),
                                              L"%y", L'\n' + fmtPath(getDisplayPath(pathTo))), _("Operation not supported between different devices."));

    //already existing: undefined behavior! (e.g. fail/overwrite)
    pathFrom.afsDevice.ref().moveAndRenameItemForSameAfsType(pathFrom.afsPath, pathTo); //throw FileError, ErrorMoveUnsupported
}


inline
void AbstractFileSystem::copyNewFolder(const AbstractPath& apSource, const AbstractPath& apTarget, bool copyFilePermissions) //throw FileError
{
    using namespace zen;

    if (typeid(apSource.afsDevice.ref()) != typeid(apTarget.afsDevice.ref()))
    {
        //fall back:
        if (copyFilePermissions)
            throw FileError(replaceCpy(_("Cannot write permissions of %x."), L"%x", fmtPath(getDisplayPath(apTarget))),
                            _("Operation not supported between different devices."));

        //already existing: fail
        createFolderPlain(apTarget); //throw FileError
    }
    else
        apSource.afsDevice.ref().copyNewFolderForSameAfsType(apSource.afsPath, apTarget, copyFilePermissions); //throw FileError
}


//already existing: fail
inline
void AbstractFileSystem::copySymlink(const AbstractPath& apSource, const AbstractPath& apTarget, bool copyFilePermissions) //throw FileError
{
    using namespace zen;

    if (typeid(apSource.afsDevice.ref()) != typeid(apTarget.afsDevice.ref()))
        throw FileError(replaceCpy(replaceCpy(_("Cannot copy symbolic link %x to %y."),
                                              L"%x", L'\n' + fmtPath(getDisplayPath(apSource))),
                                   L"%y", L'\n' + fmtPath(getDisplayPath(apTarget))), _("Operation not supported between different devices."));

    //already existing: fail
    apSource.afsDevice.ref().copySymlinkForSameAfsType(apSource.afsPath, apTarget, copyFilePermissions); //throw FileError
}
}

#endif //ABSTRACT_H_873450978453042524534234
