// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#ifndef ZSTRING_H_73425873425789
#define ZSTRING_H_73425873425789

#include "string_base.h"


    using Zchar = char;
    #define Zstr(x) x
    const Zchar FILE_NAME_SEPARATOR = '/';


//"The reason for all the fuss above" - Loki/SmartPtr
//a high-performance string for interfacing with native OS APIs in multithreaded contexts
using Zstring = zen::Zbase<Zchar>;

//for special UI-contexts: guaranteed exponential growth + ref-counting + COW + no SSO overhead
using Zstringc = zen::Zbase<char>;
//using Zstringw = zen::Zbase<wchar_t>;


/* Caveat: don't expect input/output string sizes to match:
    - different UTF-8 encoding length of upper-case chars
    - different number of upper case chars (e.g. ß => "SS" on macOS)
    - output is Unicode-normalized                                         */
Zstring getUpperCase(const Zstring& str);

//Windows, Linux: precomposed
//macOS: decomposed
Zstring getUnicodeNormalForm(const Zstring& str);
/* "In fact, Unicode declares that there is an equivalence relationship between decomposed and composed sequences,
    and conformant software should not treat canonically equivalent sequences, whether composed or decomposed or something in between, as different."
    https://www.win.tue.nl/~aeb/linux/uc/nfc_vs_nfd.html             */

struct LessUnicodeNormal { bool operator()(const Zstring& lhs, const Zstring& rhs) const { return getUnicodeNormalForm(lhs) < getUnicodeNormalForm(rhs); } };

Zstring replaceCpyAsciiNoCase(const Zstring& str, const Zstring& oldTerm, const Zstring& newTerm);

//------------------------------------------------------------------------------------------

inline bool equalNoCase(const Zstring& lhs, const Zstring& rhs) { return getUpperCase(lhs) == getUpperCase(rhs); }

struct ZstringNoCase //use as STL container key: avoid needless upper-case conversions during std::map<>::find()
{
    ZstringNoCase(const Zstring& str) : upperCase(getUpperCase(str)) {}
    Zstring upperCase;

    std::strong_ordering operator<=>(const ZstringNoCase&) const = default;
};

//------------------------------------------------------------------------------------------

/* Compare *local* file paths:
     Windows: igore case
     Linux:   byte-wise comparison
     macOS:   ignore case + Unicode normalization forms                    */
std::weak_ordering compareNativePath(const Zstring& lhs, const Zstring& rhs);

inline bool equalNativePath(const Zstring& lhs, const Zstring& rhs) { return compareNativePath(lhs, rhs) == std::weak_ordering::equivalent; }

struct LessNativePath { bool operator()(const Zstring& lhs, const Zstring& rhs) const { return std::is_lt(compareNativePath(lhs, rhs)); } };

//------------------------------------------------------------------------------------------
std::weak_ordering compareNatural(const Zstring& lhs, const Zstring& rhs);

struct LessNaturalSort { bool operator()(const Zstring& lhs, const Zstring& rhs) const { return std::is_lt(compareNatural(lhs, rhs)); } };
//------------------------------------------------------------------------------------------



inline
Zstring appendSeparator(Zstring path) //support rvalue references!
{
    if (!zen::endsWith(path, FILE_NAME_SEPARATOR))
        path += FILE_NAME_SEPARATOR;
    return path; //returning a by-value parameter => RVO if possible, r-value otherwise!
}


inline
Zstring appendPaths(const Zstring& basePath, const Zstring& relPath, Zchar pathSep)
{
    using namespace zen;

    assert(!startsWith(relPath, pathSep) && !endsWith(relPath, pathSep));
    if (relPath.empty())
        return basePath;
    if (basePath.empty())
        return relPath;

    if (startsWith(relPath, pathSep))
    {
        if (relPath.size() == 1)
            return basePath;

        if (endsWith(basePath, pathSep))
            return basePath + (relPath.c_str() + 1);
    }
    else if (!endsWith(basePath, pathSep))
    {
        Zstring output = basePath;
        output.reserve(basePath.size() + 1 + relPath.size()); //append all three strings using a single memory allocation
        return std::move(output) + pathSep + relPath;         //
    }

    return basePath + relPath;
}


inline Zstring nativeAppendPaths(const Zstring& basePath, const Zstring& relPath) { return appendPaths(basePath, relPath, FILE_NAME_SEPARATOR); }


inline
Zstring getFileExtension(const Zstring& filePath)
{
    //const Zstring fileName = afterLast(filePath, FILE_NAME_SEPARATOR, IfNotFoundReturn::all);
    //return afterLast(fileName, Zstr('.'), zen::IfNotFoundReturn::none);

    auto it = zen::findLast(filePath.begin(), filePath.end(), FILE_NAME_SEPARATOR);
    if (it == filePath.end())
        it = filePath.begin();
    else
        ++it;

    auto it2 = zen::findLast(it, filePath.end(), Zstr('.'));
    if (it2 != filePath.end())
        ++it2;

    return Zstring(it2, filePath.end());
}


//common unicode characters
const wchar_t EN_DASH = L'\u2013';
const wchar_t EM_DASH = L'\u2014';
    const wchar_t* const SPACED_DASH = L" \u2014 "; //using 'EM DASH'
const wchar_t LTR_MARK = L'\u200E'; //UTF-8: E2 80 8E
const wchar_t* const ELLIPSIS = L"\u2026"; //"..."
const wchar_t MULT_SIGN = L'\u00D7'; //fancy "x"
//const wchar_t NOBREAK_SPACE = L'\u00A0';
const wchar_t ZERO_WIDTH_SPACE = L'\u200B';

const wchar_t RTL_MARK = L'\u200F'; //UTF-8: E2 80 8F https://www.w3.org/International/questions/qa-bidi-unicode-controls
const wchar_t BIDI_DIR_ISOLATE_RTL    = L'\u2067'; //UTF-8: E2 81 A7 => not working on Win 10
const wchar_t BIDI_POP_DIR_ISOLATE    = L'\u2069'; //UTF-8: E2 81 A9 => not working on Win 10
const wchar_t BIDI_DIR_EMBEDDING_RTL  = L'\u202B'; //UTF-8: E2 80 AB => not working on Win 10
const wchar_t BIDI_POP_DIR_FORMATTING = L'\u202C'; //UTF-8: E2 80 AC => not working on Win 10

#endif //ZSTRING_H_73425873425789
