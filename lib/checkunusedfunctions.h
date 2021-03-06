/*
 * LCppC - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2019 Cppcheck team.
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
#ifndef checkunusedfunctionsH
#define checkunusedfunctionsH
//---------------------------------------------------------------------------

#include "check.h"
#include "config.h"

#include <list>
#include <string>

/// @addtogroup Checks
/** @brief Check for functions never called */
/// @{

class CPPCHECKLIB CheckUnusedFunctions : public Check {
public:
    /** @brief This constructor is used when registering the CheckUnusedFunctions */
    CheckUnusedFunctions() : Check(myName()) {
    }

    /** @brief This constructor is used when running checks. */
    explicit CheckUnusedFunctions(const Context& ctx)
        : Check(myName(), ctx) {
    }

    /** @brief Parse current TU and extract file info */
    Check::FileInfo* getFileInfo(const Context& ctx) const override;

    Check::FileInfo* loadFileInfoFromXml(const tinyxml2::XMLElement* xmlElement) const override;

    /** @brief Analyse all file infos for all TU */
    bool analyseWholeProgram(const CTU::CTUInfo* ctu, AnalyzerInformation& analyzerInformation, const Context& ctx) override;

private:

    void getErrorMessages(const Context& ctx) const override {
        CheckUnusedFunctions c(ctx);
        c.unusedFunctionError(ctx.errorLogger, emptyString, 0, "funcName");
    }

    void runChecks(const Context& ctx) override {
        (void)ctx;
    }

    void unusedFunctionError(ErrorLogger* const errorLogger, const std::string &filename, unsigned int lineNumber, const std::string &funcname);

    static const char* myName() {
        return "UnusedFunction";
    }

    std::string classInfo() const override {
        return "Check for functions that are never called\n";
    }
};
/// @}
//---------------------------------------------------------------------------
#endif // checkunusedfunctionsH
