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

#include "cmdlineparser.h"

#include "check.h"
#include "cppcheckexecutor.h"
#include "filelister.h"
#include "path.h"
#include "platform.h"
#include "settings.h"
#include "standards.h"
#include "suppressions.h"
#include "utils.h"
#include "version.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib> // EXIT_FAILURE
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <set>

#ifdef HAVE_RULES
// xml is used for rules
#include <tinyxml2.h>
#endif

static void addFilesToList(const std::string& fileList, std::vector<std::string>& pathNames)
{
    // To keep things initially simple, if the file can't be opened, just be silent and move on.
    std::istream *files;
    std::ifstream infile;
    if (fileList == "-") { // read from stdin
        files = &std::cin;
    } else {
        infile.open(fileList);
        files = &infile;
    }
    if (files && *files) {
        std::string fileName;
        while (std::getline(*files, fileName)) { // next line
            if (!fileName.empty()) {
                pathNames.emplace_back(fileName);
            }
        }
    }
}

static bool addIncludePathsToList(const std::string& fileList, std::vector<std::string>* pathNames)
{
    std::ifstream files(fileList);
    if (files) {
        std::string pathName;
        while (std::getline(files, pathName)) { // next line
            if (!pathName.empty()) {
                pathName = Path::removeQuotationMarks(pathName);
                pathName = Path::fromNativeSeparators(pathName);

                // If path doesn't end with / or \, add it
                if (!endsWith(pathName, '/'))
                    pathName += '/';

                pathNames->emplace_back(pathName);
            }
        }
        return true;
    }
    return false;
}

static bool addPathsToSet(const std::string& fileName, std::set<std::string>* set)
{
    std::vector<std::string> templist;
    if (!addIncludePathsToList(fileName, &templist))
        return false;
    set->insert(std::make_move_iterator(templist.begin()), std::make_move_iterator(templist.end()));
    return true;
}

CmdLineParser::CmdLineParser(Settings* settings, Project* project)
    : mSettings(settings)
    , mProject(project)
    , mShowHelp(false)
    , mShowVersion(false)
    , mShowErrorMessages(false)
    , mExitAfterPrint(false)
{
}

void CmdLineParser::printMessage(const std::string &message)
{
    std::cout << message << std::endl;
}

void CmdLineParser::printMessage(const char* message)
{
    std::cout << message << std::endl;
}

std::string CmdLineParser::parseEnableList(const std::string& str, std::function<bool(CmdLineParser*, const std::string&, bool)> function)
{
    // Enable parameters may be comma separated...
    if (str.find(',') != std::string::npos) {
        std::string::size_type prevPos = 0;
        std::string::size_type pos = 0;
        while ((pos = str.find(',', pos)) != std::string::npos) {
            if (pos == prevPos)
                return std::string("cppcheck: --enable parameter is empty");
            const std::string errmsg(parseEnableList(str.substr(prevPos, pos - prevPos), function));
            if (!errmsg.empty())
                return errmsg;
            ++pos;
            prevPos = pos;
        }
        if (prevPos >= str.length())
            return std::string("cppcheck: --enable parameter is empty");
        return parseEnableList(str.substr(prevPos), function);
    }

    bool enable = str[0] != '-';
    if (str.size() == (enable ? 0U : 1U))
        return std::string("cppcheck: --enable parameter is empty");

    if (!function(this, enable ? str : str.substr(1), enable))
        return std::string("cppcheck: unknown name '" + str + "'");
    return std::string();
}
bool CmdLineParser::parseEnableList_setSeverity(CmdLineParser* instance, const std::string& str, bool enable)
{
    if (str == "all")
        instance->mProject->severity.setEnabledAll(enable);
    else if (str == "warning")
        instance->mProject->severity.setEnabled(Severity::warning, enable);
    else if (str == "style")
        instance->mProject->severity.setEnabled(Severity::style, enable);
    else if (str == "performance")
        instance->mProject->severity.setEnabled(Severity::performance, enable);
    else if (str == "portability")
        instance->mProject->severity.setEnabled(Severity::portability, enable);
    else if (str == "information")
        instance->mProject->severity.setEnabled(Severity::information, enable);
    else
        return false;
    return true;
}
bool CmdLineParser::parseEnableList_setCertainty(CmdLineParser* instance, const std::string& str, bool enable)
{
    if (str == "all")
        instance->mProject->certainty.setEnabledAll(enable);
    else if (str == "safe")
        instance->mProject->certainty.setEnabled(Certainty::safe, enable);
    else if (str == "inconclusive")
        instance->mProject->certainty.setEnabled(Certainty::inconclusive, enable);
    else if (str == "experimental")
        instance->mProject->certainty.setEnabled(Certainty::experimental, enable);
    else
        return false;
    return true;
}
bool CmdLineParser::parseEnableList_setOutput(CmdLineParser* instance, const std::string& str, bool enable)
{
    if (str == "all")
        instance->mSettings->output.setEnabledAll(enable);
    else if (str == "status")
        instance->mSettings->output.setEnabled(Output::status, enable);
    else if (str == "progress")
        instance->mSettings->output.setEnabled(Output::progress, enable);
    else if (str == "verbose")
        instance->mSettings->output.setEnabled(Output::verbose, enable);
    else if (str == "config")
        instance->mSettings->output.setEnabled(Output::config, enable);
    else if (str == "findings")
        instance->mSettings->output.setEnabled(Output::findings, enable);
    else
        return false;
    return true;
}
bool CmdLineParser::parseEnableList_setChecks(CmdLineParser* instance, const std::string& str, bool enable)
{
    if (str == "all")
        instance->mProject->checks.setEnabledAll(enable);
    else
        instance->mProject->checks.setEnabled(str, enable);
    return true;
}

