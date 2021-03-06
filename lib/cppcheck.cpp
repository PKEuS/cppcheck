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
#include "cppcheck.h"

#include "check.h"
#include "checkunusedfunctions.h"
#include "ctu.h"
#include "library.h"
#include "mathlib.h"
#include "path.h"
#include "platform.h"
#include "preprocessor.h" // Preprocessor
#include "suppressions.h"
#include "timer.h"
#include "tokenize.h" // Tokenizer
#include "tokenlist.h"
#include "version.h"

#define PICOJSON_USE_INT64
#include <picojson.h>
#include <simplecpp.h>
#include <tinyxml2.h>
#include <algorithm>
#include <cstring>
#include <new>
#include <set>
#include <stdexcept>
#include <vector>
#include <memory>
#include <fstream> // <- TEMPORARY
#include <cstdio>

#ifdef HAVE_RULES
#define PCRE_STATIC
#include <pcre.h>
#endif

static const char Version[] = CPPCHECK_VERSION_STRING;
static const char ExtraVersion[] = "";

// CWE ids used
static const CWE CWE398(398U);  // Indicator of Poor Code Quality

namespace {
    struct AddonInfo {
        std::string name;
        std::string scriptFile;
        std::string args;
        std::string python;

        static std::string getFullPath(const std::string &fileName, const std::string &exename) {
            if (Path::fileExists(fileName))
                return fileName;

            const std::string exepath = Path::getPathFromFilename(exename);
            if (Path::fileExists(exepath + fileName))
                return exepath + fileName;
            if (Path::fileExists(exepath + "addons/" + fileName))
                return exepath + "addons/" + fileName;

#ifdef FILESDIR
            if (Path::fileExists(FILESDIR + ("/" + fileName)))
                return FILESDIR + ("/" + fileName);
            if (Path::fileExists(FILESDIR + ("/addons/" + fileName)))
                return FILESDIR + ("/addons/" + fileName);
#endif
            return "";
        }

        std::string parseAddonInfo(const picojson::value &json, const std::string &fileName, const std::string &exename) {
            const std::string& json_error = picojson::get_last_error();
            if (!json_error.empty()) {
                return "Loading " + fileName + " failed. " + json_error;
            }
            if (!json.is<picojson::object>())
                return "Loading " + fileName + " failed. Bad json.";
            picojson::object obj = json.get<picojson::object>();
            if (obj.count("args")) {
                if (!obj["args"].is<picojson::array>())
                    return "Loading " + fileName + " failed. args must be array.";
                for (const picojson::value &v : obj["args"].get<picojson::array>())
                    args += " " + v.get<std::string>();
            }

            if (obj.count("python")) {
                // Python was defined in the config file
                if (obj["python"].is<picojson::array>()) {
                    return "Loading " + fileName +" failed. python must not be an array.";
                }
                python = obj["python"].get<std::string>();
            } else {
                python = "";
            }

            return getAddonInfo(obj["script"].get<std::string>(), exename);
        }

        std::string getAddonInfo(const std::string &fileName, const std::string &exename) {
            if (fileName[0] == '{') {
                std::istringstream in(fileName);
                picojson::value json;
                in >> json;
                return parseAddonInfo(json, fileName, exename);
            }
            if (fileName.find(".") == std::string::npos)
                return getAddonInfo(fileName + ".py", exename);

            if (endsWith(fileName, ".py", 3)) {
                scriptFile = getFullPath(fileName, exename);
                if (scriptFile.empty())
                    return "Did not find addon " + fileName;

                std::string::size_type pos1 = scriptFile.rfind("/");
                if (pos1 == std::string::npos)
                    pos1 = 0;
                else
                    pos1++;
                std::string::size_type pos2 = scriptFile.rfind(".");
                if (pos2 < pos1)
                    pos2 = std::string::npos;
                name = scriptFile.substr(pos1, pos2 - pos1);

                return "";
            }

            if (!endsWith(fileName, ".json", 5))
                return "Failed to open addon " + fileName;

            std::ifstream fin(fileName);
            if (!fin.is_open())
                return "Failed to open " + fileName;
            picojson::value json;
            fin >> json;
            return parseAddonInfo(json, fileName, exename);
        }
    };
}

static std::string cmdFileName(std::string f)
{
    f = Path::toNativeSeparators(f);
    if (f.find(" ") != std::string::npos)
        return "\"" + f + "\"";
    return f;
}

static std::vector<std::string> split(const std::string &str, const std::string &sep=" ")
{
    std::vector<std::string> ret;
    for (std::string::size_type startPos = 0U; startPos < str.size();) {
        startPos = str.find_first_not_of(sep, startPos);
        if (startPos == std::string::npos)
            break;

        if (str[startPos] == '\"') {
            const std::string::size_type endPos = str.find("\"", startPos + 1);
            ret.push_back(str.substr(startPos + 1, endPos - startPos - 1));
            startPos = (endPos < str.size()) ? (endPos + 1) : endPos;
            continue;
        }

        const std::string::size_type endPos = str.find(sep, startPos + 1);
        ret.push_back(str.substr(startPos, endPos - startPos));
        startPos = endPos;
    }

    return ret;
}

