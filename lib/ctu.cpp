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
#include "ctu.h"
#include "astutils.h"
#include "settings.h"
#include "symboldatabase.h"
#include "tokenize.h"

#include <tinyxml2.h>
#include <iterator>  // back_inserter
//---------------------------------------------------------------------------

static const char ATTR_CALL_ID[] = "call-id";
static const char ATTR_CALL_FUNCNAME[] = "call-funcname";
static const char ATTR_CALL_ARGNR[] = "call-argnr";
static const char ATTR_CALL_ARGEXPR[] = "call-argexpr";
static const char ATTR_CALL_ARGVALUETYPE[] = "call-argvaluetype";
static const char ATTR_CALL_ARGVALUE[] = "call-argvalue";
static const char ATTR_WARNING[] = "warning";
static const char ATTR_LOC_FILENAME[] = "file";
static const char ATTR_LOC_LINENR[] = "line";
static const char ATTR_LOC_COLUMN[] = "col";
static const char ATTR_INFO[] = "info";
static const char ATTR_MY_ID[] = "my-id";
static const char ATTR_MY_ARGNR[] = "my-argnr";
static const char ATTR_MY_ARGNAME[] = "my-argname";
static const char ATTR_VALUE[] = "value";

int CTU::maxCtuDepth = 2;

static std::string getFunctionId(const Tokenizer *tokenizer, const Function *function)
{
    return tokenizer->list.file(function->tokenDef) + ':' + MathLib::toString(function->tokenDef->linenr()) + ':' + MathLib::toString(function->tokenDef->column());
}

CTU::CTUInfo::Location::Location(const Tokenizer *tokenizer, const Token *tok)
    : fileName(tokenizer->list.file(tok))
    , lineNumber(tok->linenr())
    , column(tok->column())
{
}

tinyxml2::XMLElement* CTU::CTUInfo::FunctionCall::toXMLElement(tinyxml2::XMLDocument* doc) const
{
    tinyxml2::XMLElement* entry = doc->NewElement("function-call");
    entry->SetAttribute(ATTR_CALL_ID, callId.c_str());
    entry->SetAttribute(ATTR_CALL_FUNCNAME, callFunctionName.c_str());
    entry->SetAttribute(ATTR_CALL_ARGNR, callArgNr);
    entry->SetAttribute(ATTR_LOC_FILENAME, location.fileName.c_str());
    entry->SetAttribute(ATTR_LOC_LINENR, location.lineNumber);
    entry->SetAttribute(ATTR_LOC_COLUMN, location.column);
    entry->SetAttribute(ATTR_CALL_ARGEXPR, callArgumentExpression.c_str());
    entry->SetAttribute(ATTR_MY_ARGNR, callValueType);
    entry->SetAttribute(ATTR_CALL_ARGVALUE, (int64_t)callArgValue);
    if (warning)
        entry->SetAttribute(ATTR_WARNING, true);

    for (const ErrorMessage::FileLocation &loc : callValuePath) {
        tinyxml2::XMLElement* path = doc->NewElement("path");
        entry->SetAttribute(ATTR_LOC_FILENAME, loc.getFileNative().c_str());
        entry->SetAttribute(ATTR_LOC_LINENR, loc.line);
        entry->SetAttribute(ATTR_LOC_COLUMN, loc.column);
        entry->SetAttribute(ATTR_INFO, loc.getinfo().c_str());
        entry->InsertEndChild(path);
    }
    return entry;
}

tinyxml2::XMLElement* CTU::CTUInfo::NestedCall::toXMLElement(tinyxml2::XMLDocument* doc) const
{
    tinyxml2::XMLElement* entry = doc->NewElement("nested-call");
    entry->SetAttribute(ATTR_MY_ID, myId.c_str());
    entry->SetAttribute(ATTR_MY_ARGNR, myArgNr);
    return entry;
}

