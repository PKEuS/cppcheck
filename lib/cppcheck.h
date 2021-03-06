/*
 * LCppC - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2020 Cppcheck team.
 * Copyright (C) 2020 LCppC project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

//---------------------------------------------------------------------------
#ifndef cppcheckH
#define cppcheckH
//---------------------------------------------------------------------------

#include "analyzerinfo.h"
#include "check.h"
#include "config.h"
#include "errorlogger.h"
#include "settings.h"

#include <cstddef>
#include <functional>
#include <istream>
#include <vector>
#include <map>
#include <string>

class Tokenizer;

/// @addtogroup Core
/// @{

/**
 * @brief This is the base class which will use other classes to do
 * static code analysis for C and C++ code to find possible
 * errors or places that could be improved.
 * Usage: See check() for more info.
 */
class CPPCHECKLIB CppCheck : ErrorLogger {
public:
    /**
     * @brief Constructor.
     */
    CppCheck(ErrorLogger &errorLogger,
             Settings& settings,
             Project& project,
             bool useGlobalSuppressions);

    /**
     * @brief Destructor.
     */
    ~CppCheck() override;

    /**
     * @brief This starts the actual checking. Note that you must call
     * parseFromArgs() or settings() and addFile() before calling this.
     * @return amount of errors found or 0 if none were found.
     */

    /**
      * @brief Check the file.
      * This function checks one given file for errors.
      * @param ctu CTU to be checked.
      * @return amount of errors found or 0 if none were found.
      * @note You must set settings before calling this function (by calling
      *  settings()).
      */
    unsigned int check(CTU::CTUInfo* ctu);

    /**
      * @brief Check the file.
      * This function checks one "virtual" file. The file is not read from
      * the disk but the content is given in @p content. In errors the @p path
      * is used as a filename.
      * @param ctu CTU to be checked.
      * @param content File content as a string.
      * @return amount of errors found or 0 if none were found.
      * @note You must set settings before calling this function (by calling
      *  settings()).
      */
    unsigned int check(CTU::CTUInfo* ctu, const std::string &content);

    /**
     * @brief Get reference to current settings.
     * @return a reference to current settings
     */
    Settings& settings() {
        return mSettings;
    }

    /**
     * @brief Returns current version number as a string.
     * @return version, e.g. "1.38"
     */
    static const char * version();

    /**
     * @brief Returns extra version info as a string.
     * This is for returning extra version info, like Git commit id, build
     * time/date etc.
     * @return extra version info, e.g. "04d42151" (Git commit id).
     */
    static const char * extraVersion();

    /**
     * @brief Terminate checking. The checking will be terminated as soon as possible.
     */
    void terminate() {
        Settings::terminate();
    }

    /**
     * @brief Call all "getErrorMessages" in all registered Check classes.
     * Also print out XML header and footer.
     */
    void getErrorMessages();

    void tooManyConfigsError(const std::string &file, const std::size_t numberOfConfigurations);
    void purgedConfigurationMessage(const std::string &file, const std::string& configuration);

    /** Analyse whole program, run this after all TUs has been scanned.
     * Return true if an error is reported.
     */
    bool analyseWholeProgram(AnalyzerInformation& analyzerInformation);

private:

    /** Are there "simple" rules */
    bool hasRule(const std::string &tokenlist) const;

    /** @brief There has been an internal error => Report information message */
    void internalError(const std::string &filename, const std::string &msg);

    /**
     * @brief Check a file using stream
     * @param ctu compile time unit
     * @param fileStream stream the file content can be read from
     * @return number of errors found
     */
    unsigned int checkCTU(CTU::CTUInfo* ctu, std::istream& fileStream);

    /**
     * @brief Check raw tokens
     * @param tokenizer tokenizer instance
     */
    void checkRawTokens(const Tokenizer &tokenizer);

    /**
     * @brief Check normal tokens
     * @param tokenizer tokenizer instance
     */
    void checkNormalTokens(const Tokenizer &tokenizer);

    /**
     * @brief Execute rules, if any
     * @param tokenlist token list to use (normal / simple)
     * @param tokenizer tokenizer
     */
    void executeRules(const std::string &tokenlist, const Tokenizer &tokenizer);

    /**
     * @brief Errors and warnings are directed here.
     *
     * @param msg Errors messages are normally in format
     * "[filepath:line number] Message", e.g.
     * "[main.cpp:4] Uninitialized member variable"
     */
    void reportErr(const ErrorMessage &msg) override;

    /**
     * @brief Information about progress is directed here.
     *
     * @param outmsg Message to show, e.g. "Checking main.cpp..."
     */
    void reportOut(const std::string &outmsg) override;

    std::vector<std::string> mErrorList;
    Settings& mSettings;
    Project& mProject;

    void reportProgress(const std::string &filename, const char stage[], const std::size_t value) override;

    ErrorLogger &mErrorLogger;

    CTU::CTUInfo* mCTU;

    /** @brief Current preprocessor configuration */
    std::string mCurrentConfig;

    unsigned int mExitCode;

    bool mSuppressInternalErrorFound;

    bool mUseGlobalSuppressions;

    /** Are there too many configs? */
    bool mTooManyConfigs;

    /**
     * Execute a shell command and read the output from it. Returns true if command terminated successfully.
     */
    static bool executeCommand(const std::string& exe, const std::vector<std::string>& args, const std::string& redirect, std::string* output);
};

/// @}
//---------------------------------------------------------------------------
#endif // cppcheckH
