/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2019 Cppcheck team.
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
#ifndef checkpostfixoperatorH
#define checkpostfixoperatorH
//---------------------------------------------------------------------------

#include "check.h"
#include "config.h"
#include "tokenize.h"

#include <string>

class ErrorLogger;
class Settings;
class Token;

/// @addtogroup Checks
/// @{

/**
 * @brief Using postfix operators ++ or -- rather than postfix operator.
 */

class CPPCHECKLIB CheckPostfixOperator : public Check {
public:
    /** This constructor is used when registering the CheckPostfixOperator */
    CheckPostfixOperator() : Check(myName()) {
    }

    /** This constructor is used when running checks. */
    CheckPostfixOperator(const Tokenizer* tokenizer, const Settings* settings, ErrorLogger* errorLogger, const Project* project)
        : Check(myName(), tokenizer, settings, errorLogger, project) {
    }

    void runChecks(const Tokenizer* tokenizer, const Settings* settings, ErrorLogger* errorLogger, const Project* project) override {
        if (tokenizer->isC())
            return;

        CheckPostfixOperator checkPostfixOperator(tokenizer, settings, errorLogger, project);
        checkPostfixOperator.postfixOperator();
    }

    /** Check postfix operators */
    void postfixOperator();

private:
    /** Report Error */
    void postfixOperatorError(const Token *tok);

    void getErrorMessages(ErrorLogger* errorLogger, const Settings* settings, const Project* project) const override {
        CheckPostfixOperator c(nullptr, settings, errorLogger, project);
        c.postfixOperatorError(nullptr);
    }

    static const char* myName() {
        return "PostfixOperator";
    }

    std::string classInfo() const override {
        return "Warn if using postfix operators ++ or -- rather than prefix operator\n";
    }
};
/// @}
//---------------------------------------------------------------------------
#endif // checkpostfixoperatorH