static std::string executeAddon(const AddonInfo &addonInfo,
                                const std::string &defaultPythonExe,
                                const std::string &dumpFile,
                                std::function<bool(const std::string&, const std::vector<std::string>&, const std::string&, std::string*)> executeCommand)
{
    const std::string redirect = "2>&1";

    std::string pythonExe;

    if (!addonInfo.python.empty())
        pythonExe = cmdFileName(addonInfo.python);
    else if (!defaultPythonExe.empty())
        pythonExe = cmdFileName(defaultPythonExe);
    else {
#ifdef _WIN32
        const char *p[] = { "python3.exe", "python.exe" };
#else
        const char *p[] = { "python3", "python" };
#endif
        for (size_t i = 0; i < sizeof(p)/sizeof(*p); ++i) {
            std::string out;
            if (executeCommand(p[i], { "--version" }, redirect, &out) && out.compare(0, 7, "Python ") == 0 && std::isdigit(out[7])) {
                pythonExe = p[i];
                break;
            }
        }
        if (pythonExe.empty())
            throw InternalError(nullptr, "Failed to auto detect python");
    }

    const std::string args = cmdFileName(addonInfo.scriptFile) + " --cli" + addonInfo.args + " " + cmdFileName(dumpFile);
    std::string result;
    if (!executeCommand(pythonExe, split(args), redirect, &result))
        throw InternalError(nullptr, "Failed to execute addon (command: '" + pythonExe + " " + args + "')");

    // Validate output..
    std::istringstream istr(result);
    std::string line;
    while (std::getline(istr, line)) {
        if (line.compare(0,9,"Checking ", 0, 9) != 0 && !line.empty() && line[0] != '{')
            throw InternalError(nullptr, "Failed to execute '" + pythonExe + " " + args + "'. " + result);
    }

    // Valid results
    return result;
}

/**
 * Execute a shell command and read the output from it. Returns true if command terminated successfully.
 */
bool CppCheck::executeCommand(const std::string& exe, const std::vector<std::string>& args, const std::string& redirect, std::string* output)
{
    output->clear();

    std::string joinedArgs;
    for (const std::string& arg : args) {
        if (!joinedArgs.empty())
            joinedArgs += " ";
        if (arg.find(" ") != std::string::npos)
            joinedArgs += '"' + arg + '"';
        else
            joinedArgs += arg;
    }

#ifdef _WIN32
    // Extra quoutes are needed in windows if filename has space
    std::string exe2;
    if (exe.find(" ") != std::string::npos)
        exe2 = "\"" + exe + "\"";
    else
        exe2 = exe;
    const std::string cmd = exe2 + " " + joinedArgs + " " + redirect;
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
#else
    const std::string cmd = exe + " " + joinedArgs + " " + redirect;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
#endif
    if (!pipe)
        return false;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr)
        *output += buffer;
    return true;
}

CppCheck::CppCheck(ErrorLogger &errorLogger,
                   Settings& settings,
                   Project& project,
                   bool useGlobalSuppressions)
    : mSettings(settings)
    , mProject(project)
    , mErrorLogger(errorLogger)
    , mCTU(nullptr)
    , mExitCode(0)
    , mSuppressInternalErrorFound(false)
    , mUseGlobalSuppressions(useGlobalSuppressions)
    , mTooManyConfigs(false)
{
}

CppCheck::~CppCheck()
{
}

const char * CppCheck::version()
{
    return Version;
}

const char * CppCheck::extraVersion()
{
    return ExtraVersion;
}

unsigned int CppCheck::check(CTU::CTUInfo* ctu)
{
    std::ifstream fin(ctu->sourcefile);
    return checkCTU(ctu, fin);
}

unsigned int CppCheck::check(CTU::CTUInfo* ctu, const std::string &content)
{
    std::istringstream iss(content);
    return checkCTU(ctu, iss);
}