bool CmdLineParser::parseFromArgs(int argc, const char* const argv[])
{
    bool def = false;
    bool maxconfigs = false;

    mSettings->exename = argv[0];

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            // User define
            if (std::strncmp(argv[i], "-D", 2) == 0) {
                std::string define;

                // "-D define"
                if (std::strcmp(argv[i], "-D") == 0) {
                    ++i;
                    if (i >= argc || argv[i][0] == '-') {
                        printMessage("cppcheck: argument to '-D' is missing.");
                        return false;
                    }

                    define = argv[i];
                }
                // "-Ddefine"
                else {
                    define = 2 + argv[i];
                }

                // No "=", append a "=1"
                if (define.find('=') == std::string::npos)
                    define += "=1";

                if (!mProject->userDefines.empty())
                    mProject->userDefines += ";";
                mProject->userDefines += define;

                def = true;
            }

            // -E
            else if (std::strcmp(argv[i], "-E") == 0) {
                mProject->preprocessOnly = true;
            }

            // Include paths
            else if (std::strncmp(argv[i], "-I", 2) == 0) {
                std::string path;

                // "-I path/"
                if (std::strcmp(argv[i], "-I") == 0) {
                    ++i;
                    if (i >= argc || argv[i][0] == '-') {
                        printMessage("cppcheck: argument to '-I' is missing.");
                        return false;
                    }
                    path = argv[i];
                }

                // "-Ipath/"
                else {
                    path = 2 + argv[i];
                }
                path = Path::removeQuotationMarks(path);
                path = Path::fromNativeSeparators(path);

                // If path doesn't end with / or \, add it
                if (!endsWith(path,'/'))
                    path += '/';

                mProject->includePaths.emplace_back(path);
            }

            // User undef
            else if (std::strncmp(argv[i], "-U", 2) == 0) {
                std::string undef;

                // "-U undef"
                if (std::strcmp(argv[i], "-U") == 0) {
                    ++i;
                    if (i >= argc || argv[i][0] == '-') {
                        printMessage("cppcheck: argument to '-U' is missing.");
                        return false;
                    }

                    undef = argv[i];
                }
                // "-Uundef"
                else {
                    undef = 2 + argv[i];
                }

                mProject->userUndefs.insert(undef);
            }

            else if (std::strncmp(argv[i], "--addon=", 8) == 0)
                mProject->addons.emplace_back(argv[i]+8);

            else if (std::strncmp(argv[i],"--addon-python=", 15) == 0)
                mSettings->addonPython.assign(argv[i]+15);

            // Check configuration
            else if (std::strcmp(argv[i], "--check-config") == 0)
                mSettings->checkConfiguration = true;

            // Check library definitions
            else if (std::strcmp(argv[i], "--check-library") == 0)
                mSettings->checkLibrary = true;

            else if (std::strncmp(argv[i], "--config-exclude=",17) ==0) {
                mProject->configExcludePaths.insert(Path::fromNativeSeparators(argv[i] + 17));
            }

            else if (std::strncmp(argv[i], "--config-excludes-file=", 23) == 0) {
                // open this file and read every input file (1 file name per line)
                const std::string cfgExcludesFile(23 + argv[i]);
                if (!addPathsToSet(cfgExcludesFile, &mProject->configExcludePaths)) {
                    printMessage(PROGRAMNAME ": unable to open config excludes file at '" + cfgExcludesFile + "'");
                    return false;
                }
            }

            else if (std::strncmp(argv[i], "--cppcheck-build-dir=", 21) == 0) {
                mProject->buildDir = Path::fromNativeSeparators(argv[i] + 21);
                if (endsWith(mProject->buildDir, '/'))
                    mProject->buildDir.pop_back();
            }

            // Show --debug output after the first simplifications
            else if (std::strcmp(argv[i], "--debug") == 0 ||
                     std::strcmp(argv[i], "--debug-normal") == 0)
                mSettings->debugnormal = true;

            // Show template information
            else if (std::strcmp(argv[i], "--debug-template") == 0)
                mSettings->debugtemplate = true;

            // Show debug warnings
            else if (std::strcmp(argv[i], "--debug-warnings") == 0)
                mSettings->debugwarnings = true;

            // documentation..
            else if (std::strcmp(argv[i], "--doc") == 0) {
                std::ostringstream doc;
                // Get documentation..
                for (const Check * it : Check::instances()) {
                    const std::string& name(it->name());
                    const std::string info(it->classInfo());
                    if (!name.empty() && !info.empty())
                        doc << "## " << name << " ##\n"
                            << info << "\n";
                }

                std::cout << doc.str();
                mExitAfterPrint = true;
                return true;
            }

            // dump cppcheck data
            else if (std::strcmp(argv[i], "--dump") == 0)
                mSettings->dump = true;

            else if (std::strncmp(argv[i], "--severity=", 11) == 0) {
                const std::string errmsg = parseEnableList(argv[i] + 11, parseEnableList_setSeverity);
                if (!errmsg.empty()) {
                    printMessage(errmsg);
                    return false;
                }
            }

            else if (std::strncmp(argv[i], "-s=", 3) == 0) {
                const std::string errmsg = parseEnableList(argv[i] + 3, parseEnableList_setSeverity);
                if (!errmsg.empty()) {
                    printMessage(errmsg);
                    return false;
                }
            }

            else if (std::strncmp(argv[i], "--certainty=", 12) == 0) {
                const std::string errmsg = parseEnableList(argv[i] + 12, parseEnableList_setCertainty);
                if (!errmsg.empty()) {
                    printMessage(errmsg);
                    return false;
                }
            }

            else if (std::strncmp(argv[i], "-c=", 3) == 0) {
                const std::string errmsg = parseEnableList(argv[i] + 3, parseEnableList_setCertainty);
                if (!errmsg.empty()) {
                    printMessage(errmsg);
                    return false;
                }
            }

            else if (std::strncmp(argv[i], "--checks=", 9) == 0) {
                const std::string errmsg = parseEnableList(argv[i] + 9, parseEnableList_setChecks);
                if (!errmsg.empty()) {
                    printMessage(errmsg);
                    return false;
                }
            }

            else if (std::strncmp(argv[i], "--output=", 9) == 0) {
                const std::string errmsg = parseEnableList(argv[i] + 9, parseEnableList_setOutput);
                if (!errmsg.empty()) {
                    printMessage(errmsg);
                    return false;
                }
            }

            else if (std::strncmp(argv[i], "-o=", 3) == 0) {
                const std::string errmsg = parseEnableList(argv[i] + 3, parseEnableList_setOutput);
                if (!errmsg.empty()) {
                    printMessage(errmsg);
                    return false;
                }
            }

            // print all possible error messages..
            else if (std::strcmp(argv[i], "--errorlist") == 0) {
                mShowErrorMessages = true;
                mSettings->xml = true;
                mExitAfterPrint = true;
            }

            // --error-exitcode=1
            else if (std::strncmp(argv[i], "--error-exitcode=", 17) == 0) {
                const std::string temp = argv[i]+17;
                std::istringstream iss(temp);
                if (!(iss >> mSettings->exitCode)) {
                    mSettings->exitCode = 0;
                    printMessage("cppcheck: Argument must be an integer. Try something like '--error-exitcode=1'.");
                    return false;
                }
            }

            // Exception handling inside cppcheck client
            else if (std::strcmp(argv[i], "--exception-handling") == 0)
                mSettings->exceptionHandling = true;

            else if (std::strncmp(argv[i], "--exception-handling=", 21) == 0) {
                mSettings->exceptionHandling = true;
                const std::string exceptionOutfilename = &(argv[i][21]);
                CppCheckExecutor::setExceptionOutput((exceptionOutfilename=="stderr") ? stderr : stdout);
            }

            // Filter errors
            else if (std::strncmp(argv[i], "--exitcode-suppressions=", 24) == 0) {
                // exitcode-suppressions=filename.txt
                std::string filename = 24 + argv[i];

                std::ifstream f(filename);
                if (!f.is_open()) {
                    printMessage("cppcheck: Couldn't open the file: \"" + filename + "\".");
                    return false;
                }
                const std::string errmsg(mProject->nofail.parseFile(f));
                if (!errmsg.empty()) {
                    printMessage(errmsg);
                    return false;
                }
            }

            // use a file filter
            else if (std::strncmp(argv[i], "--file-filter=", 14) == 0)
                mProject->fileFilter = argv[i] + 14;

            // file list specified
            else if (std::strncmp(argv[i], "--file-list=", 12) == 0)
                // open this file and read every input file (1 file name per line)
                addFilesToList(12 + argv[i], mPathNames);

            // Force checking of files that have "too many" configurations
            else if (std::strcmp(argv[i], "-f") == 0 || std::strcmp(argv[i], "--force") == 0)
                mProject->force = true;

            // Print help
            else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
                mPathNames.clear();
                mShowHelp = true;
                mExitAfterPrint = true;
                break;
            }

            // Ignored paths
            else if (std::strncmp(argv[i], "-i", 2) == 0) {
                std::string path;

                // "-i path/"
                if (std::strcmp(argv[i], "-i") == 0) {
                    ++i;
                    if (i >= argc || argv[i][0] == '-') {
                        printMessage("cppcheck: argument to '-i' is missing.");
                        return false;
                    }
                    path = argv[i];
                }

                // "-ipath/"
                else {
                    path = 2 + argv[i];
                }

                if (!path.empty()) {
                    path = Path::removeQuotationMarks(path);
                    path = Path::fromNativeSeparators(path);
                    path = Path::simplifyPath(path);

                    if (FileLister::isDirectory(path)) {
                        // If directory name doesn't end with / or \, add it
                        if (!endsWith(path, '/'))
                            path += '/';
                    }
                    mIgnoredPaths.emplace_back(path);
                }
            }

            else if (std::strncmp(argv[i], "--include=", 10) == 0) {
                mProject->userIncludes.emplace_back(Path::fromNativeSeparators(argv[i] + 10));
            }

            else if (std::strncmp(argv[i], "--includes-file=", 16) == 0) {
                // open this file and read every input file (1 file name per line)
                const std::string includesFile(16 + argv[i]);
                if (!addIncludePathsToList(includesFile, &mProject->includePaths)) {
                    printMessage(PROGRAMNAME ": unable to open includes file at '" + includesFile + "'");
                    return false;
                }
            }

            // Enables inline suppressions.
            else if (std::strcmp(argv[i], "--inline-suppr") == 0)
                mProject->inlineSuppressions = true;

            // Checking threads
            else if (std::strncmp(argv[i], "-j", 2) == 0) {
                // "-j 3"
                if (std::strcmp(argv[i], "-j") == 0) {
                    mSettings->jobs = 0;

                    if (i <= argc && argv[i + 1][0] != '-') {
                        std::istringstream iss(argv[i + 1]);
                        if (iss >> mSettings->jobs)
                            ++i;
                    }
                }

                // "-j3"
                else {
                    std::istringstream iss(argv[i] + 2);
                    if (!(iss >> mSettings->jobs)) {
                        printMessage("cppcheck: argument to '-j' is not a number.");
                        return false;
                    }
                }

                if (mSettings->jobs > 10000) {
                    // This limit is here just to catch typos. If someone has
                    // need for more jobs, this value should be increased.
                    printMessage("cppcheck: argument for '-j' is allowed to be 10000 at max.");
                    return false;
                }
            }

            else if (std::strncmp(argv[i], "-l", 2) == 0) {
                printMessage("cppcheck: option -l has been removed.");
            }

            // Enforce language (--language=, -x)
            else if (std::strncmp(argv[i], "--language=", 11) == 0 || std::strcmp(argv[i], "-x") == 0) {
                std::string str;
                if (argv[i][2]) {
                    str = argv[i]+11;
                } else {
                    i++;
                    if (i >= argc || argv[i][0] == '-') {
                        printMessage("cppcheck: No language given to '-x' option.");
                        return false;
                    }
                    str = argv[i];
                }

                if (str == "c")
                    mProject->enforcedLang = Project::C;
                else if (str == "c++")
                    mProject->enforcedLang = Project::CPP;
                else {
                    printMessage("cppcheck: Unknown language '" + str + "' enforced.");
                    return false;
                }
            }

            // --library
            else if (std::strncmp(argv[i], "--library=", 10) == 0) {
                mProject->addLibrary(argv[i] + 10);
            }

            // Set maximum number of #ifdef configurations to check
            else if (std::strncmp(argv[i], "--max-configs=", 14) == 0) {
                mProject->force = false;

                std::istringstream iss(14+argv[i]);
                if (!(iss >> mProject->maxConfigs)) {
                    printMessage("cppcheck: argument to '--max-configs=' is not a number.");
                    return false;
                }

                if (mProject->maxConfigs < 1) {
                    printMessage("cppcheck: argument to '--max-configs=' must be greater than 0.");
                    return false;
                }

                maxconfigs = true;
            }

            // max ctu depth
            else if (std::strncmp(argv[i], "--max-ctu-depth=", 16) == 0)
                mProject->maxCtuDepth = std::atoi(argv[i] + 16);

            // Write results in file
            else if (std::strncmp(argv[i], "--output-file=", 14) == 0)
                mProject->outputFile = Path::simplifyPath(Path::fromNativeSeparators(argv[i] + 14));

            // Specify platform
            else if (std::strncmp(argv[i], "--platform=", 11) == 0) {
                const std::string platform(11+argv[i]);

                if (platform == "win32A")
                    mProject->platform(Project::Win32A);
                else if (platform == "win32W")
                    mProject->platform(Project::Win32W);
                else if (platform == "win64")
                    mProject->platform(Project::Win64);
                else if (platform == "unix32")
                    mProject->platform(Project::Unix32);
                else if (platform == "unix64")
                    mProject->platform(Project::Unix64);
                else if (platform == "native")
                    mProject->platform(Project::Native);
                else if (platform == "unspecified")
                    mProject->platform(Project::Unspecified);
                else if (!mProject->loadPlatformFile(argv[0], platform)) {
                    std::string message("cppcheck: error: unrecognized platform: \"");
                    message += platform;
                    message += "\".";
                    printMessage(message);
                    return false;
                }
            }

            // Output relative paths
            else if (std::strcmp(argv[i], "-rp") == 0 || std::strcmp(argv[i], "--relative-paths") == 0)
                mSettings->relativePaths = true;
            else if (std::strncmp(argv[i], "-rp=", 4) == 0 || std::strncmp(argv[i], "--relative-paths=", 17) == 0) {
                mSettings->relativePaths = true;
                if (argv[i][argv[i][3]=='='?4:17] != 0) {
                    std::string paths = argv[i]+(argv[i][3]=='='?4:17);
                    for (;;) {
                        const std::string::size_type pos = paths.find(';');
                        if (pos == std::string::npos) {
                            mProject->basePaths.emplace_back(Path::fromNativeSeparators(paths));
                            break;
                        }
                        mProject->basePaths.emplace_back(Path::fromNativeSeparators(paths.substr(0, pos)));
                        paths.erase(0, pos + 1);
                    }
                } else {
                    printMessage("cppcheck: No paths specified for the '" + std::string(argv[i]) + "' option.");
                    return false;
                }
            }

            // Report progress
            else if (std::strcmp(argv[i], "--report-progress") == 0) {
                mSettings->output.enable(Output::progress);
            }

