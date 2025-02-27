// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "algorithm.h"
#include <set>
#include <unordered_map>
#include <zen/perf.h>
#include <zen/crc.h>
#include <zen/guid.h>
#include <zen/file_access.h> //needed for TempFileBuffer only
#include <zen/serialize.h>
#include "norm_filter.h"
#include "db_file.h"
#include "cmp_filetime.h"
#include "status_handler_impl.h"
#include "../afs/concrete.h"
#include "../afs/native.h"


using namespace zen;
using namespace fff;


namespace
{
class RecursiveObjectVisitorImpl
{
public:
    RecursiveObjectVisitorImpl(std::function<void (FolderPair&   folder)> onFolder,
                               std::function<void (FilePair&       file)> onFile,
                               std::function<void (SymlinkPair& symlink)> onSymlink) :
        onFolder_(onFolder), onFile_(onFile), onSymlink_(onSymlink) {}

    void execute(FileSystemObject& fsObj)
    {
        visitFSObject(fsObj,
        [&](const FolderPair&   folder) { visit(const_cast<FolderPair& >(folder )); },  //
        [&](const FilePair&       file) { visit(const_cast<FilePair&   >(file   )); },  //physical object is not const anyway
        [&](const SymlinkPair& symlink) { visit(const_cast<SymlinkPair&>(symlink)); }); //
    }

private:
    void visit(FolderPair& folder)
    {
        if (onFolder_)
            onFolder_(folder);

        for (FilePair& file : folder.refSubFiles())
            visit(file);
        for (SymlinkPair& symlink : folder.refSubLinks())
            visit(symlink);
        for (FolderPair& subFolder : folder.refSubFolders())
            visit(subFolder);
    }

    void visit(FilePair& file)
    {
        if (onFile_)
            onFile_(file);
    }

    void visit(SymlinkPair& symlink)
    {
        if (onSymlink_)
            onSymlink_(symlink);
    }

    std::function<void (FolderPair&   folder)> onFolder_;
    std::function<void (FilePair&       file)> onFile_;
    std::function<void (SymlinkPair& symlink)> onSymlink_;
};
}


void fff::recursiveObjectVisitor(FileSystemObject& fsObj,
                                 std::function<void (FolderPair&   folder)> onFolder,
                                 std::function<void (FilePair&       file)> onFile,
                                 std::function<void (SymlinkPair& symlink)> onSymlink)
{
    RecursiveObjectVisitorImpl(onFolder, onFile, onSymlink).execute(fsObj);
}


void fff::swapGrids(const MainConfiguration& mainCfg, FolderComparison& folderCmp,
                    PhaseCallback& callback /*throw X*/) //throw X
{
    std::for_each(begin(folderCmp), end(folderCmp), [](BaseFolderPair& baseFolder) { baseFolder.flip(); });

    redetermineSyncDirection(extractDirectionCfg(folderCmp, mainCfg),
                             callback); //throw FileError
}

//----------------------------------------------------------------------------------------------

namespace
{
class Redetermine
{
public:
    static void execute(const DirectionSet& dirCfgIn, ContainerObject& hierObj) { Redetermine(dirCfgIn).recurse(hierObj); }

private:
    Redetermine(const DirectionSet& dirCfgIn) : dirCfg_(dirCfgIn) {}

    void recurse(ContainerObject& hierObj) const
    {
        for (FilePair& file : hierObj.refSubFiles())
            processFile(file);
        for (SymlinkPair& link : hierObj.refSubLinks())
            processLink(link);
        for (FolderPair& folder : hierObj.refSubFolders())
            processFolder(folder);
    }

    void processFile(FilePair& file) const
    {
        const CompareFileResult cat = file.getCategory();

        //##################### schedule old temporary files for deletion ####################
        if (cat == FILE_LEFT_SIDE_ONLY && endsWith(file.getItemName<SelectSide::left>(), AFS::TEMP_FILE_ENDING))
            return file.setSyncDir(SyncDirection::left);
        else if (cat == FILE_RIGHT_SIDE_ONLY && endsWith(file.getItemName<SelectSide::right>(), AFS::TEMP_FILE_ENDING))
            return file.setSyncDir(SyncDirection::right);
        //####################################################################################

        switch (cat)
        {
            case FILE_LEFT_SIDE_ONLY:
                file.setSyncDir(dirCfg_.exLeftSideOnly);
                break;
            case FILE_RIGHT_SIDE_ONLY:
                file.setSyncDir(dirCfg_.exRightSideOnly);
                break;
            case FILE_RIGHT_NEWER:
                file.setSyncDir(dirCfg_.rightNewer);
                break;
            case FILE_LEFT_NEWER:
                file.setSyncDir(dirCfg_.leftNewer);
                break;
            case FILE_DIFFERENT_CONTENT:
                file.setSyncDir(dirCfg_.different);
                break;
            case FILE_CONFLICT:
            case FILE_DIFFERENT_METADATA: //use setting from "conflict/cannot categorize"
                if (dirCfg_.conflict == SyncDirection::none)
                    file.setSyncDirConflict(file.getCatExtraDescription()); //take over category conflict
                else
                    file.setSyncDir(dirCfg_.conflict);
                break;
            case FILE_EQUAL:
                file.setSyncDir(SyncDirection::none);
                break;
        }
    }

    void processLink(SymlinkPair& symlink) const
    {
        switch (symlink.getLinkCategory())
        {
            case SYMLINK_LEFT_SIDE_ONLY:
                symlink.setSyncDir(dirCfg_.exLeftSideOnly);
                break;
            case SYMLINK_RIGHT_SIDE_ONLY:
                symlink.setSyncDir(dirCfg_.exRightSideOnly);
                break;
            case SYMLINK_LEFT_NEWER:
                symlink.setSyncDir(dirCfg_.leftNewer);
                break;
            case SYMLINK_RIGHT_NEWER:
                symlink.setSyncDir(dirCfg_.rightNewer);
                break;
            case SYMLINK_CONFLICT:
            case SYMLINK_DIFFERENT_METADATA: //use setting from "conflict/cannot categorize"
                if (dirCfg_.conflict == SyncDirection::none)
                    symlink.setSyncDirConflict(symlink.getCatExtraDescription()); //take over category conflict
                else
                    symlink.setSyncDir(dirCfg_.conflict);
                break;
            case SYMLINK_DIFFERENT_CONTENT:
                symlink.setSyncDir(dirCfg_.different);
                break;
            case SYMLINK_EQUAL:
                symlink.setSyncDir(SyncDirection::none);
                break;
        }
    }

    void processFolder(FolderPair& folder) const
    {
        const CompareDirResult cat = folder.getDirCategory();

        //########### schedule abandoned temporary recycle bin directory for deletion  ##########
        if (cat == DIR_LEFT_SIDE_ONLY && endsWith(folder.getItemName<SelectSide::left>(), AFS::TEMP_FILE_ENDING))
            return setSyncDirectionRec(SyncDirection::left, folder); //
        else if (cat == DIR_RIGHT_SIDE_ONLY && endsWith(folder.getItemName<SelectSide::right>(), AFS::TEMP_FILE_ENDING))
            return setSyncDirectionRec(SyncDirection::right, folder); //don't recurse below!
        //#######################################################################################

        switch (cat)
        {
            case DIR_LEFT_SIDE_ONLY:
                folder.setSyncDir(dirCfg_.exLeftSideOnly);
                break;
            case DIR_RIGHT_SIDE_ONLY:
                folder.setSyncDir(dirCfg_.exRightSideOnly);
                break;
            case DIR_EQUAL:
                folder.setSyncDir(SyncDirection::none);
                break;
            case DIR_CONFLICT:
            case DIR_DIFFERENT_METADATA: //use setting from "conflict/cannot categorize"
                if (dirCfg_.conflict == SyncDirection::none)
                    folder.setSyncDirConflict(folder.getCatExtraDescription()); //take over category conflict
                else
                    folder.setSyncDir(dirCfg_.conflict);
                break;
        }

        recurse(folder);
    }

    const DirectionSet dirCfg_;
};

//---------------------------------------------------------------------------------------------------------------

//test if non-equal items exist in scanned data
bool allItemsCategoryEqual(const ContainerObject& hierObj)
{
    return std::all_of(hierObj.refSubFiles().begin(), hierObj.refSubFiles().end(),
    [](const FilePair& file) { return file.getCategory() == FILE_EQUAL; })&&

    std::all_of(hierObj.refSubLinks().begin(), hierObj.refSubLinks().end(),
    [](const SymlinkPair& link) { return link.getLinkCategory() == SYMLINK_EQUAL; })&&

    std::all_of(hierObj.refSubFolders().begin(), hierObj.refSubFolders().end(), [](const FolderPair& folder)
    {
        return folder.getDirCategory() == DIR_EQUAL && allItemsCategoryEqual(folder); //short-circuit behavior!
    });
}
}

bool fff::allElementsEqual(const FolderComparison& folderCmp)
{
    return std::all_of(begin(folderCmp), end(folderCmp), [](const BaseFolderPair& baseFolder) { return allItemsCategoryEqual(baseFolder); });
}

//---------------------------------------------------------------------------------------------------------------