unsigned int CppCheck::checkCTU(CTU::CTUInfo* ctu, std::istream& fileStream)
{
    Timer timer0("CppCheck::checkCTU", mSettings.showtime);

    mCTU = ctu;
    mExitCode = 0;
    mSuppressInternalErrorFound = false;

    // only show debug warnings for accepted C/C++ source files
    if (!Path::acceptFile(ctu->sourcefile))
        mSettings.debugwarnings = false;

    if (Settings::terminated())
        return mExitCode;

    if (mSettings.output.isEnabled(Output::status)) {
        std::string fixedpath = Path::simplifyPath(ctu->sourcefile);
        fixedpath = Path::toNativeSeparators(fixedpath);
        mErrorLogger.reportOut(std::string("Checking ") + fixedpath + std::string("..."));

        if (mSettings.verbose) {
            mErrorLogger.reportOut("Defines:" + mProject.userDefines);
            std::string undefs;
            for (const std::string& U : mProject.userUndefs) {
                if (!undefs.empty())
                    undefs += ';';
                undefs += ' ' + U;
            }
            mErrorLogger.reportOut("Undefines:" + undefs);
            std::string includePaths;
            for (const std::string &I : mProject.includePaths)
                includePaths += " -I" + I;
            mErrorLogger.reportOut("Includes:" + includePaths);
            mErrorLogger.reportOut(std::string("Platform:") + mProject.platformString());
        }
    }

    try {
        Preprocessor preprocessor(mSettings, mProject, this);
        std::set<std::string> configurations;

        simplecpp::OutputList outputList;
        std::vector<std::string> files;
        simplecpp::TokenList tokens1(fileStream, files, ctu->sourcefile, &outputList);

        // If there is a syntax error, report it and stop
        for (const simplecpp::Output &output : outputList) {
            bool err = true;
            switch (output.type) {
            case simplecpp::Output::ERROR:
            case simplecpp::Output::INCLUDE_NESTED_TOO_DEEPLY:
            case simplecpp::Output::SYNTAX_ERROR:
            case simplecpp::Output::UNHANDLED_CHAR_ERROR:
            case simplecpp::Output::EXPLICIT_INCLUDE_NOT_FOUND:
                err = true;
                break;
            case simplecpp::Output::WARNING:
            case simplecpp::Output::MISSING_HEADER:
            case simplecpp::Output::PORTABILITY_BACKSLASH:
                err = false;
                break;
            }

            if (err) {
                std::string file = Path::fromNativeSeparators(output.location.file());
                if (mSettings.relativePaths)
                    file = Path::getRelativePath(file, mProject.basePaths);

                const ErrorMessage::FileLocation loc1(file, output.location.line, output.location.col);
                std::list<ErrorMessage::FileLocation> callstack(1, loc1);

                ErrorMessage errmsg(callstack,
                                    emptyString,
                                    Severity::error,
                                    output.msg,
                                    "syntaxError",
                                    Certainty::safe);
                reportErr(errmsg);
                return mExitCode;
            }
        }

        if (!preprocessor.loadFiles(tokens1, files))
            return mExitCode;

        // write dump file xml prolog
        std::ofstream fdump;
        std::string dumpFile;
        if (mSettings.dump || !mProject.addons.empty()) {
            if (!mSettings.dumpFile.empty())
                dumpFile = mSettings.dumpFile;
            else if (!mSettings.dump && !mProject.buildDir.empty())
                dumpFile = ctu->analyzerfile + ".dump";
            else
                dumpFile = ctu->sourcefile + ".dump";

            fdump.open(dumpFile);
            if (fdump.is_open()) {
                fdump << "<?xml version=\"1.0\"?>" << std::endl;
                fdump << "<dumps>" << std::endl;
                fdump << "  <platform"
                      << " name=\"" << mProject.platformString() << '\"'
                      << " char_bit=\"" << mProject.char_bit << '\"'
                      << " short_bit=\"" << mProject.short_bit << '\"'
                      << " int_bit=\"" << mProject.int_bit << '\"'
                      << " long_bit=\"" << mProject.long_bit << '\"'
                      << " long_long_bit=\"" << mProject.long_long_bit << '\"'
                      << " pointer_bit=\"" << (mProject.sizeof_pointer * mProject.char_bit) << '\"'
                      << "/>\n";
                fdump << "  <rawtokens>" << std::endl;
                for (std::size_t i = 0; i < files.size(); ++i)
                    fdump << "    <file index=\"" << i << "\" name=\"" << ErrorLogger::toxml(files[i]) << "\"/>" << std::endl;
                for (const simplecpp::Token *tok = tokens1.cfront(); tok; tok = tok->next) {
                    fdump << "    <tok "
                          << "fileIndex=\"" << tok->location.fileIndex << "\" "
                          << "linenr=\"" << tok->location.line << "\" "
                          << "column=\"" << tok->location.col << "\" "
                          << "str=\"" << ErrorLogger::toxml(tok->str()) << "\""
                          << "/>" << std::endl;
                }
                fdump << "  </rawtokens>" << std::endl;
            }
        }

        // Parse comments and then remove them
        preprocessor.inlineSuppressions(tokens1);
        if ((mSettings.dump || !mProject.addons.empty()) && fdump.is_open()) {
            mProject.nomsg.dump(fdump);
        }
        tokens1.removeComments();
        preprocessor.removeComments();

        if (!mProject.buildDir.empty()) {
            // Get toolinfo
            std::ostringstream toolinfo;
            toolinfo << CPPCHECK_VERSION_STRING;
            toolinfo << mProject.severity.intValue() << ' ';
            toolinfo << mProject.certainty.intValue();
            toolinfo << mProject.userDefines;
            mProject.nomsg.dump(toolinfo);

            // Calculate checksum so it can be compared with old checksum / future checksums
            const uint32_t checksum = preprocessor.calculateChecksum(tokens1, toolinfo.str());
            if (ctu->tryLoadFromFile(checksum)) {
                for (auto it = ctu->mErrors.cbegin(); it != ctu->mErrors.cend(); ++it)
                    reportErr(*it);
                return mExitCode;  // known results => no need to reanalyze file
            }
        }

        // Get directives
        preprocessor.setDirectives(tokens1);
        preprocessor.simplifyPragmaAsm(&tokens1);

        preprocessor.setPlatformInfo(&tokens1);

        // Get configurations..
        if ((mProject.checkAllConfigurations && mProject.userDefines.empty()) || mProject.force) {
            Timer t("Preprocessor::getConfigs", mSettings.showtime);
            configurations = preprocessor.getConfigs(tokens1);
        } else {
            configurations.insert(mProject.userDefines);
        }

        if (mSettings.checkConfiguration) {
            for (const std::string &config : configurations)
                (void)preprocessor.getcode(tokens1, config, files, true);

            return 0;
        }

        // Run define rules on raw code
        for (const Project::Rule &rule : mProject.rules) {
            if (rule.tokenlist != "define")
                continue;

            std::string code;
            const std::list<Directive> &directives = preprocessor.getDirectives();
            for (const Directive &dir : directives) {
                if (dir.str.compare(0,8,"#define ") == 0)
                    code += "#line " + MathLib::toString(dir.linenr) + " \"" + dir.file + "\"\n" + dir.str + '\n';
            }
            Tokenizer tokenizer2(&mSettings, &mProject, this);
            std::istringstream istr2(code);
            tokenizer2.list.createTokens(istr2);
            executeRules("define", tokenizer2);
            break;
        }

        if (!mProject.force && configurations.size() > mProject.maxConfigs) {
            if (mProject.severity.isEnabled(Severity::information)) {
                tooManyConfigsError(Path::toNativeSeparators(ctu->sourcefile),configurations.size());
            } else {
                mTooManyConfigs = true;
            }
        }

        std::set<uint64_t> checksums0;
        std::set<uint64_t> checksums1;
        unsigned int checkCount = 0;
        bool hasValidConfig = false;
        std::vector<std::string> configurationError;
        for (const std::string &currCfg : configurations) {
            // bail out if terminated
            if (Settings::terminated())
                break;

            // Check only a few configurations (default 12), after that bail out, unless --force
            // was used.
            if (!mProject.force && ++checkCount > mProject.maxConfigs)
                break;

            if (!mProject.userDefines.empty()) {
                mCurrentConfig = mProject.userDefines;
                const std::vector<std::string> v1(split(mProject.userDefines, ";"));
                for (const std::string &cfg: split(currCfg, ";")) {
                    if (std::find(v1.begin(), v1.end(), cfg) == v1.end()) {
                        mCurrentConfig += ";" + cfg;
                    }
                }
            } else {
                mCurrentConfig = currCfg;
            }

            if (mProject.preprocessOnly) {
                Timer t("Preprocessor::getcode", mSettings.showtime);
                std::string codeWithoutCfg = preprocessor.getcode(tokens1, mCurrentConfig, files, true);
                t.stop();

                if (codeWithoutCfg.compare(0,5,"#file") == 0)
                    codeWithoutCfg.insert(0U, "//");
                std::string::size_type pos = 0;
                while ((pos = codeWithoutCfg.find("\n#file",pos)) != std::string::npos)
                    codeWithoutCfg.insert(pos+1U, "//");
                pos = 0;
                while ((pos = codeWithoutCfg.find("\n#endfile",pos)) != std::string::npos)
                    codeWithoutCfg.insert(pos+1U, "//");
                pos = 0;
                while ((pos = codeWithoutCfg.find(Preprocessor::macroChar,pos)) != std::string::npos)
                    codeWithoutCfg[pos] = ' ';
                reportOut(codeWithoutCfg);
                continue;
            }

            Tokenizer tokenizer(&mSettings, &mProject, this);
            tokenizer.setPreprocessor(&preprocessor);

            try {
                // Create tokens, skip rest of iteration if failed
                {
                    Timer timer("Tokenizer::createTokens", mSettings.showtime);
                    simplecpp::TokenList tokensP = preprocessor.preprocess(tokens1, mCurrentConfig, files, true);
                    tokenizer.createTokens(std::move(tokensP));
                }
                hasValidConfig = true;

                // If only errors are printed, print filename after the check
                if (mSettings.output.isEnabled(Output::status) && (!mCurrentConfig.empty() || checkCount > 1)) {
                    std::string fixedpath = Path::simplifyPath(ctu->sourcefile);
                    fixedpath = Path::toNativeSeparators(fixedpath);
                    mErrorLogger.reportOut("Checking " + fixedpath + ": " + mCurrentConfig + "...");
                }

                if (!tokenizer.tokens())
                    continue;

                // skip rest of iteration if just checking configuration
                if (mSettings.checkConfiguration)
                    continue;

                // Check raw tokens
                checkRawTokens(tokenizer);

                // Simplify tokens into normal form, skip rest of iteration if failed
                if (!tokenizer.simplifyTokens0(mCurrentConfig))
                    continue;

                // Skip if we already met the same token list
                if (mProject.force || mProject.maxConfigs > 1) {
                    const uint64_t checksum = tokenizer.list.calculateChecksum();
                    if (!checksums0.insert(checksum).second) {
                        if (mSettings.debugwarnings)
                            purgedConfigurationMessage(ctu->sourcefile, mCurrentConfig);
                        continue;
                    }
                }

                if (!tokenizer.simplifyTokens1())
                    continue;

                // dump xml if --dump
                if ((mSettings.dump || !mProject.addons.empty()) && fdump.is_open()) {
                    fdump << "<dump cfg=\"" << ErrorLogger::toxml(mCurrentConfig) << "\">" << std::endl;
                    fdump << "  <standards>" << std::endl;
                    fdump << "    <c version=\"" << mProject.standards.getC() << "\"/>" << std::endl;
                    fdump << "    <cpp version=\"" << mProject.standards.getCPP() << "\"/>" << std::endl;
                    fdump << "  </standards>" << std::endl;
                    preprocessor.dump(fdump);
                    tokenizer.dump(fdump);
                    fdump << "</dump>" << std::endl;
                }

                // Skip if we already met the same simplified token list
                if (mProject.force || mProject.maxConfigs > 1) {
                    const uint64_t checksum = tokenizer.list.calculateChecksum();
                    if (!checksums1.insert(checksum).second) {
                        if (mSettings.debugwarnings)
                            purgedConfigurationMessage(ctu->sourcefile, mCurrentConfig);
                        continue;
                    }
                }

                // Check normal tokens
                checkNormalTokens(tokenizer);

            } catch (const simplecpp::Output &o) {
                // #error etc during preprocessing
                configurationError.push_back((mCurrentConfig.empty() ? "\'\'" : mCurrentConfig) + " : [" + o.location.file() + ':' + MathLib::toString(o.location.line) + "] " + o.msg);
                --checkCount; // don't count invalid configurations
                continue;

            } catch (const InternalError &e) {
                std::list<ErrorMessage::FileLocation> locationList;
                if (e.token) {
                    ErrorMessage::FileLocation loc(e.token, &tokenizer.list);
                    locationList.push_back(loc);
                } else {
                    ErrorMessage::FileLocation loc(tokenizer.list.getSourceFilePath(), 0, 0);
                    ErrorMessage::FileLocation loc2(ctu->sourcefile, 0, 0);
                    locationList.push_back(loc2);
                    locationList.push_back(loc);
                }
                ErrorMessage errmsg(locationList,
                                    tokenizer.list.getSourceFilePath(),
                                    Severity::error,
                                    e.errorMessage,
                                    e.id,
                                    Certainty::safe);

                if (errmsg.severity == Severity::error || mProject.severity.isEnabled(errmsg.severity))
                    reportErr(errmsg);
            }
        }

        if (!hasValidConfig && configurations.size() > 1 && mProject.severity.isEnabled(Severity::information)) {
            std::string msg = "This file is not analyzed. Cppcheck failed to extract a valid configuration. Use -v for more details.\n"
                              "This file is not analyzed. Cppcheck failed to extract a valid configuration. The tested configurations have these preprocessor errors:";
            for (const std::string &s : configurationError)
                msg += '\n' + s;

            std::list<ErrorMessage::FileLocation> locationList;
            ErrorMessage::FileLocation loc;
            loc.setfile(Path::toNativeSeparators(ctu->sourcefile));
            locationList.push_back(loc);
            ErrorMessage errmsg(locationList,
                                loc.getFileNative(),
                                Severity::information,
                                msg,
                                "noValidConfiguration",
                                Certainty::safe);
            reportErr(errmsg);
        }

        // dumped all configs, close root </dumps> element now
        if ((mSettings.dump || !mProject.addons.empty()) && fdump.is_open())
            fdump << "</dumps>" << std::endl;

        if (!mProject.addons.empty()) {
            fdump.close();

            for (const std::string &addon : mProject.addons) {
                struct AddonInfo addonInfo;
                const std::string &failedToGetAddonInfo = addonInfo.getAddonInfo(addon, mSettings.exename);
                if (!failedToGetAddonInfo.empty()) {
                    reportOut(failedToGetAddonInfo);
                    mExitCode = 1;
                    continue;
                }
                const std::string results =
                    executeAddon(addonInfo, mSettings.addonPython, dumpFile, executeCommand);
                std::istringstream istr(results);
                std::string line;

                while (std::getline(istr, line)) {
                    if (line.compare(0,1,"{") != 0)
                        continue;

                    picojson::value res;
                    std::istringstream istr2(line);
                    istr2 >> res;
                    if (!res.is<picojson::object>())
                        continue;

                    picojson::object obj = res.get<picojson::object>();

                    const std::string fileName = obj["file"].get<std::string>();
                    const int64_t lineNumber = obj["linenr"].get<int64_t>();
                    const int64_t column = obj["column"].get<int64_t>();

                    ErrorMessage errmsg;

                    errmsg.callStack.emplace_back(ErrorMessage::FileLocation(fileName, lineNumber, column));

                    errmsg.id = obj["addon"].get<std::string>() + "-" + obj["errorId"].get<std::string>();
                    const std::string text = obj["message"].get<std::string>();
                    errmsg.setmsg(text);
                    const std::string severity = obj["severity"].get<std::string>();
                    errmsg.severity = Severity::fromString(severity);
                    if (errmsg.severity == Severity::SeverityType::none)
                        continue;
                    errmsg.file0 = fileName;

                    reportErr(errmsg);
                }
            }
            std::remove(dumpFile.c_str());
        }

        if (!mProject.buildDir.empty())
            ctu->writeFile();
    } catch (const std::runtime_error &e) {
        internalError(ctu->sourcefile, e.what());
    } catch (const std::bad_alloc &e) {
        internalError(ctu->sourcefile, e.what());
    } catch (const InternalError &e) {
        internalError(ctu->sourcefile, e.errorMessage);
        mExitCode=1; // e.g. reflect a syntax error
    }

    mErrorList.clear();

    return mExitCode;
}

