// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef ALGORITHM_H_34218518475321452548
#define ALGORITHM_H_34218518475321452548

#include <functional>
#include <span>
#include "structures.h"
#include "file_hierarchy.h"
#include "soft_filter.h"
#include "process_callback.h"


namespace fff
{
void recursiveObjectVisitor(FileSystemObject& fsObj,
                            std::function<void (FolderPair&   folder)> onFolder,
                            std::function<void (FilePair&       file)> onFile,
                            std::function<void (SymlinkPair& symlink)> onSymlink);

void swapGrids(const MainConfiguration& mainCfg, FolderComparison& folderCmp,
               PhaseCallback& callback /*throw X*/); //throw X

std::vector<std::pair<BaseFolderPair*, SyncDirectionConfig>> extractDirectionCfg(FolderComparison& folderCmp, const MainConfiguration& mainCfg);

void redetermineSyncDirection(const std::vector<std::pair<BaseFolderPair*, SyncDirectionConfig>>& directCfgs,
                              PhaseCallback& callback /*throw X*/); //throw X

void setSyncDirectionRec(SyncDirection newDirection, FileSystemObject& fsObj); //set new direction (recursively)

bool allElementsEqual(const FolderComparison& folderCmp);

//filtering
void applyFiltering  (FolderComparison& folderCmp, const MainConfiguration& mainCfg); //full filter apply
void addHardFiltering(BaseFolderPair& baseFolder, const Zstring& excludeFilter);     //exclude additional entries only
void addSoftFiltering(BaseFolderPair& baseFolder, const SoftFilter& timeSizeFilter); //exclude additional entries only

void applyTimeSpanFilter(FolderComparison& folderCmp, time_t timeFrom, time_t timeTo); //overwrite current active/inactive settings

void setActiveStatus(bool newStatus, FolderComparison& folderCmp); //activate or deactivate all rows
void setActiveStatus(bool newStatus, FileSystemObject& fsObj);     //activate or deactivate row: (not recursively anymore)

struct PathDependency
{
    AbstractPath basePathParent;
    AbstractPath basePathChild;
    Zstring relPath; //filled if child path is subfolder of parent path; empty if child path == parent path
};
std::optional<PathDependency> getPathDependency(const AbstractPath& basePathL, const PathFilter& filterL,
                                                const AbstractPath& basePathR, const PathFilter& filterR);

std::pair<std::wstring, int> getSelectedItemsAsString( //returns string with item names and total count of selected(!) items, NOT total files/dirs!
    std::span<const FileSystemObject* const> selectionLeft,   //all pointers need to be bound!
    std::span<const FileSystemObject* const> selectionRight); //

//manual copy to alternate folder:
void copyToAlternateFolder(std::span<const FileSystemObject* const> rowsToCopyOnLeft,  //all pointers need to be bound!
                           std::span<const FileSystemObject* const> rowsToCopyOnRight, //
                           const Zstring& targetFolderPathPhrase,
                           bool keepRelPaths,
                           bool overwriteIfExists,
                           WarningDialogs& warnings,
                           ProcessCallback& callback /*throw X*/); //throw X

//manual deletion of files on main grid
void deleteFromGridAndHD(const std::vector<FileSystemObject*>& rowsToDeleteOnLeft,  //refresh GUI grid after deletion to remove invalid rows
                         const std::vector<FileSystemObject*>& rowsToDeleteOnRight, //all pointers need to be bound!
                         const std::vector<std::pair<BaseFolderPair*, SyncDirectionConfig>>& directCfgs, //attention: rows will be physically deleted!
                         bool useRecycleBin,
                         //global warnings:
                         bool& warnRecyclerMissing,
                         ProcessCallback& callback /*throw X*/); //throw X

struct FileDescriptor
{
    AbstractPath path;
    FileAttributes attr;

    std::weak_ordering operator<=>(const FileDescriptor&) const = default;
};

//get native Win32 paths or create temporary copy for SFTP/MTP, etc.
class TempFileBuffer
{
public:
    TempFileBuffer() {}
    ~TempFileBuffer();

    Zstring getAndCreateFolderPath(); //throw FileError

    Zstring getTempPath(const FileDescriptor& descr) const; //returns empty if not in buffer (item not existing, error during copy)

    //contract: only add files not yet in the buffer!
    void createTempFiles(const std::set<FileDescriptor>& workLoad, ProcessCallback& callback /*throw X*/); //throw X

private:
    TempFileBuffer           (const TempFileBuffer&) = delete;
    TempFileBuffer& operator=(const TempFileBuffer&) = delete;

    void createTempFolderPath(); //throw FileError

    std::map<FileDescriptor, Zstring> tempFilePaths_;
    Zstring tempFolderPath_;
};
}
#endif //ALGORITHM_H_34218518475321452548
