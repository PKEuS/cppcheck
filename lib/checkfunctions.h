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
#ifndef checkfunctionsH
#define checkfunctionsH
//---------------------------------------------------------------------------

#include "check.h"
#include "config.h"
#include "library.h"
#include "settings.h"
#include "errortypes.h"

#include <map>
#include <string>
#include <utility>

namespace ValueFlow {
    class Value;
}  // namespace ValueFlow


/// @addtogroup Checks
/// @{

/**
 * @brief Check for bad function usage
 */

class CPPCHECKLIB CheckFunctions : public Check {
public:
    /** This constructor is used when registering the CheckFunctions */
    CheckFunctions() : Check(myName()) {
    }

    /** This constructor is used when running checks. */
    explicit CheckFunctions(const Context& ctx)
        : Check(myName(), ctx) {
    }

    /** @brief Run checks against the normal token list */
    void runChecks(const Context& ctx) override {
        CheckFunctions checkFunctions(ctx);

        // Checks
        checkFunctions.checkIgnoredReturnValue();

        // --check-library : functions with nonmatching configuration
        checkFunctions.checkLibraryMatchFunctions();

        checkFunctions.checkProhibitedFunctions();
        checkFunctions.invalidFunctionUsage();
        checkFunctions.checkMathFunctions();
        checkFunctions.memsetZeroBytes();
        checkFunctions.memsetInvalid2nd3rdParam();
    }

    /** Check for functions that should not be used */
    void checkProhibitedFunctions();

    /**
    * @brief Invalid function usage (invalid input value / overlapping data)
    *
    * %Check that given function parameters are valid according to the standard
    * - wrong radix given for strtol/strtoul
    * - overlapping data when using sprintf/snprintf
    * - wrong input value according to library
    */
    void invalidFunctionUsage();

    /** @brief %Check for ignored return values. */
    void checkIgnoredReturnValue();

    /** @brief %Check for parameters given to math function that do not make sense*/
    void checkMathFunctions();

    /** @brief %Check for filling zero bytes with memset() */
    void memsetZeroBytes();

    /** @brief %Check for invalid 2nd or 3rd parameter of memset() */
    void memsetInvalid2nd3rdParam();

    /** @brief --check-library: warn for unconfigured function calls */
    void checkLibraryMatchFunctions();

private:
    void invalidFunctionArgError(const Token *tok, const std::string &functionName, std::size_t argnr, const ValueFlow::Value *invalidValue, const std::string &validstr);
    void invalidFunctionArgBoolError(const Token *tok, const std::string &functionName, int argnr);
    void invalidFunctionArgStrError(const Token *tok, const std::string &functionName, unsigned int argnr);
    void ignoredReturnValueError(const Token* tok, const std::string& function);
    void ignoredReturnErrorCode(const Token* tok, const std::string& function);
    void mathfunctionCallWarning(const Token *tok, const unsigned int numParam = 1);
    void mathfunctionCallWarning(const Token *tok, const std::string& oldexp, const std::string& newexp);
    void memsetZeroBytesError(const Token *tok);
    void memsetFloatError(const Token *tok, const std::string &var_value);
    void memsetValueOutOfRangeError(const Token *tok, const std::string &value);
    void memsetSizeArgumentAsCharLiteralError(const Token* tok);
    void memsetSizeArgumentAsCharError(const Token* tok);

    void getErrorMessages(const Context& ctx) const override {
        CheckFunctions c(ctx);

        for (std::map<std::string, Library::WarnInfo>::const_iterator i = ctx.project->library.functionwarn.cbegin(); i != ctx.project->library.functionwarn.cend(); ++i) {
            c.reportError(nullptr, Severity::style, i->first+"Called", i->second.message);
        }

        c.invalidFunctionArgError(nullptr, "func_name", 1, nullptr,"1:4");
        c.invalidFunctionArgBoolError(nullptr, "func_name", 1);
        c.invalidFunctionArgStrError(nullptr, "func_name", 1);
        c.ignoredReturnValueError(nullptr, "malloc");
        c.mathfunctionCallWarning(nullptr);
        c.mathfunctionCallWarning(nullptr, "1 - erf(x)", "erfc(x)");
        c.memsetZeroBytesError(nullptr);
        c.memsetFloatError(nullptr,  "varname");
        c.memsetValueOutOfRangeError(nullptr,  "varname");
        c.memsetSizeArgumentAsCharLiteralError(nullptr);
        c.memsetSizeArgumentAsCharError(nullptr);
    }

    static const char* myName() {
        return "Functions";
    }

    std::string classInfo() const override {
        return "Check function usage:\n"
               "- return value of certain functions not used\n"
               "- invalid input values for functions\n"
               "- Warn if a function is called whose usage is discouraged\n"
               "- memset() third argument is zero\n"
               "- memset() with a value out of range as the 2nd parameter\n"
               "- memset() with a float as the 2nd parameter\n"
               "- memset() with a char as the 3nd parameter\n";
    }
};
/// @}
//---------------------------------------------------------------------------
#endif // checkfunctionsH