void CppCheck::internalError(const std::string &filename, const std::string &msg)
{
    const std::string fixedpath = Path::toNativeSeparators(filename);
    const std::string fullmsg("Bailing out from checking " + fixedpath + " since there was an internal error: " + msg);

    if (mProject.severity.isEnabled(Severity::information)) {
        const ErrorMessage::FileLocation loc1(filename, 0, 0);
        std::list<ErrorMessage::FileLocation> callstack(1, loc1);

        ErrorMessage errmsg(callstack,
                            emptyString,
                            Severity::information,
                            fullmsg,
                            "internalError",
                            Certainty::safe);

        mErrorLogger.reportErr(errmsg);
    } else {
        // Report on stdout
        mErrorLogger.reportOut(fullmsg);
    }
}

//---------------------------------------------------------------------------
// CppCheck - A function that checks a raw token list
//---------------------------------------------------------------------------
void CppCheck::checkRawTokens(const Tokenizer &tokenizer)
{
    // Execute rules for "raw" code
    executeRules("raw", tokenizer);
}

//---------------------------------------------------------------------------
// CppCheck - A function that checks a normal token list
//---------------------------------------------------------------------------

void CppCheck::checkNormalTokens(const Tokenizer &tokenizer)
{
    Context ctx(this, &mSettings, &mProject, &tokenizer);

    // Analyse the tokens..
    mCTU->parseTokens(&tokenizer);
    for (const Check *check : Check::instances()) {
        Check::FileInfo *fi = check->getFileInfo(ctx);
        if (fi != nullptr) {
            mCTU->addCheckInfo(check->name(), fi);
        }
    }

    // call all "runChecks" in all registered Check classes
    for (Check *check : Check::instances()) {
        if (Settings::terminated())
            return;

        if (Tokenizer::isMaxTime())
            return;

        if (!mProject.checks.isEnabled(check->name()))
            continue;

        Timer timerRunChecks(check->name() + "::runChecks", mSettings.showtime);
        check->runChecks(ctx);
    }

    executeRules("normal", tokenizer);
    executeRules("simple", tokenizer);
}