tinyxml2::XMLElement* CTU::CTUInfo::UnsafeUsage::toXMLElement(tinyxml2::XMLDocument* doc) const
{
    tinyxml2::XMLElement* entry = doc->NewElement("unsafe-usage");
    entry->SetAttribute(ATTR_MY_ID, myId.c_str());
    entry->SetAttribute(ATTR_MY_ARGNR, myArgNr);
    entry->SetAttribute(ATTR_MY_ARGNAME, myArgumentName.c_str());
    entry->SetAttribute(ATTR_LOC_FILENAME, location.fileName.c_str());
    entry->SetAttribute(ATTR_LOC_LINENR, location.lineNumber);
    entry->SetAttribute(ATTR_LOC_COLUMN, location.column);
    entry->SetAttribute(ATTR_VALUE, (int64_t)value);
    return entry;
}

CTU::CTUInfo::CallBase::CallBase(const Tokenizer *tokenizer, const Token *callToken)
    : callId(getFunctionId(tokenizer, callToken->function()))
    , callArgNr(0)
    , callFunctionName(callToken->next()->astOperand1()->expressionString())
    , location(CTU::CTUInfo::Location(tokenizer, callToken))
{
}

CTU::CTUInfo::NestedCall::NestedCall(const Tokenizer *tokenizer, const Function *myFunction, const Token *callToken)
    : CallBase(tokenizer, callToken)
    , myId(getFunctionId(tokenizer, myFunction))
    , myArgNr(0)
{
}

static std::string readAttrString(const tinyxml2::XMLElement *e, const char *attr, bool *error)
{
    const char *value = e->Attribute(attr);
    if (!value && error)
        *error = true;
    return value ? value : "";
}

static int64_t readAttrInt64(const tinyxml2::XMLElement *e, const char *attr, bool *error)
{
    int64_t value = 0;
    if (e->QueryInt64Attribute(attr, &value) == tinyxml2::XML_WRONG_ATTRIBUTE_TYPE)
        *error = true;
    return value;
}

static unsigned int readAttrUInt(const tinyxml2::XMLElement* e, const char* attr, bool* error)
{
    unsigned int value = 0;
    if (e->QueryUnsignedAttribute(attr, &value) == tinyxml2::XML_WRONG_ATTRIBUTE_TYPE)
        *error = true;
    return value;
}

bool CTU::CTUInfo::CallBase::loadBaseFromXml(const tinyxml2::XMLElement *xmlElement)
{
    bool error = false;
    callId = readAttrString(xmlElement, ATTR_CALL_ID, &error);
    callFunctionName = readAttrString(xmlElement, ATTR_CALL_FUNCNAME, &error);
    callArgNr = readAttrUInt(xmlElement, ATTR_CALL_ARGNR, &error);
    location.fileName = readAttrString(xmlElement, ATTR_LOC_FILENAME, &error);
    location.lineNumber = readAttrUInt(xmlElement, ATTR_LOC_LINENR, &error);
    location.column = readAttrUInt(xmlElement, ATTR_LOC_COLUMN, &error);
    return !error;
}

bool CTU::CTUInfo::FunctionCall::loadFromXml(const tinyxml2::XMLElement *xmlElement)
{
    if (!loadBaseFromXml(xmlElement))
        return false;
    bool error=false;
    callArgumentExpression = readAttrString(xmlElement, ATTR_CALL_ARGEXPR, &error);
    callValueType = (ValueFlow::Value::ValueType)readAttrUInt(xmlElement, ATTR_CALL_ARGVALUETYPE, &error);
    callArgValue = readAttrInt64(xmlElement, ATTR_CALL_ARGVALUE, &error);
    const char *w = xmlElement->Attribute(ATTR_WARNING);
    warning = w && std::strcmp(w, "true") == 0;
    for (const tinyxml2::XMLElement *e2 = xmlElement->FirstChildElement(); !error && e2; e2 = e2->NextSiblingElement()) {
        if (std::strcmp(e2->Name(), "path") != 0)
            continue;
        ErrorMessage::FileLocation loc;
        loc.setfile(readAttrString(e2, ATTR_LOC_FILENAME, &error));
        loc.line = readAttrInt64(e2, ATTR_LOC_LINENR, &error);
        loc.column = readAttrUInt(e2, ATTR_LOC_COLUMN, &error);
        loc.setinfo(readAttrString(e2, ATTR_INFO, &error));
    }
    return !error;
}