namespace
{
template <SelectSide side> inline
bool matchesDbEntry(const FilePair& file, const InSyncFile* dbFile, const std::vector<unsigned int>& ignoreTimeShiftMinutes)
{
    if (file.isEmpty<side>())
        return !dbFile;
    else if (!dbFile)
        return false;

    const InSyncDescrFile& descrDb = SelectParam<side>::ref(dbFile->left, dbFile->right);

    return //we're not interested in "fileTimeTolerance" here!
        sameFileTime(file.getLastWriteTime<side>(), descrDb.modTime, FAT_FILE_TIME_PRECISION_SEC, ignoreTimeShiftMinutes) &&
        file.getFileSize<side>() == dbFile->fileSize;
    //note: we do *not* consider file ID here, but are only interested in *visual* changes. Consider user moving data to some other medium, this is not a change!
}


//check whether database entry is in sync considering *current* comparison settings
inline
bool stillInSync(const InSyncFile& dbFile, CompareVariant compareVar, int fileTimeTolerance, const std::vector<unsigned int>& ignoreTimeShiftMinutes)
{
    switch (compareVar)
    {
        case CompareVariant::timeSize:
            if (dbFile.cmpVar == CompareVariant::content) return true; //special rule: this is certainly "good enough" for CompareVariant::timeSize!

            //case-sensitive short name match is a database invariant!
            return sameFileTime(dbFile.left.modTime, dbFile.right.modTime, fileTimeTolerance, ignoreTimeShiftMinutes);

        case CompareVariant::content:
            //case-sensitive short name match is a database invariant!
            return dbFile.cmpVar == CompareVariant::content;
        //in contrast to comparison, we don't care about modification time here!

        case CompareVariant::size: //file size/case-sensitive short name always matches on both sides for an "in-sync" database entry
            return true;
    }
    assert(false);
    return false;
}

//--------------------------------------------------------------------

//check whether database entry and current item match: *irrespective* of current comparison settings
template <SelectSide side> inline
bool matchesDbEntry(const SymlinkPair& symlink, const InSyncSymlink* dbSymlink, const std::vector<unsigned int>& ignoreTimeShiftMinutes)
{
    if (symlink.isEmpty<side>())
        return !dbSymlink;
    else if (!dbSymlink)
        return false;

    const InSyncDescrLink& descrDb = SelectParam<side>::ref(dbSymlink->left, dbSymlink->right);

    return sameFileTime(symlink.getLastWriteTime<side>(), descrDb.modTime, FAT_FILE_TIME_PRECISION_SEC, ignoreTimeShiftMinutes);
}


//check whether database entry is in sync considering *current* comparison settings
inline
bool stillInSync(const InSyncSymlink& dbLink, CompareVariant compareVar, int fileTimeTolerance, const std::vector<unsigned int>& ignoreTimeShiftMinutes)
{
    switch (compareVar)
    {
        case CompareVariant::timeSize:
            if (dbLink.cmpVar == CompareVariant::content || dbLink.cmpVar == CompareVariant::size)
                return true; //special rule: this is already "good enough" for CompareVariant::timeSize!

            //case-sensitive short name match is a database invariant!
            return sameFileTime(dbLink.left.modTime, dbLink.right.modTime, fileTimeTolerance, ignoreTimeShiftMinutes);

        case CompareVariant::content:
        case CompareVariant::size: //== categorized by content! see comparison.cpp, ComparisonBuffer::compareBySize()
            //case-sensitive short name match is a database invariant!
            return dbLink.cmpVar == CompareVariant::content || dbLink.cmpVar == CompareVariant::size;
    }
    assert(false);
    return false;
}

//--------------------------------------------------------------------

//check whether database entry and current item match: *irrespective* of current comparison settings
template <SelectSide side> inline
bool matchesDbEntry(const FolderPair& folder, const InSyncFolder* dbFolder)
{
    const bool haveDbEntry = dbFolder && dbFolder->status != InSyncFolder::DIR_STATUS_STRAW_MAN;
    return haveDbEntry == !folder.isEmpty<side>();
}


inline
bool stillInSync(const InSyncFolder& dbFolder)
{
    //case-sensitive short name match is a database invariant!
    //InSyncFolder::DIR_STATUS_STRAW_MAN considered
    return true;
}

//----------------------------------------------------------------------------------------------

class DetectMovedFiles
{
public:
    static void execute(BaseFolderPair& baseFolder, const InSyncFolder& dbFolder) { DetectMovedFiles(baseFolder, dbFolder); }

private:
    DetectMovedFiles(BaseFolderPair& baseFolder, const InSyncFolder& dbFolder) :
        cmpVar_           (baseFolder.getCompVariant()),
        fileTimeTolerance_(baseFolder.getFileTimeTolerance()),
        ignoreTimeShiftMinutes_(baseFolder.getIgnoredTimeShift())
    {
        recurse(baseFolder, &dbFolder, &dbFolder);

        purgeDuplicates<SelectSide::left >(filesL_,  exLeftOnlyById_);
        purgeDuplicates<SelectSide::right>(filesR_, exRightOnlyById_);

        if ((!exLeftOnlyById_ .empty() || !exLeftOnlyByPath_ .empty()) &&
            (!exRightOnlyById_.empty() || !exRightOnlyByPath_.empty()))
            detectMovePairs(dbFolder);
    }

    void recurse(ContainerObject& hierObj, const InSyncFolder* dbFolderL, const InSyncFolder* dbFolderR)
    {
        for (FilePair& file : hierObj.refSubFiles())
        {
            const AFS::FingerPrint filePrintL = file.getFilePrint<SelectSide::left >();
            const AFS::FingerPrint filePrintR = file.getFilePrint<SelectSide::right>();

            if (filePrintL != 0) filesL_.push_back(&file); //collect *all* prints for uniqueness check!
            if (filePrintR != 0) filesR_.push_back(&file); //

            auto getDbEntry = [](const InSyncFolder* dbFolder, const Zstring& fileName) -> const InSyncFile*
            {
                if (dbFolder)
                    if (const auto it = dbFolder->files.find(fileName);
                        it != dbFolder->files.end())
                        return &it->second;
                return nullptr;
            };

            if (const CompareFileResult cat = file.getCategory();
                cat == FILE_LEFT_SIDE_ONLY)
            {
                if (const InSyncFile* dbEntry = getDbEntry(dbFolderL, file.getItemName<SelectSide::left>()))
                    exLeftOnlyByPath_.emplace(dbEntry, &file);
            }
            else if (cat == FILE_RIGHT_SIDE_ONLY)
            {
                if (const InSyncFile* dbEntry = getDbEntry(dbFolderR, file.getItemName<SelectSide::right>()))
                    exRightOnlyByPath_.emplace(dbEntry, &file);
            }
        }

        for (FolderPair& folder : hierObj.refSubFolders())
        {
            auto getDbEntry = [](const InSyncFolder* dbFolder, const Zstring& folderName) -> const InSyncFolder*
            {
                if (dbFolder)
                    if (const auto it = dbFolder->folders.find(folderName);
                        it != dbFolder->folders.end())
                        return &it->second;
                return nullptr;
            };
            const InSyncFolder* dbEntryL = getDbEntry(dbFolderL, folder.getItemName<SelectSide::left>());
            const InSyncFolder* dbEntryR = dbEntryL;
            if (dbFolderL != dbFolderR ||
                getUnicodeNormalForm(folder.getItemName<SelectSide::left >()) !=
                getUnicodeNormalForm(folder.getItemName<SelectSide::right>()))
                dbEntryR = getDbEntry(dbFolderR, folder.getItemName<SelectSide::right>());

            recurse(folder, dbEntryL, dbEntryR);
        }
    }

    template <SelectSide side>
    static void purgeDuplicates(std::vector<FilePair*>& files,
                                std::unordered_map<AFS::FingerPrint, FilePair*>& exOneSideById)
    {
        if (!files.empty())
        {
            std::sort(files.begin(), files.end(), [](const FilePair* lhs, const FilePair* rhs)
            { return lhs->getFilePrint<side>() < rhs->getFilePrint<side>(); });

            AFS::FingerPrint prevPrint = files[0]->getFilePrint<side>();

            for (auto it = files.begin() + 1; it != files.end(); ++it)
                if (const AFS::FingerPrint filePrint = (*it)->getFilePrint<side>();
                    prevPrint != filePrint)
                    prevPrint = filePrint;
                else //duplicate file ID! NTFS hard link/symlink?
                {
                    const auto dupFirst = it - 1;
                    const auto dupLast = std::find_if(it + 1, files.end(), [prevPrint](const FilePair* file)
                    { return file->getFilePrint<side>() != prevPrint; });

                    //remove from model: do *not* store invalid file prints in sync.ffs_db!
                    std::for_each(dupFirst, dupLast, [](FilePair* file) { file->clearFilePrint<side>(); });
                    it = dupLast - 1;
                }

            //collect unique file prints for files existing on one side only:
            constexpr CompareFileResult oneSideOnlyTag = side == SelectSide::left ? FILE_LEFT_SIDE_ONLY : FILE_RIGHT_SIDE_ONLY;

            for (FilePair* file : files)
                if (file->getCategory() == oneSideOnlyTag)
                    if (const AFS::FingerPrint filePrint = file->getFilePrint<side>();
                        filePrint != 0) //skip duplicates marked by clearFilePrint()
                        exOneSideById.emplace(filePrint, file);
        }
    }

    void detectMovePairs(const InSyncFolder& container) const
    {
        for (const auto& [fileName, dbAttrib] : container.files)
            findAndSetMovePair(dbAttrib);

        for (const auto& [folderName, subFolder] : container.folders)
            detectMovePairs(subFolder);
    }