//---------------------------------------------------------------------------

bool CppCheck::hasRule(const std::string &tokenlist) const
{
#ifdef HAVE_RULES
    for (const Project::Rule &rule : mProject.rules) {
        if (rule.tokenlist == tokenlist)
            return true;
    }
#else
    (void)tokenlist;
#endif
    return false;
}


#ifdef HAVE_RULES

static const char * pcreErrorCodeToString(const int pcreExecRet)
{
    switch (pcreExecRet) {
    case PCRE_ERROR_NULL:
        return "Either code or subject was passed as NULL, or ovector was NULL "
               "and ovecsize was not zero (PCRE_ERROR_NULL)";
    case PCRE_ERROR_BADOPTION:
        return "An unrecognized bit was set in the options argument (PCRE_ERROR_BADOPTION)";
    case PCRE_ERROR_BADMAGIC:
        return "PCRE stores a 4-byte \"magic number\" at the start of the compiled code, "
               "to catch the case when it is passed a junk pointer and to detect when a "
               "pattern that was compiled in an environment of one endianness is run in "
               "an environment with the other endianness. This is the error that PCRE "
               "gives when the magic number is not present (PCRE_ERROR_BADMAGIC)";
    case PCRE_ERROR_UNKNOWN_NODE:
        return "While running the pattern match, an unknown item was encountered in the "
               "compiled pattern. This error could be caused by a bug in PCRE or by "
               "overwriting of the compiled pattern (PCRE_ERROR_UNKNOWN_NODE)";
    case PCRE_ERROR_NOMEMORY:
        return "If a pattern contains back references, but the ovector that is passed "
               "to pcre_exec() is not big enough to remember the referenced substrings, "
               "PCRE gets a block of memory at the start of matching to use for this purpose. "
               "If the call via pcre_malloc() fails, this error is given. The memory is "
               "automatically freed at the end of matching. This error is also given if "
               "pcre_stack_malloc() fails in pcre_exec(). "
               "This can happen only when PCRE has been compiled with "
               "--disable-stack-for-recursion (PCRE_ERROR_NOMEMORY)";
    case PCRE_ERROR_NOSUBSTRING:
        return "This error is used by the pcre_copy_substring(), pcre_get_substring(), "
               "and pcre_get_substring_list() functions (see below). "
               "It is never returned by pcre_exec() (PCRE_ERROR_NOSUBSTRING)";
    case PCRE_ERROR_MATCHLIMIT:
        return "The backtracking limit, as specified by the match_limit field in a pcre_extra "
               "structure (or defaulted) was reached. "
               "See the description above (PCRE_ERROR_MATCHLIMIT)";
    case PCRE_ERROR_CALLOUT:
        return "This error is never generated by pcre_exec() itself. "
               "It is provided for use by callout functions that want to yield a distinctive "
               "error code. See the pcrecallout documentation for details (PCRE_ERROR_CALLOUT)";
    case PCRE_ERROR_BADUTF8:
        return "A string that contains an invalid UTF-8 byte sequence was passed as a subject, "
               "and the PCRE_NO_UTF8_CHECK option was not set. If the size of the output vector "
               "(ovecsize) is at least 2, the byte offset to the start of the the invalid UTF-8 "
               "character is placed in the first element, and a reason code is placed in the "
               "second element. The reason codes are listed in the following section. For "
               "backward compatibility, if PCRE_PARTIAL_HARD is set and the problem is a truncated "
               "UTF-8 character at the end of the subject (reason codes 1 to 5), "
               "PCRE_ERROR_SHORTUTF8 is returned instead of PCRE_ERROR_BADUTF8";
    case PCRE_ERROR_BADUTF8_OFFSET:
        return "The UTF-8 byte sequence that was passed as a subject was checked and found to "
               "be valid (the PCRE_NO_UTF8_CHECK option was not set), but the value of "
               "startoffset did not point to the beginning of a UTF-8 character or the end of "
               "the subject (PCRE_ERROR_BADUTF8_OFFSET)";
    case PCRE_ERROR_PARTIAL:
        return "The subject string did not match, but it did match partially. See the "
               "pcrepartial documentation for details of partial matching (PCRE_ERROR_PARTIAL)";
    case PCRE_ERROR_BADPARTIAL:
        return "This code is no longer in use. It was formerly returned when the PCRE_PARTIAL "
               "option was used with a compiled pattern containing items that were not supported "
               "for partial matching. From release 8.00 onwards, there are no restrictions on "
               "partial matching (PCRE_ERROR_BADPARTIAL)";
    case PCRE_ERROR_INTERNAL:
        return "An unexpected internal error has occurred. This error could be caused by a bug "
               "in PCRE or by overwriting of the compiled pattern (PCRE_ERROR_INTERNAL)";
    case PCRE_ERROR_BADCOUNT:
        return"This error is given if the value of the ovecsize argument is negative "
              "(PCRE_ERROR_BADCOUNT)";
    case PCRE_ERROR_RECURSIONLIMIT :
        return "The internal recursion limit, as specified by the match_limit_recursion "
               "field in a pcre_extra structure (or defaulted) was reached. "
               "See the description above (PCRE_ERROR_RECURSIONLIMIT)";
    case PCRE_ERROR_DFA_UITEM:
        return "PCRE_ERROR_DFA_UITEM";
    case PCRE_ERROR_DFA_UCOND:
        return "PCRE_ERROR_DFA_UCOND";
    case PCRE_ERROR_DFA_WSSIZE:
        return "PCRE_ERROR_DFA_WSSIZE";
    case PCRE_ERROR_DFA_RECURSE:
        return "PCRE_ERROR_DFA_RECURSE";
    case PCRE_ERROR_NULLWSLIMIT:
        return "PCRE_ERROR_NULLWSLIMIT";
    case PCRE_ERROR_BADNEWLINE:
        return "An invalid combination of PCRE_NEWLINE_xxx options was "
               "given (PCRE_ERROR_BADNEWLINE)";
    case PCRE_ERROR_BADOFFSET:
        return "The value of startoffset was negative or greater than the length "
               "of the subject, that is, the value in length (PCRE_ERROR_BADOFFSET)";
    case PCRE_ERROR_SHORTUTF8:
        return "This error is returned instead of PCRE_ERROR_BADUTF8 when the subject "
               "string ends with a truncated UTF-8 character and the PCRE_PARTIAL_HARD option is set. "
               "Information about the failure is returned as for PCRE_ERROR_BADUTF8. "
               "It is in fact sufficient to detect this case, but this special error code for "
               "PCRE_PARTIAL_HARD precedes the implementation of returned information; "
               "it is retained for backwards compatibility (PCRE_ERROR_SHORTUTF8)";
    case PCRE_ERROR_RECURSELOOP:
        return "This error is returned when pcre_exec() detects a recursion loop "
               "within the pattern. Specifically, it means that either the whole pattern "
               "or a subpattern has been called recursively for the second time at the same "
               "position in the subject string. Some simple patterns that might do this "
               "are detected and faulted at compile time, but more complicated cases, "
               "in particular mutual recursions between two different subpatterns, "
               "cannot be detected until run time (PCRE_ERROR_RECURSELOOP)";
    case PCRE_ERROR_JIT_STACKLIMIT:
        return "This error is returned when a pattern that was successfully studied "
               "using a JIT compile option is being matched, but the memory available "
               "for the just-in-time processing stack is not large enough. See the pcrejit "
               "documentation for more details (PCRE_ERROR_JIT_STACKLIMIT)";
    case PCRE_ERROR_BADMODE:
        return "This error is given if a pattern that was compiled by the 8-bit library "
               "is passed to a 16-bit or 32-bit library function, or vice versa (PCRE_ERROR_BADMODE)";
    case PCRE_ERROR_BADENDIANNESS:
        return "This error is given if a pattern that was compiled and saved is reloaded on a "
               "host with different endianness. The utility function pcre_pattern_to_host_byte_order() "
               "can be used to convert such a pattern so that it runs on the new host (PCRE_ERROR_BADENDIANNESS)";
    case PCRE_ERROR_DFA_BADRESTART:
        return "PCRE_ERROR_DFA_BADRESTART";
#if PCRE_MAJOR >= 8 && PCRE_MINOR >= 32
    case PCRE_ERROR_BADLENGTH:
        return "This error is given if pcre_exec() is called with a negative value for the length argument (PCRE_ERROR_BADLENGTH)";
    case PCRE_ERROR_JIT_BADOPTION:
        return "This error is returned when a pattern that was successfully studied using a JIT compile "
               "option is being matched, but the matching mode (partial or complete match) does not correspond "
               "to any JIT compilation mode. When the JIT fast path function is used, this error may be "
               "also given for invalid options. See the pcrejit documentation for more details (PCRE_ERROR_JIT_BADOPTION)";
#endif
    }
    return "";
}