bool CTU::CTUInfo::NestedCall::loadFromXml(const tinyxml2::XMLElement *xmlElement)
{
    if (!loadBaseFromXml(xmlElement))
        return false;
    bool error = false;
    myId = readAttrString(xmlElement, ATTR_MY_ID, &error);
    myArgNr = readAttrUInt(xmlElement, ATTR_MY_ARGNR, &error);
    return !error;
}

void CTU::CTUInfo::loadFromXml(const tinyxml2::XMLElement *xmlElement)
{
    for (const tinyxml2::XMLElement *e = xmlElement->FirstChildElement(); e; e = e->NextSiblingElement()) {
        if (std::strcmp(e->Name(), "function-call") == 0) {
            FunctionCall functionCall;
            if (functionCall.loadFromXml(e))
                functionCalls.push_back(functionCall);
        } else if (std::strcmp(e->Name(), "nested-call") == 0) {
            NestedCall nestedCall;
            if (nestedCall.loadFromXml(e))
                nestedCalls.push_back(nestedCall);
        }
    }
}

std::map<std::string, std::vector<const CTU::CTUInfo::CallBase *>> CTU::CTUInfo::getCallsMap() const
{
    std::map<std::string, std::vector<const CTU::CTUInfo::CallBase *>> ret;
    for (const CTU::CTUInfo::NestedCall &nc : nestedCalls)
        ret[nc.callId].push_back(&nc);
    for (const CTU::CTUInfo::FunctionCall &fc : functionCalls)
        ret[fc.callId].push_back(&fc);
    return ret;
}

std::list<CTU::CTUInfo::UnsafeUsage> CTU::loadUnsafeUsageListFromXml(const tinyxml2::XMLElement *xmlElement)
{
    std::list<CTU::CTUInfo::UnsafeUsage> ret;
    for (const tinyxml2::XMLElement *e = xmlElement->FirstChildElement(); e; e = e->NextSiblingElement()) {
        if (std::strcmp(e->Name(), "unsafe-usage") != 0)
            continue;
        bool error = false;
        CTUInfo::UnsafeUsage unsafeUsage;
        unsafeUsage.myId = readAttrString(e, ATTR_MY_ID, &error);
        unsafeUsage.myArgNr = readAttrUInt(e, ATTR_MY_ARGNR, &error);
        unsafeUsage.myArgumentName = readAttrString(e, ATTR_MY_ARGNAME, &error);
        unsafeUsage.location.fileName = readAttrString(e, ATTR_LOC_FILENAME, &error);
        unsafeUsage.location.lineNumber = readAttrUInt(e, ATTR_LOC_LINENR, &error);
        unsafeUsage.location.column = readAttrUInt(e, ATTR_LOC_COLUMN, &error);
        unsafeUsage.value = readAttrInt64(e, ATTR_VALUE, &error);

        if (!error)
            ret.push_back(unsafeUsage);
    }
    return ret;
}

static int isCallFunction(const Scope *scope, std::size_t argnr, const Token **tok)
{
    const Variable * const argvar = scope->function->getArgumentVar(argnr);
    if (!argvar->isPointer())
        return -1;
    for (const Token *tok2 = scope->bodyStart; tok2 != scope->bodyEnd; tok2 = tok2->next()) {
        if (tok2->variable() != argvar)
            continue;
        if (!Token::Match(tok2->previous(), "[(,] %var% [,)]"))
            break;
        int argnr2 = 1;
        const Token *prev = tok2;
        while (prev && prev->str() != "(") {
            if (Token::Match(prev,"]|)"))
                prev = prev->link();
            else if (prev->str() == ",")
                ++argnr2;
            prev = prev->previous();
        }
        if (!prev || !Token::Match(prev->previous(), "%name% ("))
            break;
        if (!prev->astOperand1() || !prev->astOperand1()->function())
            break;
        *tok = prev->previous();
        return argnr2;
    }
    return -1;
}


