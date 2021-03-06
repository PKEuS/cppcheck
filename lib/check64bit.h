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
#ifndef check64bitH
#define check64bitH
//---------------------------------------------------------------------------

#include "check.h"
#include "config.h"

#include <string>


/// @addtogroup Checks
/// @{

/**
 * @brief Check for 64-bit portability issues
 */

class CPPCHECKLIB Check64BitPortability : public Check {
public:
    /** This constructor is used when registering the Check64BitPortability */
    Check64BitPortability() : Check(myName()) {
    }

    /** This constructor is used when running checks. */
    explicit Check64BitPortability(const Context& ctx)
        : Check(myName(), ctx) {
    }

    /** @brief Run checks against the normal token list */
    void runChecks(const Context& ctx) override {
        Check64BitPortability check64BitPortability(ctx);
        check64BitPortability.pointerassignment();
    }

    /** Check for pointer assignment */
    void pointerassignment();

private:

    void assignmentAddressToIntegerError(const Token *tok);
    void assignmentIntegerToAddressError(const Token *tok);
    void returnIntegerError(const Token *tok);
    void returnPointerError(const Token *tok);

    void getErrorMessages(const Context& ctx) const override {
        Check64BitPortability c(ctx);
        c.assignmentAddressToIntegerError(nullptr);
        c.assignmentIntegerToAddressError(nullptr);
        c.returnIntegerError(nullptr);
        c.returnPointerError(nullptr);
    }

    static const char* myName() {
        return "64BitPortability";
    }

    std::string classInfo() const override {
        return "Check if there are 64-bit portability issues:\n"
               "- assign address to/from int/long\n"
               "- casting address from/to integer when returning from function\n";
    }
};
/// @}
//---------------------------------------------------------------------------
#endif // check64bitH