#endif // HAVE_RULES


void CppCheck::executeRules(const std::string &tokenlist, const Tokenizer &tokenizer)
{
    (void)tokenlist;
    (void)tokenizer;

#ifdef HAVE_RULES
    // There is no rule to execute
    if (!hasRule(tokenlist))
        return;

    // Write all tokens in a string that can be parsed by pcre
    std::ostringstream ostr;
    for (const Token *tok = tokenizer.tokens(); tok; tok = tok->next())
        ostr << " " << tok->str();
    const std::string str(ostr.str());

    for (const Project::Rule &rule : mProject.rules) {
        if (rule.pattern.empty() || rule.id.empty() || rule.severity == Severity::none || rule.tokenlist != tokenlist)
            continue;

        const char *pcreCompileErrorStr = nullptr;
        int erroffset = 0;
        pcre * const re = pcre_compile(rule.pattern.c_str(),0,&pcreCompileErrorStr,&erroffset,nullptr);
        if (!re) {
            if (pcreCompileErrorStr) {
                const std::string msg = "pcre_compile failed: " + std::string(pcreCompileErrorStr);
                const ErrorMessage errmsg(std::list<ErrorMessage::FileLocation>(),
                                          emptyString,
                                          Severity::error,
                                          msg,
                                          "pcre_compile",
                                          Certainty::safe);

                reportErr(errmsg);
            }
            continue;
        }

        // Optimize the regex, but only if PCRE_CONFIG_JIT is available
#ifdef PCRE_CONFIG_JIT
        const char *pcreStudyErrorStr = nullptr;
        pcre_extra * const pcreExtra = pcre_study(re, PCRE_STUDY_JIT_COMPILE, &pcreStudyErrorStr);
        // pcre_study() returns NULL for both errors and when it can not optimize the regex.
        // The last argument is how one checks for errors.
        // It is NULL if everything works, and points to an error string otherwise.
        if (pcreStudyErrorStr) {
            const std::string msg = "pcre_study failed: " + std::string(pcreStudyErrorStr);
            const ErrorMessage errmsg(std::list<ErrorMessage::FileLocation>(),
                                      emptyString,
                                      Severity::error,
                                      msg,
                                      "pcre_study",
                                      Certainty::safe);

            reportErr(errmsg);
            // pcre_compile() worked, but pcre_study() returned an error. Free the resources allocated by pcre_compile().
            pcre_free(re);
            continue;
        }
#else
        const pcre_extra * const pcreExtra = nullptr;
#endif

        int pos = 0;
        int ovector[30]= {0};
        while (pos < (int)str.size()) {
            const int pcreExecRet = pcre_exec(re, pcreExtra, str.c_str(), (int)str.size(), pos, 0, ovector, 30);
            if (pcreExecRet < 0) {
                const std::string errorMessage = pcreErrorCodeToString(pcreExecRet);
                if (!errorMessage.empty()) {
                    const ErrorMessage errmsg(std::list<ErrorMessage::FileLocation>(),
                                              emptyString,
                                              Severity::error,
                                              std::string("pcre_exec failed: ") + errorMessage,
                                              "pcre_exec",
                                              Certainty::safe);

                    reportErr(errmsg);
                }
                break;
            }
            const unsigned int pos1 = (unsigned int)ovector[0];
            const unsigned int pos2 = (unsigned int)ovector[1];

            // jump to the end of the match for the next pcre_exec
            pos = (int)pos2;

            // determine location..
            ErrorMessage::FileLocation loc;
            loc.setfile(tokenizer.list.getSourceFilePath());
            loc.line = 0;

            std::size_t len = 0;
            for (const Token *tok = tokenizer.tokens(); tok; tok = tok->next()) {
                len = len + 1U + tok->str().size();
                if (len > pos1) {
                    loc.setfile(tokenizer.list.getFiles().at(tok->fileIndex()));
                    loc.line = tok->linenr();
                    break;
                }
            }

            const std::list<ErrorMessage::FileLocation> callStack(1, loc);

            // Create error message
            std::string summary;
            if (rule.summary.empty())
                summary = "found '" + str.substr(pos1, pos2 - pos1) + "'";
            else
                summary = rule.summary;
            const ErrorMessage errmsg(callStack, tokenizer.list.getSourceFilePath(), rule.severity, summary, rule.id, Certainty::safe);

            // Report error
            reportErr(errmsg);
        }

        pcre_free(re);
#ifdef PCRE_CONFIG_JIT
        // Free up the EXTRA PCRE value (may be NULL at this point)
        if (pcreExtra) {
            pcre_free_study(pcreExtra);
        }
#endif
    }
#endif
}