void CTU::CTUInfo::parseTokens(const Tokenizer *tokenizer)
{
    const SymbolDatabase * const symbolDatabase = tokenizer->getSymbolDatabase();

    // Parse all functions in TU
    for (const Scope &scope : symbolDatabase->scopeList) {
        if (!scope.isExecutable() || scope.type != Scope::eFunction || !scope.function)
            continue;
        const Function *const function = scope.function;

        // source function calls
        for (const Token *tok = scope.bodyStart; tok != scope.bodyEnd; tok = tok->next()) {
            if (tok->str() != "(" || !tok->astOperand1() || !tok->astOperand2())
                continue;
            if (!tok->astOperand1()->function())
                continue;
            const std::vector<const Token *> args(getArguments(tok->previous()));
            for (unsigned int argnr = 0; argnr < args.size(); ++argnr) {
                const Token *argtok = args[argnr];
                if (!argtok)
                    continue;
                for (const ValueFlow::Value &value : argtok->values()) {
                    if ((!value.isIntValue() || value.intvalue != 0 || value.isInconclusive()) && !value.isBufferSizeValue())
                        continue;
                    // Skip impossible values since they cannot be represented
                    if (value.isImpossible())
                        continue;
                    CTUInfo::FunctionCall functionCall;
                    functionCall.callValueType = value.valueType;
                    functionCall.callId = getFunctionId(tokenizer, tok->astOperand1()->function());
                    functionCall.callFunctionName = tok->astOperand1()->expressionString();
                    functionCall.location = CTUInfo::Location(tokenizer,tok);
                    functionCall.callArgNr = argnr + 1;
                    functionCall.callArgumentExpression = argtok->expressionString();
                    functionCall.callArgValue = value.intvalue;
                    functionCall.warning = !value.errorSeverity();
                    for (const ErrorPathItem &i : value.errorPath) {
                        ErrorMessage::FileLocation loc;
                        loc.setfile(tokenizer->list.file(i.first));
                        loc.line = i.first->linenr();
                        loc.column = i.first->column();
                        loc.setinfo(i.second);
                        functionCall.callValuePath.push_back(loc);
                    }
                    functionCalls.push_back(functionCall);
                }
                // array
                if (argtok->variable() && argtok->variable()->isArray() && argtok->variable()->dimensions().size()==1 && argtok->variable()->dimension(0)>1) {
                    CTUInfo::FunctionCall functionCall;
                    functionCall.callValueType = ValueFlow::Value::ValueType::BUFFER_SIZE;
                    functionCall.callId = getFunctionId(tokenizer, tok->astOperand1()->function());
                    functionCall.callFunctionName = tok->astOperand1()->expressionString();
                    functionCall.location = CTUInfo::Location(tokenizer, tok);
                    functionCall.callArgNr = argnr + 1;
                    functionCall.callArgumentExpression = argtok->expressionString();
                    functionCall.callArgValue = argtok->variable()->dimension(0) * argtok->valueType()->typeSize(*tokenizer->list.getProject());
                    functionCall.warning = false;
                    functionCalls.push_back(functionCall);
                }
                // &var => buffer
                if (argtok->isUnaryOp("&") && argtok->astOperand1()->variable() && argtok->astOperand1()->valueType() && !argtok->astOperand1()->variable()->isArray()) {
                    CTUInfo::FunctionCall functionCall;
                    functionCall.callValueType = ValueFlow::Value::ValueType::BUFFER_SIZE;
                    functionCall.callId = getFunctionId(tokenizer, tok->astOperand1()->function());
                    functionCall.callFunctionName = tok->astOperand1()->expressionString();
                    functionCall.location = CTUInfo::Location(tokenizer, tok);
                    functionCall.callArgNr = argnr + 1;
                    functionCall.callArgumentExpression = argtok->expressionString();
                    functionCall.callArgValue = argtok->astOperand1()->valueType()->typeSize(*tokenizer->list.getProject());
                    functionCall.warning = false;
                    functionCalls.push_back(functionCall);
                }
                // pointer to uninitialized data..
                if (!argtok->isUnaryOp("&"))
                    continue;
                argtok = argtok->astOperand1();
                if (!argtok || !argtok->valueType() || argtok->valueType()->pointer != 0)
                    continue;
                if (argtok->values().size() != 1U)
                    continue;
                const ValueFlow::Value &v = argtok->values().front();
                if (v.valueType == ValueFlow::Value::ValueType::UNINIT && !v.isInconclusive()) {
                    CTUInfo::FunctionCall functionCall;
                    functionCall.callValueType = ValueFlow::Value::ValueType::UNINIT;
                    functionCall.callId = getFunctionId(tokenizer, tok->astOperand1()->function());
                    functionCall.callFunctionName = tok->astOperand1()->expressionString();
                    functionCall.location = CTUInfo::Location(tokenizer, tok);
                    functionCall.callArgNr = argnr + 1;
                    functionCall.callArgValue = 0;
                    functionCall.callArgumentExpression = argtok->expressionString();
                    functionCall.warning = false;
                    functionCalls.push_back(functionCall);
                    continue;
                }
            }
        }

        // Nested function calls
        for (std::size_t argnr = 0; argnr < function->argCount(); ++argnr) {
            const Token *tok;
            const int argnr2 = isCallFunction(&scope, argnr, &tok);
            if (argnr2 > 0) {
                CTUInfo::NestedCall nestedCall(tokenizer, function, tok);
                nestedCall.myArgNr = argnr + 1;
                nestedCall.callArgNr = argnr2;
                nestedCalls.push_back(nestedCall);
            }
        }
    }
}