    template <SelectSide side>
    static bool sameSizeAndDate(const FilePair& file, const InSyncFile& dbFile)
    {
        return file.getFileSize<side>() == dbFile.fileSize &&
               file.getLastWriteTime<side>() == SelectParam<side>::ref(dbFile.left, dbFile.right).modTime;
        /* do NOT consider FAT_FILE_TIME_PRECISION_SEC:
            1. if DB contains file metadata collected during folder comparison we can be as precise as we want here
            2. if DB contains file metadata *estimated* directly after file copy:
                - most file systems store file times with sub-second precision...
                - ...except for FAT, but FAT does not have stable file IDs after file copy anyway (see comment below)
            => file time comparison with seconds precision is fine!

        PS: *never* allow a tolerance as container predicate!!
            => no strict weak ordering relation! reason: no transitivity of equivalence!          */
    }

    template <SelectSide side>
    FilePair* getAssocFilePair(const InSyncFile& dbFile) const
    {
        const std::unordered_map<const InSyncFile*, FilePair*>& exOneSideByPath = SelectParam<side>::ref(exLeftOnlyByPath_, exRightOnlyByPath_);
        const std::unordered_map<AFS::FingerPrint,  FilePair*>& exOneSideById   = SelectParam<side>::ref(exLeftOnlyById_,   exRightOnlyById_);

        if (const auto it = exOneSideByPath.find(&dbFile);
            it != exOneSideByPath.end())
            return it->second; //if there is an association by path, don't care if there is also an association by ID,
        //even if the association by path doesn't match time and size while the association by ID does!
        //there doesn't seem to be (any?) value in allowing this!

        if (const AFS::FingerPrint filePrint = SelectParam<side>::ref(dbFile.left, dbFile.right).filePrint;
            filePrint != 0)
            if (const auto it = exOneSideById.find(filePrint);
                it != exOneSideById.end())
                return it->second;

        return nullptr;
    }

    void findAndSetMovePair(const InSyncFile& dbFile) const
    {
        if (stillInSync(dbFile, cmpVar_, fileTimeTolerance_, ignoreTimeShiftMinutes_))
            if (FilePair* fileLeftOnly = getAssocFilePair<SelectSide::left>(dbFile))
                if (sameSizeAndDate<SelectSide::left>(*fileLeftOnly, dbFile))
                    if (FilePair* fileRightOnly = getAssocFilePair<SelectSide::right>(dbFile))
                        if (sameSizeAndDate<SelectSide::right>(*fileRightOnly, dbFile))
                        {
                            assert((!fileLeftOnly ->getMoveRef() &&
                                    !fileRightOnly->getMoveRef()) ||
                                   (fileLeftOnly ->getMoveRef() == fileRightOnly->getId() &&
                                    fileRightOnly->getMoveRef() == fileLeftOnly ->getId()));

                            if (fileLeftOnly ->getMoveRef() == nullptr && //needless check!? file prints are unique in this context!
                                fileRightOnly->getMoveRef() == nullptr)   //
                            {
                                fileLeftOnly ->setMoveRef(fileRightOnly->getId()); //found a pair, mark it!
                                fileRightOnly->setMoveRef(fileLeftOnly ->getId()); //
                            }
                        }
    }

    const CompareVariant cmpVar_;
    const int fileTimeTolerance_;
    const std::vector<unsigned int> ignoreTimeShiftMinutes_;

    std::vector<FilePair*> filesL_; //collection of *all* file items (with non-null filePrint)
    std::vector<FilePair*> filesR_; // => detect duplicate file IDs

    std::unordered_map<AFS::FingerPrint, FilePair*>  exLeftOnlyById_; //MSVC: twice as fast as std::map for 1 million items!
    std::unordered_map<AFS::FingerPrint, FilePair*> exRightOnlyById_;

    std::unordered_map<const InSyncFile*, FilePair*>  exLeftOnlyByPath_; //MSVC: only 4% faster than std::map for 1 million items!
    std::unordered_map<const InSyncFile*, FilePair*> exRightOnlyByPath_;

    /*  Detect Renamed Files:

         X  ->  |_|      Create right
        |_| ->   Y       Delete right

        resolve as: Rename Y to X on right

        Algorithm:
        ----------
        DB-file left  <--- (name, size, date) --->  DB-file right
              |                                          |
              |  (file ID, size, date)                   |  (file ID, size, date)
              |            or                            |            or
              |  (file path, size, date)                 |  (file path, size, date)
             \|/                                        \|/
        file left only                             file right only

       FAT caveat: file IDs are generally not stable when file is either moved or renamed!
         1. Move/rename operations on FAT cannot be detected reliably.
         2. database generally contains wrong file ID on FAT after renaming from .ffs_tmp files => correct file IDs in database only after next sync
         3. even exFAT screws up (but less than FAT) and changes IDs after file move. Did they learn nothing from the past?           */
};

//----------------------------------------------------------------------------------------------

class RedetermineTwoWay
{
public:
    static void execute(BaseFolderPair& baseFolder, const InSyncFolder& dbFolder) { RedetermineTwoWay(baseFolder, dbFolder); }

private:
    RedetermineTwoWay(BaseFolderPair& baseFolder, const InSyncFolder& dbFolder) :
        cmpVar_                (baseFolder.getCompVariant()),
        fileTimeTolerance_     (baseFolder.getFileTimeTolerance()),
        ignoreTimeShiftMinutes_(baseFolder.getIgnoredTimeShift())
    {
        //-> considering filter not relevant:
        //  if stricter filter than last time: all ok;
        //  if less strict filter (if file ex on both sides -> conflict, fine; if file ex. on one side: copy to other side: fine)
        recurse(baseFolder, &dbFolder, &dbFolder);
    }

    void recurse(ContainerObject& hierObj, const InSyncFolder* dbFolderL, const InSyncFolder* dbFolderR) const
    {
        for (FilePair& file : hierObj.refSubFiles())
            processFile(file, dbFolderL, dbFolderR);
        for (SymlinkPair& link : hierObj.refSubLinks())
            processSymlink(link, dbFolderL, dbFolderR);
        for (FolderPair& folder : hierObj.refSubFolders())
            processDir(folder, dbFolderL, dbFolderR);
    }

    void processFile(FilePair& file, const InSyncFolder* dbFolderL, const InSyncFolder* dbFolderR) const
    {
        const CompareFileResult cat = file.getCategory();
        if (cat == FILE_EQUAL)
            return;

        //##################### schedule old temporary files for deletion ####################
        if (cat == FILE_LEFT_SIDE_ONLY && endsWith(file.getItemName<SelectSide::left>(), AFS::TEMP_FILE_ENDING))
            return file.setSyncDir(SyncDirection::left);
        else if (cat == FILE_RIGHT_SIDE_ONLY && endsWith(file.getItemName<SelectSide::right>(), AFS::TEMP_FILE_ENDING))
            return file.setSyncDir(SyncDirection::right);
        //####################################################################################

        //try to find corresponding database entry
        auto getDbEntry = [](const InSyncFolder* dbFolder, const Zstring& fileName) -> const InSyncFile*
        {
            if (dbFolder)
            {
                auto it = dbFolder->files.find(fileName);
                if (it != dbFolder->files.end())
                    return &it->second;
            }
            return nullptr;
        };
        const InSyncFile* dbEntryL = getDbEntry(dbFolderL, file.getItemName<SelectSide::left>());
        const InSyncFile* dbEntryR = dbEntryL;
        if (dbFolderL != dbFolderR || getUnicodeNormalForm(file.getItemName<SelectSide::left>()) != getUnicodeNormalForm(file.getItemName<SelectSide::right>()))
            dbEntryR = getDbEntry(dbFolderR, file.getItemName<SelectSide::right>());

        //evaluation
        const bool changeOnLeft  = !matchesDbEntry<SelectSide::left >(file, dbEntryL, ignoreTimeShiftMinutes_);
        const bool changeOnRight = !matchesDbEntry<SelectSide::right>(file, dbEntryR, ignoreTimeShiftMinutes_);

        if (changeOnLeft != changeOnRight)
        {
            //if database entry not in sync according to current settings! -> set direction based on sync status only!
            if ((dbEntryL && !stillInSync(*dbEntryL, cmpVar_, fileTimeTolerance_, ignoreTimeShiftMinutes_)) ||
                (dbEntryR && !stillInSync(*dbEntryR, cmpVar_, fileTimeTolerance_, ignoreTimeShiftMinutes_)))
                file.setSyncDirConflict(txtDbNotInSync_);
            else
                file.setSyncDir(changeOnLeft ? SyncDirection::right : SyncDirection::left);
        }
        else
        {
            if (changeOnLeft)
                file.setSyncDirConflict(txtBothSidesChanged_);
            else
                file.setSyncDirConflict(txtNoSideChanged_);
        }
    }

