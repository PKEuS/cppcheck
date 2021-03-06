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
#ifndef checkpostfixoperatorH
#define checkpostfixoperatorH
//---------------------------------------------------------------------------

#include "check.h"
#include "config.h"
#include "tokenize.h"

#include <string>

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
    explicit CheckPostfixOperator(const Context& ctx)
        : Check(myName(), ctx) {
    }

    void runChecks(const Context& ctx) override {
        if (ctx.tokenizer->isC())
            return;

        CheckPostfixOperator checkPostfixOperator(ctx);
        checkPostfixOperator.postfixOperator();
    }

    /** Check postfix operators */
    void postfixOperator();

private:
    /** Report Error */
    void postfixOperatorError(const Token *tok);

    void getErrorMessages(const Context& ctx) const override {
        CheckPostfixOperator c(ctx);
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