static std::vector<std::pair<const Token *, MathLib::bigint>> getUnsafeFunction(const Tokenizer *tokenizer, const Project* project, const Scope *scope, std::size_t argnr, const Check *check, bool (*isUnsafeUsage)(const Check *check, const Token *argtok, MathLib::bigint *value))
{
    std::vector<std::pair<const Token *, MathLib::bigint>> ret;
    const Variable * const argvar = scope->function->getArgumentVar(argnr);
    if (!argvar->isArrayOrPointer())
        return ret;
    for (const Token *tok2 = scope->bodyStart; tok2 != scope->bodyEnd; tok2 = tok2->next()) {
        if (Token::Match(tok2, ")|else {")) {
            tok2 = tok2->linkAt(1);
            if (Token::findmatch(tok2->link(), "return|throw", tok2))
                return ret;
            int indirect = 0;
            if (argvar->valueType())
                indirect = argvar->valueType()->pointer;
            if (isVariableChanged(tok2->link(), tok2, indirect, argvar->declarationId(), false, project, tokenizer->isCPP()))
                return ret;
        }
        if (Token::Match(tok2, "%oror%|&&|?")) {
            tok2 = tok2->findExpressionStartEndTokens().second;
            continue;
        }
        if (tok2->variable() != argvar)
            continue;
        MathLib::bigint value = 0;
        if (!isUnsafeUsage(check, tok2, &value))
            return ret; // TODO: Is this a read? then continue..
        ret.emplace_back(tok2, value);
        return ret;
    }
    return ret;
}

std::list<CTU::CTUInfo::UnsafeUsage> CTU::getUnsafeUsage(const Context& ctx, const Check *check, bool (*isUnsafeUsage)(const Check *check, const Token *argtok, MathLib::bigint *_value))
{
    std::list<CTU::CTUInfo::UnsafeUsage> unsafeUsage;

    // Parse all functions in TU

    for (const Scope &scope : ctx.symbolDB->scopeList) {
        if (!scope.isExecutable() || scope.type != Scope::eFunction || !scope.function)
            continue;
        const Function *const function = scope.function;

        // "Unsafe" functions unconditionally reads data before it is written..
        for (std::size_t argnr = 0; argnr < function->argCount(); ++argnr) {
            for (const std::pair<const Token *, MathLib::bigint> &v : getUnsafeFunction(ctx.tokenizer, ctx.project, &scope, argnr, check, isUnsafeUsage)) {
                const Token *tok = v.first;
                MathLib::bigint value = v.second;
                unsafeUsage.emplace_back(getFunctionId(ctx.tokenizer, function), static_cast<unsigned int>(argnr+1), tok->str(), CTU::CTUInfo::Location(ctx.tokenizer,tok), value);
            }
        }
    }

    return unsafeUsage;
}