    void processSymlink(SymlinkPair& symlink, const InSyncFolder* dbFolderL, const InSyncFolder* dbFolderR) const
    {
        const CompareSymlinkResult cat = symlink.getLinkCategory();
        if (cat == SYMLINK_EQUAL)
            return;

        //try to find corresponding database entry
        auto getDbEntry = [](const InSyncFolder* dbFolder, const Zstring& linkName) -> const InSyncSymlink*
        {
            if (dbFolder)
            {
                auto it = dbFolder->symlinks.find(linkName);
                if (it != dbFolder->symlinks.end())
                    return &it->second;
            }
            return nullptr;
        };
        const InSyncSymlink* dbEntryL = getDbEntry(dbFolderL, symlink.getItemName<SelectSide::left>());
        const InSyncSymlink* dbEntryR = dbEntryL;
        if (dbFolderL != dbFolderR || getUnicodeNormalForm(symlink.getItemName<SelectSide::left>()) != getUnicodeNormalForm(symlink.getItemName<SelectSide::right>()))
            dbEntryR = getDbEntry(dbFolderR, symlink.getItemName<SelectSide::right>());

        //evaluation
        const bool changeOnLeft  = !matchesDbEntry<SelectSide::left >(symlink, dbEntryL, ignoreTimeShiftMinutes_);
        const bool changeOnRight = !matchesDbEntry<SelectSide::right>(symlink, dbEntryR, ignoreTimeShiftMinutes_);

        if (changeOnLeft != changeOnRight)
        {
            //if database entry not in sync according to current settings! -> set direction based on sync status only!
            if ((dbEntryL && !stillInSync(*dbEntryL, cmpVar_, fileTimeTolerance_, ignoreTimeShiftMinutes_)) ||
                (dbEntryR && !stillInSync(*dbEntryR, cmpVar_, fileTimeTolerance_, ignoreTimeShiftMinutes_)))
                symlink.setSyncDirConflict(txtDbNotInSync_);
            else
                symlink.setSyncDir(changeOnLeft ? SyncDirection::right : SyncDirection::left);
        }
        else
        {
            if (changeOnLeft)
                symlink.setSyncDirConflict(txtBothSidesChanged_);
            else
                symlink.setSyncDirConflict(txtNoSideChanged_);
        }
    }

    void processDir(FolderPair& folder, const InSyncFolder* dbFolderL, const InSyncFolder* dbFolderR) const
    {
        const CompareDirResult cat = folder.getDirCategory();

        //########### schedule abandoned temporary recycle bin directory for deletion  ##########
        if (cat == DIR_LEFT_SIDE_ONLY && endsWith(folder.getItemName<SelectSide::left>(), AFS::TEMP_FILE_ENDING))
            return setSyncDirectionRec(SyncDirection::left, folder); //
        else if (cat == DIR_RIGHT_SIDE_ONLY && endsWith(folder.getItemName<SelectSide::right>(), AFS::TEMP_FILE_ENDING))
            return setSyncDirectionRec(SyncDirection::right, folder); //don't recurse below!
        //#######################################################################################

        //try to find corresponding database entry
        auto getDbEntry = [](const InSyncFolder* dbFolder, const Zstring& folderName) -> const InSyncFolder*
        {
            if (dbFolder)
            {
                auto it = dbFolder->folders.find(folderName);
                if (it != dbFolder->folders.end())
                    return &it->second;
            }
            return nullptr;
        };
        const InSyncFolder* dbEntryL = getDbEntry(dbFolderL, folder.getItemName<SelectSide::left>());
        const InSyncFolder* dbEntryR = dbEntryL;
        if (dbFolderL != dbFolderR || getUnicodeNormalForm(folder.getItemName<SelectSide::left>()) != getUnicodeNormalForm(folder.getItemName<SelectSide::right>()))
            dbEntryR = getDbEntry(dbFolderR, folder.getItemName<SelectSide::right>());

        if (cat != DIR_EQUAL)
        {
            //evaluation
            const bool changeOnLeft  = !matchesDbEntry<SelectSide::left >(folder, dbEntryL);
            const bool changeOnRight = !matchesDbEntry<SelectSide::right>(folder, dbEntryR);

            if (changeOnLeft != changeOnRight)
            {
                //if database entry not in sync according to current settings! -> set direction based on sync status only!
                if ((dbEntryL && !stillInSync(*dbEntryL)) ||
                    (dbEntryR && !stillInSync(*dbEntryR)))
                    folder.setSyncDirConflict(txtDbNotInSync_);
                else
                    folder.setSyncDir(changeOnLeft ? SyncDirection::right : SyncDirection::left);
            }
            else
            {
                if (changeOnLeft)
                    folder.setSyncDirConflict(txtBothSidesChanged_);
                else
                    folder.setSyncDirConflict(txtNoSideChanged_);
            }
        }

        recurse(folder, dbEntryL, dbEntryR);
    }

    const Zstringc txtBothSidesChanged_ = utfTo<Zstringc>(_("Both sides have changed since last synchronization."));
    const Zstringc txtNoSideChanged_    = utfTo<Zstringc>(_("Cannot determine sync-direction:") + L" \n" + _("No change since last synchronization."));
    const Zstringc txtDbNotInSync_      = utfTo<Zstringc>(_("Cannot determine sync-direction:") + L" \n" + _("The database entry is not in sync considering current settings."));

    const CompareVariant cmpVar_;
    const int fileTimeTolerance_;
    const std::vector<unsigned int> ignoreTimeShiftMinutes_;
};
}


std::vector<std::pair<BaseFolderPair*, SyncDirectionConfig>> fff::extractDirectionCfg(FolderComparison& folderCmp, const MainConfiguration& mainCfg)
{
    if (folderCmp.empty())
        return {};

    //merge first and additional pairs
    std::vector<LocalPairConfig> allPairs;
    allPairs.push_back(mainCfg.firstPair);
    allPairs.insert(allPairs.end(),
                    mainCfg.additionalPairs.begin(), //add additional pairs
                    mainCfg.additionalPairs.end());

    if (folderCmp.size() != allPairs.size())
        throw std::logic_error("Contract violation! " + std::string(__FILE__) + ':' + numberTo<std::string>(__LINE__));

    std::vector<std::pair<BaseFolderPair*, SyncDirectionConfig>> output;

    for (auto it = folderCmp.begin(); it != folderCmp.end(); ++it)
    {
        BaseFolderPair& baseFolder = **it;
        const LocalPairConfig& lpc = allPairs[it - folderCmp.begin()];

        output.emplace_back(&baseFolder, lpc.localSyncCfg ? lpc.localSyncCfg->directionCfg : mainCfg.syncCfg.directionCfg);
    }
    return output;
}


void fff::redetermineSyncDirection(const std::vector<std::pair<BaseFolderPair*, SyncDirectionConfig>>& directCfgs,
                                   PhaseCallback& callback /*throw X*/) //throw X
{
    if (directCfgs.empty())
        return;

    std::unordered_set<const BaseFolderPair*> allEqualPairs;
    std::unordered_map<const BaseFolderPair*, SharedRef<const InSyncFolder>> lastSyncStates;

    //best effort: always set sync directions (even on DB load error and when user cancels during file loading)
    ZEN_ON_SCOPE_EXIT
    (
        //*INDENT-OFF*
        for (const auto& [baseFolder, dirCfg] : directCfgs)
            if (!allEqualPairs.contains(baseFolder))
            {
                auto it = lastSyncStates.find(baseFolder);
                const InSyncFolder* lastSyncState = it != lastSyncStates.end() ? &it->second.ref() : nullptr;

                //set sync directions
                if (dirCfg.var == SyncVariant::twoWay)
                {
                    if (lastSyncState)
                        RedetermineTwoWay::execute(*baseFolder, *lastSyncState);
                    else //default fallback
                    {
                        std::wstring msg = _("Setting directions for first synchronization: Old files will be overwritten with newer files.");
                        if (directCfgs.size() > 1)
                            msg += L'\n' + AFS::getDisplayPath(baseFolder->getAbstractPath<SelectSide::left >()) + L' ' + getVariantNameWithSymbol(dirCfg.var) + L' ' +
                                          AFS::getDisplayPath(baseFolder->getAbstractPath<SelectSide::right>());

                        try { callback.logInfo(msg); /*throw X*/} catch (...) {};

                        Redetermine::execute(getTwoWayUpdateSet(), *baseFolder);
                    }
                }
                else
                    Redetermine::execute(extractDirections(dirCfg), *baseFolder);

                //detect renamed files
                if (lastSyncState)
                    DetectMovedFiles::execute(*baseFolder, *lastSyncState);
            }
        //*INDENT-ON*
    );

    std::vector<const BaseFolderPair*> baseFoldersForDbLoad;
    for (const auto& [baseFolder, dirCfg] : directCfgs)
        if (dirCfg.var == SyncVariant::twoWay || detectMovedFilesEnabled(dirCfg))
        {
            if (allItemsCategoryEqual(*baseFolder)) //nothing to do: don't even try to open DB files
                allEqualPairs.insert(baseFolder);
            else
                baseFoldersForDbLoad.push_back(baseFolder);
        }

    //(try to) load sync-database files
    lastSyncStates = loadLastSynchronousState(baseFoldersForDbLoad,
                                              callback /*throw X*/); //throw X

    callback.updateStatus(_("Calculating sync directions...")); //throw X
    callback.requestUiUpdate(true /*force*/); //throw X
}

//---------------------------------------------------------------------------------------------------------------

namespace
{
void setSyncDirectionImpl(FilePair& file, SyncDirection newDirection)
{
    if (file.getCategory() != FILE_EQUAL)
        file.setSyncDir(newDirection);
}

void setSyncDirectionImpl(SymlinkPair& symlink, SyncDirection newDirection)
{
    if (symlink.getLinkCategory() != SYMLINK_EQUAL)
        symlink.setSyncDir(newDirection);
}

void setSyncDirectionImpl(FolderPair& folder, SyncDirection newDirection)
{
    if (folder.getDirCategory() != DIR_EQUAL)
        folder.setSyncDir(newDirection);

    for (FilePair& file : folder.refSubFiles())
        setSyncDirectionImpl(file, newDirection);
    for (SymlinkPair& link : folder.refSubLinks())
        setSyncDirectionImpl(link, newDirection);
    for (FolderPair& subFolder : folder.refSubFolders())
        setSyncDirectionImpl(subFolder, newDirection);
}
}