void CppCheck::tooManyConfigsError(const std::string &file, const std::size_t numberOfConfigurations)
{
    if (!mProject.severity.isEnabled(Severity::information) && !mTooManyConfigs)
        return;

    mTooManyConfigs = false;

    if (mProject.severity.isEnabled(Severity::information) && file.empty())
        return;

    std::list<ErrorMessage::FileLocation> loclist;
    if (!file.empty()) {
        ErrorMessage::FileLocation location;
        location.setfile(file);
        loclist.push_back(location);
    }

    std::ostringstream msg;
    msg << "Too many #ifdef configurations - cppcheck only checks " << mProject.maxConfigs;
    if (numberOfConfigurations > mProject.maxConfigs)
        msg << " of " << numberOfConfigurations << " configurations. Use --force to check all configurations.\n";
    if (file.empty())
        msg << " configurations. Use --force to check all configurations. For more details, use --enable=information.\n";
    msg << "The checking of the file will be interrupted because there are too many "
        "#ifdef configurations. Checking of all #ifdef configurations can be forced "
        "by --force command line option or from GUI preferences. However that may "
        "increase the checking time.";
    if (file.empty())
        msg << " For more details, use --enable=information.";


    ErrorMessage errmsg(loclist,
                        emptyString,
                        Severity::information,
                        msg.str(),
                        "toomanyconfigs",
                        Certainty::safe,
                        CWE398);

    reportErr(errmsg);
}