static bool findPath(const std::string &callId,
                     unsigned int callArgNr,
                     MathLib::bigint unsafeValue,
                     CTU::CTUInfo::InvalidValueType invalidValue,
                     const std::map<std::string, std::vector<const CTU::CTUInfo::CallBase *>> &callsMap,
                     const CTU::CTUInfo::CallBase *path[10],
                     int index,
                     bool warning)
{
    if (index >= CTU::maxCtuDepth || index >= 10)
        return false;

    const std::map<std::string, std::vector<const CTU::CTUInfo::CallBase *>>::const_iterator it = callsMap.find(callId);
    if (it == callsMap.end())
        return false;

    for (const CTU::CTUInfo::CallBase *c : it->second) {
        if (c->callArgNr != callArgNr)
            continue;

        const CTU::CTUInfo::FunctionCall *functionCall = dynamic_cast<const CTU::CTUInfo::FunctionCall *>(c);
        if (functionCall) {
            if (!warning && functionCall->warning)
                continue;
            switch (invalidValue) {
            case CTU::CTUInfo::InvalidValueType::null:
                if (functionCall->callValueType != ValueFlow::Value::ValueType::INT || functionCall->callArgValue != 0)
                    continue;
                break;
            case CTU::CTUInfo::InvalidValueType::uninit:
                if (functionCall->callValueType != ValueFlow::Value::ValueType::UNINIT)
                    continue;
                break;
            case CTU::CTUInfo::InvalidValueType::bufferOverflow:
                if (functionCall->callValueType != ValueFlow::Value::ValueType::BUFFER_SIZE)
                    continue;
                if (unsafeValue < 0 || unsafeValue >= functionCall->callArgValue)
                    break;
                continue;
            }
            path[index] = functionCall;
            return true;
        }

        const CTU::CTUInfo::NestedCall *nestedCall = dynamic_cast<const CTU::CTUInfo::NestedCall *>(c);
        if (!nestedCall)
            continue;

        if (findPath(nestedCall->myId, nestedCall->myArgNr, unsafeValue, invalidValue, callsMap, path, index + 1, warning)) {
            path[index] = nestedCall;
            return true;
        }
    }

    return false;
}

std::list<ErrorMessage::FileLocation> CTU::CTUInfo::getErrorPath(InvalidValueType invalidValue,
        const CTU::CTUInfo::UnsafeUsage &unsafeUsage,
        const std::map<std::string, std::vector<const CTU::CTUInfo::CallBase *>> &callsMap,
        const char info[],
        const FunctionCall * * const functionCallPtr,
        bool warning) const
{
    std::list<ErrorMessage::FileLocation> locationList;

    const CTU::CTUInfo::CallBase *path[10] = {nullptr};

    if (!findPath(unsafeUsage.myId, unsafeUsage.myArgNr, unsafeUsage.value, invalidValue, callsMap, path, 0, warning))
        return locationList;

    const std::string value1 = (invalidValue == InvalidValueType::null) ? "null" : "uninitialized";

    for (int index = 9; index >= 0; index--) {
        if (!path[index])
            continue;

        const CTU::CTUInfo::FunctionCall *functionCall = dynamic_cast<const CTU::CTUInfo::FunctionCall *>(path[index]);

        if (functionCall) {
            if (functionCallPtr)
                *functionCallPtr = functionCall;
            std::copy(functionCall->callValuePath.cbegin(), functionCall->callValuePath.cend(), std::back_inserter(locationList));
        }

        ErrorMessage::FileLocation fileLoc(path[index]->location.fileName, path[index]->location.lineNumber, path[index]->location.column);
        fileLoc.setinfo("Calling function " + path[index]->callFunctionName + ", " + MathLib::toString(path[index]->callArgNr) + getOrdinalText(path[index]->callArgNr) + " argument is " + value1);
        locationList.push_back(fileLoc);
    }

    ErrorMessage::FileLocation fileLoc2(unsafeUsage.location.fileName, unsafeUsage.location.lineNumber, unsafeUsage.location.column);
    fileLoc2.setinfo(replaceStr(info, "ARG", unsafeUsage.myArgumentName));
    locationList.push_back(fileLoc2);

    return locationList;
}