void fff::setSyncDirectionRec(SyncDirection newDirection, FileSystemObject& fsObj)
{
    //process subdirectories also!
    visitFSObject(fsObj,
    [&](const FolderPair&   folder) { setSyncDirectionImpl(const_cast<FolderPair& >(folder ), newDirection); },  //
    [&](const FilePair&       file) { setSyncDirectionImpl(const_cast<FilePair&   >(file   ), newDirection); },  //physical object is not const anyway
    [&](const SymlinkPair& symlink) { setSyncDirectionImpl(const_cast<SymlinkPair&>(symlink), newDirection); }); //

}

//--------------- functions related to filtering ------------------------------------------------------------------------------------

namespace
{
template <bool include>
void inOrExcludeAllRows(ContainerObject& hierObj)
{
    for (FilePair& file : hierObj.refSubFiles())
        file.setActive(include);
    for (SymlinkPair& link : hierObj.refSubLinks())
        link.setActive(include);
    for (FolderPair& folder : hierObj.refSubFolders())
    {
        folder.setActive(include);
        inOrExcludeAllRows<include>(folder); //recurse
    }
}
}


void fff::setActiveStatus(bool newStatus, FolderComparison& folderCmp)
{
    if (newStatus)
        std::for_each(begin(folderCmp), end(folderCmp), [](BaseFolderPair& baseFolder) { inOrExcludeAllRows<true>(baseFolder); }); //include all rows
    else
        std::for_each(begin(folderCmp), end(folderCmp), [](BaseFolderPair& baseFolder) { inOrExcludeAllRows<false>(baseFolder); }); //exclude all rows
}


void fff::setActiveStatus(bool newStatus, FileSystemObject& fsObj)
{
    fsObj.setActive(newStatus);

    //process subdirectories also!
    visitFSObject(fsObj, [&](const FolderPair& folder)
    {
        if (newStatus)
            inOrExcludeAllRows<true>(const_cast<FolderPair&>(folder)); //object is not physically const here anyway
        else
            inOrExcludeAllRows<false>(const_cast<FolderPair&>(folder)); //
    },
    [](const FilePair& file) {}, [](const SymlinkPair& symlink) {});
}

namespace
{
enum FilterStrategy
{
    STRATEGY_SET,
    STRATEGY_AND
    //STRATEGY_OR ->  usage of inOrExcludeAllRows doesn't allow for strategy "or"
};

template <FilterStrategy strategy> struct Eval;

template <>
struct Eval<STRATEGY_SET> //process all elements
{
    template <class T>
    static bool process(const T& obj) { return true; }
};

template <>
struct Eval<STRATEGY_AND>
{
    template <class T>
    static bool process(const T& obj) { return obj.isActive(); }
};


template <FilterStrategy strategy>
class ApplyHardFilter
{
public:
    static void execute(ContainerObject& hierObj, const PathFilter& filterProcIn) { ApplyHardFilter(hierObj, filterProcIn); }

private:
    ApplyHardFilter(ContainerObject& hierObj, const PathFilter& filterProcIn) : filterProc(filterProcIn)  { recurse(hierObj); }

    void recurse(ContainerObject& hierObj) const
    {
        for (FilePair& file : hierObj.refSubFiles())
            processFile(file);
        for (SymlinkPair& link : hierObj.refSubLinks())
            processLink(link);
        for (FolderPair& folder : hierObj.refSubFolders())
            processDir(folder);
    }

    void processFile(FilePair& file) const
    {
        if (Eval<strategy>::process(file))
            file.setActive(filterProc.passFileFilter(file.getRelativePathAny()));
    }

    void processLink(SymlinkPair& symlink) const
    {
        if (Eval<strategy>::process(symlink))
            symlink.setActive(filterProc.passFileFilter(symlink.getRelativePathAny()));
    }

    void processDir(FolderPair& folder) const
    {
        bool childItemMightMatch = true;
        const bool filterPassed = filterProc.passDirFilter(folder.getRelativePathAny(), &childItemMightMatch);

        if (Eval<strategy>::process(folder))
            folder.setActive(filterPassed);

        if (!childItemMightMatch) //use same logic like directory traversing here: evaluate filter in subdirs only if objects could match
        {
            inOrExcludeAllRows<false>(folder); //exclude all files dirs in subfolders => incompatible with STRATEGY_OR!
            return;
        }

        recurse(folder);
    }

    const PathFilter& filterProc;
};


template <FilterStrategy strategy>
class ApplySoftFilter //falsify only! -> can run directly after "hard/base filter"
{
public:
    static void execute(ContainerObject& hierObj, const SoftFilter& timeSizeFilter) { ApplySoftFilter(hierObj, timeSizeFilter); }

private:
    ApplySoftFilter(ContainerObject& hierObj, const SoftFilter& timeSizeFilter) : timeSizeFilter_(timeSizeFilter) { recurse(hierObj); }

    void recurse(fff::ContainerObject& hierObj) const
    {
        for (FilePair& file : hierObj.refSubFiles())
            processFile(file);
        for (SymlinkPair& link : hierObj.refSubLinks())
            processLink(link);
        for (FolderPair& folder : hierObj.refSubFolders())
            processDir(folder);
    }

    void processFile(FilePair& file) const
    {
        if (Eval<strategy>::process(file))
        {
            if (file.isEmpty<SelectSide::left>())
                file.setActive(matchSize<SelectSide::right>(file) &&
                               matchTime<SelectSide::right>(file));
            else if (file.isEmpty<SelectSide::right>())
                file.setActive(matchSize<SelectSide::left>(file) &&
                               matchTime<SelectSide::left>(file));
            else
            {
                //the only case with partially unclear semantics:
                //file and time filters may match or not match on each side, leaving a total of 16 combinations for both sides!
                /*
                               ST S T -       ST := match size and time
                               ---------       S := match size only
                            ST |I|I|I|I|       T := match time only
                            ------------       - := no match
                             S |I|E|?|E|
                            ------------       I := include row
                             T |I|?|E|E|       E := exclude row
                            ------------       ? := unclear
                             - |I|E|E|E|
                            ------------
                */
                //let's set ? := E
                file.setActive((matchSize<SelectSide::right>(file) &&
                                matchTime<SelectSide::right>(file)) ||
                               (matchSize<SelectSide::left>(file) &&
                                matchTime<SelectSide::left>(file)));
            }
        }
    }

    void processLink(SymlinkPair& symlink) const
    {
        if (Eval<strategy>::process(symlink))
        {
            if (symlink.isEmpty<SelectSide::left>())
                symlink.setActive(matchTime<SelectSide::right>(symlink));
            else if (symlink.isEmpty<SelectSide::right>())
                symlink.setActive(matchTime<SelectSide::left>(symlink));
            else
                symlink.setActive(matchTime<SelectSide::right>(symlink) ||
                                  matchTime<SelectSide::left> (symlink));
        }
    }

    void processDir(FolderPair& folder) const
    {
        if (Eval<strategy>::process(folder))
            folder.setActive(timeSizeFilter_.matchFolder()); //if date filter is active we deactivate all folders: effectively gets rid of empty folders!

        recurse(folder);
    }

    template <SelectSide side, class T>
    bool matchTime(const T& obj) const
    {
        return timeSizeFilter_.matchTime(obj.template getLastWriteTime<side>());
    }

    template <SelectSide side, class T>
    bool matchSize(const T& obj) const
    {
        return timeSizeFilter_.matchSize(obj.template getFileSize<side>());
    }

    const SoftFilter timeSizeFilter_;
};
}


void fff::addHardFiltering(BaseFolderPair& baseFolder, const Zstring& excludeFilter)
{
    ApplyHardFilter<STRATEGY_AND>::execute(baseFolder, NameFilter(FilterConfig().includeFilter, excludeFilter));
}


void fff::addSoftFiltering(BaseFolderPair& baseFolder, const SoftFilter& timeSizeFilter)
{
    if (!timeSizeFilter.isNull()) //since we use STRATEGY_AND, we may skip a "null" filter
        ApplySoftFilter<STRATEGY_AND>::execute(baseFolder, timeSizeFilter);
}


void fff::applyFiltering(FolderComparison& folderCmp, const MainConfiguration& mainCfg)
{
    if (folderCmp.empty())
        return;
    else if (folderCmp.size() != mainCfg.additionalPairs.size() + 1)
        throw std::logic_error("Contract violation! " + std::string(__FILE__) + ':' + numberTo<std::string>(__LINE__));

    //merge first and additional pairs
    std::vector<LocalPairConfig> allPairs;
    allPairs.push_back(mainCfg.firstPair);
    allPairs.insert(allPairs.end(),
                    mainCfg.additionalPairs.begin(), //add additional pairs
                    mainCfg.additionalPairs.end());

    for (auto it = allPairs.begin(); it != allPairs.end(); ++it)
    {
        BaseFolderPair& baseFolder = *folderCmp[it - allPairs.begin()];

        const NormalizedFilter normFilter = normalizeFilters(mainCfg.globalFilter, it->localFilter);

        //"set" hard filter
        ApplyHardFilter<STRATEGY_SET>::execute(baseFolder, normFilter.nameFilter.ref());

        //"and" soft filter
        addSoftFiltering(baseFolder, normFilter.timeSizeFilter);
    }
}