void CppCheck::purgedConfigurationMessage(const std::string &file, const std::string& configuration)
{
    mTooManyConfigs = false;

    if (mProject.severity.isEnabled(Severity::information) && file.empty())
        return;

    std::list<ErrorMessage::FileLocation> loclist;
    if (!file.empty()) {
        ErrorMessage::FileLocation location;
        location.setfile(file);
        loclist.push_back(location);
    }

    ErrorMessage errmsg(loclist,
                        emptyString,
                        Severity::information,
                        "The configuration '" + configuration + "' was not checked because its code equals another one.",
                        "purgedConfiguration",
                        Certainty::safe);

    reportErr(errmsg);
}

//---------------------------------------------------------------------------

void CppCheck::reportErr(const ErrorMessage &msg)
{
    mSuppressInternalErrorFound = false;

    if (!mProject.library.reportErrors(msg.file0))
        return;

    const std::string errmsg = msg.toString(mSettings.verbose);
    if (errmsg.empty())
        return;

    // Alert only about unique errors
    if (std::find(mErrorList.begin(), mErrorList.end(), errmsg) != mErrorList.end())
        return;

    const Suppressions::ErrorMessage errorMessage = msg.toSuppressionsErrorMessage();

    if (mUseGlobalSuppressions) {
        if (mProject.nomsg.isSuppressed(errorMessage)) {
            mSuppressInternalErrorFound = true;
            return;
        }
    } else {
        if (mProject.nomsg.isSuppressedLocal(errorMessage)) {
            mSuppressInternalErrorFound = true;
            return;
        }
    }

    if (!mProject.nofail.isSuppressed(errorMessage) && !mProject.nomsg.isSuppressed(errorMessage)) {
        mExitCode = 1;
    }

    mErrorList.push_back(errmsg);

    mErrorLogger.reportErr(msg);
    if (mCTU)
        mCTU->reportErr(msg);
}

void CppCheck::reportOut(const std::string &outmsg)
{
    mErrorLogger.reportOut(outmsg);
}

void CppCheck::reportProgress(const std::string &filename, const char stage[], const std::size_t value)
{
    mErrorLogger.reportProgress(filename, stage, value);
}

void CppCheck::getErrorMessages()
{
    Settings s(mSettings);
    Project p(mProject);
    p.severity.enable(Severity::warning);
    p.severity.enable(Severity::style);
    p.severity.enable(Severity::portability);
    p.severity.enable(Severity::performance);
    p.severity.enable(Severity::information);

    purgedConfigurationMessage("","");

    mTooManyConfigs = true;
    tooManyConfigsError("",0U);

    Context ctx(this, &s, &p);

    // call all "getErrorMessages" in all registered Check classes
    for (std::list<Check *>::const_iterator it = Check::instances().begin(); it != Check::instances().end(); ++it)
        (*it)->getErrorMessages(ctx);

    s.checkConfiguration = true;
    Preprocessor::getErrorMessages(ctx);
}

bool CppCheck::analyseWholeProgram(AnalyzerInformation& analyzerInformation)
{
    bool errors = false;

    Context ctx(this, &mSettings, &mProject);

    // Init CTU
    CTU::maxCtuDepth = mProject.maxCtuDepth;
    // Analyse the tokens
    CTU::CTUInfo combinedCTU(emptyString, 0, emptyString); /// TODO: Remove this
    for (auto it = analyzerInformation.getCTUs().cbegin(); it != analyzerInformation.getCTUs().cend(); ++it) {
        combinedCTU.functionCalls.insert(combinedCTU.functionCalls.end(), it->functionCalls.begin(), it->functionCalls.end());
        combinedCTU.nestedCalls.insert(combinedCTU.nestedCalls.end(), it->nestedCalls.begin(), it->nestedCalls.end());
    }
    for (Check* check : Check::instances())
        errors |= check->analyseWholeProgram(&combinedCTU, analyzerInformation, ctx);
    return errors && (mExitCode > 0);
}