void CTU::CTUInfo::addCheckInfo(const std::string& check, Check::FileInfo* fileInfo)
{
    mCheckInfo[check] = fileInfo;
}

CTU::CTUInfo::~CTUInfo()
{
    for (auto it = mCheckInfo.begin(); it != mCheckInfo.end(); ++it)
        delete it->second;
}

Check::FileInfo* CTU::CTUInfo::getCheckInfo(const std::string& check) const
{
    auto it = mCheckInfo.find(check);
    if (it != mCheckInfo.cend())
        return it->second;
    return nullptr;
}

void CTU::CTUInfo::reportErr(const ErrorMessage& msg)
{
    mErrors.push_back(msg);
}

bool CTU::CTUInfo::tryLoadFromFile(uint32_t checksum)
{
    mChecksum = checksum;
    if (sourcefile.empty() || !analyzerfileExists)
        return false;

    tinyxml2::XMLDocument doc;
    const tinyxml2::XMLError error = doc.LoadFile(analyzerfile.c_str());
    if (error != tinyxml2::XML_SUCCESS)
        return false;

    const tinyxml2::XMLElement* const rootNode = doc.FirstChildElement();
    if (rootNode == nullptr)
        return false;

    const char* attr = rootNode->Attribute("checksum");
    if (!attr || attr != std::to_string(checksum))
        return false;

    // Take errors and other known information from cache file
    for (const tinyxml2::XMLElement* e = rootNode->FirstChildElement(); e; e = e->NextSiblingElement()) {
        if (std::strcmp(e->Name(), "error") == 0)
            mErrors.emplace_back(e);
        else {
            for (Check* check : Check::instances()) {
                if (check->name() == e->Name()) {
                    Check::FileInfo* fi = check->loadFileInfoFromXml(e);
                    addCheckInfo(check->name(), fi);
                    break;
                }
            }
        }
    }

    return true;
}

void CTU::CTUInfo::writeFile()
{
    if (sourcefile.empty())
        return;

    tinyxml2::XMLDocument doc;
    doc.InsertFirstChild(doc.NewDeclaration());

    tinyxml2::XMLElement* root = doc.NewElement("analyzerinfo");
    doc.InsertEndChild(root);
    root->SetAttribute("checksum", mChecksum);

    for (auto e = mErrors.cbegin(); e != mErrors.cend(); ++e) {
        tinyxml2::XMLElement* error = e->toXMLElement(&doc);
        root->InsertEndChild(error);
    }

    for (auto e = functionCalls.cbegin(); e != functionCalls.cend(); ++e) {
        tinyxml2::XMLElement* error = e->toXMLElement(&doc);
        root->InsertEndChild(error);
    }
    for (auto e = nestedCalls.cbegin(); e != nestedCalls.cend(); ++e) {
        tinyxml2::XMLElement* error = e->toXMLElement(&doc);
        root->InsertEndChild(error);
    }

    for (auto ci = mCheckInfo.cbegin(); ci != mCheckInfo.cend(); ++ci) {
        tinyxml2::XMLElement* checkinfo = ci->second->toXMLElement(&doc);
        if (checkinfo)
            root->InsertEndChild(checkinfo);
    }

    doc.SaveFile(analyzerfile.c_str());
}