class FilterByTimeSpan
{
public:
    static void execute(ContainerObject& hierObj, time_t timeFrom, time_t timeTo) { FilterByTimeSpan(hierObj, timeFrom, timeTo); }

private:
    FilterByTimeSpan(ContainerObject& hierObj,
                     time_t timeFrom,
                     time_t timeTo) :
        timeFrom_(timeFrom),
        timeTo_(timeTo) { recurse(hierObj); }

    void recurse(ContainerObject& hierObj) const
    {
        for (FilePair& file : hierObj.refSubFiles())
            processFile(file);
        for (SymlinkPair& link : hierObj.refSubLinks())
            processLink(link);
        for (FolderPair& folder : hierObj.refSubFolders())
            processDir(folder);
    }

    void processFile(FilePair& file) const
    {
        if (file.isEmpty<SelectSide::left>())
            file.setActive(matchTime<SelectSide::right>(file));
        else if (file.isEmpty<SelectSide::right>())
            file.setActive(matchTime<SelectSide::left>(file));
        else
            file.setActive(matchTime<SelectSide::right>(file) ||
                           matchTime<SelectSide::left>(file));
    }

    void processLink(SymlinkPair& link) const
    {
        if (link.isEmpty<SelectSide::left>())
            link.setActive(matchTime<SelectSide::right>(link));
        else if (link.isEmpty<SelectSide::right>())
            link.setActive(matchTime<SelectSide::left>(link));
        else
            link.setActive(matchTime<SelectSide::right>(link) ||
                           matchTime<SelectSide::left> (link));
    }

    void processDir(FolderPair& folder) const
    {
        folder.setActive(false);
        recurse(folder);
    }

    template <SelectSide side, class T>
    bool matchTime(const T& obj) const
    {
        return timeFrom_ <= obj.template getLastWriteTime<side>() &&
               obj.template getLastWriteTime<side>() <= timeTo_;
    }

    const time_t timeFrom_;
    const time_t timeTo_;
};


void fff::applyTimeSpanFilter(FolderComparison& folderCmp, time_t timeFrom, time_t timeTo)
{
    std::for_each(begin(folderCmp), end(folderCmp), [&](BaseFolderPair& baseFolder) { FilterByTimeSpan::execute(baseFolder, timeFrom, timeTo); });
}


std::optional<PathDependency> fff::getPathDependency(const AbstractPath& basePathL, const PathFilter& filterL,
                                                     const AbstractPath& basePathR, const PathFilter& filterR)
{
    if (!AFS::isNullPath(basePathL) && !AFS::isNullPath(basePathR))
    {
        if (basePathL.afsDevice == basePathR.afsDevice)
        {
            const std::vector<Zstring> relPathL = split(basePathL.afsPath.value, FILE_NAME_SEPARATOR, SplitOnEmpty::skip);
            const std::vector<Zstring> relPathR = split(basePathR.afsPath.value, FILE_NAME_SEPARATOR, SplitOnEmpty::skip);

            const bool leftParent = relPathL.size() <= relPathR.size();

            const auto& relPathP = leftParent ? relPathL : relPathR;
            const auto& relPathC = leftParent ? relPathR : relPathL;

            if (std::equal(relPathP.begin(), relPathP.end(), relPathC.begin(), [](const Zstring& lhs, const Zstring& rhs) { return equalNoCase(lhs, rhs); }))
            {
                Zstring relDirPath;
                std::for_each(relPathC.begin() + relPathP.size(), relPathC.end(), [&](const Zstring& itemName)
                {
                    relDirPath = nativeAppendPaths(relDirPath, itemName);
                });
                const AbstractPath& basePathP = leftParent ? basePathL : basePathR;
                const AbstractPath& basePathC = leftParent ? basePathR : basePathL;

                const PathFilter& filterP = leftParent ? filterL : filterR;
                //if there's a dependency, check if the sub directory is (fully) excluded via filter
                //=> easy to check but still insufficient in general:
                // - one folder may have a *.txt include-filter, the other a *.lng include filter => no dependencies, but "childItemMightMatch = true" below!
                // - user may have manually excluded the conflicting items or changed the filter settings without running a re-compare
                bool childItemMightMatch = true;
                if (relDirPath.empty() || filterP.passDirFilter(relDirPath, &childItemMightMatch) || childItemMightMatch)
                    return PathDependency({basePathP, basePathC, relDirPath});
            }
        }
    }
    return {};
}

//############################################################################################################

std::pair<std::wstring, int> fff::getSelectedItemsAsString(std::span<const FileSystemObject* const> selectionLeft,
                                                           std::span<const FileSystemObject* const> selectionRight)
{
    //don't use wxString! its dumb linear allocation strategy brings perf down to a crawl!
    std::wstring fileList; //
    int totalDelCount = 0;

    for (const FileSystemObject* fsObj : selectionLeft)
        if (!fsObj->isEmpty<SelectSide::left>())
        {
            fileList += AFS::getDisplayPath(fsObj->getAbstractPath<SelectSide::left>()) + L'\n';
            ++totalDelCount;
        }

    for (const FileSystemObject* fsObj : selectionRight)
        if (!fsObj->isEmpty<SelectSide::right>())
        {
            fileList += AFS::getDisplayPath(fsObj->getAbstractPath<SelectSide::right>()) + L'\n';
            ++totalDelCount;
        }

    return {fileList, totalDelCount};
}


namespace
{
template <SelectSide side>
void copyToAlternateFolderFrom(const std::vector<const FileSystemObject*>& rowsToCopy,
                               const AbstractPath& targetFolderPath,
                               bool keepRelPaths,
                               bool overwriteIfExists,
                               ProcessCallback& callback /*throw X*/) //throw X
{
    auto notifyItemCopy = [&](const std::wstring& statusText, const std::wstring& displayPath)
    {
        std::wstring msg = replaceCpy(statusText, L"%x", fmtPath(displayPath));
        callback.logInfo(msg);                 //throw X
        callback.updateStatus(std::move(msg)); //
    };
    const std::wstring txtCreatingFile  (_("Creating file %x"         ));
    const std::wstring txtCreatingFolder(_("Creating folder %x"       ));
    const std::wstring txtCreatingLink  (_("Creating symbolic link %x"));

    auto copyItem = [&](const AbstractPath& targetPath, //throw FileError
                        const std::function<void(const std::function<void()>& deleteTargetItem)>& copyItemPlain) //throw FileError
    {
        //start deleting existing target as required by copyFileTransactional():
        //best amortized performance if "already existing" is the most common case
        std::exception_ptr deletionError;
        auto tryDeleteTargetItem = [&]
        {
            if (overwriteIfExists)
                try { AFS::removeFilePlain(targetPath); /*throw FileError*/ }
                catch (FileError&) { deletionError = std::current_exception(); } //probably "not existing" error, defer evaluation
            //else: copyFileTransactional() => undefined behavior! (e.g. fail/overwrite/auto-rename)
        };

        try
        {
            copyItemPlain(tryDeleteTargetItem); //throw FileError
        }
        catch (FileError&)
        {
            bool alreadyExisting = false;
            try
            {
                AFS::getItemType(targetPath); //throw FileError
                alreadyExisting = true;
            }
            catch (FileError&) {} //=> not yet existing (=> fine, no path issue) or access error:
            //- let's pretend it doesn't happen :> if it does, worst case: the retry fails with (useless) already existing error
            //- itemStillExists()? too expensive, considering that "already existing" is the most common case

            if (alreadyExisting)
            {
                if (deletionError)
                    std::rethrow_exception(deletionError);
                throw;
            }

            //parent folder missing  => create + retry
            //parent folder existing (maybe externally created shortly after copy attempt) => retry
            if (const std::optional<AbstractPath>& targetParentPath = AFS::getParentPath(targetPath))
                AFS::createFolderIfMissingRecursion(*targetParentPath); //throw FileError

            //retry:
            copyItemPlain(nullptr /*deleteTargetItem*/); //throw FileError
        }
    };

    for (const FileSystemObject* fsObj : rowsToCopy)
        tryReportingError([&]
    {
        const Zstring& relPath = keepRelPaths ? fsObj->getRelativePath<side>() : fsObj->getItemName<side>();
        const AbstractPath sourcePath = fsObj->getAbstractPath<side>();
        const AbstractPath targetPath = AFS::appendRelPath(targetFolderPath, relPath);

        visitFSObject(*fsObj, [&](const FolderPair& folder)
        {
            ItemStatReporter statReporter(1, 0, callback);
            notifyItemCopy(txtCreatingFolder, AFS::getDisplayPath(targetPath));

            AFS::createFolderIfMissingRecursion(targetPath); //throw FileError
            statReporter.reportDelta(1, 0);
            //folder might already exist: see creation of intermediate directories below
        },

        [&](const FilePair& file)
        {
            std::wstring statusMsg = replaceCpy(txtCreatingFile, L"%x", fmtPath(AFS::getDisplayPath(targetPath)));
            callback.logInfo(statusMsg); //throw X
            PercentStatReporter statReporter(std::move(statusMsg), file.getFileSize<side>(), callback); //throw X

            const FileAttributes attr = file.getAttributes<side>();
            const AFS::StreamAttributes sourceAttr{attr.modTime, attr.fileSize, attr.filePrint};

            copyItem(targetPath, [&](const std::function<void()>& deleteTargetItem) //throw FileError
            {
                //already existing + !overwriteIfExists: undefined behavior! (e.g. fail/overwrite/auto-rename)
                /*const AFS::FileCopyResult result =*/ AFS::copyFileTransactional(sourcePath, sourceAttr, targetPath, //throw FileError, ErrorFileLocked, X
                                                                                  false /*copyFilePermissions*/, true /*transactionalCopy*/, deleteTargetItem,
                                                                                  [&](int64_t bytesDelta)
                {
                    statReporter.updateStatus(0, bytesDelta); //throw X
                    callback.requestUiUpdate(); //throw X  => not reliably covered by PercentStatReporter::updateStatus()! e.g. during first few seconds: STATUS_PERCENT_DELAY!
                });
                //result.errorModTime? => probably irrelevant (behave like Windows Explorer)
            });
            statReporter.updateStatus(1, 0); //throw X
        },

        [&](const SymlinkPair& symlink)
        {
            ItemStatReporter statReporter(1, 0, callback);
            notifyItemCopy(txtCreatingLink, AFS::getDisplayPath(targetPath));

            copyItem(targetPath, [&](const std::function<void()>& deleteTargetItem) //throw FileError
            {
                deleteTargetItem(); //throw FileError
                AFS::copySymlink(sourcePath, targetPath, false /*copyFilePermissions*/); //throw FileError
            });
            statReporter.reportDelta(1, 0);
        });

        callback.requestUiUpdate(); //throw X
    }, callback); //throw X
}
}