#ifdef HAVE_RULES
            // Rule given at command line
            else if (std::strncmp(argv[i], "--rule=", 7) == 0) {
                Project::Rule rule;
                rule.pattern = 7 + argv[i];
                mProject->rules.emplace_back(rule);
            }

            // Rule file
            else if (std::strncmp(argv[i], "--rule-file=", 12) == 0) {
                tinyxml2::XMLDocument doc;
                if (doc.LoadFile(12+argv[i]) == tinyxml2::XML_SUCCESS) {
                    tinyxml2::XMLElement *node = doc.FirstChildElement();
                    for (; node && strcmp(node->Value(), "rule") == 0; node = node->NextSiblingElement()) {
                        Project::Rule rule;

                        tinyxml2::XMLElement *tokenlist = node->FirstChildElement("tokenlist");
                        if (tokenlist)
                            rule.tokenlist = tokenlist->GetText();

                        tinyxml2::XMLElement *pattern = node->FirstChildElement("pattern");
                        if (pattern) {
                            rule.pattern = pattern->GetText();
                        }

                        tinyxml2::XMLElement *message = node->FirstChildElement("message");
                        if (message) {
                            tinyxml2::XMLElement *severity = message->FirstChildElement("severity");
                            if (severity)
                                rule.severity = Severity::fromString(severity->GetText());

                            tinyxml2::XMLElement *id = message->FirstChildElement("id");
                            if (id)
                                rule.id = id->GetText();

                            tinyxml2::XMLElement *summary = message->FirstChildElement("summary");
                            if (summary)
                                rule.summary = summary->GetText() ? summary->GetText() : "";
                        }

                        if (!rule.pattern.empty())
                            mProject->rules.emplace_back(rule);
                    }
                } else {
                    printMessage("cppcheck: error: unable to load rule-file: " + std::string(12+argv[i]));
                    return false;
                }
            }
