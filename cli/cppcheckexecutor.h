/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2020 Cppcheck team.
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

#ifndef CPPCHECKEXECUTOR_H
#define CPPCHECKEXECUTOR_H

#include "errorlogger.h"
#include "settings.h"
#include "analyzerinfo.h"

#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <string>

class CppCheck;
class Library;

/**
 * This class works as an example of how CppCheck can be used in external
 * programs without very little knowledge of the internal parts of the
 * program itself. If you wish to use cppcheck e.g. as a part of IDE,
 * just rewrite this class for your needs and possibly use other methods
 * from CppCheck class instead the ones used here.
 */
class CppCheckExecutor : public ErrorLogger {
public:
    /**
     * Constructor
     */
    CppCheckExecutor();

    /**
     * Destructor
     */
    ~CppCheckExecutor() override;

    /**
     * Starts the checking.
     *
     * @param argc from main()
     * @param argv from main()
     * @return EXIT_FAILURE if arguments are invalid or no input files
     *         were found.
     *         If errors are found and --error-exitcode is used,
     *         given value is returned instead of default 0.
     *         If no errors are found, 0 is returned.
     */
    int check(int argc, const char* const argv[]);

    /**
     * Information about progress is directed here. This should be
     * called by the CppCheck class only.
     *
     * @param outmsg Progress message e.g. "Checking main.cpp..."
     */
    void reportOut(const std::string &outmsg) override;

    /** xml output of errors */
    void reportErr(const ErrorMessage &msg) override;

    void reportProgress(const std::string &filename, const char stage[], const std::size_t value) override;

    /**
     * Output information messages.
     */
    void reportInfo(const ErrorMessage &msg) override;

    /**
     * Information about how many files have been checked
     *
     * @param fileindex This many files have been checked.
     * @param filecount This many files there are in total.
     * @param sizedone The sum of sizes of the files checked.
     * @param sizetotal The total sizes of the files.
     */
    void reportStatus(std::size_t fileindex, std::size_t filecount, std::size_t sizedone, std::size_t sizetotal) override;

    /**
     * @param exception_output Output file
     */
    static void setExceptionOutput(FILE* exceptionOutput);
    /**
    * @return file name to be used for output from exception handler. Has to be either "stdout" or "stderr".
    */
    static FILE* getExceptionOutput();

    /**
    * Tries to load a library and prints warning/error messages
    * @return false, if an error occurred (except unknown XML elements)
    */
    static bool tryLoadLibrary(Library& destination, const char* basepath, const char* filename);

    /**
     * Helper function to supply settings. This can be used for testing.
     * @return Reference to an Settings instance
     */
    Settings& settings() {
        return mSettings;
    }

protected:

    /**
     * Helper function to print out errors. Appends a line change.
     * @param errmsg String printed to error stream
     */
    void reportErr(const std::string &errmsg);

    /**
     * @brief Parse command line args and get settings and file lists
     * from there.
     *
     * @param cppcheck cppcheck instance
     * @param argc argc from main()
     * @param argv argv from main()
     * @return false when errors are found in the input
     */
    bool parseFromArgs(CppCheck *cppcheck, int argc, const char* const argv[]);

private:

    /**
     * Wrapper around check_internal
     *   - installs optional platform dependent signal handling
     *
     * @param cppcheck cppcheck instance
     * @param argc from main()
     * @param argv from main()
     **/
    int check_wrapper(CppCheck& cppcheck, int argc, const char* const argv[]);

    /**
    * Starts the checking.
    *
    * @param cppcheck cppcheck instance
    * @param argc from main()
    * @param argv from main()
    * @return EXIT_FAILURE if arguments are invalid or no input files
    *         were found.
    *         If errors are found and --error-exitcode is used,
    *         given value is returned instead of default 0.
    *         If no errors are found, 0 is returned.
    */
    int check_internal(CppCheck& cppcheck, int argc, const char* const argv[]);

    /**
     * Settings of this instance
     */
    Settings mSettings;

    /**
     * Used to filter out duplicate error messages.
     */
    std::set<std::string> mShownErrors;

    /**
     * Filename associated with size of file
     */
    AnalyzerInformation mAnalyzerInformation;

    /**
     * Report progress time
     */
    std::time_t mLatestProgressOutputTime;

    /**
     * Output file name for exception handler
     */
    static FILE* mExceptionOutput;

    /**
     * Error output
     */
    std::ofstream *mErrorOutput;

    /**
     * Has --errorlist been given?
     */
    bool mShowAllErrors;
};

#endif // CPPCHECKEXECUTOR_H