void fff::copyToAlternateFolder(std::span<const FileSystemObject* const> rowsToCopyOnLeft,
                                std::span<const FileSystemObject* const> rowsToCopyOnRight,
                                const Zstring& targetFolderPathPhrase,
                                bool keepRelPaths,
                                bool overwriteIfExists,
                                WarningDialogs& warnings,
                                ProcessCallback& callback /*throw X*/) //throw X
{
    std::vector<const FileSystemObject*> itemSelectionLeft (rowsToCopyOnLeft .begin(), rowsToCopyOnLeft .end());
    std::vector<const FileSystemObject*> itemSelectionRight(rowsToCopyOnRight.begin(), rowsToCopyOnRight.end());
    std::erase_if(itemSelectionLeft,  [](const FileSystemObject* fsObj) { return fsObj->isEmpty<SelectSide::left >(); }); //needed for correct stats!
    std::erase_if(itemSelectionRight, [](const FileSystemObject* fsObj) { return fsObj->isEmpty<SelectSide::right>(); }); //

    const int itemTotal = static_cast<int>(itemSelectionLeft.size() + itemSelectionRight.size());
    int64_t bytesTotal = 0;

    for (const FileSystemObject* fsObj : itemSelectionLeft)
        visitFSObject(*fsObj, [](const FolderPair& folder) {},
    [&](const FilePair& file) { bytesTotal += static_cast<int64_t>(file.getFileSize<SelectSide::left>()); }, [](const SymlinkPair& symlink) {});

    for (const FileSystemObject* fsObj : itemSelectionRight)
        visitFSObject(*fsObj, [](const FolderPair& folder) {},
    [&](const FilePair& file) { bytesTotal += static_cast<int64_t>(file.getFileSize<SelectSide::right>()); }, [](const SymlinkPair& symlink) {});

    callback.initNewPhase(itemTotal, bytesTotal, ProcessPhase::none); //throw X

    //------------------------------------------------------------------------------

    const AbstractPath targetFolderPath = createAbstractPath(targetFolderPathPhrase);

    copyToAlternateFolderFrom<SelectSide::left >(itemSelectionLeft,  targetFolderPath, keepRelPaths, overwriteIfExists, callback);
    copyToAlternateFolderFrom<SelectSide::right>(itemSelectionRight, targetFolderPath, keepRelPaths, overwriteIfExists, callback);
}

//############################################################################################################

namespace
{
template <SelectSide side>
void deleteFromGridAndHDOneSide(std::vector<FileSystemObject*>& rowsToDelete,
                                bool useRecycleBin,
                                PhaseCallback& callback)
{
    auto notifyItemDeletion = [&](const std::wstring& statusText, const std::wstring& displayPath)
    {
        std::wstring msg = replaceCpy(statusText, L"%x", fmtPath(displayPath));
        callback.logInfo(msg);                 //throw X
        callback.updateStatus(std::move(msg)); //
    };

    std::wstring txtRemovingFile;
    std::wstring txtRemovingDirectory;
    std::wstring txtRemovingSymlink;

    if (useRecycleBin)
    {
        txtRemovingFile      = _("Moving file %x to the recycle bin");
        txtRemovingDirectory = _("Moving folder %x to the recycle bin");
        txtRemovingSymlink   = _("Moving symbolic link %x to the recycle bin");
    }
    else
    {
        txtRemovingFile      = _("Deleting file %x");
        txtRemovingDirectory = _("Deleting folder %x");
        txtRemovingSymlink   = _("Deleting symbolic link %x");
    }


    for (FileSystemObject* fsObj : rowsToDelete) //all pointers are required(!) to be bound
        tryReportingError([&]
    {
        ItemStatReporter statReporter(1, 0, callback);

        if (!fsObj->isEmpty<side>()) //element may be implicitly deleted, e.g. if parent folder was deleted first
        {
            visitFSObject(*fsObj,
                          [&](const FolderPair& folder)
            {
                if (useRecycleBin)
                {
                    notifyItemDeletion(txtRemovingDirectory, AFS::getDisplayPath(folder.getAbstractPath<side>())); //throw X

                    AFS::recycleItemIfExists(folder.getAbstractPath<side>()); //throw FileError
                    statReporter.reportDelta(1, 0);
                }
                else
                {
                    auto onBeforeFileDeletion = [&](const std::wstring& displayPath)
                    {
                        notifyItemDeletion(txtRemovingFile, displayPath); //throw X
                        statReporter.reportDelta(1, 0);
                    };
                    auto onBeforeDirDeletion = [&](const std::wstring& displayPath)
                    {
                        notifyItemDeletion(txtRemovingDirectory, displayPath); //throw X
                        statReporter.reportDelta(1, 0);
                    };

                    AFS::removeFolderIfExistsRecursion(folder.getAbstractPath<side>(), onBeforeFileDeletion, onBeforeDirDeletion); //throw FileError
                }
            },

            [&](const FilePair& file)
            {
                notifyItemDeletion(txtRemovingFile, AFS::getDisplayPath(file.getAbstractPath<side>())); //throw X

                if (useRecycleBin)
                    AFS::recycleItemIfExists(file.getAbstractPath<side>()); //throw FileError
                else
                    AFS::removeFileIfExists(file.getAbstractPath<side>()); //throw FileError
                statReporter.reportDelta(1, 0);
            },

            [&](const SymlinkPair& symlink)
            {
                notifyItemDeletion(txtRemovingSymlink, AFS::getDisplayPath(symlink.getAbstractPath<side>())); //throw X

                if (useRecycleBin)
                    AFS::recycleItemIfExists(symlink.getAbstractPath<side>()); //throw FileError
                else
                    AFS::removeSymlinkIfExists(symlink.getAbstractPath<side>()); //throw FileError
                statReporter.reportDelta(1, 0);
            });

            fsObj->removeObject<side>(); //if directory: removes recursively!
        }

        //remain transactional as much as possible => allow for abort only *after* updating file model
        callback.requestUiUpdate(); //throw X
    }, callback); //throw X
}


template <SelectSide side>
void categorize(const std::vector<FileSystemObject*>& rows,
                std::vector<FileSystemObject*>& deletePermanent,
                std::vector<FileSystemObject*>& deleteRecyler,
                bool useRecycleBin,
                std::map<AbstractPath, bool>& recyclerSupported,
                PhaseCallback& callback) //throw X
{
    auto hasRecycler = [&](const AbstractPath& baseFolderPath) -> bool
    {
        auto it = recyclerSupported.find(baseFolderPath); //perf: avoid duplicate checks!
        if (it != recyclerSupported.end())
            return it->second;

        const std::wstring msg = replaceCpy(_("Checking recycle bin availability for folder %x..."), L"%x", fmtPath(AFS::getDisplayPath(baseFolderPath)));

        bool recSupported = false;
        tryReportingError([&]{
            recSupported = AFS::supportsRecycleBin(baseFolderPath); //throw FileError
        }, callback); //throw X

        recyclerSupported.emplace(baseFolderPath, recSupported);
        return recSupported;
    };

    for (FileSystemObject* row : rows)
        if (!row->isEmpty<side>())
        {
            if (useRecycleBin && hasRecycler(row->base().getAbstractPath<side>())) //Windows' ::SHFileOperation() will delete permanently anyway, but we have a superior deletion routine
                deleteRecyler.push_back(row);
            else
                deletePermanent.push_back(row);
        }
}
}