#endif

            // show timing information..
            else if (std::strncmp(argv[i], "--showtime=", 11) == 0) {
                const std::string showtimeMode = argv[i] + 11;
                if (showtimeMode == "file")
                    mSettings->showtime = Settings::SHOWTIME_FILE;
                else if (showtimeMode == "summary")
                    mSettings->showtime = Settings::SHOWTIME_SUMMARY;
                else if (showtimeMode == "top5")
                    mSettings->showtime = Settings::SHOWTIME_TOP5;
                else if (showtimeMode.empty())
                    mSettings->showtime = Settings::SHOWTIME_NONE;
                else {
                    printMessage("cppcheck: error: unrecognized showtime mode: \"" + showtimeMode + "\". Supported modes: file, summary, top5.");
                    return false;
                }
            }

            // --std
            else if (std::strcmp(argv[i], "--std=c89") == 0) {
                mProject->standards.c = Standards::C89;
            } else if (std::strcmp(argv[i], "--std=c99") == 0) {
                mProject->standards.c = Standards::C99;
            } else if (std::strcmp(argv[i], "--std=c11") == 0) {
                mProject->standards.c = Standards::C11;
            } else if (std::strcmp(argv[i], "--std=c++03") == 0) {
                mProject->standards.cpp = Standards::CPP03;
            } else if (std::strcmp(argv[i], "--std=c++11") == 0) {
                mProject->standards.cpp = Standards::CPP11;
            } else if (std::strcmp(argv[i], "--std=c++14") == 0) {
                mProject->standards.cpp = Standards::CPP14;
            } else if (std::strcmp(argv[i], "--std=c++17") == 0) {
                mProject->standards.cpp = Standards::CPP17;
            } else if (std::strcmp(argv[i], "--std=c++20") == 0) {
                mProject->standards.cpp = Standards::CPP20;
            }

            else if (std::strncmp(argv[i], "--suppress=", 11) == 0) {
                const std::string suppression = argv[i]+11;
                const std::string errmsg(mProject->nomsg.addSuppressionLine(suppression));
                if (!errmsg.empty()) {
                    printMessage(errmsg);
                    return false;
                }
            }

            // Filter errors
            else if (std::strncmp(argv[i], "--suppressions-list=", 20) == 0) {
                std::string filename = argv[i]+20;
                std::ifstream f(filename);
                if (!f.is_open()) {
                    std::string message("cppcheck: Couldn't open the file: \"");
                    message += filename;
                    message += "\".";
                    if (std::count(filename.begin(), filename.end(), ',') > 0 ||
                        std::count(filename.begin(), filename.end(), '.') > 1) {
                        // If user tried to pass multiple files (we can only guess that)
                        // e.g. like this: --suppressions-list=a.txt,b.txt
                        // print more detailed error message to tell user how he can solve the problem
                        message += "\nIf you want to pass two files, you can do it e.g. like this:";
                        message += "\n    cppcheck --suppressions-list=a.txt --suppressions-list=b.txt file.cpp";
                    }

                    printMessage(message);
                    return false;
                }
                const std::string errmsg(mProject->nomsg.parseFile(f));
                if (!errmsg.empty()) {
                    printMessage(errmsg);
                    return false;
                }
            }

            else if (std::strncmp(argv[i], "--suppress-xml=", 15) == 0) {
                const char * filename = argv[i] + 15;
                const std::string errmsg(mProject->nomsg.parseXmlFile(filename));
                if (!errmsg.empty()) {
                    printMessage(errmsg);
                    return false;
                }
            }

            // Output formatter
            else if (std::strcmp(argv[i], "--template") == 0 ||
                     std::strncmp(argv[i], "--template=", 11) == 0) {
                // "--template format"
                if (argv[i][10] == '=')
                    mSettings->templateFormat = argv[i] + 11;
                else if ((i+1) < argc && argv[i+1][0] != '-') {
                    ++i;
                    mSettings->templateFormat = argv[i];
                } else {
                    printMessage("cppcheck: argument to '--template' is missing.");
                    return false;
                }
            }

            else if (std::strcmp(argv[i], "--template-location") == 0 ||
                     std::strncmp(argv[i], "--template-location=", 20) == 0) {
                // "--template-location format"
                if (argv[i][19] == '=')
                    mSettings->templateLocation = argv[i] + 20;
                else if ((i+1) < argc && argv[i+1][0] != '-') {
                    ++i;
                    mSettings->templateLocation = argv[i];
                } else {
                    printMessage("cppcheck: argument to '--template' is missing.");
                    return false;
                }
            }

            else if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0)
                mSettings->verbose = true;

            else if (std::strcmp(argv[i], "--version") == 0) {
                mShowVersion = true;
                mExitAfterPrint = true;
                return true;
            }

            // Write results in results.xml
            else if (std::strcmp(argv[i], "--xml") == 0)
                mSettings->xml = true;

            // Define the XML file version (and enable XML output)
            else if (std::strncmp(argv[i], "--xml-version=", 14) == 0) {
                const std::string numberString(argv[i]+14);

                std::istringstream iss(numberString);
                if (!(iss >> mSettings->xml_version)) {
                    printMessage("cppcheck: argument to '--xml-version' is not a number.");
                    return false;
                }

                if (mSettings->xml_version != 2) {
                    // We only have xml version 2
                    printMessage("cppcheck: '--xml-version' can only be 2.");
                    return false;
                }

                // Enable also XML if version is set
                mSettings->xml = true;
            }

            else {
                std::string message("cppcheck: error: unrecognized command line option: \"");
                message += argv[i];
                message += "\".";
                printMessage(message);
                return false;
            }
        }

        else {
            mPathNames.emplace_back(Path::fromNativeSeparators(Path::removeQuotationMarks(argv[i])));
        }
    }

    // Default template format..
    if (mSettings->templateFormat.empty())
        mSettings->templateFormat = "{callstack}: ({severity}{certainty:, certainty}) {message}";
    else if (mSettings->templateFormat == "gcc") {
        mSettings->templateFormat = "{file}:{line}:{column}: warning: {message} [{id}]\\n{code}";
        mSettings->templateLocation = "{file}:{line}:{column}: note: {info}\\n{code}";
    } else if (mSettings->templateFormat == "vs")
        mSettings->templateFormat = "{file}({line}): {severity}: {message}";
    else if (mSettings->templateFormat == "edit")
        mSettings->templateFormat = "{file} +{line}: {severity}: {message}";
    else if (mSettings->templateFormat == "cppcheck2") {
        mSettings->templateFormat = "{file}:{line}:{column}: {severity}:{certainty:certainty:} {message} [{id}]\\n{code}";
        if (mSettings->templateLocation.empty())
            mSettings->templateLocation = "{file}:{line}:{column}: note: {info}\\n{code}";
    }

    if (mProject->force || maxconfigs)
        mProject->checkAllConfigurations = true;

    if (mProject->force)
        mProject->maxConfigs = ~0U;

    else if ((def || mProject->preprocessOnly) && !maxconfigs)
        mProject->maxConfigs = 1U;

    if (argc <= 1) {
        mShowHelp = true;
        mExitAfterPrint = true;
    }

    if (mShowHelp) {
        printHelp();
        return true;
    }

    // Print error only if we have "real" command and expect files
    if (!mExitAfterPrint && mPathNames.empty()) {
        printMessage("cppcheck: No C or C++ source files found.");
        return false;
    }

    // Use paths _pathnames if no base paths for relative path output are given
    if (mProject->basePaths.empty() && mSettings->relativePaths)
        mProject->basePaths = mPathNames;

    return true;
}

