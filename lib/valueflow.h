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

//---------------------------------------------------------------------------
#ifndef valueflowH
#define valueflowH
//---------------------------------------------------------------------------

#include "config.h"
#include "mathlib.h"
#include "utils.h"

#include <list>
#include <string>
#include <utility>
#include <vector>

class ErrorLogger;
class Settings;
class SymbolDatabase;
class Token;
class TokenList;
class ValueType;
class Variable;

namespace ValueFlow {
    struct increment {
        template <class T>
        void operator()(T& x) const {
            x++;
        }
    };
    struct decrement {
        template <class T>
        void operator()(T& x) const {
            x--;
        }
    };
    class CPPCHECKLIB Value {
    public:
        typedef std::pair<const Token *, std::string> ErrorPathItem;
        typedef std::list<ErrorPathItem> ErrorPath;

        explicit Value(long long val = 0)
            : valueType(ValueType::INT),
              bound(Bound::Point),
              safe(false),
              conditional(false),
              intvalue(val),
              tokvalue(nullptr),
              floatValue(0.0),
              varvalue(val),
              condition(nullptr),
              varId(0U),
              indirect(0),
              path(0),
              defaultArg(false),
              moveKind(MoveKind::NonMovedVariable),
              lifetimeKind(LifetimeKind::Object),
              lifetimeScope(LifetimeScope::Local),
              valueKind(ValueKind::Possible)
        {}
        Value(const Token *c, long long val);

        bool equalValue(const ValueFlow::Value& rhs) const {
            if (valueType != rhs.valueType)
                return false;
            switch (valueType) {
            case ValueType::INT:
                if (intvalue != rhs.intvalue)
                    return false;
                break;
            case ValueType::TOK:
                if (tokvalue != rhs.tokvalue)
                    return false;
                break;
            case ValueType::FLOAT:
                // TODO: Write some better comparison
                if (floatValue > rhs.floatValue || floatValue < rhs.floatValue)
                    return false;
                break;
            case ValueType::MOVED:
                if (moveKind != rhs.moveKind)
                    return false;
                break;
            case ValueType::UNINIT:
                break;
            case ValueType::BUFFER_SIZE:
                if (intvalue != rhs.intvalue)
                    return false;
                break;
            case ValueType::CONTAINER_SIZE:
                if (intvalue != rhs.intvalue)
                    return false;
                break;
            case ValueType::LIFETIME:
                if (tokvalue != rhs.tokvalue)
                    return false;
            }
            return true;
        }

        template <class F>
        void visitValue(F f) {
            switch (valueType) {
            case ValueType::INT:
            case ValueType::BUFFER_SIZE:
            case ValueType::CONTAINER_SIZE: {
                f(intvalue);
                break;
            }
            case ValueType::FLOAT: {
                f(floatValue);
                break;
            }
            case ValueType::UNINIT:
            case ValueType::TOK:
            case ValueType::LIFETIME:
            case ValueType::MOVED:
                break;
            }
        }

        bool operator==(const Value &rhs) const {
            if (!equalValue(rhs))
                return false;

            return varvalue == rhs.varvalue &&
                   condition == rhs.condition &&
                   varId == rhs.varId &&
                   conditional == rhs.conditional &&
                   defaultArg == rhs.defaultArg &&
                   indirect == rhs.indirect &&
                   valueKind == rhs.valueKind;
        }

        bool operator!=(const Value &rhs) const {
            return !(*this == rhs);
        }

        void decreaseRange() {
            if (bound == Bound::Lower)
                visitValue(increment{});
            else if (bound == Bound::Upper)
                visitValue(decrement{});
        }

        void invertBound() {
            if (bound == Bound::Lower)
                bound = Bound::Upper;
            else if (bound == Bound::Upper)
                bound = Bound::Lower;
        }

        void invertRange() {
            invertBound();
            decreaseRange();
        }

        std::string infoString() const;

        enum ValueType : uint8_t { INT, TOK, FLOAT, MOVED, UNINIT, CONTAINER_SIZE, LIFETIME, BUFFER_SIZE } valueType;
        bool isIntValue() const {
            return valueType == ValueType::INT;
        }
        bool isTokValue() const {
            return valueType == ValueType::TOK;
        }
        bool isFloatValue() const {
            return valueType == ValueType::FLOAT;
        }
        bool isMovedValue() const {
            return valueType == ValueType::MOVED;
        }
        bool isUninitValue() const {
            return valueType == ValueType::UNINIT;
        }
        bool isContainerSizeValue() const {
            return valueType == ValueType::CONTAINER_SIZE;
        }
        bool isLifetimeValue() const {
            return valueType == ValueType::LIFETIME;
        }
        bool isBufferSizeValue() const {
            return valueType == ValueType::BUFFER_SIZE;
        }

        bool isLocalLifetimeValue() const {
            return valueType == ValueType::LIFETIME && lifetimeScope == LifetimeScope::Local;
        }

        bool isArgumentLifetimeValue() const {
            return valueType == ValueType::LIFETIME && lifetimeScope == LifetimeScope::Argument;
        }