void fff::deleteFromGridAndHD(const std::vector<FileSystemObject*>& rowsToDeleteOnLeft,  //refresh GUI grid after deletion to remove invalid rows
                              const std::vector<FileSystemObject*>& rowsToDeleteOnRight, //all pointers need to be bound!
                              const std::vector<std::pair<BaseFolderPair*, SyncDirectionConfig>>& directCfgs, //attention: rows will be physically deleted!
                              bool useRecycleBin,
                              bool& warnRecyclerMissing,
                              ProcessCallback& callback /*throw X*/) //throw X
{
    if (directCfgs.empty())
        return;

    //build up mapping from base directory to corresponding direction config
    std::unordered_map<const BaseFolderPair*, SyncDirectionConfig> baseFolderCfgs;
    for (const auto& [baseFolder, dirCfg] : directCfgs)
        baseFolderCfgs[baseFolder] = dirCfg;

    std::vector<FileSystemObject*> deleteLeft  = rowsToDeleteOnLeft;
    std::vector<FileSystemObject*> deleteRight = rowsToDeleteOnRight;

    std::erase_if(deleteLeft,  [](const FileSystemObject* fsObj) { return fsObj->isEmpty<SelectSide::left >(); }); //needed?
    std::erase_if(deleteRight, [](const FileSystemObject* fsObj) { return fsObj->isEmpty<SelectSide::right>(); }); //yes, for correct stats:

    const int itemCount = static_cast<int>(deleteLeft.size() + deleteRight.size());
    callback.initNewPhase(itemCount, 0, ProcessPhase::none); //throw X

    //------------------------------------------------------------------------------

    //ensure cleanup: redetermination of sync-directions and removal of invalid rows
    auto updateDirection = [&]
    {
        //update sync direction: we cannot do a full redetermination since the user may already have entered manual changes
        std::vector<FileSystemObject*> rowsToDelete;
        append(rowsToDelete, deleteLeft);
        append(rowsToDelete, deleteRight);
        removeDuplicates(rowsToDelete);

        for (auto it = rowsToDelete.begin(); it != rowsToDelete.end(); ++it)
        {
            FileSystemObject& fsObj = **it; //all pointers are required(!) to be bound

            if (fsObj.isEmpty<SelectSide::left>() != fsObj.isEmpty<SelectSide::right>()) //make sure objects exists on one side only
            {
                auto cfgIter = baseFolderCfgs.find(&fsObj.base());
                assert(cfgIter != baseFolderCfgs.end());
                if (cfgIter != baseFolderCfgs.end())
                {
                    SyncDirection newDir = SyncDirection::none;

                    if (cfgIter->second.var == SyncVariant::twoWay)
                        newDir = fsObj.isEmpty<SelectSide::left>() ? SyncDirection::right : SyncDirection::left;
                    else
                    {
                        const DirectionSet& dirCfg = extractDirections(cfgIter->second);
                        newDir = fsObj.isEmpty<SelectSide::left>() ? dirCfg.exRightSideOnly : dirCfg.exLeftSideOnly;
                    }

                    setSyncDirectionRec(newDir, fsObj); //set new direction (recursively)
                }
            }
        }

        //last step: cleanup empty rows: this one invalidates all pointers!
        for (const auto& [baseFolder, dirCfg] : directCfgs)
            BaseFolderPair::removeEmpty(*baseFolder);
    };
    ZEN_ON_SCOPE_EXIT(updateDirection()); //MSVC: assert is a macro and it doesn't play nice with ZEN_ON_SCOPE_EXIT, surprise... wasn't there something about macros being "evil"?

    //categorize rows into permanent deletion and recycle bin
    std::vector<FileSystemObject*> deletePermanentLeft;
    std::vector<FileSystemObject*> deletePermanentRight;
    std::vector<FileSystemObject*> deleteRecylerLeft;
    std::vector<FileSystemObject*> deleteRecylerRight;

    std::map<AbstractPath, bool> recyclerSupported;
    categorize<SelectSide::left >(deleteLeft,  deletePermanentLeft,  deleteRecylerLeft,  useRecycleBin, recyclerSupported, callback); //throw X
    categorize<SelectSide::right>(deleteRight, deletePermanentRight, deleteRecylerRight, useRecycleBin, recyclerSupported, callback); //

    //windows: check if recycle bin really exists; if not, Windows will silently delete, which is wrong
    if (useRecycleBin &&
    std::any_of(recyclerSupported.begin(), recyclerSupported.end(), [](const auto& item) { return !item.second; }))
    {
        std::wstring msg = _("The recycle bin is not supported by the following folders. Deleted or overwritten files will not be able to be restored:") + L'\n';

        for (const auto& [folderPath, supported] : recyclerSupported)
            if (!supported)
                msg += L'\n' + AFS::getDisplayPath(folderPath);

        callback.reportWarning(msg, warnRecyclerMissing); //throw?
    }

    deleteFromGridAndHDOneSide<SelectSide::left>(deleteRecylerLeft,   true,  callback);
    deleteFromGridAndHDOneSide<SelectSide::left>(deletePermanentLeft, false, callback);

    deleteFromGridAndHDOneSide<SelectSide::right>(deleteRecylerRight,   true,  callback);
    deleteFromGridAndHDOneSide<SelectSide::right>(deletePermanentRight, false, callback);
}

//############################################################################################################

TempFileBuffer::~TempFileBuffer()
{
    if (!tempFolderPath_.empty())
        try
        {
            removeDirectoryPlainRecursion(tempFolderPath_); //throw FileError
        }
        catch (FileError&) { assert(false); }
        warn_static("log, maybe?")
}


void TempFileBuffer::createTempFolderPath() //throw FileError
{
    if (tempFolderPath_.empty())
    {
        //generate random temp folder path e.g. C:\Users\Zenju\AppData\Local\Temp\FFS-068b2e88
        const uint32_t shortGuid = getCrc32(generateGUID()); //no need for full-blown (pseudo-)random numbers for this one-time invocation

        const Zstring& tempPathTmp = appendSeparator(getTempFolderPath()) + //throw FileError
                                     Zstr("FFS-") + printNumber<Zstring>(Zstr("%08x"), static_cast<unsigned int>(shortGuid));

        createDirectoryIfMissingRecursion(tempPathTmp); //throw FileError

        tempFolderPath_ = tempPathTmp;
    }
}


Zstring TempFileBuffer::getAndCreateFolderPath() //throw FileError
{
    createTempFolderPath(); //throw FileError
    return tempFolderPath_;
}


//returns empty if not available (item not existing, error during copy)
Zstring TempFileBuffer::getTempPath(const FileDescriptor& descr) const
{
    auto it = tempFilePaths_.find(descr);
    if (it != tempFilePaths_.end())
        return it->second;
    return Zstring();
}


void TempFileBuffer::createTempFiles(const std::set<FileDescriptor>& workLoad, ProcessCallback& callback /*throw X*/) //throw X
{
    const int itemTotal = static_cast<int>(workLoad.size());
    int64_t bytesTotal = 0;

    for (const FileDescriptor& descr : workLoad)
        bytesTotal += descr.attr.fileSize;

    callback.initNewPhase(itemTotal, bytesTotal, ProcessPhase::none); //throw X
    //------------------------------------------------------------------------------

    const std::wstring errMsg = tryReportingError([&]
    {
        createTempFolderPath(); //throw FileError
    }, callback); //throw X
    if (!errMsg.empty()) return;

    for (const FileDescriptor& descr : workLoad)
    {
        assert(!tempFilePaths_.contains(descr)); //ensure correct stats, NO overwrite-copy => caller-contract!

        MemoryStreamOut<std::string> cookie; //create hash to distinguish different versions and file locations
        writeNumber   (cookie, descr.attr.modTime);
        writeNumber   (cookie, descr.attr.fileSize);
        writeNumber   (cookie, descr.attr.filePrint);
        writeNumber   (cookie, descr.attr.isFollowedSymlink);
        writeContainer(cookie, AFS::getInitPathPhrase(descr.path));

        const uint16_t crc16 = getCrc16(cookie.ref());
        const Zstring descrHash = printNumber<Zstring>(Zstr("%04x"), static_cast<unsigned int>(crc16));

        const Zstring fileName = AFS::getItemName(descr.path);

        auto it = findLast(fileName.begin(), fileName.end(), Zstr('.')); //gracefully handle case of missing "."
        const Zstring tempFileName = Zstring(fileName.begin(), it) + Zstr('~') + descrHash + Zstring(it, fileName.end());

        const Zstring tempFilePath = appendSeparator(tempFolderPath_) + tempFileName;
        const AFS::StreamAttributes sourceAttr{descr.attr.modTime, descr.attr.fileSize, descr.attr.filePrint};

        tryReportingError([&]
        {
            std::wstring statusMsg = replaceCpy(_("Creating file %x"), L"%x", fmtPath(tempFilePath));
            callback.logInfo(statusMsg); //throw X
            PercentStatReporter statReporter(std::move(statusMsg), descr.attr.fileSize, callback); //throw X

            //already existing: undefined behavior! (e.g. fail/overwrite/auto-rename)
            /*const AFS::FileCopyResult result =*/
            AFS::copyFileTransactional(descr.path, sourceAttr, //throw FileError, ErrorFileLocked, X
                                       createItemPathNative(tempFilePath),
                                       false /*copyFilePermissions*/, true /*transactionalCopy*/, nullptr /*onDeleteTargetFile*/,
                                       [&](int64_t bytesDelta)
            {
                statReporter.updateStatus(0, bytesDelta); //throw X
                callback.requestUiUpdate(); //throw X  => not reliably covered by PercentStatReporter::updateStatus()! e.g. during first few seconds: STATUS_PERCENT_DELAY!
            });
            //result.errorModTime? => irrelevant for temp files!
            statReporter.updateStatus(1, 0); //throw X

            tempFilePaths_[descr] = tempFilePath;
        }, callback); //throw X

        callback.requestUiUpdate(); //throw X
    }
}