void CmdLineParser::printHelp()
{
    std::cout << PROGRAMNAME " - A tool for static C/C++ code analysis\n"
              "\n"
              "Syntax:\n"
              "    lcppc [OPTIONS] [files or paths]\n"
              "\n"
              "If a directory is given instead of a filename, *.cpp, *.cxx, *.cc, *.c++, *.c,\n"
              "*.tpp, and *.txx files are checked recursively from the given directory.\n"
              "\n"
              "For some options listed below, IDs have to be given to enable or disable certain\n"
              "behaviour. For disabling, add a - in front of the ID. Several IDs can be given\n"
              "as a comma-separated list. The ID 'all' affects all possible IDs for the switch.\n"
              "Example: '-s=-all,performance' disables all severities but performance.\n"
              "\n"
              "Options:\n"
              "    --addon=<addon>\n"
              "                         Execute addon. i.e. --addon=cert. If options must be\n"
              "                         provided a json configuration is needed.\n"
              "    --addon-python=<python interpreter>\n"
              "                         You can specify the python interpreter either in the\n"
              "                         addon json files or through this command line option.\n"
              "                         If not present, Cppcheck will try \"python3\" first and\n"
              "                         then \"python\".\n"
              "    --certainty=<id>\n"
              "    -c=<id>              Enables messages of given level of certainty. The\n"
              "                         available ids are:\n"
              "                          * all\n"
              "                                  Enable all levels defined below.\n"
              "                          * safe\n"
              "                                  Messages where " PROGRAMNAME " is sure to be correct.\n"
              "                          * inconclusive\n"
              "                                  Inconclusive checks at the trade-off of\n"
              "                                  getting more false-positives.\n"
              "                          * experimental\n"
              "                                  Experimental checks.\n"
              "                         Default is: -c=safe\n"
              "    --checks=<id>        Enables/disables certain checks.\n"
              "                         Default is: --checks=all,-missingInclude,-unusedFunction\n"
              "    --cppcheck-build-dir=<dir>\n"
              "                         Cppcheck working  directory. Advantages are:\n"
              "                          * Incremental analysis: Cppcheck will reuse the results if\n"
              "                            the hash for a file is unchanged.\n"
              "                          * Some useful debug information, i.e. commands used to\n"
              "                            execute clang/clang-tidy/addons.\n"
              "    --check-config       Check " PROGRAMNAME " configuration. The normal code\n"
              "                         analysis is disabled by this flag.\n"
              "    --check-library      Show information messages when library files have\n"
              "                         incomplete info.\n"
              "    --config-exclude=<dir>\n"
              "                         Path (prefix) to be excluded from configuration\n"
              "                         checking. Preprocessor configurations defined in\n"
              "                         headers (but not sources) matching the prefix will not\n"
              "                         be considered for evaluation.\n"
              "    --config-excludes-file=<file>\n"
              "                         A file that contains a list of config-excludes\n"
              "    --doc                Print a list of all available checks.\n"
              "    --dump               Dump xml data for each translation unit. The dump\n"
              "                         files have the extension .dump and contain ast,\n"
              "                         tokenlist, symboldatabase, valueflow.\n"
              "    -D<ID>               Define preprocessor symbol. Unless --max-configs or\n"
              "                         --force is used, " PROGRAMNAME " will only check the given\n"
              "                         configuration when -D is used.\n"
              "                         Example: '-DDEBUG=1 -D__cplusplus'.\n"
              "    -E                   Print preprocessor output on stdout and don't do any\n"
              "                         further processing.\n"
              "    --severity=<id>\n"
              "    -s=<id>              Enable checks of given severity. The available ids are:\n"
              "                          * error\n"
              "                                  Enable error messages\n"
              "                          * all\n"
              "                                  Enables messages of all severities.\n"
              "                          * warning\n"
              "                                  Enable warning messages\n"
              "                          * style\n"
              "                                  Enable style messages\n"
              "                          * performance\n"
              "                                  Enable performance messages\n"
              "                          * portability\n"
              "                                  Enable portability messages\n"
              "                          * information\n"
              "                                  Enable information messages.\n"
              "                         Several ids can be given if you separate them with\n"
              "                         commas. See also --std\n"
              "    --error-exitcode=<n> If errors are found, integer [n] is returned instead of\n"
              "                         the default '0'. '" << EXIT_FAILURE << "' is returned\n"
              "                         if arguments are not valid or if no input files are\n"
              "                         provided. Note that your operating system can modify\n"
              "                         this value, e.g. '256' can become '0'.\n"
              "    --errorlist          Print a list of all the error messages in XML format.\n"
              "    --exitcode-suppressions=<file>\n"
              "                         Used when certain messages should be displayed but\n"
              "                         should not cause a non-zero exitcode.\n"
              "    --file-filter=<str>  Analyze only those files matching the given filter str\n"
              "                         Example: --file-filter=*bar.cpp analyzes only files\n"
              "                                  that end with bar.cpp.\n"
              "    --file-list=<file>   Specify the files to check in a text file. Add one\n"
              "                         filename per line. When file is '-,' the file list will\n"
              "                         be read from standard input.\n"
              "    -f, --force          Force checking of all configurations in files. If used\n"
              "                         together with '--max-configs=', the last option is the\n"
              "                         one that is effective.\n"
              "    -h, --help           Print this help.\n"
              "    -I <dir>             Give path to search for include files. Give several -I\n"
              "                         parameters to give several paths. First given path is\n"
              "                         searched for contained header files first. If paths are\n"
              "                         relative to source files, this is not needed.\n"
              "    --includes-file=<file>\n"
              "                         Specify directory paths to search for included header\n"
              "                         files in a text file. Add one include path per line.\n"
              "                         First given path is searched for contained header\n"
              "                         files first. If paths are relative to source files,\n"
              "                         this is not needed.\n"
              "    --include=<file>\n"
              "                         Force inclusion of a file before the checked file.\n"
              "    -i <dir or file>     Give a source file or source file directory to exclude\n"
              "                         from the check. This applies only to source files so\n"
              "                         header files included by source files are not matched.\n"
              "                         Directory name is matched to all parts of the path.\n"
              "    --inline-suppr       Enable inline suppressions. Use them by placing one or\n"
              "                         more comments, like: '// cppcheck-suppress warningId'\n"
              "                         on the lines before the warning to suppress.\n"
              "    -j <jobs>            Start <jobs> threads to do the checking simultaneously.\n"
              "                         If <jobs> is not specified, the number of threads is\n"
              "                         chosen automatically.\n"
              "    --language=<language>, -x <language>\n"
              "                         Forces " PROGRAMNAME " to check all files as the given\n"
              "                         language. Valid values are: c, c++\n"
              "    --library=<cfg>      Load file <cfg> that contains information about types\n"
              "                         and functions. With such information " PROGRAMNAME "\n"
              "                         understands your code better and therefore you\n"
              "                         get better results. The std.cfg file that is\n"
              "                         distributed with " PROGRAMNAME " is loaded automatically.\n"
              "                         For more information about library files, read the\n"
              "                         manual.\n"
              "    --max-ctu-depth=N    Max depth in whole program analysis. The default value\n"
              "                         is 2. A larger value will mean more errors can be found\n"
              "                         but also means the analysis will be slower.\n"
              "    --output-file=<file> Write results to file, rather than standard error.\n"
              "    --max-configs=<limit>\n"
              "                         Maximum number of configurations to check in a file\n"
              "                         before skipping it. Default is '12'. If used together\n"
              "                         with '--force', the last option is the one that is\n"
              "                         effective.\n"
              "    --output=<id>\n"
              "    -o=<id>              Enables different kinds of output. Available IDs:\n"
              "                          * findings\n"
              "                                  Outputs " PROGRAMNAME "'s findings.\n"
              "                          * debug\n"
              "                                  Enables debugging output.\n"
              "                          * status\n"
              "                                  Prints the current file and configuration.\n"
              "                          * progress\n"
              "                                  Enables progress reports (implies status).\n"
              "                          * verbose\n"
              "                                  Output more detailed error information.\n"
              "                          * config\n"
              "                                  Check " PROGRAMNAME " configuration.\n"
              "                         Default is: -o=findings,status\n"
              "    --platform=<type>, --platform=<file>\n"
              "                         Specifies platform specific types and sizes. The\n"
              "                         available builtin platforms are:\n"
              "                          * unix32\n"
              "                                 32 bit unix variant\n"
              "                          * unix64\n"
              "                                 64 bit unix variant\n"
              "                          * win32A\n"
              "                                 32 bit Windows ASCII character encoding\n"
              "                          * win32W\n"
              "                                 32 bit Windows UNICODE character encoding\n"
              "                          * win64\n"
              "                                 64 bit Windows\n"
              "                          * avr8\n"
              "                                 8 bit AVR microcontrollers\n"
              "                          * native\n"
              "                                 Type sizes of host system are assumed, but no\n"
              "                                 further assumptions.\n"
              "                          * unspecified\n"
              "                                 Unknown type sizes\n"
              "    -rp, --relative-paths\n"
              "    -rp=<paths>, --relative-paths=<paths>\n"
              "                         Use relative paths in output. When given, <paths> are\n"
              "                         used as base. You can separate multiple paths by ';'.\n"
              "                         Otherwise path where source files are searched is used.\n"
              "                         We use string comparison to create relative paths, so\n"
              "                         using e.g. ~ for home folder does not work. It is\n"
              "                         currently only possible to apply the base paths to\n"
              "                         files that are on a lower level in the directory tree.\n"
#ifdef HAVE_RULES
              "    --rule=<rule>        Match regular expression.\n"
              "    --rule-file=<file>   Use given rule file. For more information, see:\n"
              "                         http://sourceforge.net/projects/cppcheck/files/Articles/\n"
#endif
              "    --std=<id>           Set standard.\n"
              "                         The available options are:\n"
              "                          * c89\n"
              "                                 C code is C89 compatible\n"
              "                          * c99\n"
              "                                 C code is C99 compatible\n"
              "                          * c11\n"
              "                                 C code is C11 compatible (default)\n"
              "                          * c++03\n"
              "                                 C++ code is C++03 compatible\n"
              "                          * c++11\n"
              "                                 C++ code is C++11 compatible\n"
              "                          * c++14\n"
              "                                 C++ code is C++14 compatible\n"
              "                          * c++17\n"
              "                                 C++ code is C++17 compatible\n"
              "                          * c++20\n"
              "                                 C++ code is C++20 compatible (default)\n"
              "    --suppress=<spec>    Suppress warnings that match <spec>. The format of\n"
              "                         <spec> is:\n"
              "                         [error id]:[filename]:[line]\n"
              "                         The [filename] and [line] are optional. If [error id]\n"
              "                         is a wildcard '*', all error ids match.\n"
              "    --suppressions-list=<file>\n"
              "                         Suppress warnings listed in the file. Each suppression\n"
              "                         is in the same format as <spec> above.\n"
              "    --suppress-xml=<file>\n"
              "                         Suppress warnings listed in a xml file. XML file should\n"
              "                         follow the manual.pdf format specified in section.\n"
              "                         `6.4 XML suppressions` .\n"
              "    --template='<text>'  Format the error messages. Available fields:\n"
              "                           {file}              file name\n"
              "                           {line}              line number\n"
              "                           {column}            column number\n"
              "                           {callstack}         show a callstack. Example:\n"
              "                                                 [file.c:1] -> [file.c:100]\n"
              "                           {certainty:certainty} if warning is not safe,\n"
              "                                                 certainty is written\n"
              "                           {severity}          severity\n"
              "                           {message}           warning message\n"
              "                           {id}                warning id\n"
              "                           {cwe}               CWE id (Common Weakness Enumeration)\n"
              "                           {code}              show the real code\n"
              "                           \\t                 insert tab\n"
              "                           \\n                 insert newline\n"
              "                           \\r                 insert carriage return\n"
              "                         Example formats:\n"
              "                         '{file}:{line},{severity},{id},{message}' or\n"
              "                         '{file}({line}):({severity}) {message}' or\n"
              "                         '{callstack} {message}'\n"
              "                         Pre-defined templates: gcc (default), cppcheck2 (old default), vs, edit.\n"
              "    --template-location='<text>'\n"
              "                         Format error message location. If this is not provided\n"
              "                         then no extra location info is shown.\n"
              "                         Available fields:\n"
              "                           {file}      file name\n"
              "                           {line}      line number\n"
              "                           {column}    column number\n"
              "                           {info}      location info\n"
              "                           {code}      show the real code\n"
              "                           \\t         insert tab\n"
              "                           \\n         insert newline\n"
              "                           \\r         insert carriage return\n"
              "                         Example format (gcc-like):\n"
              "                         '{file}:{line}:{column}: note: {info}\\n{code}'\n"
              "    -U<ID>               Undefine preprocessor symbol. Use -U to explicitly\n"
              "                         hide certain #ifdef <ID> code paths from checking.\n"
              "                         Example: '-UDEBUG'\n"
              "    -v, --verbose        Output more detailed error information.\n"
              "    --version            Print out version number.\n"
              "    --xml                Write results in xml format to error stream (stderr).\n"
              "\n"
              "Example usage:\n"
              "  # Recursively check the current folder. Print the progress on the screen and\n"
              "  # write errors to a file:\n"
              "  lcppc . 2> err.txt\n"
              "\n"
              "  # Recursively check ../myproject/ and print progress:\n"
              "  lcppc --output=progress ../myproject/\n"
              "\n"
              "  # Check test.cpp, enable all checks:\n"
              "  lcppc --severity=all --certainty=inconclusive --library=posix test.cpp\n"
              "\n"
              "  # Check f.cpp and search include files from inc1/ and inc2/:\n"
              "  lcppc -I inc1/ -I inc2/ f.cpp\n"
              "\n"
              "For more information:\n"
              "    http://cppcheck.net/manual.pdf\n"
              "\n"
              "Many thanks to the 3rd party libraries we use:\n"
              " * tinyxml2 -- loading project/library/ctu files.\n"
              " * picojson -- loading compile database.\n"
              " * pcre -- rules.\n"
              " * qt -- used in GUI\n";
}