        bool isNonValue() const {
            return isMovedValue() || isUninitValue() || isLifetimeValue();
        }

        /** The value bound  */
        enum class Bound : uint8_t { Upper, Lower, Point } bound;

        /** value relies on safe checking */
        bool safe;

        /** Conditional value */
        bool conditional;

        /** int value */
        long long intvalue;

        /** token value - the token that has the value. this is used for pointer aliases, strings, etc. */
        const Token *tokvalue;

        /** float value */
        double floatValue;

        /** For calculated values - variable value that calculated value depends on */
        long long varvalue;

        /** Condition that this value depends on */
        const Token *condition;

        ErrorPath errorPath;

        /** For calculated values - varId that calculated value depends on */
        unsigned int varId;

        int indirect;

        /** Path id */
        MathLib::bigint path;

        /** Is this value passed as default parameter to the function? */
        bool defaultArg;

        /** kind of moved  */
        enum class MoveKind : uint8_t { NonMovedVariable, MovedVariable, ForwardedVariable } moveKind;

        enum class LifetimeKind : uint8_t {Object, SubObject, Lambda, Iterator, Address} lifetimeKind;

        enum class LifetimeScope : uint8_t { Local, Argument } lifetimeScope;

        static const char* toString(MoveKind moveKind);

        /** How known is this value */
        enum class ValueKind : uint8_t {
            /** This value is possible, other unlisted values may also be possible */
            Possible,
            /** Only listed values are possible */
            Known,
            /** Inconclusive */
            Inconclusive,
            /** Listed values are impossible */
            Impossible
        } valueKind;

        void setKnown() {
            valueKind = ValueKind::Known;
        }

        bool isKnown() const {
            return valueKind == ValueKind::Known;
        }

        void setPossible() {
            valueKind = ValueKind::Possible;
        }

        bool isPossible() const {
            return valueKind == ValueKind::Possible;
        }

        bool isImpossible() const {
            return valueKind == ValueKind::Impossible;
        }

        void setImpossible() {
            valueKind = ValueKind::Impossible;
        }

        void setInconclusive(bool inconclusive = true) {
            if (inconclusive)
                valueKind = ValueKind::Inconclusive;
        }

        bool isInconclusive() const {
            return valueKind == ValueKind::Inconclusive;
        }

        void changeKnownToPossible() {
            if (isKnown())
                valueKind = ValueKind::Possible;
        }

        bool errorSeverity() const {
            return !condition && !defaultArg;
        }
    };

    /// Constant folding of expression. This can be used before the full ValueFlow has been executed (ValueFlow::setValues).
    const ValueFlow::Value * valueFlowConstantFoldAST(Token *expr, const Settings *settings);

    /// Perform valueflow analysis.
    void setValues(TokenList *tokenlist, SymbolDatabase* symboldatabase, ErrorLogger *errorLogger, const Settings *settings);

    std::string eitherTheConditionIsRedundant(const Token *condition);

    unsigned int getSizeOf(const ValueType &vt, const Settings *settings);
}

struct LifetimeToken {
    const Token* token;
    ValueFlow::Value::ErrorPath errorPath;
    bool addressOf;
    bool inconclusive;

    LifetimeToken() : token(nullptr), errorPath(), addressOf(false), inconclusive(false) {}

    LifetimeToken(const Token* token, ValueFlow::Value::ErrorPath errorPath)
        : token(token), errorPath(std::move(errorPath)), addressOf(false), inconclusive(false)
    {}

    LifetimeToken(const Token* token, bool addressOf, ValueFlow::Value::ErrorPath errorPath)
        : token(token), errorPath(std::move(errorPath)), addressOf(addressOf), inconclusive(false)
    {}

    static std::vector<LifetimeToken> setAddressOf(std::vector<LifetimeToken> v, bool b) {
        for (LifetimeToken& x : v)
            x.addressOf = b;
        return v;
    }

    static std::vector<LifetimeToken> setInconclusive(std::vector<LifetimeToken> v, bool b) {
        for (LifetimeToken& x : v)
            x.inconclusive = b;
        return v;
    }
};

const Token *parseCompareInt(const Token *tok, ValueFlow::Value &true_value, ValueFlow::Value &false_value);

std::vector<LifetimeToken> getLifetimeTokens(const Token* tok, ValueFlow::Value::ErrorPath errorPath = ValueFlow::Value::ErrorPath{}, int depth = 20);

const Variable* getLifetimeVariable(const Token* tok, ValueFlow::Value::ErrorPath& errorPath, bool* addressOf = nullptr);

const Variable* getLifetimeVariable(const Token* tok);

bool isLifetimeBorrowed(const Token *tok, const Settings *settings);

std::string lifetimeType(const Token *tok, const ValueFlow::Value *val);

std::string lifetimeMessage(const Token *tok, const ValueFlow::Value *val, ValueFlow::Value::ErrorPath &errorPath);

ValueFlow::Value getLifetimeObjValue(const Token *tok);

#endif // valueflowH
